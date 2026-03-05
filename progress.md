# Surface Laptop 5 S2Idle / Hibernate Fix: Progress

## The Problem

Surface Laptop 5 (Alder Lake, 12th gen Intel) suffers "death sleep": closing the lid triggers s2idle suspend, but the system never wakes. The display stays black, keyboard/power button unresponsive, requiring a hard power cycle. Hibernate has a similar but distinct failure mode involving time desync and display corruption (32Hz eDP).

## The Fix: Two Components

### 1. Boot Parameters

```
mem_sleep_default=deep intel_iommu=off acpi_osi="Windows 2022" nvme_core.default_ps_max_latency_us=0 i915.enable_dc=0 snd_hda_intel.power_save=0
```

| Parameter | Purpose |
|---|---|
| `mem_sleep_default=deep` | Forces S3 deep sleep instead of s2idle (avoids the broken s0ix path entirely) |
| `intel_iommu=off` | Prevents IOMMU faults during suspend/resume that cause device failures |
| `acpi_osi="Windows 2022"` | Makes ACPI firmware expose Windows-specific power tables, GPE routing, device power states |
| `nvme_core.default_ps_max_latency_us=0` | Disables NVMe power states that cause the drive to not wake |
| `i915.enable_dc=0` | Disables display core power gating, prevents the display controller from entering states it can't exit |
| `snd_hda_intel.power_save=0` | Keeps audio codec awake, avoids codec resume failures that block the whole suspend path |

### 2. Kernel Module: `surface_s2idle_fix.ko`

A comprehensive kernel module (v2.1) that handles everything the kernel and firmware get wrong.

**Source**: `surface_s2idle_fix.c`
**Install paths**: `/lib/modules/6.18.7-surface-1/updates/` (primary, initramfs) and `/extra/`

## Module Architecture

### Dual Init System
- `module_init` for when loaded as a module
- `late_initcall` + built-in detection for when compiled into the kernel
- Prevents double-initialization via `already_initialized` flag

### PM Notifier (Power Management Callbacks)

Handles `PM_SUSPEND_PREPARE`, `PM_POST_SUSPEND`, `PM_HIBERNATION_PREPARE`, `PM_POST_HIBERNATION`, `PM_POST_RESTORE`:

**Pre-sleep (common path)**:
- Saves PADCFG registers for Community 0 Pads 40-50 (GPIO pins for touchpad, keyboard, sensors)
- Saves the Community 0 PADCFGLOCK register
- Unmasks GPE 0x6C (the lid switch GPE) to ensure lid-open can wake the system

**Post-sleep** (`fix_post_sleep_common`, has `schedule_work` param):
- Restores PADCFG registers if firmware corrupted them (compares saved vs current)
- Re-checks GPE 0x6C mask state
- When `schedule_work=true`: tracks rapid wakes, schedules lid failsafe/poll work
- When `schedule_work=false`: skips all work scheduling (used during post-image poweroff path)

**PM_POST_HIBERNATION** (fires in BOTH paths, resume detection needed):

PM_POST_HIBERNATION fires twice: once after image write (about to power off) and once after successful hibernate resume (restored kernel). PM_POST_RESTORE only fires on FAILED restore (boot kernel couldn't load image), so it's useless for the success path.

Detection method: compare monotonic vs wall clock delta since PM_HIBERNATION_PREPARE:
- **Pre-poweroff path**: both deltas are small (~3-5s), similar values
- **Resume path**: monotonic delta is tiny (clock frozen during poweroff), wall clock jumped hours/days (RTC kept counting)
- Threshold: `mono_delta < 30s && real_delta > 60s` = resume

On **pre-poweroff** detection:
- Calls `fix_post_sleep_common` with `schedule_work=false`
- Does NOT schedule any delayed work (machine about to shut down)

On **resume** detection:
- Calls `fix_post_sleep_common` with `schedule_work=false` (no failsafe, no rapid wake tracking)
- Immediately reports current lid state to input layer via `report_lid_state()`
- Starts lid poll for ongoing monitoring (no failsafe needed for hibernate)
- Schedules kernel-space RTC time sync (3s delay)
- Schedules NTP sync (15s delay)
- Display state reconciliation (32Hz eDP fix via i915 debugfs)

**PM_POST_RESTORE** (failed image restore only):
- Only fires when the boot kernel's `software_resume()` fails to load the hibernate image
- Calls `fix_post_sleep_common` with `schedule_work=false`, clean up only

### GPE 0x52 Unmask (ACPI IRQ Handler)

GPE 0x52 fires during sleep transitions and gets auto-masked by the kernel. The module's ACPI IRQ handler immediately unmasks it. Without this, subsequent sleep cycles fail because the GPE never fires again.

### WiFi Recovery

Delayed workqueue (5s after resume) runs `nmcli` commands to reconnect WiFi, which often drops during suspend/resume.

### PADCFG Correction

The firmware sometimes corrupts GPIO pad configurations during suspend/resume. The module saves the good values before sleep and restores them after, comparing each register and logging any corrections.

Key addresses (Community 0):
- Base: `0xFD6D0000`
- PADCFG range: Pads 40-50, offset `0x500` to `0x588`
- PADCFGLOCK: offset `0x0E0`

## v2.1: Kernel-Space RTC Time Sync (Current)

After hibernate resume, the system clock is stuck at pre-hibernate time. v2.0 used userspace `hwclock --hctosys` which failed silently. v2.1 does it properly from kernel space.

### Architecture: Defense-in-Depth

**Pre-hibernate**: Save wall clock via `ktime_get_real_ts64()` into `saved_pre_hibernate_ts`. This survives hibernate because it's in the module's BSS (restored image contains it).

**Post-hibernate Stage 1 (3s delay)**: `time_sync_retry_fn`

0. **Chrony offline**: `chronyc offline` pauses chrony BEFORE touching the clock. After hibernate, chrony's internal NTP tracking state is stale (from before hibernate). If we set the clock from RTC first, chrony sees the "wrong" time and steps it BACKWARD by hours, blanking the display via GDM/mutter confusion.
1. **Kernel-space RTC read**: `rtc_class_open("rtc0")` + `rtc_read_time()` + `rtc_tm_to_time64()`, direct hardware read, no userspace
2. **Delta validation**: RTC time minus saved pre-hibernate time must be >= 0 and < 30 days (2,592,000 seconds). Catches corrupt RTC, negative time travel, firmware bugs
3. **Kernel-space clock set**: `do_settimeofday64()`, direct kernel timekeeper update, no syscall, no call_usermodehelper
4. **Verify-after-set**: Read back via `ktime_get_real_ts64()` and confirm drift within +/- 2 seconds
5. **Userspace fallback**: If ANY kernel-space step fails, falls back to `date -u -s` from RTC sysfs
6. **Display fix**: Triggers i915 display state reconciliation via debugfs read
7. **Backlight restore**: Writes `max_brightness` to `brightness` via `/sys/class/backlight/intel_backlight/` to force backlight on after display reconciliation (the modeset can momentarily blank the display)

**Post-hibernate Stage 2 (15s delay)**: `time_sync_ntp_fn`
- Runs `chronyc online` to bring chrony back (was taken offline in Stage 1)
- Runs `chronyc makestep` for NTP refinement after source re-selection

### Why Kernel-Space (Windows RE Findings)

Binary Ninja analysis of Windows Surface drivers confirmed:
- `intelpep.sys`: NO RTC/time operations, only power engine plugin work
- `msgpioclx.sys`: GPIO D0Entry/D0Exit only, no time
- `iaLPSS2_GPIO2_ADL.sys`: PADCFG save/restore only, no time
- **`ntoskrnl.exe`**: Handles time via `HalQueryRealTimeClock` -> `KeSetSystemTime`

Windows does it in kernel space. Now we do too.

### Verified Kernel Symbols (6.18.7-surface-1)

```
do_settimeofday64     EXPORT_SYMBOL       vmlinux
rtc_tm_to_time64      EXPORT_SYMBOL       vmlinux
rtc_read_time         EXPORT_SYMBOL_GPL   vmlinux
rtc_class_open        EXPORT_SYMBOL_GPL   vmlinux
rtc_class_close       EXPORT_SYMBOL_GPL   vmlinux
ktime_get_real_ts64   always available via ktime.h
```

## Hibernate Death Sleep: Technical Details

### The Two-Stage Failure

1. **Time desync**: System clock stuck at pre-hibernate time. Breaks TLS (cert validation), Kerberos, NTP, log correlation, cron, everything time-dependent
2. **Display corruption**: eDP panel locks to 32Hz instead of 60Hz. Unusable lag

### The Timing Gap

dmesg shows a 1.26s gap between `PM: hibernation exit` and the module's post-hibernate handler. During this gap, the kernel's own `rtc_resume()` runs but doesn't call `do_settimeofday64()`. The timekeeping core's `timekeeping_resume()` reads the persistent clock but can get stale values if the RTC hasn't fully powered up yet.

### 32Hz Display Fix

After hibernate, the i915 driver sometimes negotiates 32Hz on the eDP panel instead of 60Hz. Reading `/sys/kernel/debug/dri/0000:00:02.0/i915_display_info` forces the driver to reconcile its state, which triggers a mode re-negotiation back to 60Hz.

## File Layout

```
/home/user/Desktop/s2lid/
  surface_s2idle_fix.c          # Module source (v2.1)
  surface_s2idle_fix.ko         # Compiled module
  Makefile                      # Build system
  dkms.conf                     # DKMS config
  progress.md                   # This file
  surface-re/                   # Windows driver reverse engineering
    binja-analysis/
      SYNTHESIS.txt             # Cross-driver synthesis
      intelpep_hibernate_time.txt
      gpio_hibernate_transitions.txt
      msgpioclx_hibernate.txt
      analyze_hibernate_time.py # Binary Ninja script
  debug-snapshots/              # dmesg captures, debug data

/lib/modules/6.18.7-surface-1/
  updates/surface_s2idle_fix.ko # Primary install (initramfs)
  extra/surface_s2idle_fix.ko   # Secondary install
```

## Post-Hibernate Display Blank Fix

### The Bug
After hibernate resume, the screen goes black 1-3 seconds after the login screen appears. Keyboard tap brings it back. NOT actual s2idle (no PM suspend entry in journal).

### Root Cause: Three Factors
1. **Chrony backward clock step** (primary): After hibernate, chrony's internal NTP tracking state is stale. When the module sets the correct time from RTC at +3s, chrony detects "System clock wrong by -39563 seconds" at +9s and steps the clock BACKWARD ~11 hours. This confuses GDM/mutter display management.
2. **Display modeset blanking**: Reading `i915_display_info` forces `drm_modeset_lock_all()` which can momentarily blank the display during hardware state reconciliation.
3. **Bogus rapid wake detection**: `ktime_get()` monotonic clock doesn't advance during hibernate power-off, so `ktime_ms_delta()` always returns ~5s, incorrectly triggering rapid wake tracking and failsafe scheduling.

### The Fix
1. `chronyc offline` before RTC correction, `chronyc online` 15s later (prevents stale NTP state from fighting the clock set)
2. Backlight restore via sysfs after display_fix modeset (works without X/Wayland)
3. PM_POST_RESTORE passes `schedule_work=false`, skips failsafe and rapid wake tracking entirely
4. Immediate lid state report to input layer after hibernate resume (prevents stale ACPI button state from confusing logind)

## v2.2: GPE 0x52 Wake Mask Fix (Hibernate Double-Press)

### The Bug
After hibernate, the system appears fully off but requires TWO power button presses to boot. First press does absolutely nothing. Second press cold boots and resumes successfully. Regular shutdown only needs one press.

### Root Cause
During S4/S5 entry, `acpi_hw_legacy_sleep()` (hwsleep.c:66) calls `acpi_hw_enable_all_wakeup_gpes()` which walks all GPE registers and writes the `enable_for_wake` bitmask to hardware. The ACPI button driver marks GPE 0x52 with `ACPI_GPE_CAN_WAKE` (scan.c:1024), so GPE 0x52 gets **RE-ENABLED as a wakeup source** right before the power transition, AFTER the module's `gpe52_force_disable()` already disabled it.

Then during the power-down transition, VNN rail drops, PADCFG corruption fires a phantom GPE 0x52 edge, and the phantom wake immediately aborts S4/S5 entry. System enters a limbo state where the first power button press is consumed/ignored by the EC.

Regular shutdown is unaffected because no freeze/thaw cycle occurs, so PADCFG is clean and no phantom GPE fires even though GPE 0x52 is in the wake mask.

### The Fix
`acpi_set_gpe_wake_mask(NULL, LID_GPE, ACPI_GPE_DISABLE)` clears GPE 0x52 from the `enable_for_wake` register, preventing `acpi_hw_enable_all_wakeup_gpes()` from re-enabling it during S4/S5 entry. This is an exported, proven ACPICA API already used by the EC driver and ACPI wakeup handler.

GPE 0x52 never needs to be a wakeup source:
- **s2idle**: module handles lid wake via LPS0 ops
- **S4 (hibernate)**: power button handles wake
- **S5 (off)**: power button handles wake

Applied in three places:
1. `gpe52_force_disable()`: added as first call (belt-and-suspenders for every sleep cycle)
2. `surface_s2idle_fix_init()`: disabled at module load (covers from first boot)
3. `surface_s2idle_fix_exit()`: restored to ACPI_GPE_ENABLE (clean unload)

### Key kernel source references
- `drivers/acpi/acpica/hwsleep.c:66` - `acpi_hw_enable_all_wakeup_gpes()` call during sleep entry
- `drivers/acpi/acpica/hwgpe.c:488-515` - walks GPE blocks, writes `enable_for_wake` to hardware
- `drivers/acpi/acpica/evxfgpe.c:491-554` - `acpi_set_gpe_wake_mask()` implementation, ACPI_EXPORT_SYMBOL
- `drivers/acpi/scan.c:1024-1030` - ACPI button driver marks GPEs with ACPI_GPE_CAN_WAKE

## Current State

- **v2.2 compiled, installed, initramfs rebuilt**: Ready for reboot + hibernate test
- **What to verify after hibernate**:
  - Single power button press should wake from hibernate
  - `dmesg | grep "wake mask"` should show: `init: GPE 0x52 ... wake mask disabled`
  - `dmesg | grep time_sync` should show: `time_sync: kernel-space RTC set OK (verified, drift=Xs)`
  - `date` should show correct time immediately after resume
  - Display should be 60Hz not 32Hz

## Version History

| Version | Change |
|---|---|
| v1.0 | Initial: PADCFG save/restore, GPE unmask, WiFi recovery |
| v1.x | Added GPE 0x52 unmask via ACPI IRQ handler, lid _STA polling |
| v2.0 | Rewrote time sync to userspace (hwclock + chronyc), simplified pre-sleep, fixed 32Hz display bug |
| v2.1 | Kernel-space RTC time sync with delta validation, verify-after-set, userspace fallback. Informed by Windows RE. |
| v2.1.1 | Fix post-hibernate display blank: chrony offline/online cycle, backlight restore, PM_POST_HIBERNATION resume detection (mono vs real clock delta), immediate lid report. Fixed critical misconception: PM_POST_RESTORE only fires on FAILED restore, not success. |
| v2.2 | Fix hibernate double power button press: `acpi_set_gpe_wake_mask(NULL, LID_GPE, ACPI_GPE_DISABLE)` prevents `acpi_hw_enable_all_wakeup_gpes()` from re-enabling GPE 0x52 during S4/S5 entry. Root cause: phantom PADCFG corruption edge fires after ACPI sleep code re-enables wakeup GPEs, aborting S4/S5 entry. |

## GitHub

Repository: `wowitsjack/Surface-Linux-Lid-Fix`
Branch: dev (main for stable)
