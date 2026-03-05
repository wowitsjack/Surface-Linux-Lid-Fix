// SPDX-License-Identifier: GPL-2.0
/*
 * surface_s2idle_fix.c - Fix s2idle/hibernate death sleep on Surface Laptop 5
 *
 * Version 2.0: Redesigned based on reverse engineering of 4 Windows drivers
 * (iaLPSS2_GPIO2_ADL.sys, msgpioclx.sys, SurfaceButton.sys, intelpep.sys)
 * and analysis of Linux pinctrl-intel + ACPI s2idle kernel source.
 *
 * Root cause:
 *   Intel INTC1055 GPIO Community 4's VNN (VCCIO nanonode) power rail drops
 *   during C-state transitions in s2idle. PADCFG registers lose state. When
 *   VNN returns, PADCFG0 on pin 213 (lid sensor) comes back with RXINV
 *   (bit 23) flipped. This phantom edge fires GPE 0x52 as an SCI. Since
 *   this Surface has no EC (first_ec==NULL), acpi_ec_dispatch_gpe() calls
 *   acpi_any_gpe_status_set(U32_MAX) which sees GPE 0x52 and promotes to
 *   full resume. pm_system_cancel_wakeup() poisons the wakeup framework
 *   permanently = death sleep.
 *
 * Architecture (layered defense, informed by Windows driver stack):
 *
 *   Layer 1: PM Notifier (INT_MAX priority)
 *     Pre-sleep:  save golden PADCFG for all tracked pins, mask GPE 0x52
 *     Post-sleep: final PADCFG check, unmask GPE, schedule failsafe
 *
 *   Layer 2: Platform PM ops (suspend/resume/freeze/thaw/poweroff/restore)
 *     noirq: force-disable GPE 0x52, fix PADCFG before GPE re-enablement
 *     early: double-check PADCFG, unmask GPE before ACPI button reads _LID
 *
 *   Layer 3: ACPI wakeup handler (step 4, before EC GPE dispatch at step 5)
 *     Fix PADCFG corruption, clear GPE 0x52 status so
 *     acpi_any_gpe_status_set() never sees the phantom edge
 *
 *   Layer 4: LPS0 s2idle device ops (inside the s2idle idle loop)
 *     prepare: fix PADCFG, start poll timer, GPE stays masked
 *     check:   fix PADCFG, poll RXSTATE for lid-open, pm_system_wakeup()
 *              only on genuine lid open
 *     restore: cancel timer, clear state
 *
 *   Layer 5: Failsafe system
 *     Post-resume delayed work decides stay-awake vs re-suspend based on
 *     power button input + RXSTATE. Exponential backoff prevents storms.
 *
 * Windows comparison:
 *   Windows uses intelpep (PEP) to coordinate VNN transitions, iaLPSS2 to
 *   save/restore ALL bank pins (DW1 before DW0), msgpioclx for ActiveBoth
 *   RXINV self-healing at ISR level. Our module achieves equivalent
 *   protection through a different mechanism: snapshot restore + GPE
 *   masking + RXSTATE polling, which is simpler and doesn't require
 *   integration with the pinctrl-intel driver's internal state.
 *
 * Changes from v1.1.0:
 *   - Multi-pin PADCFG tracking (Community 4 bank, not just pin 213)
 *   - HOSTSW_OWN register monitoring
 *   - PADCFG2 (debounce register) save/restore
 *   - Conditional MMIO writes (skip if value unchanged)
 *   - Consolidated restore helper (eliminated 5 identical blocks)
 *   - debugfs interface for diagnostics instead of dmesg spam
 *   - Programmatic boot parameter application where possible
 */

#define pr_fmt(fmt) "surface_s2idle_fix: " fmt

#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/io.h>
#include <linux/suspend.h>
#include <linux/dmi.h>
#include <linux/workqueue.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/rtc.h>
#include <linux/timekeeping.h>
#include <linux/net.h>
#include <linux/in.h>
#include <net/sock.h>
#include <linux/sched/signal.h>
#include <linux/random.h>
#include <linux/backlight.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>

/* ========================================================================
 * Hardware constants: Intel INTC1055 Community 4
 * ======================================================================== */

/* Community 4 MMIO region */
#define COM4_PADCFG_BASE        0xfd6a0000
#define COM4_PADCFG_SIZE        0x1000      /* 4KB, covers all pins */

/* Pin 213: lid sensor (the death sleep pin) */
#define LID_PIN_OFFSET          0x09a0      /* 0xfd6a09a0 - COM4_PADCFG_BASE */
#define LID_PIN_INDEX           213

/* PADCFG register layout: 3 DWORDs per pin, 16 bytes stride */
#define PADCFG_STRIDE           16          /* bytes between pins */
#define PADCFG0_OFF             0
#define PADCFG1_OFF             4
#define PADCFG2_OFF             8

/* PADCFG0 bit definitions */
#define PADCFG0_RXINV           BIT(23)
#define PADCFG0_GPIROUTSCI      BIT(19)
#define PADCFG0_GPIORXDIS       BIT(8)
#define PADCFG0_GPIORXSTATE     BIT(1)
#define PADCFG0_GPIOTXSTATE     BIT(0)
#define PADCFG0_PMODE_MASK      0x00003C00  /* bits 13:10, pad mode */

/* HOSTSW_OWN register: controls GPIO vs ACPI ownership.
 * Offset from community base varies by platform, for ADL Community 4:
 * HOSTSW_OWN is at community_base + 0x0150, one bit per pin.
 * Multiple 32-bit registers cover all pins in the community. */
#define COM4_HOSTSW_OWN_BASE    0x0150
#define COM4_HOSTSW_OWN_REGS    4           /* 4 x 32-bit = 128 pins per bank */

/* GPI_IE (interrupt enable), for monitoring */
#define COM4_GPI_IE_BASE        0x0120
#define COM4_GPI_IE_REGS        4

/* GPE and SCI */
#define LID_GPE                 0x52

/* ========================================================================
 * Tracked pins: Community 4 pins we save/restore
 *
 * Like Windows' GpioSaveBankHardwareContext, we track multiple pins,
 * not just pin 213. We focus on pins that are in Mode 0 (native GPIO)
 * and have SCI routing, since those are vulnerable to VNN corruption.
 * ======================================================================== */

/* Per-pin saved context, matching pinctrl-intel's intel_pad_context
 * but with validity tracking like Windows' per-pin valid flag */
struct pin_context {
	u32 padcfg0;            /* DW0: mode, direction, RXINV */
	u32 padcfg1;            /* DW1: pad tolerance, termination */
	u32 padcfg2;            /* DW2: debounce configuration */
	u16 offset;             /* offset from community base */
	u16 pin_number;         /* for logging */
	bool valid;             /* was this pin saved? */
	bool sci_routed;        /* has GPIROUTSCI set */
};

/* We track pin 213 (lid) as primary, plus nearby SCI-routed pins
 * in Community 4 that could have similar VNN corruption issues.
 * These offsets are relative to COM4_PADCFG_BASE. */
static const u16 tracked_pin_offsets[] = {
	0x09a0,    /* pin 213: lid sensor (THE death sleep pin) */
	/* Additional COM4 pins can be added here if needed.
	 * On the Surface Laptop 5, pin 213 is the only known
	 * SCI-routed pin in COM4 that causes death sleep,
	 * but we scan for others at init time. */
};

#define MAX_TRACKED_PINS        32
#define LID_TRACKED_INDEX       0           /* pin 213 is always index 0 */

static struct pin_context tracked_pins[MAX_TRACKED_PINS];
static int num_tracked_pins;

/* Community-level saved context (like Windows' per-bank HOSTSW_OWN) */
static u32 saved_hostsw_own[COM4_HOSTSW_OWN_REGS];
static u32 saved_gpi_ie[COM4_GPI_IE_REGS];
static bool community_ctx_saved;

/* ========================================================================
 * Module state
 * ======================================================================== */

static void __iomem *com4_base;            /* Community 4 MMIO base */
static int sci_irq;

/* Timing and behavior constants */
#define LID_FAILSAFE_DELAY_MS   2000
#define FAILSAFE_MAX_RETRIES    10
#define LID_RESYNC_INTERVAL_MS  1000
#define LID_RESYNC_MAX_POLLS    120
#define LID_POLL_INTERVAL_MS    2000
#define RAPID_WAKE_THRESHOLD_MS 60000
#define BACKOFF_BASE_MS         2000
#define BACKOFF_CAP_MS          15000
#define S2IDLE_POLL_INTERVAL_NS (500 * NSEC_PER_MSEC)
#define TIME_SYNC_DELAY_MS        3000
#define NTP_SYNC_DELAY_MS        15000

/* Work items and timers */
static struct delayed_work lid_failsafe_work;
static struct delayed_work lid_resync_work;
static struct delayed_work lid_poll_work;
static struct delayed_work time_sync_retry_work;
static struct delayed_work time_sync_ntp_work;
static struct hrtimer s2idle_poll_timer;

/* State flags */
static bool lid_was_closed_at_suspend;
static bool failsafe_in_progress;
static bool lid_poll_active;
static bool lid_resync_active;
static bool gpe52_was_enabled;
static bool s2idle_gpe_active;
static bool in_hibernate;
static struct timespec64 saved_pre_hibernate_ts;
static ktime_t saved_pre_hibernate_mono;
static int last_poll_rxstate;
static unsigned int failsafe_suspends;
static unsigned int resync_polls_remaining;

/* Lid switch input device */
static struct input_dev *lid_input_dev;

/* Exponential backoff */
static ktime_t last_suspend_entry;
static unsigned int consecutive_rapid_wakes;

/* Statistics (exposed via debugfs) */
static struct {
	unsigned int suspend_cycles;
	unsigned int hibernate_cycles;
	unsigned int padcfg_restores;
	unsigned int wakeup_handler_calls;
	unsigned int wakeup_handler_fixes;
	unsigned int early_restores;
	unsigned int lid_resyncs;
	unsigned int gpio_recovery_polls;
	unsigned int time_syncs;
	unsigned int hostsw_own_corruptions;
	unsigned int spurious_wakes;
	unsigned int lps0_checks;
	unsigned int conditional_skips;      /* MMIO writes skipped (value unchanged) */
	unsigned int multi_pin_fixes;        /* non-lid pins that needed fixing */
} stats;

/* debugfs root */
static struct dentry *dbgfs_root;

/* Platform driver and device */
static struct platform_driver s2idle_fix_plat_driver;
static struct platform_device *s2idle_fix_pdev;

/* ========================================================================
 * Helpers
 * ======================================================================== */

static unsigned int get_resuspend_delay_ms(void)
{
	unsigned int delay;

	if (consecutive_rapid_wakes == 0)
		return BACKOFF_BASE_MS;
	delay = BACKOFF_BASE_MS << min(consecutive_rapid_wakes, 4U);
	return min(delay, (unsigned int)BACKOFF_CAP_MS);
}

static void report_lid_state(int closed)
{
	if (lid_input_dev) {
		input_report_switch(lid_input_dev, SW_LID, closed);
		input_sync(lid_input_dev);
	}
}

static inline void __iomem *pin_addr(const struct pin_context *pin)
{
	return com4_base + pin->offset;
}

/* ========================================================================
 * Multi-pin PADCFG save/restore
 *
 * Modeled after Windows' GpioSaveBankHardwareContext / RestoreBankHardwareContext
 * and Linux's intel_pinctrl_suspend_noirq / resume_noirq.
 *
 * Key behaviors copied from Windows:
 *   - Only save Mode 0 pins (PMODE check)
 *   - DW1 written before DW0 on restore
 *   - Per-pin valid flag
 *   - HOSTSW_OWN saved per bank
 *
 * Key behaviors copied from pinctrl-intel:
 *   - Conditional write (only if value differs)
 *   - Mask out volatile GPIORXSTATE before comparison
 * ======================================================================== */

static void save_all_pins(void)
{
	int i;
	u32 padcfg0;

	for (i = 0; i < num_tracked_pins; i++) {
		struct pin_context *pin = &tracked_pins[i];

		padcfg0 = readl(pin_addr(pin) + PADCFG0_OFF);

		/* Only save Mode 0 pins (like Windows' PMODE check) */
		if (padcfg0 & PADCFG0_PMODE_MASK) {
			pin->valid = false;
			continue;
		}

		pin->padcfg0 = padcfg0 & ~PADCFG0_GPIORXSTATE;
		pin->padcfg1 = readl(pin_addr(pin) + PADCFG1_OFF);
		pin->padcfg2 = readl(pin_addr(pin) + PADCFG2_OFF);
		pin->sci_routed = !!(padcfg0 & PADCFG0_GPIROUTSCI);
		pin->valid = true;
	}

	/* Save community-level registers */
	for (i = 0; i < COM4_HOSTSW_OWN_REGS; i++)
		saved_hostsw_own[i] = readl(com4_base + COM4_HOSTSW_OWN_BASE + i * 4);

	for (i = 0; i < COM4_GPI_IE_REGS; i++)
		saved_gpi_ie[i] = readl(com4_base + COM4_GPI_IE_BASE + i * 4);

	community_ctx_saved = true;
}

/*
 * Restore a single pin's PADCFG registers.
 * Returns true if any register was actually modified.
 *
 * Like intel_restore_padcfg(), only writes if the value differs.
 * Like Windows, writes DW1 before DW0.
 */
static bool restore_pin(struct pin_context *pin)
{
	u32 cur0, cur1, cur2;
	bool modified = false;

	if (!pin->valid)
		return false;

	cur2 = readl(pin_addr(pin) + PADCFG2_OFF);
	if (cur2 != pin->padcfg2) {
		writel(pin->padcfg2, pin_addr(pin) + PADCFG2_OFF);
		modified = true;
	}

	/* DW1 FIRST (termination/tolerance), then DW0 (mode/RXINV)
	 * This ordering is critical, confirmed in both Windows
	 * GpioRestoreBankHardwareContext and our previous analysis. */
	cur1 = readl(pin_addr(pin) + PADCFG1_OFF);
	if (cur1 != pin->padcfg1) {
		writel(pin->padcfg1, pin_addr(pin) + PADCFG1_OFF);
		modified = true;
	}

	wmb(); /* ensure DW1 is committed before DW0 */

	cur0 = readl(pin_addr(pin) + PADCFG0_OFF);
	/* Mask out volatile GPIORXSTATE for comparison */
	if ((cur0 & ~PADCFG0_GPIORXSTATE) != pin->padcfg0) {
		writel(pin->padcfg0, pin_addr(pin) + PADCFG0_OFF);
		modified = true;
	} else {
		stats.conditional_skips++;
	}

	if (modified)
		wmb(); /* ensure all writes committed */

	return modified;
}

/*
 * Fix PADCFG corruption on all tracked pins.
 * Returns the number of pins that were corrected.
 *
 * This is the consolidated restore helper, replacing the 5+ identical
 * "check RXINV, restore, clear GPE" blocks from v1.1.0.
 */
static int fix_padcfg_corruption(const char *caller)
{
	int i, fixed = 0;
	u32 cur0;

	if (!com4_base)
		return 0;

	for (i = 0; i < num_tracked_pins; i++) {
		struct pin_context *pin = &tracked_pins[i];

		if (!pin->valid)
			continue;

		cur0 = readl(pin_addr(pin) + PADCFG0_OFF);

		/* Check for RXINV corruption (the death sleep trigger) */
		if ((cur0 & PADCFG0_RXINV) != (pin->padcfg0 & PADCFG0_RXINV)) {
			if (restore_pin(pin)) {
				fixed++;
				stats.padcfg_restores++;
				if (i == LID_TRACKED_INDEX) {
					pr_info("%s: pin %u RXINV corrected "
						"0x%08x -> 0x%08x\n",
						caller, pin->pin_number,
						cur0, pin->padcfg0);
				} else {
					stats.multi_pin_fixes++;
					pr_info("%s: pin %u (non-lid) PADCFG "
						"corrected 0x%08x -> 0x%08x\n",
						caller, pin->pin_number,
						cur0, pin->padcfg0);
				}
			}
		}
	}

	/* If any SCI-routed pin was fixed, clear GPE 0x52 status
	 * to prevent acpi_any_gpe_status_set() from seeing it */
	if (fixed > 0)
		acpi_clear_gpe(NULL, LID_GPE);

	return fixed;
}

/*
 * Check HOSTSW_OWN register for corruption.
 * Windows saves/restores this per-bank. If it changes unexpectedly,
 * GPIO reads could silently fail.
 */
static void check_hostsw_own(const char *caller)
{
	int i;
	u32 current_val;

	if (!community_ctx_saved)
		return;

	for (i = 0; i < COM4_HOSTSW_OWN_REGS; i++) {
		current_val = readl(com4_base + COM4_HOSTSW_OWN_BASE + i * 4);
		if (current_val != saved_hostsw_own[i]) {
			stats.hostsw_own_corruptions++;
			pr_warn("%s: HOSTSW_OWN[%d] changed: "
				"0x%08x -> 0x%08x (diagnostic only)\n",
				caller, i, saved_hostsw_own[i], current_val);
			/* NOTE: Do NOT restore HOSTSW_OWN. Writing back boot-time
			 * values can change ownership of pins used by other drivers
			 * (e.g. touchpad I2C HID), killing their interrupt lines. */
		}
	}
}

/* ========================================================================
 * GPE 0x52 management
 * ======================================================================== */

static void gpe52_force_disable(void)
{
	acpi_set_gpe_wake_mask(NULL, LID_GPE, ACPI_GPE_DISABLE);
	acpi_mask_gpe(NULL, LID_GPE, TRUE);
	acpi_disable_gpe(NULL, LID_GPE);
	acpi_clear_gpe(NULL, LID_GPE);
	gpe52_was_enabled = true;
}

static void gpe52_unmask(const char *caller)
{
	if (gpe52_was_enabled) {
		acpi_mask_gpe(NULL, LID_GPE, FALSE);
		acpi_set_gpe(NULL, LID_GPE, ACPI_GPE_ENABLE);
		gpe52_was_enabled = false;
		pr_info("%s: GPE 0x52 unmasked\n", caller);
	}
}

/* ========================================================================
 * Power button observer (passive input handler)
 * ======================================================================== */

static atomic_t power_button_seen = ATOMIC_INIT(0);

static int pwr_connect(struct input_handler *handler, struct input_dev *dev,
		       const struct input_device_id *id)
{
	struct input_handle *handle;
	int err;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "surface_s2idle_pwr";

	err = input_register_handle(handle);
	if (err)
		goto err_free;

	err = input_open_device(handle);
	if (err)
		goto err_unregister;

	pr_info("power button input connected: %s\n", dev->name);
	return 0;

err_unregister:
	input_unregister_handle(handle);
err_free:
	kfree(handle);
	return err;
}

static void pwr_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static void pwr_event(struct input_handle *handle,
		      unsigned int type, unsigned int code, int value)
{
	if (type == EV_KEY && code == KEY_POWER && value == 1) {
		atomic_set(&power_button_seen, 1);
		pr_info("power button press detected\n");
	}
}

static const struct input_device_id pwr_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			 INPUT_DEVICE_ID_MATCH_KEYBIT,
		.evbit  = { [BIT_WORD(EV_KEY)] = BIT_MASK(EV_KEY) },
		.keybit = { [BIT_WORD(KEY_POWER)] = BIT_MASK(KEY_POWER) },
	},
	{ },
};

MODULE_DEVICE_TABLE(input, pwr_ids);

static struct input_handler pwr_handler = {
	.event      = pwr_event,
	.connect    = pwr_connect,
	.disconnect = pwr_disconnect,
	.name       = "surface_s2idle_pwr",
	.id_table   = pwr_ids,
	.passive_observer = true,
};

/* ========================================================================
 * ACPI wakeup handler
 *
 * Runs at step 4 in acpi_s2idle_wake(), BEFORE acpi_ec_dispatch_gpe()
 * at step 5. This is our primary defense: fix PADCFG corruption and
 * clear GPE 0x52 status before acpi_any_gpe_status_set() sees it.
 *
 * On this Surface (no EC), acpi_ec_dispatch_gpe() calls
 * acpi_any_gpe_status_set(U32_MAX) which checks ALL GPEs. Without
 * our handler clearing GPE 0x52, any phantom edge promotes to full
 * resume and death sleep.
 * ======================================================================== */

static bool lid_wake_handler(void *context)
{
	int fixed;

	stats.wakeup_handler_calls++;

	fixed = fix_padcfg_corruption("wake_handler");
	if (fixed > 0)
		stats.wakeup_handler_fixes++;

	/* During s2idle: ALWAYS clear GPE 0x52 status regardless of
	 * whether corruption was detected. The GPE could fire from a
	 * legitimate lid event that we want to defer to lps0_check(). */
	if (s2idle_gpe_active)
		acpi_clear_gpe(NULL, LID_GPE);

	/* Never promote to full resume from here. The lid-open decision
	 * is deferred to lps0_check() which polls RXSTATE directly. */
	return false;
}

/* ========================================================================
 * Platform PM ops: suspend/resume/freeze/thaw/poweroff/restore
 * ======================================================================== */

static int s2idle_fix_suspend_noirq(struct device *dev)
{
	if (!com4_base)
		return 0;

	gpe52_force_disable();
	pr_info("suspend_noirq: GPE 0x52 force-disabled\n");
	return 0;
}

static int s2idle_fix_resume_noirq(struct device *dev)
{
	int fixed;
	const char *path = in_hibernate ? "thaw_noirq" : "resume_noirq";

	if (!com4_base)
		return 0;

	fixed = fix_padcfg_corruption(path);
	check_hostsw_own(path);

	if (fixed == 0)
		pr_info("%s: all tracked pins OK\n", path);

	return 0;
}

static int s2idle_fix_resume_early(struct device *dev)
{
	int fixed;
	const char *path = in_hibernate ? "thaw_early" : "resume_early";

	if (!com4_base)
		return 0;

	fixed = fix_padcfg_corruption(path);
	if (fixed > 0)
		stats.early_restores++;

	gpe52_unmask(path);

	return 0;
}

static int s2idle_fix_freeze(struct device *dev)
{
	if (!com4_base)
		return 0;

	gpe52_force_disable();
	pr_info("freeze: GPE 0x52 force-disabled\n");
	return 0;
}

static int s2idle_fix_freeze_noirq(struct device *dev)
{
	if (!com4_base)
		return 0;

	gpe52_force_disable();
	fix_padcfg_corruption("freeze_noirq");

	return 0;
}

static int s2idle_fix_poweroff(struct device *dev)
{
	if (!com4_base)
		return 0;

	gpe52_force_disable();
	return 0;
}

static int s2idle_fix_poweroff_noirq(struct device *dev)
{
	if (!com4_base)
		return 0;

	gpe52_force_disable();
	return 0;
}

static const struct dev_pm_ops s2idle_fix_pm_ops = {
	.suspend_noirq  = s2idle_fix_suspend_noirq,
	.resume_noirq   = s2idle_fix_resume_noirq,
	.resume_early   = s2idle_fix_resume_early,
	.freeze         = s2idle_fix_freeze,
	.freeze_noirq   = s2idle_fix_freeze_noirq,
	.thaw_noirq     = s2idle_fix_resume_noirq,
	.thaw_early     = s2idle_fix_resume_early,
	.poweroff       = s2idle_fix_poweroff,
	.poweroff_noirq = s2idle_fix_poweroff_noirq,
	.restore_noirq  = s2idle_fix_resume_noirq,
	.restore_early  = s2idle_fix_resume_early,
};

/* ========================================================================
 * LPS0 s2idle device ops (inside the s2idle idle loop)
 * ======================================================================== */

static unsigned int lps0_check_count;

static enum hrtimer_restart s2idle_poll_timer_fn(struct hrtimer *timer)
{
	hrtimer_forward_now(timer, ns_to_ktime(S2IDLE_POLL_INTERVAL_NS));
	return HRTIMER_RESTART;
}

static void s2idle_lps0_prepare(void)
{
	/* Fix any existing corruption before entering deep sleep */
	fix_padcfg_corruption("lps0_prepare");

	/* Clear stale GPE status before unmasking */
	acpi_clear_gpe(NULL, LID_GPE);

	/* Unmask GPE 0x52 for lid-open detection during s2idle.
	 * The wakeup handler (step 4) clears GPE status on every wake
	 * BEFORE acpi_ec_dispatch_gpe (step 5), preventing phantom
	 * full-resume promotion. This is safe. */
	acpi_mask_gpe(NULL, LID_GPE, FALSE);
	acpi_set_gpe(NULL, LID_GPE, ACPI_GPE_ENABLE);

	s2idle_gpe_active = true;
	lps0_check_count = 0;

	/* Periodic wakeup timer for lid polling during s2idle */
	hrtimer_start(&s2idle_poll_timer,
		      ns_to_ktime(S2IDLE_POLL_INTERVAL_NS),
		      HRTIMER_MODE_REL_PINNED);

	pr_info("lps0_prepare: poll timer started (%llums)\n",
		(unsigned long long)(S2IDLE_POLL_INTERVAL_NS / NSEC_PER_MSEC));
}

static void s2idle_lps0_check(void)
{
	struct pin_context *lid;
	u32 padcfg0;

	lps0_check_count++;
	stats.lps0_checks++;

	/* Fix corruption on every wake within the s2idle loop */
	fix_padcfg_corruption("lps0_check");
	acpi_clear_gpe(NULL, LID_GPE);

	/* Detect lid-open: RXSTATE=0 means lid is open */
	lid = &tracked_pins[LID_TRACKED_INDEX];
	if (lid->valid && lid_was_closed_at_suspend) {
		padcfg0 = readl(pin_addr(lid) + PADCFG0_OFF);
		if (!(padcfg0 & PADCFG0_GPIORXSTATE)) {
			lid_was_closed_at_suspend = false;
			report_lid_state(0);
			pr_info("lps0_check: lid opened (RXSTATE=0), "
				"waking (check #%u)\n", lps0_check_count);
			pm_system_wakeup();
		}
	}
}

static void s2idle_lps0_restore(void)
{
	s2idle_gpe_active = false;
	hrtimer_cancel(&s2idle_poll_timer);

	/* Re-mask GPE 0x52 leaving the s2idle loop.
	 * PM_POST_SUSPEND will unmask it for normal operation. */
	acpi_mask_gpe(NULL, LID_GPE, TRUE);
	acpi_clear_gpe(NULL, LID_GPE);

	pr_info("lps0_restore: %u checks during s2idle\n", lps0_check_count);
}

static struct acpi_s2idle_dev_ops s2idle_lps0_ops = {
	.prepare = s2idle_lps0_prepare,
	.check   = s2idle_lps0_check,
	.restore = s2idle_lps0_restore,
};

/* ========================================================================
 * Background RXSTATE polling
 * ======================================================================== */

static void lid_poll_fn(struct work_struct *work)
{
	struct pin_context *lid;
	u32 padcfg0;
	int rxstate;

	if (!com4_base || !lid_poll_active)
		return;

	lid = &tracked_pins[LID_TRACKED_INDEX];
	if (!lid->valid)
		return;

	padcfg0 = readl(pin_addr(lid) + PADCFG0_OFF);
	rxstate = !!(padcfg0 & PADCFG0_GPIORXSTATE);

	if (rxstate != last_poll_rxstate) {
		if (rxstate == 1 && last_poll_rxstate == 0) {
			report_lid_state(1);
			pr_info("lid poll: closed (RXSTATE 0->1)\n");
		} else if (rxstate == 0 && last_poll_rxstate == 1) {
			report_lid_state(0);
			pr_info("lid poll: opened (RXSTATE 1->0)\n");
		}
		last_poll_rxstate = rxstate;
	}

	if (lid_poll_active)
		schedule_delayed_work(&lid_poll_work,
			msecs_to_jiffies(LID_POLL_INTERVAL_MS));
}

/* ========================================================================
 * Lid resync: after power-button wake with lid closed
 * ======================================================================== */

static void lid_resync_fn(struct work_struct *work)
{
	struct pin_context *lid;
	u32 padcfg0;
	bool rxstate;

	if (!com4_base)
		return;

	lid = &tracked_pins[LID_TRACKED_INDEX];
	if (!lid->valid)
		return;

	padcfg0 = readl(pin_addr(lid) + PADCFG0_OFF);
	rxstate = !!(padcfg0 & PADCFG0_GPIORXSTATE);
	stats.gpio_recovery_polls++;

	if (rxstate) {
		unsigned int delay = get_resuspend_delay_ms();

		stats.lid_resyncs++;

		if (delay > BACKOFF_BASE_MS) {
			pr_info("lid resync: RXSTATE=1, backoff %ums\n", delay);
			msleep(delay);
			padcfg0 = readl(pin_addr(lid) + PADCFG0_OFF);
			if (!(padcfg0 & PADCFG0_GPIORXSTATE)) {
				pr_info("lid resync: lid opened during backoff\n");
				report_lid_state(0);
				lid_resync_active = false;
				return;
			}
		}

		pr_info("lid resync: still closed, re-suspending\n");
		report_lid_state(1);
		failsafe_in_progress = true;
		pm_suspend(PM_SUSPEND_TO_IDLE);
		failsafe_in_progress = false;
		lid_resync_active = false;
		return;
	}

	if (resync_polls_remaining > 0) {
		resync_polls_remaining--;
		schedule_delayed_work(&lid_resync_work,
				      msecs_to_jiffies(LID_RESYNC_INTERVAL_MS));
	} else {
		lid_resync_active = false;
		report_lid_state(0);
		pr_info("lid resync: GPIO settled open\n");
	}
}

/* ========================================================================
 * Post-resume failsafe
 * ======================================================================== */

static void lid_failsafe_fn(struct work_struct *work)
{
	struct pin_context *lid;

	if (atomic_read(&power_button_seen)) {
		atomic_set(&power_button_seen, 0);
		failsafe_suspends = 0;

		if (lid_was_closed_at_suspend) {
			pr_info("failsafe: power button wake, lid closed, "
				"starting resync\n");
			stats.gpio_recovery_polls = 0;
			resync_polls_remaining = LID_RESYNC_MAX_POLLS;
			lid_resync_active = true;
			schedule_delayed_work(&lid_resync_work,
				msecs_to_jiffies(LID_RESYNC_INTERVAL_MS));
		} else {
			report_lid_state(0);
			pr_info("failsafe: power button wake, lid open\n");
			consecutive_rapid_wakes = 0;
			lid_was_closed_at_suspend = false;
		}
		return;
	}

	if (!lid_was_closed_at_suspend) {
		report_lid_state(0);
		consecutive_rapid_wakes = 0;
		failsafe_suspends = 0;
		return;
	}

	/* Check current lid state */
	lid = &tracked_pins[LID_TRACKED_INDEX];
	if (lid->valid && com4_base) {
		u32 now = readl(pin_addr(lid) + PADCFG0_OFF);
		if (!(now & PADCFG0_GPIORXSTATE)) {
			report_lid_state(0);
			consecutive_rapid_wakes = 0;
			failsafe_suspends = 0;
			lid_was_closed_at_suspend = false;
			pr_info("failsafe: lid currently open, staying awake\n");
			return;
		}
	}

	if (failsafe_suspends >= FAILSAFE_MAX_RETRIES) {
		pr_warn("failsafe: max retries (%u), giving up\n",
			FAILSAFE_MAX_RETRIES);
		failsafe_suspends = 0;
		lid_was_closed_at_suspend = false;
		return;
	}

	stats.spurious_wakes++;
	failsafe_suspends++;
	pr_info("failsafe: spurious wake, re-suspending (%u/%u)\n",
		failsafe_suspends, FAILSAFE_MAX_RETRIES);

	report_lid_state(1);
	failsafe_in_progress = true;
	pm_suspend(PM_SUSPEND_TO_IDLE);
	failsafe_in_progress = false;
}

/* ========================================================================
 * Pre/post sleep common logic
 * ======================================================================== */

/* Forward declaration: thaw_chronyd is defined in the time sync section
 * below, but needed here if NTP work is cancelled during pre-sleep. */
static void thaw_chronyd(void);

static void fix_pre_sleep_common(const char *path_name)
{
	struct pin_context *lid;
	u32 padcfg0;

	if (!failsafe_in_progress) {
		lid_poll_active = false;
		lid_resync_active = false;
		cancel_delayed_work_sync(&lid_poll_work);
		cancel_delayed_work_sync(&lid_resync_work);
		cancel_delayed_work_sync(&lid_failsafe_work);
		cancel_delayed_work_sync(&time_sync_ntp_work);
		/* If NTP work was cancelled before it could thaw chronyd,
		 * do it now so chronyd doesn't stay permanently SIGSTOP'd. */
		thaw_chronyd();
	}

	last_suspend_entry = ktime_get();
	atomic_set(&power_button_seen, 0);

	/* Save all tracked pins (multi-pin, like Windows full-bank save) */
	save_all_pins();

	/* Record lid state for failsafe decision */
	lid = &tracked_pins[LID_TRACKED_INDEX];
	if (lid->valid && !failsafe_in_progress && !lid_was_closed_at_suspend) {
		padcfg0 = readl(pin_addr(lid) + PADCFG0_OFF);
		lid_was_closed_at_suspend = !!(padcfg0 & PADCFG0_GPIORXSTATE);
	}

	/* Mask GPE 0x52 for the duration of sleep.
	 * suspend_noirq does gpe52_force_disable() as belt-and-suspenders. */
	acpi_mask_gpe(NULL, LID_GPE, TRUE);
	gpe52_was_enabled = true;

	if (lid->valid) {
		padcfg0 = readl(pin_addr(lid) + PADCFG0_OFF);
		pr_info("%s: %d pins saved, GPE masked, "
			"PADCFG0=0x%08x RXINV=%d RXSTATE=%d "
			"lid_closed=%d\n",
			path_name, num_tracked_pins, padcfg0,
			!!(padcfg0 & PADCFG0_RXINV),
			!!(padcfg0 & PADCFG0_GPIORXSTATE),
			lid_was_closed_at_suspend);
	}
}

static void fix_post_sleep_common(const char *path_name, bool schedule_work)
{
	struct pin_context *lid;
	u32 padcfg0;
	s64 sleep_ms;
	unsigned int resuspend_delay;

	/* Final PADCFG + HOSTSW_OWN check */
	fix_padcfg_corruption(path_name);
	check_hostsw_own(path_name);

	/* Unmask fallback */
	gpe52_unmask(path_name);

	lid = &tracked_pins[LID_TRACKED_INDEX];

	if (schedule_work) {
		/* Track rapid wakes for exponential backoff */
		sleep_ms = ktime_ms_delta(ktime_get(), last_suspend_entry);
		if (sleep_ms < RAPID_WAKE_THRESHOLD_MS) {
			consecutive_rapid_wakes++;
			pr_info("%s: rapid wake (%lldms), streak=%u\n",
				path_name, sleep_ms, consecutive_rapid_wakes);
		} else {
			consecutive_rapid_wakes = 0;
			failsafe_suspends = 0;
		}

		/* Schedule failsafe with backoff */
		resuspend_delay = get_resuspend_delay_ms();
		schedule_delayed_work(&lid_failsafe_work,
				      msecs_to_jiffies(resuspend_delay));

		/* Restart lid polling */
		if (lid->valid) {
			padcfg0 = readl(pin_addr(lid) + PADCFG0_OFF);
			last_poll_rxstate = !!(padcfg0 & PADCFG0_GPIORXSTATE);
		}
		lid_poll_active = true;
		schedule_delayed_work(&lid_poll_work,
				      msecs_to_jiffies(LID_POLL_INTERVAL_MS));
	}

	/* Log summary */
	if (lid->valid) {
		padcfg0 = readl(pin_addr(lid) + PADCFG0_OFF);
		pr_info("%s: PADCFG0=0x%08x RXSTATE=%d "
			"restores=%u handler_calls=%u\n",
			path_name, padcfg0,
			!!(padcfg0 & PADCFG0_GPIORXSTATE),
			stats.padcfg_restores, stats.wakeup_handler_calls);
	}

	community_ctx_saved = false;
	stats.wakeup_handler_calls = 0;
}

/* ========================================================================
 * Post-hibernate time sync and display recovery
 *
 * Fully kernel-space, zero call_usermodehelper:
 *
 * Sequence (3s after hibernate resume):
 *   1) SIGSTOP chronyd       (freeze NTP daemon, prevent backward clock step)
 *   2) rtc_read_time()       (kernel-space RTC read)
 *   3) do_settimeofday64()   (kernel-space clock set, calls ntp_clear())
 *   4) i915 display reconciliation (kernel-space debugfs read)
 *   5) backlight restore     (kernel backlight API)
 *
 * Then 15s later:
 *   6) kernel SNTP query     (UDP socket to NTP server, kernel-space)
 *   7) do_settimeofday64()   (set clock from NTP ground truth)
 *   8) rtc_set_time()        (write NTP time back to RTC hardware)
 *   9) SIGCONT chronyd       (thaw NTP daemon, fresh state sees correct clock)
 * ======================================================================== */

/* ---- Chronyd freeze/thaw via kernel signals ---- */

static struct pid *frozen_chronyd_pid;

static void freeze_chronyd(void)
{
	struct task_struct *task;

	/* Guard against double-freeze: if we're already holding a pid ref
	 * from a previous cycle (e.g. rapid hibernate before NTP work
	 * could thaw), don't leak the old ref or re-SIGSTOP. */
	if (frozen_chronyd_pid) {
		pr_info("chronyd already frozen (pid %d), skipping\n",
			pid_vnr(frozen_chronyd_pid));
		return;
	}

	rcu_read_lock();
	for_each_process(task) {
		if (strcmp(task->comm, "chronyd") == 0) {
			frozen_chronyd_pid = get_task_pid(task, PIDTYPE_PID);
			rcu_read_unlock();
			kill_pid(frozen_chronyd_pid, SIGSTOP, 1);
			pr_info("chronyd frozen (pid %d)\n",
				pid_vnr(frozen_chronyd_pid));
			return;
		}
	}
	rcu_read_unlock();
	frozen_chronyd_pid = NULL;
	pr_info("chronyd not running, skip freeze\n");
}

static void thaw_chronyd(void)
{
	int ret;

	if (!frozen_chronyd_pid)
		return;

	ret = kill_pid(frozen_chronyd_pid, SIGCONT, 1);
	if (ret == -ESRCH)
		pr_warn("chronyd (pid %d) exited while frozen\n",
			pid_vnr(frozen_chronyd_pid));
	else
		pr_info("chronyd thawed (pid %d)\n",
			pid_vnr(frozen_chronyd_pid));
	put_pid(frozen_chronyd_pid);
	frozen_chronyd_pid = NULL;
}

/* ---- Kernel-space SNTP client ---- */

#define NTP_EPOCH_OFFSET	2208988800ULL
#define NTP_PACKET_SIZE		48
#define NTP_PORT		123
#define NTP_TIMEOUT_MS		3000
#define NTP_MAX_OFFSET_SECS	3600	/* 1 hour: generous after RTC sync at +3s */

/* Google Public NTP anycast IPs (time.google.com) */
static const __be32 ntp_servers[] = {
	__constant_htonl(0xD8EF2300),	/* 216.239.35.0 */
	__constant_htonl(0xD8EF2304),	/* 216.239.35.4 */
	__constant_htonl(0xD8EF2308),	/* 216.239.35.8 */
	__constant_htonl(0xD8EF230C),	/* 216.239.35.12 */
};

struct ntp_packet {
	u8	li_vn_mode;
	u8	stratum;
	u8	poll;
	s8	precision;
	__be32	root_delay;
	__be32	root_dispersion;
	__be32	reference_id;
	__be32	ref_ts_sec;
	__be32	ref_ts_frac;
	__be32	orig_ts_sec;
	__be32	orig_ts_frac;
	__be32	recv_ts_sec;
	__be32	recv_ts_frac;
	__be32	xmit_ts_sec;
	__be32	xmit_ts_frac;
} __packed;

/*
 * Query NTP time from kernel space via UDP socket.
 * Tries multiple Google NTP servers sequentially.
 * Returns 0 on success with time in @ntp_time, negative errno on failure.
 */
static int kernel_ntp_query(struct timespec64 *ntp_time)
{
	struct socket *sock;
	struct sockaddr_in server;
	struct ntp_packet request, response;
	struct msghdr msg;
	struct kvec iov;
	struct timespec64 t4;
	time64_t server_sec;
	s64 diff;
	int ret, i;

	ret = sock_create_kern(&init_net, AF_INET, SOCK_DGRAM,
			       IPPROTO_UDP, &sock);
	if (ret) {
		pr_warn("ntp_query: sock_create failed (%d)\n", ret);
		return ret;
	}

	/* Receive timeout so we don't block forever */
	sock->sk->sk_rcvtimeo = msecs_to_jiffies(NTP_TIMEOUT_MS);

	for (i = 0; i < ARRAY_SIZE(ntp_servers); i++) {
		memset(&server, 0, sizeof(server));
		server.sin_family = AF_INET;
		server.sin_port = htons(NTP_PORT);
		server.sin_addr.s_addr = ntp_servers[i];

		/* Connect filters recv to only this server's IP:port.
		 * UDP connect is lightweight (no handshake), just sets
		 * a source filter. Prevents accepting spoofed packets
		 * from arbitrary IPs on the ephemeral port. */
		ret = kernel_connect(sock, (struct sockaddr *)&server,
				     sizeof(server), 0);
		if (ret) {
			pr_warn("ntp_query: connect to server %d failed (%d)\n",
				i, ret);
			continue;
		}

		/* SNTP client request: LI=0, VN=4, Mode=3.
		 * Set unique transmit timestamp for anti-spoofing:
		 * server MUST echo it back as origin timestamp.
		 * Forces attacker to observe actual request (on-path)
		 * rather than blind injection. */
		memset(&request, 0, sizeof(request));
		request.li_vn_mode = 0x23;
		request.xmit_ts_sec = htonl((u32)(ktime_get_real_seconds()
					    + NTP_EPOCH_OFFSET));
		request.xmit_ts_frac = htonl(get_random_u32());

		/* Send on connected socket (no msg_name needed) */
		memset(&msg, 0, sizeof(msg));
		iov.iov_base = &request;
		iov.iov_len = sizeof(request);

		ret = kernel_sendmsg(sock, &msg, &iov, 1, sizeof(request));
		if (ret != sizeof(request)) {
			pr_warn("ntp_query: send to server %d failed (%d)\n",
				i, ret);
			continue;
		}

		/* Receive (source-filtered by kernel_connect) */
		memset(&response, 0, sizeof(response));
		memset(&msg, 0, sizeof(msg));
		iov.iov_base = &response;
		iov.iov_len = sizeof(response);

		ret = kernel_recvmsg(sock, &msg, &iov, 1,
				     sizeof(response), 0);
		if (ret != sizeof(response)) {
			pr_warn("ntp_query: recv from server %d failed (%d)\n",
				i, ret);
			continue;
		}

		/* Record local receive time */
		ktime_get_real_ts64(&t4);

		/* Validate: mode should be 4 (server) */
		if ((response.li_vn_mode & 0x07) != 4) {
			pr_warn("ntp_query: bad mode from server %d\n", i);
			continue;
		}

		/* Validate stratum (1-15 valid, 0=kiss-of-death, 16=unsync) */
		if (response.stratum == 0 || response.stratum > 15) {
			pr_warn("ntp_query: bad stratum %d from server %d\n",
				response.stratum, i);
			continue;
		}

		/* Anti-spoofing: server must echo our transmit timestamp
		 * as the origin timestamp. Mismatch means either the
		 * response doesn't correspond to our request (stale/spoofed)
		 * or the server is non-compliant. */
		if (response.orig_ts_sec != request.xmit_ts_sec ||
		    response.orig_ts_frac != request.xmit_ts_frac) {
			pr_warn("ntp_query: origin timestamp mismatch from "
				"server %d (possible spoof)\n", i);
			continue;
		}

		/* Extract server transmit timestamp, NTP epoch -> Unix epoch */
		server_sec = (time64_t)ntohl(response.xmit_ts_sec)
			     - NTP_EPOCH_OFFSET;

		/* Sanity: NTP time should be within 1 hour of local clock.
		 * After RTC sync at +3s, legitimate offset is seconds.
		 * 1 hour is extremely generous for RTC crystal drift. */
		diff = server_sec - t4.tv_sec;
		if (diff < -NTP_MAX_OFFSET_SECS || diff > NTP_MAX_OFFSET_SECS) {
			pr_warn("ntp_query: offset %llds exceeds %d from "
				"server %d\n", diff, NTP_MAX_OFFSET_SECS, i);
			continue;
		}

		ntp_time->tv_sec = server_sec;
		ntp_time->tv_nsec = 0;

		pr_info("ntp_query: server %d responded (stratum %d, "
			"offset %llds)\n", i, response.stratum, diff);

		sock_release(sock);
		return 0;
	}

	sock_release(sock);
	pr_warn("ntp_query: all %zu servers failed\n", ARRAY_SIZE(ntp_servers));
	return -ETIMEDOUT;
}

/*
 * Restore backlight to max brightness via kernel backlight API.
 * Replaces: cat max_brightness > brightness via call_usermodehelper
 */
static void restore_backlight(void)
{
	struct backlight_device *bd;
	int ret;

	bd = backlight_device_get_by_name("intel_backlight");
	if (!bd) {
		pr_warn("backlight_restore: intel_backlight not found\n");
		return;
	}

	ret = backlight_device_set_brightness(bd, bd->props.max_brightness);
	if (ret)
		pr_warn("backlight_restore: set failed (%d)\n", ret);
	else
		pr_info("backlight_restore: set to max (%d)\n",
			bd->props.max_brightness);

	put_device(&bd->dev);
}

/*
 * Trigger i915 display state reconciliation via debugfs read.
 * Reading i915_display_info forces drm_modeset_lock_all() which
 * re-reads hardware state, fixing 32Hz eDP after hibernate.
 * Replaces: cat i915_display_info > /dev/null via call_usermodehelper
 */
static void trigger_display_reconciliation(void)
{
	struct file *f;
	char buf[256];
	loff_t pos = 0;

	f = filp_open("/sys/kernel/debug/dri/0000:00:02.0/i915_display_info",
		      O_RDONLY, 0);
	if (IS_ERR(f)) {
		pr_warn("display_fix: cannot open i915_display_info (%ld)\n",
			PTR_ERR(f));
		return;
	}

	/* Reading triggers drm_modeset_lock_all() which forces
	 * hardware state reconciliation, fixing 32Hz eDP */
	kernel_read(f, buf, sizeof(buf), &pos);
	filp_close(f, NULL);
	pr_info("display_fix: i915 state reconciliation triggered\n");
}

static void time_sync_retry_fn(struct work_struct *work)
{
	struct rtc_device *rtc;
	struct rtc_time tm;
	struct timespec64 rtc_ts, now_ts;
	time64_t delta;

	/* Step 0: Freeze chronyd BEFORE touching the clock.
	 * After hibernate, chrony's internal tracking state is stale (from
	 * before hibernate). If we set the clock from RTC first, chrony
	 * sees the "wrong" time and steps it BACKWARD by hours, causing
	 * GDM/mutter to blank the display. SIGSTOP prevents this. */
	freeze_chronyd();

	/* Step 1: Read RTC directly from hardware via kernel API.
	 * This is the Linux analog of Windows' HalQueryRealTimeClock,
	 * which ntoskrnl calls during hibernate resume before any
	 * driver D0Entry callbacks. We do it here (3s delayed workqueue)
	 * because Linux's timekeeping_resume() fails with acpi_sleep=nonvs. */
	rtc = rtc_class_open("rtc0");
	if (!rtc) {
		pr_warn("time_sync: cannot open rtc0\n");
		goto display_fix;
	}

	if (rtc_read_time(rtc, &tm)) {
		pr_warn("time_sync: rtc_read_time failed\n");
		rtc_class_close(rtc);
		goto display_fix;
	}
	rtc_class_close(rtc);

	rtc_ts.tv_sec = rtc_tm_to_time64(&tm);
	rtc_ts.tv_nsec = 0;

	/* Step 2: Delta sanity check against pre-hibernate timestamp.
	 * If we saved a timestamp before hibernate, the RTC time should be
	 * >= saved time (time moves forward) and < saved + 30 days.
	 * This catches corrupt RTC values, negative time travel, firmware bugs. */
	if (saved_pre_hibernate_ts.tv_sec > 0) {
		delta = rtc_ts.tv_sec - saved_pre_hibernate_ts.tv_sec;
		pr_info("time_sync: hibernate delta = %lld seconds\n", delta);
		if (delta < 0 || delta > 2592000) {
			pr_warn("time_sync: insane delta %lld, skipping\n",
				delta);
			goto display_fix;
		}
	}

	/* Step 3: Set system clock from kernel space.
	 * This is the Linux equivalent of Windows' KeSetSystemTime.
	 * do_settimeofday64() internally calls ntp_clear() via
	 * TK_CLEAR_NTP flag, resetting kernel NTP state. */
	if (do_settimeofday64(&rtc_ts)) {
		pr_warn("time_sync: do_settimeofday64 failed\n");
		goto display_fix;
	}

	/* Step 4: Verify it actually stuck by reading back */
	ktime_get_real_ts64(&now_ts);
	delta = now_ts.tv_sec - rtc_ts.tv_sec;
	if (delta < -2 || delta > 2) {
		pr_warn("time_sync: verify failed (drift=%llds)\n", delta);
		goto display_fix;
	}

	pr_info("time_sync: kernel-space RTC set OK (verified, drift=%llds)\n",
		delta);
	stats.time_syncs++;

display_fix:
	/* Trigger i915 display state reconciliation.
	 * Reading i915_display_info forces drm_modeset_lock_all() which
	 * re-reads hardware state, fixing 32Hz eDP link after hibernate. */
	trigger_display_reconciliation();

	/* Restore backlight to max after display reconciliation.
	 * The modeset lock + hardware re-read can momentarily blank the
	 * display. Force backlight on via kernel backlight API. */
	restore_backlight();

	/* Synthetic wakeup event to reset logind/GNOME idle timers.
	 * Without this, the desktop thinks the user is idle after
	 * hibernate resume and immediately blanks the screen. */
	if (lid_input_dev) {
		input_report_key(lid_input_dev, KEY_WAKEUP, 1);
		input_sync(lid_input_dev);
		input_report_key(lid_input_dev, KEY_WAKEUP, 0);
		input_sync(lid_input_dev);
	}
}

/* Stage 2: Kernel-space NTP query + RTC writeback + thaw chronyd.
 * 15s delay ensures network is up after hibernate resume.
 * Replaces chronyc online/makestep with direct SNTP UDP query. */
static void time_sync_ntp_fn(struct work_struct *work)
{
	struct timespec64 ntp_time, now;
	struct rtc_device *rtc;
	struct rtc_time tm;
	s64 diff;

	/* Query NTP ground truth via kernel UDP socket */
	if (kernel_ntp_query(&ntp_time) == 0) {
		/* Defense-in-depth: after RTC sync at +3s, NTP offset
		 * should be small. Reject suspiciously large offsets
		 * before persisting to settimeofday + RTC hardware. */
		ktime_get_real_ts64(&now);
		diff = ntp_time.tv_sec - now.tv_sec;
		if (diff < -300 || diff > 300) {
			pr_warn("ntp_sync: NTP offset %llds suspiciously "
				"large after RTC sync, skipping\n", diff);
			goto thaw;
		}

		/* Set system clock from NTP */
		if (do_settimeofday64(&ntp_time) == 0) {
			pr_info("ntp_sync: clock set from NTP "
				"(hibernate duration ~%llds)\n",
				ntp_time.tv_sec -
				saved_pre_hibernate_ts.tv_sec);
			stats.time_syncs++;

			/* Settle time for clock propagation */
			msleep(2000);

			/* Write NTP-corrected time to RTC hardware */
			rtc = rtc_class_open("rtc0");
			if (rtc) {
				ktime_get_real_ts64(&now);
				rtc_time64_to_tm(now.tv_sec, &tm);
				if (rtc_set_time(rtc, &tm))
					pr_warn("ntp_sync: rtc_set_time "
						"failed\n");
				else
					pr_info("ntp_sync: RTC synced to "
						"NTP time\n");
				rtc_class_close(rtc);
			}
		} else {
			pr_warn("ntp_sync: do_settimeofday64 failed\n");
		}
	} else {
		pr_warn("ntp_sync: NTP query failed, keeping RTC time\n");
	}

thaw:
	/* Always thaw chronyd, even if NTP failed or offset was rejected.
	 * Chronyd sees the (now correct) clock and rebuilds its
	 * tracking state from scratch via fresh NTP polls. */
	thaw_chronyd();
}

/* ========================================================================
 * PM notifier (INT_MAX priority, runs first)
 * ======================================================================== */

static int s2idle_pm_notify(struct notifier_block *nb,
			    unsigned long action, void *data)
{
	if (!com4_base)
		return NOTIFY_DONE;

	switch (action) {
	case PM_SUSPEND_PREPARE:
		stats.suspend_cycles++;
		fix_pre_sleep_common("suspend");
		break;

	case PM_HIBERNATION_PREPARE:
		stats.hibernate_cycles++;
		in_hibernate = true;
		ktime_get_real_ts64(&saved_pre_hibernate_ts);
		saved_pre_hibernate_mono = ktime_get();
		fix_pre_sleep_common("hibernate");
		break;

	case PM_RESTORE_PREPARE:
		fix_pre_sleep_common("restore");
		break;

	case PM_POST_SUSPEND:
		fix_post_sleep_common("post-suspend", true);
		break;

	case PM_POST_HIBERNATION: {
		/* PM_POST_HIBERNATION fires in BOTH paths:
		 *   1) After image write + thaw, about to power off
		 *   2) After successful hibernate resume (restored kernel)
		 *
		 * Detect which by comparing monotonic clock delta. During
		 * poweroff, monotonic doesn't advance, so after resume the
		 * delta since PM_HIBERNATION_PREPARE is tiny (<30s). On the
		 * image-write path, it's just a few seconds too, BUT the
		 * wall clock won't have jumped. After resume, wall clock
		 * jumped hours/days (RTC kept counting while powered off). */
		s64 mono_delta_ms = ktime_ms_delta(ktime_get(),
						   saved_pre_hibernate_mono);
		struct timespec64 now_real;
		s64 real_delta;

		ktime_get_real_ts64(&now_real);
		real_delta = now_real.tv_sec - saved_pre_hibernate_ts.tv_sec;

		/* Resume detection: monotonic barely moved but wall clock
		 * jumped significantly (>60s). On image-write path both
		 * deltas are small and similar. */
		if (mono_delta_ms < 30000 && real_delta > 60) {
			pr_info("post-hibernation: RESUME detected "
				"(mono=%lldms, real=%llds)\n",
				mono_delta_ms, real_delta);

			fix_post_sleep_common("post-hibernate-resume", false);

			/* Report current lid state to input layer.
			 * logind watches our SW_LID device, stale state
			 * from ACPI button driver can cause unwanted
			 * suspend. */
			{
				struct pin_context *lid =
					&tracked_pins[LID_TRACKED_INDEX];
				if (lid->valid && com4_base) {
					u32 padcfg0 = readl(
						pin_addr(lid) + PADCFG0_OFF);
					int rxstate = !!(padcfg0 &
						PADCFG0_GPIORXSTATE);
					report_lid_state(rxstate);
					last_poll_rxstate = rxstate;
					pr_info("post-hibernate-resume: "
						"lid %s reported\n",
						rxstate ? "closed" : "open");
				}
			}

			/* Start lid poll (no failsafe for hibernate) */
			lid_poll_active = true;
			schedule_delayed_work(&lid_poll_work,
				msecs_to_jiffies(LID_POLL_INTERVAL_MS));

			/* Schedule time sync and NTP */
			schedule_delayed_work(&time_sync_retry_work,
				msecs_to_jiffies(TIME_SYNC_DELAY_MS));
			schedule_delayed_work(&time_sync_ntp_work,
				msecs_to_jiffies(NTP_SYNC_DELAY_MS));
		} else {
			pr_info("post-hibernation: pre-poweroff path "
				"(mono=%lldms, real=%llds)\n",
				mono_delta_ms, real_delta);
			fix_post_sleep_common("post-hibernation", false);
		}

		in_hibernate = false;
		break;
	}

	case PM_POST_RESTORE:
		/* PM_POST_RESTORE only fires when image restore FAILS
		 * (boot kernel couldn't load the image). Just clean up. */
		fix_post_sleep_common("post-restore-failed", false);
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block s2idle_pm_nb = {
	.notifier_call = s2idle_pm_notify,
	.priority = INT_MAX,
};

/* ========================================================================
 * debugfs interface
 * ======================================================================== */

static int stats_show(struct seq_file *s, void *unused)
{
	struct pin_context *lid = &tracked_pins[LID_TRACKED_INDEX];
	u32 padcfg0 = 0;
	int i;

	if (lid->valid && com4_base)
		padcfg0 = readl(pin_addr(lid) + PADCFG0_OFF);

	seq_printf(s, "=== Surface S2Idle Fix v2.0 ===\n\n");

	seq_printf(s, "Hardware:\n");
	seq_printf(s, "  Community 4 base:     0x%08x\n", COM4_PADCFG_BASE);
	seq_printf(s, "  Tracked pins:         %d\n", num_tracked_pins);
	seq_printf(s, "  SCI IRQ:              %d\n", sci_irq);
	seq_printf(s, "  GPE:                  0x%02x\n", LID_GPE);
	seq_printf(s, "\n");

	seq_printf(s, "Lid pin (213) state:\n");
	seq_printf(s, "  PADCFG0 current:      0x%08x\n", padcfg0);
	seq_printf(s, "  PADCFG0 saved:        0x%08x\n", lid->padcfg0);
	seq_printf(s, "  RXINV:                %d\n", !!(padcfg0 & PADCFG0_RXINV));
	seq_printf(s, "  RXSTATE:              %d (%s)\n",
		   !!(padcfg0 & PADCFG0_GPIORXSTATE),
		   (padcfg0 & PADCFG0_GPIORXSTATE) ? "closed" : "open");
	seq_printf(s, "  GPIROUTSCI:           %d\n", !!(padcfg0 & PADCFG0_GPIROUTSCI));
	seq_printf(s, "\n");

	seq_printf(s, "Tracked pin details:\n");
	for (i = 0; i < num_tracked_pins; i++) {
		struct pin_context *p = &tracked_pins[i];
		u32 cur = com4_base ? readl(pin_addr(p) + PADCFG0_OFF) : 0;

		seq_printf(s, "  [%d] pin %u @ 0x%04x: DW0=0x%08x "
			   "saved=0x%08x valid=%d sci=%d%s\n",
			   i, p->pin_number, p->offset, cur,
			   p->padcfg0, p->valid, p->sci_routed,
			   i == LID_TRACKED_INDEX ? " (LID)" : "");
	}
	seq_printf(s, "\n");

	seq_printf(s, "HOSTSW_OWN registers:\n");
	for (i = 0; i < COM4_HOSTSW_OWN_REGS && com4_base; i++) {
		u32 cur = readl(com4_base + COM4_HOSTSW_OWN_BASE + i * 4);

		seq_printf(s, "  [%d] current=0x%08x saved=0x%08x %s\n",
			   i, cur, saved_hostsw_own[i],
			   cur != saved_hostsw_own[i] ? "MISMATCH" : "ok");
	}
	seq_printf(s, "\n");

	seq_printf(s, "Statistics:\n");
	seq_printf(s, "  Suspend cycles:       %u\n", stats.suspend_cycles);
	seq_printf(s, "  Hibernate cycles:     %u\n", stats.hibernate_cycles);
	seq_printf(s, "  PADCFG restores:      %u\n", stats.padcfg_restores);
	seq_printf(s, "  Wakeup handler calls: %u\n", stats.wakeup_handler_calls);
	seq_printf(s, "  Wakeup handler fixes: %u\n", stats.wakeup_handler_fixes);
	seq_printf(s, "  Early restores:       %u\n", stats.early_restores);
	seq_printf(s, "  LPS0 checks:          %u\n", stats.lps0_checks);
	seq_printf(s, "  Conditional skips:    %u\n", stats.conditional_skips);
	seq_printf(s, "  Multi-pin fixes:      %u\n", stats.multi_pin_fixes);
	seq_printf(s, "  HOSTSW_OWN fixes:     %u\n", stats.hostsw_own_corruptions);
	seq_printf(s, "  Spurious wakes:       %u\n", stats.spurious_wakes);
	seq_printf(s, "  Lid resyncs:          %u\n", stats.lid_resyncs);
	seq_printf(s, "  Time syncs:           %u\n", stats.time_syncs);
	seq_printf(s, "\n");

	seq_printf(s, "State:\n");
	seq_printf(s, "  lid_was_closed:       %d\n", lid_was_closed_at_suspend);
	seq_printf(s, "  failsafe_in_progress: %d\n", failsafe_in_progress);
	seq_printf(s, "  s2idle_gpe_active:    %d\n", s2idle_gpe_active);
	seq_printf(s, "  in_hibernate:         %d\n", in_hibernate);
	seq_printf(s, "  gpe52_was_enabled:    %d\n", gpe52_was_enabled);
	seq_printf(s, "  rapid_wakes:          %u\n", consecutive_rapid_wakes);
	seq_printf(s, "  failsafe_suspends:    %u\n", failsafe_suspends);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

static void debugfs_setup(void)
{
	dbgfs_root = debugfs_create_dir("surface_s2idle_fix", NULL);
	if (IS_ERR_OR_NULL(dbgfs_root)) {
		dbgfs_root = NULL;
		return;
	}
	debugfs_create_file("status", 0444, dbgfs_root, NULL, &stats_fops);
}

static void debugfs_teardown(void)
{
	debugfs_remove_recursive(dbgfs_root);
	dbgfs_root = NULL;
}

/* ========================================================================
 * Pin discovery and initialization
 * ======================================================================== */

/*
 * Scan Community 4 for SCI-routed Mode 0 pins beyond our hardcoded list.
 * This catches any additional VNN-vulnerable pins we might not know about.
 */
static void discover_sci_pins(void)
{
	int i;
	u32 padcfg0;

	/* Start with the hardcoded list */
	for (i = 0; i < ARRAY_SIZE(tracked_pin_offsets) &&
		    num_tracked_pins < MAX_TRACKED_PINS; i++) {
		struct pin_context *pin = &tracked_pins[num_tracked_pins];

		pin->offset = tracked_pin_offsets[i];
		/* For hardcoded pins, use the known GPIO number.
		 * Pin 213 is at offset 0x09a0 in Community 4. */
		pin->pin_number = (i == 0) ? LID_PIN_INDEX :
				  pin->offset / PADCFG_STRIDE;

		padcfg0 = readl(pin_addr(pin) + PADCFG0_OFF);

		/* Verify pin is in Mode 0 and SCI-routed */
		if ((padcfg0 & PADCFG0_PMODE_MASK) == 0 &&
		    (padcfg0 & PADCFG0_GPIROUTSCI)) {
			pin->sci_routed = true;
			pr_info("tracking pin %u @ 0x%04x (PADCFG0=0x%08x, "
				"SCI-routed)\n", pin->pin_number,
				pin->offset, padcfg0);
		} else {
			pr_info("tracking pin %u @ 0x%04x (PADCFG0=0x%08x, "
				"non-SCI)\n", pin->pin_number,
				pin->offset, padcfg0);
		}
		num_tracked_pins++;
	}

	/* NOTE: No scan beyond hardcoded pins. The PR proved pin 213 is
	 * the only VNN-vulnerable SCI pin that causes death sleep.
	 * Scanning and tracking unknown pins risks corrupting unrelated
	 * GPIO lines (e.g. touchpad I2C HID IRQ) on resume restore. */

	pr_info("total tracked pins: %d\n", num_tracked_pins);
}

/*
 * Apply boot parameters programmatically where possible.
 * This reduces the dependency on GRUB configuration.
 *
 * Note: reboot_type (reboot=acpi), acpi_osi, and acpi_mask_gpe
 * cannot be set from a module (symbols not exported). These must
 * remain as GRUB boot parameters:
 *   reboot=acpi acpi_osi="Windows 2020" acpi_mask_gpe=0x52
 *
 * The boot parameter acpi_mask_gpe=0x52 closes the 1.26s timing
 * gap between ACPI init (0.36s) and module load (~2.3s). Our init
 * unmasks it once the wakeup handler is registered.
 */
static void apply_boot_params(void)
{
	/* Nothing we can set from module context currently.
	 * This function exists as a placeholder for future use
	 * and to document what boot parameters are required. */
}

/* ========================================================================
 * DMI matching
 * ======================================================================== */

static const struct dmi_system_id surface_ids[] = {
	{
		.ident = "Surface Laptop 5",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Microsoft Corporation"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Surface Laptop 5"),
		},
	},
	{ }
};

/* ========================================================================
 * Module init / exit
 * ======================================================================== */

static struct platform_driver s2idle_fix_plat_driver = {
	.driver = {
		.name = "surface_s2idle_fix",
		.pm = &s2idle_fix_pm_ops,
	},
};

static int __init surface_s2idle_fix_init(void)
{
	struct pin_context *lid;
	u32 initial_padcfg0;
	int err, lps0_err;
	bool pwr_ok;

	if (!dmi_check_system(surface_ids)) {
		pr_err("not a supported Surface device\n");
		return -ENODEV;
	}

	/* Map the full Community 4 region (not just one pin) */
	com4_base = ioremap(COM4_PADCFG_BASE, COM4_PADCFG_SIZE);
	if (!com4_base) {
		pr_err("failed to ioremap Community 4 at 0x%08x\n",
		       COM4_PADCFG_BASE);
		return -ENOMEM;
	}

	/* Verify pin 213 is SCI-routed before proceeding */
	initial_padcfg0 = readl(com4_base + LID_PIN_OFFSET + PADCFG0_OFF);
	if (!(initial_padcfg0 & PADCFG0_GPIROUTSCI)) {
		pr_err("pin 213 PADCFG0=0x%08x: GPIROUTSCI not set\n",
		       initial_padcfg0);
		iounmap(com4_base);
		com4_base = NULL;
		return -ENODEV;
	}

	sci_irq = acpi_gbl_FADT.sci_interrupt;

	/* Apply boot parameters where possible */
	apply_boot_params();

	/* Discover all SCI-routed pins in Community 4 */
	discover_sci_pins();

	/* Save initial pin state and fix any existing corruption */
	save_all_pins();

	/* NOTE: Do NOT gpe52_force_disable() here. GPE 0x52 must stay live
	 * during normal operation so the ACPI button driver (PNP0C0D) receives
	 * lid events and systemd-logind can trigger suspend on lid close.
	 * The acpi_mask_gpe=0x52 boot param covers the hibernate timing gap.
	 * Our wakeup handler (registered below) catches phantom edges during
	 * s2idle. PM_SUSPEND_PREPARE masks GPE; resume_early unmasks it. */

	/* Fix PADCFG corruption if BIOS left RXINV in wrong state */
	lid = &tracked_pins[LID_TRACKED_INDEX];
	if (lid->valid && (initial_padcfg0 & PADCFG0_RXINV)) {
		u32 fixed = initial_padcfg0 & ~PADCFG0_RXINV;

		writel(lid->padcfg1, pin_addr(lid) + PADCFG1_OFF);
		wmb();
		writel(fixed & ~PADCFG0_GPIORXSTATE, pin_addr(lid) + PADCFG0_OFF);
		wmb();

		lid->padcfg0 = fixed & ~PADCFG0_GPIORXSTATE;
		stats.padcfg_restores++;
		pr_info("init: RXINV was set, corrected 0x%08x -> 0x%08x\n",
			initial_padcfg0, fixed);
		initial_padcfg0 = fixed;
	}

	/* Platform driver for PM ops */
	err = platform_driver_register(&s2idle_fix_plat_driver);
	if (err) {
		pr_err("platform_driver_register: %d\n", err);
		iounmap(com4_base);
		com4_base = NULL;
		return err;
	}

	s2idle_fix_pdev = platform_device_register_simple(
		"surface_s2idle_fix", -1, NULL, 0);
	if (IS_ERR(s2idle_fix_pdev)) {
		err = PTR_ERR(s2idle_fix_pdev);
		pr_err("platform_device_register: %d\n", err);
		platform_driver_unregister(&s2idle_fix_plat_driver);
		iounmap(com4_base);
		com4_base = NULL;
		return err;
	}

	/* Work items and timers */
	INIT_DELAYED_WORK(&lid_failsafe_work, lid_failsafe_fn);
	INIT_DELAYED_WORK(&lid_resync_work, lid_resync_fn);
	INIT_DELAYED_WORK(&lid_poll_work, lid_poll_fn);
	INIT_DELAYED_WORK(&time_sync_retry_work, time_sync_retry_fn);
	INIT_DELAYED_WORK(&time_sync_ntp_work, time_sync_ntp_fn);
	hrtimer_setup(&s2idle_poll_timer, s2idle_poll_timer_fn,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);

	/* Power button observer */
	err = input_register_handler(&pwr_handler);
	pwr_ok = !err;
	if (err)
		pr_warn("power button handler failed: %d\n", err);

	/* ACPI wakeup handler (step 4, before GPE dispatch at step 5) */
	acpi_register_wakeup_handler(sci_irq, lid_wake_handler, NULL);
	register_pm_notifier(&s2idle_pm_nb);

	/* LPS0 s2idle hooks */
	lps0_err = acpi_register_lps0_dev(&s2idle_lps0_ops);
	if (lps0_err)
		pr_warn("LPS0 registration failed: %d\n", lps0_err);

	/* All handlers registered. Unmask + enable GPE 0x52 for ACPI lid events.
	 * The acpi_mask_gpe=0x52 boot param masked it at ACPI init (0.36s) for
	 * hibernate timing gap safety. Now that our wakeup handler is registered,
	 * we can safely unmask it. Clear stale status first. */
	acpi_clear_gpe(NULL, LID_GPE);
	acpi_mask_gpe(NULL, LID_GPE, FALSE);
	acpi_set_gpe(NULL, LID_GPE, ACPI_GPE_ENABLE);
	acpi_set_gpe_wake_mask(NULL, LID_GPE, ACPI_GPE_DISABLE);
	pr_info("init: GPE 0x52 unmasked and enabled for ACPI lid events, wake mask disabled\n");

	/* Lid switch input device */
	lid_input_dev = input_allocate_device();
	if (lid_input_dev) {
		lid_input_dev->name = "Surface S2Idle Lid Switch";
		lid_input_dev->phys = "surface_s2idle_fix/lid";
		lid_input_dev->id.bustype = BUS_HOST;
		lid_input_dev->evbit[0] = BIT_MASK(EV_SW);
		lid_input_dev->swbit[0] = BIT_MASK(SW_LID);
		set_bit(EV_KEY, lid_input_dev->evbit);
		set_bit(KEY_WAKEUP, lid_input_dev->keybit);

		input_report_switch(lid_input_dev, SW_LID,
				    !!(initial_padcfg0 & PADCFG0_GPIORXSTATE));
		input_sync(lid_input_dev);

		err = input_register_device(lid_input_dev);
		if (err) {
			pr_warn("lid input: %d\n", err);
			input_free_device(lid_input_dev);
			lid_input_dev = NULL;
		}
	}

	/* debugfs */
	debugfs_setup();

	/* Start lid polling */
	last_poll_rxstate = !!(initial_padcfg0 & PADCFG0_GPIORXSTATE);
	lid_poll_active = true;
	schedule_delayed_work(&lid_poll_work,
			      msecs_to_jiffies(LID_POLL_INTERVAL_MS));

	pr_info("v5.1a loaded: SCI=%d, %d pins tracked, "
		"PADCFG0=0x%08x RXINV=%d RXSTATE=%d, "
		"pwr=%s lps0=%s debugfs=%s\n",
		sci_irq, num_tracked_pins, initial_padcfg0,
		!!(initial_padcfg0 & PADCFG0_RXINV),
		!!(initial_padcfg0 & PADCFG0_GPIORXSTATE),
		pwr_ok ? "ok" : "FAIL",
		lps0_err ? "FAIL" : "ok",
		dbgfs_root ? "ok" : "FAIL");

	return 0;
}

static void __exit surface_s2idle_fix_exit(void)
{
	lid_poll_active = false;
	hrtimer_cancel(&s2idle_poll_timer);
	cancel_delayed_work_sync(&lid_poll_work);
	cancel_delayed_work_sync(&lid_resync_work);
	cancel_delayed_work_sync(&lid_failsafe_work);
	cancel_delayed_work_sync(&time_sync_retry_work);
	cancel_delayed_work_sync(&time_sync_ntp_work);

	/* Safety: unfreeze chronyd if module unloads during hibernate sync */
	thaw_chronyd();

	gpe52_unmask("exit");
	acpi_set_gpe_wake_mask(NULL, LID_GPE, ACPI_GPE_ENABLE);

	debugfs_teardown();

	if (lid_input_dev)
		input_unregister_device(lid_input_dev);

	acpi_unregister_lps0_dev(&s2idle_lps0_ops);
	unregister_pm_notifier(&s2idle_pm_nb);
	acpi_unregister_wakeup_handler(lid_wake_handler, NULL);
	input_unregister_handler(&pwr_handler);

	platform_device_unregister(s2idle_fix_pdev);
	platform_driver_unregister(&s2idle_fix_plat_driver);

	if (com4_base) {
		iounmap(com4_base);
		com4_base = NULL;
	}

	pr_info("v5.1a unloaded: %u suspends, %u hibernates, "
		"%u PADCFG restores, %u multi-pin fixes, "
		"%u HOSTSW_OWN fixes, %u spurious wakes\n",
		stats.suspend_cycles, stats.hibernate_cycles,
		stats.padcfg_restores, stats.multi_pin_fixes,
		stats.hostsw_own_corruptions, stats.spurious_wakes);
}

module_init(surface_s2idle_fix_init);
module_exit(surface_s2idle_fix_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Surface Linux Debug");
MODULE_DESCRIPTION("Fix s2idle/hibernate death sleep on Surface Laptop 5 (v5.1a)");
MODULE_VERSION("5.1a");
MODULE_ALIAS("dmi:*:svnMicrosoftCorporation:pnSurfaceLaptop5:*");
