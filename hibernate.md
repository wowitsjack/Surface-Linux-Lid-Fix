# Surface Laptop 5: Hibernate & S2Idle Death Sleep

## The Problem

The Surface Laptop 5 running Linux suffers from "death sleep" during both s2idle (standby) and hibernate. On hibernate, the system enters a two-stage failure:

**Stage 1, "Light Death" (~10-14 seconds):** Screen goes black instantly. Keyboard lights up and pulses gently (normal backlight behavior). No display output, no response to input. The system appears to be stuck in a half-powered state, as if it blocked the screen but is still running behind the scenes.

**Stage 2, "Dead" (~30+ seconds):** System becomes completely unresponsive. Requires holding the power button for 20 seconds to force power off. A short press during this stage briefly lights up the keyboard for ~10 seconds before returning to dead state.

After force-off, the hibernate image survives on disk. The next normal boot successfully resumes from hibernation. The death sleep prevents the system from ever reaching a fully powered-off state on its own.

This same two-stage behavior also occurs during lid-close suspend (s2idle). The kernel module in this directory (`surface_s2idle_fix.c`) fixes s2idle death sleep completely. Hibernate required a different approach.

## Environment

- **Device:** Microsoft Surface Laptop 5
- **OS:** Ubuntu 25.10
- **Kernel:** 6.18.7-surface-1 (linux-surface)
- **Swap:** swap.img file, resume_offset=4524032
- **Hibernate mode:** `platform shutdown` (set in `/etc/systemd/sleep.conf.d/hibernate.conf`)
- **Sleep state:** s2idle (Low-power S0 idle used by default)

## Root Cause

### The Hardware Bug

Intel INTC1055 pinctrl power-gating corrupts **pin 213's PADCFG0 RXINV bit** during low-power transitions. This pin is the lid sensor, routed to generate SCI interrupts via **GPE 0x52**.

When the RXINV bit flips, it creates a phantom edge that fires a spurious SCI. The ACPI handler for GPE 0x52 calls `pm_system_cancel_wakeup()`, which permanently poisons the wakeup framework. The system can never wake up. Death sleep.

### Why It Affects Hibernate

For s2idle, the corruption happens during C-state transitions in the idle loop. The module catches it with ACPI wakeup handlers and LPS0 callbacks.

For hibernate, the corruption occurs during the thaw/restore path after the BIOS reinitializes hardware and the restored kernel brings devices back online. But there's a deeper problem.

### The 1.26-Second Timing Gap

This is why no module-level fix could solve hibernate death sleep:

```
[0.360s] ACPI: Enabled 8 GPEs in block 00 to 7F    <-- GPE 0x52 goes LIVE
[1.616s] surface_s2idle_fix: loaded                  <-- Module loads 1.26s LATER
```

GPE 0x52 is active and capable of firing for 1.26 seconds before the kernel module can do anything about it. During hibernate resume, the fresh boot kernel initializes ACPI, enables GPEs, and then loads the hibernate image. If the PADCFG corruption fires GPE 0x52 during that 1.26-second window, the wakeup framework is poisoned before the module ever gets a chance to intervene.

No amount of `freeze_noirq`, `poweroff_noirq`, or `gpe52_force_disable()` calls from within the module can fix a problem that happens before the module exists in memory.

### The "Fake Shutdown" Problem

Related to the death sleep: Surface devices have a known issue where they don't fully power off. Users report waking up to dead batteries after "shutting down." The system enters a state that looks off but isn't, continuing to drain power.

Documented in linux-surface GitHub issues:
- [#1058](https://github.com/linux-surface/linux-surface/issues/1058): "SL5/SP9 does not power off properly"
- [#1227](https://github.com/linux-surface/linux-surface/issues/1227): "Battery drain in shutdown...?"
- [#758](https://github.com/linux-surface/linux-surface/issues/758): "[SGO3] Recurring Sleep of death"

This "fake shutdown" behavior is part of the same firmware/ACPI interaction problem that causes death sleep.

## The Kernel Module (`surface_s2idle_fix.c`)

Version 2.0.0, 1505 lines. Fixes s2idle death sleep completely. Provides partial hibernate support (PADCFG correction during thaw/restore, but can't fix the timing gap). Post-hibernate time sync via `hwclock --hctosys --utc` + `chronyc makestep`.

### What It Does

**S2idle protection (working):**
- Init: `acpi_mask_gpe(NULL, LID_GPE, FALSE)` unmasks GPE 0x52 after wakeup handler registration (v2.0.0 fix for persistent boot param mask)
- Masks GPE 0x52 via `acpi_mask_gpe(TRUE)` during `PM_SUSPEND_PREPARE`
- ACPI wakeup handler (registered on SCI IRQ 9) runs before `acpi_ec_dispatch_gpe()`, fixes PADCFG corruption and clears GPE 0x52 status to prevent spurious full resume promotion
- LPS0 hooks: `prepare` unmasks GPE 0x52 inside the s2idle idle loop for lid-open detection, `check` polls RXSTATE for genuine lid-open, `restore` re-masks GPE 0x52
- Post-resume failsafe: 2-second delayed work item checks if wake was spurious (lid closed, no power button), re-suspends if so
- Exponential backoff: 2s/4s/8s/15s delays prevent rapid sleep-wake storms, max 10 retries
- RXSTATE polling: background poller emits SW_LID input events for desktop environment lid state tracking
- Passive KEY_POWER observer: distinguishes real power-button wakes from spurious GPE wakes

**Hibernate support (module-level, necessary but insufficient alone):**
- `freeze` / `freeze_noirq`: Aggressively disables GPE 0x52 (mask + disable + clear) during device quiesce
- `thaw_noirq` / `thaw_early`: Fixes PADCFG corruption after BIOS reinitialization, unmasks GPE 0x52
- `restore_noirq` / `restore_early`: Same as thaw, for the hibernate image restore path
- `poweroff` / `poweroff_noirq`: Disables GPE 0x52 before shutdown after hibernate image save
- `PM_HIBERNATION_PREPARE`: Saves PADCFG golden copies, masks GPE 0x52, cancels work queues
- `PM_POST_HIBERNATION`: Final PADCFG check, unmask GPE, schedule failsafe, restart polling, schedule time sync
- Init-time GPE masking: On module load, immediately masks GPE 0x52 for hibernate restore safety
- Post-hibernate time sync: 3s delayed `hwclock --hctosys --utc` + `chronyc makestep` (fixes stale clock / 32Hz display bug)

### Module Files

- `surface_s2idle_fix.c` - 1505 lines, the kernel module source (v2.0.0)
- `Makefile` - standard out-of-tree module build
- `dkms.conf` - DKMS config
- Installed at `/lib/modules/6.18.7-surface-1/extra/surface_s2idle_fix.ko`
- Included in initramfs for early loading
- GitHub: `https://github.com/wowitsjack/Surface-Linux-Lid-Fix` (dev branch)

## Failed Fix Attempts

Three rounds of module-level fixes were attempted for hibernate death sleep. All failed identically: the same two-stage death sleep with no change in behavior.

### Round 1: PM Notifier + Thaw/Restore Callbacks

Added `PM_HIBERNATION_PREPARE` and `PM_POST_HIBERNATION` cases to the PM notifier. Added `thaw_noirq`, `thaw_early`, `restore_noirq`, `restore_early` PM ops. These correctly mask GPE 0x52 during hibernate and fix PADCFG on thaw/restore.

**Result:** Death sleep unchanged. The module's PM ops run at the right times, but the problem is in the boot-time window before the module loads.

### Round 2: Init-Time GPE Masking from Initramfs

Module load moved to initramfs. On init, immediately calls `acpi_mask_gpe()` and `acpi_clear_gpe()` for GPE 0x52. The idea: module loads from initramfs at 1.6 seconds, catches GPE 0x52 early.

**Result:** Death sleep unchanged. Module loads at 1.616s, but GPEs are already active at 0.360s. The 1.26-second gap is too wide.

### Round 3: Aggressive Force-Disable

Added `gpe52_force_disable()` helper that calls all three: `acpi_mask_gpe()`, `acpi_disable_gpe()`, `acpi_clear_gpe()`. Applied it to every PM callback: `freeze`, `freeze_noirq`, `suspend_noirq`, `poweroff`, `poweroff_noirq`, and init. Belt-and-suspenders approach.

**Result:** Death sleep unchanged. Aggressive masking/disabling cannot help when the problem occurs 1.26 seconds before the module exists.

### Why All Three Failed

The fundamental issue: kernel modules cannot run code during the ACPI subsystem initialization at 0.360s. By the time any module loads (even from initramfs), GPE 0x52 has been active for over a second. If PADCFG corruption fires it during that window, the wakeup framework is already poisoned.

The fix had to come from the kernel itself, via boot parameters that configure ACPI behavior before GPE initialization.

## The Fix: Kernel Boot Parameters

The following parameters were added to `GRUB_CMDLINE_LINUX_DEFAULT` in `/etc/default/grub`:

```
pci=hpiosize=0 acpi=force reboot=acpi acpi_sleep=nonvs acpi_osi="Windows 2020" acpi_mask_gpe=0x52
```

### What Each Parameter Does

**`acpi_mask_gpe=0x52`** (the critical one)
Tells the kernel's ACPI subsystem to mask GPE 0x52 at ACPI initialization time, at 0.360s, the exact moment GPEs go live. This closes the 1.26-second timing gap that no module could reach. With this parameter, the kernel itself logs `ACPI: Masking GPE 0x52.` before any handler can fire.

**`acpi_osi="Windows 2020"`**
Tells the Surface UEFI firmware that the OS is "Windows 2020". Microsoft's firmware contains different ACPI code paths depending on the reported OS. With this _OSI string, the firmware uses modern power management paths that handle sleep state transitions correctly. Without it, the firmware may use legacy paths that don't properly manage power-gating.

**`acpi_sleep=nonvs`**
Tells the kernel NOT to save/restore ACPI NVS (Non-Volatile Storage) memory during suspend/resume. The NVS region can contain firmware state that, when restored, puts the system back into the half-powered death sleep state. Skipping NVS save/restore prevents the firmware from getting confused during the suspend/resume cycle.

**`reboot=acpi`**
Forces the system to use ACPI methods for shutdown and reboot instead of the keyboard controller method. This directly addresses the "fake shutdown" problem documented in GitHub issues #1058 and #1227, where Surface devices don't fully power off because the default shutdown method doesn't properly signal the firmware.

**`acpi=force`**
Forces ACPI to be enabled even if the BIOS/firmware would normally suggest otherwise. Ensures all ACPI power management infrastructure is active.

**`pci=hpiosize=0`**
Disables PCI hotplug IO size reservation. Prevents unnecessary PCI resource allocation that can interfere with power management on the Surface platform.

### Full GRUB Configuration

```
GRUB_CMDLINE_LINUX_DEFAULT="quiet splash resume=UUID=3242ebd8-6476-4a7c-aec8-2606712f8ef7 resume_offset=4524032 i915.enable_psr=0 i915.enable_dc=0 pci=hpiosize=0 acpi=force reboot=acpi acpi_sleep=nonvs acpi_osi=\"Windows 2020\" acpi_mask_gpe=0x52"
```

After editing `/etc/default/grub`, run `sudo update-grub` to apply.

## Verification

### dmesg Trace: Successful Hibernate Cycle

```
[    0.360008] ACPI: Masking GPE 0x52.                              <- KERNEL masks GPE at ACPI init
[    1.616728] surface_s2idle_fix: init: masked GPE 0x52             <- Module loads, adds its own mask
[    1.616842] surface_s2idle_fix: loaded: SCI=9 PADCFG0=0x40080100  <- Clean initial state
[  153.727433] surface_s2idle_fix: hibernate: masked GPE 0x52        <- Hibernate triggered
[  156.553088] ACPI: PM: Preparing to enter system sleep state S4    <- Entering S4
[  156.555334] ACPI: PM: Saving platform NVS memory                  <- NVS save
[  156.639564] ACPI: PM: Restoring platform NVS memory               <- NVS restore
[  156.675024] ACPI: PM: Waking up from system sleep state S4        <- Waking from S4
[  156.707500] surface_s2idle_fix: thaw_noirq: PADCFG0 OK            <- No corruption!
[  156.708825] surface_s2idle_fix: thaw_early: PADCFG0 OK            <- Still clean
[  156.708906] surface_s2idle_fix: thaw_early: unmasked GPE 0x52     <- GPE restored
[  157.747700] surface_s2idle_fix: post-hibernation: rapid wake       <- Normal post-hibernate
[  161.767506] surface_s2idle_fix: failsafe: lid was open, staying awake  <- Clean resume
```

The entire hibernate cycle completes cleanly. No PADCFG corruption, no death sleep, clean thaw and resume.

### GPE 0x52 Status

After boot with the new parameters:
```
$ cat /sys/firmware/acpi/interrupts/gpe52
       2     STS enabled      unmasked
```

GPE 0x52 is properly managed: masked at boot by the kernel parameter, then selectively unmasked by the module when needed for lid detection.

## Both Fixes Work Together

The kernel boot parameters and the kernel module are complementary:

| Problem | Boot Parameters | Module |
|---|---|---|
| GPE 0x52 timing gap (0.36s-1.6s) | `acpi_mask_gpe=0x52` covers it | Cannot reach this window |
| PADCFG corruption during s2idle | Not addressed | Wakeup handler + LPS0 hooks fix it |
| Spurious wake promotion in s2idle | Not addressed | Clears GPE status before `acpi_any_gpe_status_set()` |
| Lid-open detection during s2idle | Not addressed | LPS0 check polls RXSTATE, calls `pm_system_wakeup()` |
| Spurious wake re-suspend | Not addressed | Failsafe work item with exponential backoff |
| Lid state for desktop environment | Not addressed | SW_LID input device with RXSTATE polling |
| Firmware shutdown behavior | `reboot=acpi` fixes fake shutdown | Not addressed |
| Firmware power management paths | `acpi_osi="Windows 2020"` | Not addressed |
| NVS state confusion | `acpi_sleep=nonvs` | Not addressed |
| PADCFG correction on hibernate thaw | Timing gap closed by boot param | `thaw_noirq`/`thaw_early` correct PADCFG if needed |

**Remove the boot parameters:** Hibernate death sleep returns. S2idle still works (module handles it).
**Remove the module:** S2idle death sleep returns. Hibernate might work (boot params cover timing gap) but loses all the s2idle-loop protection, lid detection, failsafe re-suspend, and SW_LID events.

**Both are needed for complete protection.**

## Post-Hibernate Display Refresh Rate Bug (32Hz Lock)

### The Problem

After hibernate resume, the display gets stuck at 32Hz instead of the normal 60Hz. The system is otherwise functional, but the low refresh rate makes it visually broken. The only fix previously known was a full reboot.

### Root Cause

During hibernate resume, the BIOS reinitializes hardware but the i915 driver's software state and the actual hardware state fall out of sync. The eDP link training doesn't complete properly, resulting in the panel running at 32Hz instead of the correct 60Hz. The correct working configuration is `port_clock=432000, lane_count=2` at 2496x1664@60Hz.

### The Fix

Reading the i915 display debugfs file forces a state reconciliation:

```bash
cat /sys/kernel/debug/dri/0000:00:02.0/i915_display_info > /dev/null
```

### Why It Works

Reading `i915_display_info` is not a purely passive operation. Internally it:

1. Acquires `drm_modeset_lock_all()` across all CRTCs and connectors
2. Reads the full hardware state of every display pipeline
3. On lock release, the driver can trigger a state reconciliation
4. The driver detects the mismatch between software state and hardware state
5. This triggers a corrective modeset with proper eDP link retraining

The power domain cycling triggered by `intel_display_power_get()` / `intel_display_power_put()` calls during the debugfs read forces the display hardware to re-evaluate its state.

### Discovery

Found accidentally during a debugging session (session: `resilient-honking-muffin`). An agent was launched to investigate the post-hibernate display zombie state by reading various i915 debugfs files. While reading those files for diagnostic purposes, the display spontaneously fixed itself. The "investigation" was the cure.

### Integration

This should be added to a post-hibernate resume hook (e.g. systemd service or the existing module's `PM_POST_HIBERNATION` path) so the debugfs read happens automatically after every hibernate resume.
