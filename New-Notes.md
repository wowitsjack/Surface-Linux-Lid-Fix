# Surface S2Idle Fix: Session Notes (2026-03-04)

## Current State

Module version 2.0.0, 1505 lines, freshly compiled, installed at `/lib/modules/6.18.7-surface-1/extra/surface_s2idle_fix.ko`, initramfs rebuilt. **Needs a full reboot before testing time sync fix.** Death sleep and lid-close bugs confirmed fixed.

`hwclock` was installed in a prior session (`apt install util-linux-extra`). Time sync depends on it.

## What We Built This Session

### The "32Hz Of Death" Bug

After hibernate resume (using the GRUB boot parameters that fix death sleep), the system time goes hours/days wrong. This causes:
- i915 display locks at 32Hz (eDP link training timeouts use wrong time)
- HDA audio crackles (DMA period timing corrupted)
- Setting the correct time via `date`, NTP, or `hwclock --hctosys` instantly fixes all three symptoms

Root cause: the restored kernel image carries stale timekeeping state. With `acpi_sleep=nonvs`, `timekeeping_resume()` fails to correct the clock properly.

### Post-Hibernate Time Sync (v2.0.0 rewrite)

The old 3-tier kernel-space approach (`do_settimeofday64()` + hwclock + retry) was removed entirely. It failed silently in PM notifier context.

Replaced with a simple userspace approach using two functions:

**`run_cmd(cmd, caller)`** (lines 1024-1040)
- Generic helper. Calls `call_usermodehelper("/bin/sh", "-c", cmd, UMH_WAIT_PROC)`
- Sets PATH to `/sbin:/usr/sbin:/bin:/usr/bin`
- Logs success/failure with command name and return code

**`time_sync_retry_fn(work)`** (lines 1042-1050)
- Delayed worker, scheduled 3s after hibernate resume
- Stage 1: `hwclock --hctosys --utc` (force system clock from hardware RTC, explicit UTC)
- Stage 2: `chronyc makestep` (force NTP step-correct, not slew)
- `stats.time_syncs++` only on hwclock success

### Integration Points

- `PM_POST_HIBERNATION`: `schedule_delayed_work(&time_sync_retry_work, msecs_to_jiffies(3000))`
- `PM_POST_RESTORE`: same
- `PM_POST_SUSPEND`: does NOT schedule time sync (suspend doesn't need it)
- Module init: `INIT_DELAYED_WORK(&time_sync_retry_work, time_sync_retry_fn)`
- Module exit: `cancel_delayed_work_sync(&time_sync_retry_work)`

### Includes, Defines

```c
/* rtc.h removed: time sync now uses userspace hwclock + chronyc */
#include <linux/umh.h>          // line 75

#define TIME_SYNC_DELAY_MS  3000  // line 178

static struct delayed_work time_sync_retry_work;  // line 184
```

## v2.0.0 Changes (2026-03-04)

### Bug Fix: GPE 0x52 Unmask at Init (THE critical fix)

**Symptom:** Closing lid caused semi-wake state, mouse death on first wake, re-sleep, then clean wake.

**Root cause:** `acpi_mask_gpe=0x52` boot parameter masks GPE 0x52 at ACPI init (0.36s). Module init only called `acpi_set_gpe(ENABLE)` but never `acpi_mask_gpe(FALSE)`. Enable and unmask are SEPARATE operations in ACPI GPE subsystem. The hardware mask bit stayed set forever.

**Proof:** `cat /sys/firmware/acpi/interrupts/gpe52` showed `0 enabled masked`. Zero interrupts ever. ACPI button driver (PNP0C0D, event0) was deaf to lid events. logind fell back to module's 2s poller on event2, causing delayed/broken suspend entry.

**Fix:** Added `acpi_mask_gpe(NULL, LID_GPE, FALSE)` to init after wakeup handler registration.

### Bug Fix: Pre-sleep GPE Masking Simplified

Replaced `gpe52_force_disable()` (mask + disable + clear) in `fix_pre_sleep_common()` with simple `acpi_mask_gpe(TRUE)` + `gpe52_was_enabled = true`. The `suspend_noirq` handler still does `gpe52_force_disable()` at the noirq level.

### Time Sync: Complete Rewrite

Old 3-tier kernel-space approach removed entirely (it failed silently in PM notifier context). Replaced with userspace approach: `hwclock --hctosys --utc` + `chronyc makestep` via 3s delayed work. See "Post-Hibernate Time Sync" section above.

## What Needs To Happen Next

1. **Reboot** to load v2.0.0 module
2. **Hibernate**: `systemctl hibernate`
3. Resume and check: `sudo dmesg | grep "time_sync"` should show:
   ```
   surface_s2idle_fix: time_sync: 'hwclock --hctosys --utc' ok
   surface_s2idle_fix: time_sync: 'chronyc makestep' ok
   ```
4. Verify time is correct: `date` and `timedatectl`

## If Time Sync Doesn't Fix 32Hz

There's a second known fix: reading the i915 display debugfs file forces a hardware state reconciliation:

```bash
cat /sys/kernel/debug/dri/0000:00:02.0/i915_display_info > /dev/null
```

This was discovered accidentally during a debugging session. Reading `i915_display_info` acquires `drm_modeset_lock_all()`, reads full hardware state, and on lock release the driver detects mismatch between software and hardware state, triggering a corrective modeset with proper eDP link retraining.

If the time sync alone doesn't fix 32Hz, this debugfs read could be integrated into `time_sync_retry_fn` using the existing `run_cmd()` helper.

## All Boot Parameters (required, in /etc/default/grub)

```
GRUB_CMDLINE_LINUX_DEFAULT="quiet splash resume=UUID=3242ebd8-6476-4a7c-aec8-2606712f8ef7 resume_offset=4524032 i915.enable_psr=0 i915.enable_dc=0 pci=hpiosize=0 acpi=force reboot=acpi acpi_sleep=nonvs acpi_osi=\"Windows 2020\" acpi_mask_gpe=0x52"
```

All six ACPI/PCI params are required. `acpi_mask_gpe=0x52` is the critical one that closes the 1.26-second timing gap the module can't reach. See `hibernate.md` for full explanation of each parameter.

## Module File Locations

- Source: `/home/user/Desktop/s2lid/surface_s2idle_fix.c`
- Installed: `/lib/modules/6.18.7-surface-1/extra/surface_s2idle_fix.ko`
- In initramfs: rebuilt via `update-initramfs -u`
- GitHub: `https://github.com/wowitsjack/Surface-Linux-Lid-Fix` (dev branch)

## Environment

- Device: Microsoft Surface Laptop 5
- OS: Ubuntu 25.10
- Kernel: 6.18.7-surface-1 (linux-surface)
- Swap: swap.img, resume_offset=4524032
- Hibernate mode: platform shutdown (`/etc/systemd/sleep.conf.d/hibernate.conf`)
- Sleep state: s2idle (Low-power S0 idle)
- RTC: UTC (LocalRTC=no, no /etc/adjtime)
- Timezone: Australia/Brisbane (AEST, +1000)

## Verified Kernel Symbol Exports

Key symbol used by time sync:

```
call_usermodehelper     EXPORT_SYMBOL      (run userspace command)
```

The old RTC kernel symbols (`rtc_class_open`, `do_settimeofday64`, etc.) are no longer used. Time sync is now entirely userspace via `hwclock` and `chronyc`.

## Session History

- Wrote the original s2idle fix module (v1.0 -> v1.1.0)
- Discovered the 6 GRUB boot parameters that fix hibernate death sleep
- Session `resilient-honking-muffin`: discovered the i915 debugfs fix for 32Hz
- Previous continuation session: wrote HANDOFF.md, then implemented the 3-tier time sync
- Audited and fixed 2 bugs in old time sync (PM notifier deadlock risk, local-time RTC)
- v2.0.0: Fixed GPE 0x52 unmask (death sleep on lid close), simplified pre-sleep GPE masking, rewrote time sync to userspace approach
- Created GitHub repo `wowitsjack/Surface-Linux-Lid-Fix` with dev branch
