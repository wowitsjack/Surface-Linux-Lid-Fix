# Surface S2Idle Fix: Complete Handoff Document

**Date:** 2026-03-04 (updated)
**Machine:** Microsoft Surface Laptop 5
**OS:** Ubuntu 25.10
**Kernel:** 6.18.7-surface-1 (linux-surface custom kernel)
**Project path:** `/home/user/Desktop/s2lid/`
**GitHub:** `https://github.com/wowitsjack/Surface-Linux-Lid-Fix` (dev branch)

---

## 1. THE PROBLEM (What We're Fixing)

The Surface Laptop 5 running Linux suffers from **"death sleep"**, a condition where the system enters sleep/hibernate and NEVER WAKES UP. Requires a 20-second power button hold to force-off.

### Root Cause Chain

1. **Hardware bug:** Intel INTC1055 pinctrl power-gates pin 213 (lid sensor) during low-power transitions
2. **Corruption:** The power-gating flips the **PADCFG0 RXINV bit** (bit 23) at physical address `0xfd6a09a0`
3. **Phantom SCI:** The flipped RXINV creates a phantom edge that fires a spurious SCI interrupt on **GPE 0x52**
4. **Wakeup poisoning:** The ACPI handler for GPE 0x52 calls `pm_system_cancel_wakeup()`, which permanently poisons the wakeup framework
5. **Death:** System can never complete a wake cycle. Stuck in half-powered limbo forever.

This affects BOTH:
- **S2idle (suspend-to-idle):** corruption during C-state transitions in the idle loop
- **Hibernate (S4):** corruption during thaw/restore after BIOS reinitializes hardware

### The 1.26-Second Timing Gap (Critical Concept)

```
[0.360s] ACPI: Enabled 8 GPEs in block 00 to 7F    <-- GPE 0x52 goes LIVE
[1.616s] surface_s2idle_fix: loaded                  <-- Module loads 1.26s LATER
```

No loadable kernel module, not even from initramfs, can close this gap. GPE 0x52 is active and capable of firing for 1.26 seconds before ANY module code runs. During hibernate resume, the fresh boot kernel initializes ACPI, enables GPEs, then loads the hibernate image. If PADCFG corruption fires GPE 0x52 in that window, the wakeup framework is poisoned before the module exists in memory.

---

## 2. CURRENT WORKING FIX (Two Parts, Both Required)

### Part A: Kernel Boot Parameters (6 total)

Currently in GRUB (`/etc/default/grub`):

```
pci=hpiosize=0 acpi=force reboot=acpi acpi_sleep=nonvs acpi_osi="Windows 2020" acpi_mask_gpe=0x52
```

Currently active (verified via `/proc/cmdline`):
```
BOOT_IMAGE=/vmlinuz-6.18.7-surface-1 root=UUID=3242ebd8-6476-4a7c-aec8-2606712f8ef7 ro quiet splash resume=UUID=3242ebd8-6476-4a7c-aec8-2606712f8ef7 resume_offset=4524032 i915.enable_psr=0 i915.enable_dc=0 pci=hpiosize=0 acpi=force reboot=acpi acpi_sleep=nonvs "acpi_osi=Windows 2020" acpi_mask_gpe=0x52 crashkernel=2G-4G:320M,4G-32G:512M,32G-64G:1024M,64G-128G:2048M,128G-:4096M vt.handoff=7
```

What each does:

| Parameter | What It Does | Can Be Replaced By Built-in Code? |
|---|---|---|
| `acpi_mask_gpe=0x52` | Masks GPE 0x52 at ACPI init (0.360s), closing the timing gap | YES, via `acpi_mask_gpe(NULL, 0x52, TRUE)` at `subsys_initcall_sync` (level 4s, runs right after ACPI init level 4) |
| `acpi_sleep=nonvs` | Skips ACPI NVS memory save/restore during suspend/resume | YES, via `acpi_nvs_nosave()` (declared `__init` in `include/linux/acpi.h:533`, callable from built-in `__init` code) |
| `reboot=acpi` | Forces ACPI reboot method instead of keyboard controller | YES, via `reboot_type = BOOT_ACPI` (extern in `include/linux/reboot.h:37`, accessible from built-in code) |
| `acpi_osi="Windows 2020"` | Tells firmware to use Windows 2020 ACPI code paths | YES, via `acpi_install_interface("Windows 2020")` (EXPORT_SYMBOL, callable from anywhere) |
| `acpi=force` | Forces ACPI enabled | PROBABLY UNNECESSARY, Surface firmware always has ACPI |
| `pci=hpiosize=0` | Disables PCI hotplug IO size reservation | PROBABLY UNNECESSARY, may not be needed for the death sleep fix |

### Part B: Kernel Module (`surface_s2idle_fix.c`)

Currently loaded (`lsmod` confirms `surface_s2idle_fix`).

**Version:** 2.0.0
**Size:** 1505 lines
**Location:** `/home/user/Desktop/s2lid/surface_s2idle_fix.c`
**Installed at:** `/lib/modules/6.18.7-surface-1/extra/surface_s2idle_fix.ko`
**Loaded via:** systemd modules-load.d, included in initramfs

The module handles ALL runtime s2idle/hibernate protection:

**S2idle protection:**
- PM notifier (INT_MAX priority): masks GPE 0x52 via `acpi_mask_gpe(TRUE)` during `PM_SUSPEND_PREPARE`, unmasks during `PM_POST_SUSPEND`
- Init: `acpi_mask_gpe(NULL, LID_GPE, FALSE)` unmasks GPE 0x52 after wakeup handler registration (fixes boot param `acpi_mask_gpe=0x52` persistent mask)
- ACPI wakeup handler (SCI IRQ 9): runs BEFORE `acpi_ec_dispatch_gpe()`, fixes PADCFG corruption, clears GPE 0x52 status to prevent spurious full resume
- LPS0 hooks (`prepare`/`check`/`restore`): unmask GPE inside s2idle loop for lid detection, poll RXSTATE for genuine lid-open, re-mask before full resume
- Failsafe delayed work: 2s after wake, checks lid state + power button, re-suspends if spurious
- Exponential backoff: 2s/4s/8s/15s delays, max 10 retries
- RXSTATE polling: background poller emits SW_LID input events
- Power button observer: passive input handler distinguishes real vs spurious wakes

**Hibernate protection:**
- `freeze`/`freeze_noirq`: disables GPE 0x52 during device quiesce (uses triple approach: mask + disable + clear)
- `thaw_noirq`/`thaw_early`: fixes PADCFG corruption after BIOS reinitialization
- `restore_noirq`/`restore_early`: same for hibernate image restore path
- `poweroff`/`poweroff_noirq`: disables GPE 0x52 before shutdown after image save
- `PM_HIBERNATION_PREPARE`/`PM_POST_HIBERNATION`: hibernate-specific pre/post sleep
- Init-time GPE masking: immediately masks GPE 0x52 on load for hibernate restore safety
- Post-hibernate time sync: 3s delayed `hwclock --hctosys --utc` + `chronyc makestep` via `call_usermodehelper`

**Key hardware constants:**
- `LID_PADCFG0_PHYS = 0xfd6a09a0` (pin 213, INTC1055 community)
- `LID_PADCFG1_PHYS = 0xfd6a09a4`
- `LID_GPE = 0x52`
- `PADCFG0_RXINV = BIT(23)` (the bit that gets corrupted)
- `PADCFG0_GPIROUTSCI = BIT(19)` (SCI routing, verified on init)
- `PADCFG0_GPIORXSTATE = BIT(1)` (actual lid state: 1=closed, 0=open)

**DMI match:** Surface Laptop 5 only (`Microsoft Corporation` + `Surface Laptop 5`)

### Systemd Sleep Config

`/etc/systemd/sleep.conf.d/hibernate.conf`:
```
[Sleep]
HibernateMode=platform shutdown
```

### How They Work Together

| Protection Layer | Boot Params Handle | Module Handles |
|---|---|---|
| GPE 0x52 timing gap (0.36s-1.6s) | `acpi_mask_gpe=0x52` | Cannot reach this window |
| PADCFG corruption during s2idle | N/A | Wakeup handler + LPS0 hooks |
| Spurious wake promotion | N/A | Clears GPE before `acpi_any_gpe_status_set()` |
| Lid-open detection in s2idle | N/A | LPS0 check polls RXSTATE |
| Spurious wake re-suspend | N/A | Failsafe + exponential backoff |
| Lid state for DE | N/A | SW_LID input device |
| Fake shutdown | `reboot=acpi` | N/A |
| Firmware PM paths | `acpi_osi="Windows 2020"` | N/A |
| NVS confusion | `acpi_sleep=nonvs` | N/A |

**Remove boot params:** Hibernate dies. S2idle still works.
**Remove module:** S2idle dies. Hibernate might partially work but loses all runtime protection.

---

## 3. THE GOAL (What The User Wants)

**The user wants to eliminate the 6 boot parameters by baking their functionality into the linux-surface kernel as built-in code.**

The user does NOT want:
- A separate loadable module for the boot param replacement
- Documentation or README files
- Research-only output

The user DOES want:
- Code that lives in the linux-surface kernel tree
- Built-in (`=y`) kernel code, not a module
- Something that "just works" for all Surface Laptop 5 users without any GRUB editing

### Why Built-in Changes Everything

When code is compiled into the kernel (not as a module), it gains access to things modules cannot touch:

1. **`acpi_nvs_nosave()`**: Declared `void __init` in `include/linux/acpi.h:533`. The `__init` attribute means this function is discarded after boot. Only callable from other `__init` code. A loadable module's `module_init` is NOT `__init` when built as a module, but IS `__init` when built into the kernel. So built-in code CAN call it.

2. **`reboot_type`**: Declared `extern enum reboot_type reboot_type;` in `include/linux/reboot.h:37`. NOT in Module.symvers (verified: `grep reboot_type Module.symvers` returns nothing). NOT exported. But built-in code accesses kernel globals directly, no EXPORT_SYMBOL needed.

3. **`subsys_initcall_sync` (level 4s)**: Defined as `__define_initcall(fn, 4s)` in `include/linux/init.h:301`. ACPI subsystem uses `subsys_initcall` (level 4). Level 4s runs RIGHT AFTER level 4. The timing gap between ACPI init and level 4s is microseconds, not 1.26 seconds. This is the key insight.

4. **`acpi_install_interface()`**: EXPORT_SYMBOL (in Module.symvers), callable from anywhere. Would work as a module too, but timing matters.

5. **`acpi_mask_gpe()`**: EXPORT_SYMBOL (in Module.symvers), callable from anywhere.

### Verified Kernel Headers

All of these declarations exist in the kernel build tree at `/lib/modules/6.18.7-surface-1/build/`:

```
include/linux/acpi.h:533    -> void __init acpi_nvs_nosave(void);
include/linux/reboot.h:32   -> BOOT_ACPI = 'a',
include/linux/reboot.h:37   -> extern enum reboot_type reboot_type;
include/linux/init.h:300    -> subsys_initcall(fn)      = level 4
include/linux/init.h:301    -> subsys_initcall_sync(fn)  = level 4s
include/linux/init.h:305    -> device_initcall(fn)       = level 6
```

### The Dual Initcall Architecture

The proposed solution uses TWO `__init` functions at different initcall levels:

**Early init (`subsys_initcall_sync`, level 4s):** `surface_s2idle_early_init()`
- Runs microseconds after ACPI subsystem init (level 4)
- DMI check for Surface Laptop 5
- `acpi_mask_gpe(NULL, 0x52, TRUE)` -> replaces `acpi_mask_gpe=0x52` boot param
- `acpi_nvs_nosave()` -> replaces `acpi_sleep=nonvs` boot param
- `reboot_type = BOOT_ACPI` -> replaces `reboot=acpi` boot param
- `acpi_install_interface("Windows 2020")` -> replaces `acpi_osi="Windows 2020"` boot param
- Guarded by `#ifndef MODULE` so this ONLY compiles when built-in

**Normal init (`device_initcall`, level 6):** existing `surface_s2idle_fix_init()`
- The existing module_init function, unchanged in behavior
- Runs at the standard time, sets up all the runtime protection
- When built as a module, this is `module_init()`. When built-in, it's `device_initcall()` (level 6).

### What About `acpi=force` and `pci=hpiosize=0`?

Analysis from prior sessions concluded these are likely unnecessary:
- `acpi=force`: Surface firmware always has ACPI. This was belt-and-suspenders.
- `pci=hpiosize=0`: May have been added during debugging, unclear if it contributes to the fix.

The user should test removing these two after the built-in code is working to confirm they're not needed.

---

## 4. WHAT HAS NOT BEEN DONE YET

**The implementation has not started.** The plan was fully designed in a previous session but the user rejected `ExitPlanMode` (which is the "approve plan and start coding" gate). This does NOT mean the user rejected the plan, it may mean they just didn't want the formal approval flow.

### What Needs To Be Built

1. **Modify `surface_s2idle_fix.c`:**
   - Add `#include <linux/reboot.h>` (for `reboot_type` and `BOOT_ACPI`)
   - Add a new `__init` function `surface_s2idle_early_init()` with:
     - DMI check (same `surface_ids` table)
     - `acpi_mask_gpe(NULL, LID_GPE, TRUE);`
     - `acpi_nvs_nosave();`
     - `reboot_type = BOOT_ACPI;`
     - `acpi_install_interface("Windows 2020");`
     - `pr_info` logging for each action
   - Wrap it in `#ifndef MODULE` / `#endif` guards
   - Add `subsys_initcall_sync(surface_s2idle_early_init);` inside the guards
   - Keep the existing `module_init(surface_s2idle_fix_init)` and `module_exit(surface_s2idle_fix_exit)` unchanged

2. **Create a Kconfig entry** for integration into the linux-surface kernel tree:
   ```
   config SURFACE_S2IDLE_FIX
       tristate "Surface Laptop 5 s2idle/hibernate death sleep fix"
       depends on ACPI && DMI
       default y
       help
         Fixes death sleep on Surface Laptop 5 caused by Intel INTC1055
         pinctrl power-gating corrupting pin 213 PADCFG0 RXINV bit.
         When built-in (=y), also replaces the need for boot parameters
         acpi_mask_gpe=0x52, acpi_sleep=nonvs, reboot=acpi, and
         acpi_osi="Windows 2020".
   ```

3. **Update Makefile** for kernel tree integration:
   ```
   obj-$(CONFIG_SURFACE_S2IDLE_FIX) += surface_s2idle_fix.o
   ```
   (Currently it's `obj-m := surface_s2idle_fix.o` for out-of-tree build)

4. **Test:** Build as built-in (`=y`), remove all 6 boot params from GRUB, test hibernate and s2idle

---

## 5. MODULE INTERNALS (Deep Technical Reference)

### Key Data Structures

- `lid_padcfg_base`: `void __iomem *`, ioremap of `0xfd6a09a0`, 8 bytes mapped
- `saved_padcfg0` / `saved_padcfg1`: Golden copies of PADCFG registers (RXSTATE masked out of padcfg0)
- `s2idle_gpe_active`: bool, true between `lps0_prepare` and `lps0_restore` (inside s2idle idle loop)
- `in_hibernate`: bool, true between `PM_HIBERNATION_PREPARE` and `PM_POST_HIBERNATION`
- `failsafe_in_progress`: bool, set by work functions calling `pm_suspend()` to prevent deadlock on `cancel_delayed_work_sync()`
- `gpe52_was_enabled`: bool, tracks whether GPE 0x52 needs re-enabling in resume path
- `lid_was_closed_at_suspend`: bool, cached lid state for failsafe decision
- `power_button_seen`: atomic_t, set by passive input handler on KEY_POWER

### Delayed Work Items

1. `lid_failsafe_work` (`lid_failsafe_fn`): 2s after resume, decides stay-awake vs re-suspend
2. `lid_resync_work` (`lid_resync_fn`): after power-button wake with lid closed, polls RXSTATE until GPIO settles
3. `lid_poll_work` (`lid_poll_fn`): background 2s interval RXSTATE poller, emits SW_LID events
4. `time_sync_retry_work` (`time_sync_retry_fn`): 3s after hibernate resume, runs `hwclock --hctosys --utc` then `chronyc makestep`

### PM Callback Order

**S2idle path:**
```
PM_SUSPEND_PREPARE -> fix_pre_sleep_common("suspend")
  suspend_noirq -> gpe52_force_disable()
    [s2idle loop begins]
    lps0_prepare -> fix PADCFG, clear GPE, unmask GPE, s2idle_gpe_active=true
      [hardware sleeps, wakes on any interrupt]
      lid_wake_handler -> fix PADCFG, clear GPE status (prevents full resume promotion)
    lps0_check -> fix PADCFG, check RXSTATE, pm_system_wakeup() if lid open
    lps0_restore -> s2idle_gpe_active=false, re-mask GPE
    [s2idle loop ends on genuine wake]
  resume_noirq -> fix PADCFG
  resume_early -> fix PADCFG, unmask GPE
PM_POST_SUSPEND -> fix_post_sleep_common("post-suspend")
  -> schedule failsafe work, restart lid polling
```

**Hibernate path:**
```
PM_HIBERNATION_PREPARE -> fix_pre_sleep_common("hibernate"), in_hibernate=true
  freeze -> gpe52_force_disable()
  freeze_noirq -> gpe52_force_disable(), fix PADCFG
  [snapshot created]
  thaw_noirq -> fix PADCFG (reuses s2idle_fix_resume_noirq)
  thaw_early -> fix PADCFG, unmask GPE (reuses s2idle_fix_resume_early)
  [decides to poweroff]
  poweroff -> gpe52_force_disable()
  poweroff_noirq -> gpe52_force_disable()
  [system powers off, hibernate image on disk]
PM_POST_HIBERNATION -> fix_post_sleep_common("post-hibernation")
  -> in_hibernate = false
  -> schedule time_sync_retry_work (3s delay: hwclock + chronyc makestep)
  -> schedule failsafe work, restart lid polling

  [FRESH BOOT for restore]
  [Module loads from initramfs, immediately masks GPE 0x52]
  PM_RESTORE_PREPARE -> fix_pre_sleep_common("restore")
  freeze -> gpe52_force_disable()
  freeze_noirq -> gpe52_force_disable(), fix PADCFG
  [hibernate image restored to memory]
  restore_noirq -> fix PADCFG (reuses s2idle_fix_resume_noirq)
  restore_early -> fix PADCFG, unmask GPE (reuses s2idle_fix_resume_early)
PM_POST_RESTORE -> fix_post_sleep_common("post-restore")
  -> schedule time_sync_retry_work (3s delay)
  -> schedule failsafe work, restart lid polling
```

### The `gpe52_force_disable()` Triple

```c
acpi_mask_gpe(NULL, LID_GPE, TRUE);   // Software mask
acpi_disable_gpe(NULL, LID_GPE);       // Clear hardware enable bit
acpi_clear_gpe(NULL, LID_GPE);         // Clear status bit
```

This triple ensures GPE 0x52 cannot fire even if `acpi_enable_all_wakeup_gpes()` is called later (which happens during syscore_suspend).

### PADCFG Correction Pattern (Used Everywhere)

```c
if ((current & PADCFG0_RXINV) != (saved & PADCFG0_RXINV)) {
    writel(saved_padcfg1, base + 4);  // Restore PADCFG1 first
    wmb();                             // Write barrier
    writel(saved_padcfg0, base);       // Then PADCFG0
    wmb();                             // Write barrier
    acpi_clear_gpe(NULL, LID_GPE);     // Clear any resulting GPE
}
```

PADCFG1 is written first because some Intel pinctrl implementations latch both registers together.

---

## 6. FILES IN THE PROJECT

```
/home/user/Desktop/s2lid/
├── surface_s2idle_fix.c     # 1505 lines, the kernel module source (v2.0.0)
├── Makefile                 # Out-of-tree module build (obj-m)
├── dkms.conf                # DKMS packaging
├── FINDINGS.md              # Research findings, Windows RE, kernel internals
├── HANDOFF.md               # This file, complete handoff document
├── New-Notes.md             # Session notes
├── hibernate.md             # Comprehensive documentation of the problem and fix
├── surface-re/              # Windows driver reverse engineering artifacts
├── debug-snapshots/         # Debug data directory
├── surface_s2idle_fix.ko    # Compiled module binary
└── [build artifacts]        # .o, .mod, .mod.c, .mod.o, Module.symvers, etc.
```

### Related System Files

- `/etc/default/grub` - Contains the 6 boot parameters
- `/etc/systemd/sleep.conf.d/hibernate.conf` - `HibernateMode=platform shutdown`
- `/lib/modules/6.18.7-surface-1/extra/surface_s2idle_fix.ko` - Installed module
- `/lib/modules/6.18.7-surface-1/build/` - Kernel headers/build tree

---

## 7. PRIOR SESSION HISTORY

1. **Initial module development** - Created the s2idle fix module, PADCFG correction, GPE masking, LPS0 hooks, failsafe re-suspend
2. **Hibernate fix attempts (3 rounds, all failed)** - PM notifier, thaw/restore callbacks, init-time masking, aggressive force-disable. All failed because of the 1.26s timing gap.
3. **Boot parameter discovery** - Found the 6 GRUB parameters that fix hibernate
4. **Display refresh rate issue** - Post-hibernate, display stuck at 32Hz (separate symptom)
5. **Kernelifying analysis** - Deep feasibility study of replacing boot params with built-in code. Confirmed all necessary kernel symbols/headers exist. Designed the dual initcall architecture.
6. **v2.0.0 fixes (2026-03-04):**
   - **GPE 0x52 unmask fix:** Added `acpi_mask_gpe(NULL, LID_GPE, FALSE)` to init. Fixed lid-close causing semi-wake state, mouse death, and re-sleep cycle. Root cause: `acpi_mask_gpe=0x52` boot param created persistent mask that `acpi_set_gpe(ENABLE)` did not clear.
   - **Pre-sleep simplification:** Replaced `gpe52_force_disable()` in `fix_pre_sleep_common()` with simple `acpi_mask_gpe(TRUE)`.
   - **Time sync rewrite:** Removed kernel-space `do_settimeofday64()` approach (failed silently in PM notifier context). Replaced with userspace `hwclock --hctosys --utc` + `chronyc makestep` via 3s delayed work after hibernate resume.
7. **Current state** - v2.0.0 built and installed, awaiting reboot to test time sync fix. Death sleep and lid-close bugs confirmed fixed.

---

## 8. IMPORTANT USER PREFERENCES

- The user wants ACTION, not documentation. Do not spam the project with docs.
- Follow instructions EXACTLY as stated. Do not substitute what you think is better.
- The user's machine password is `pass2weak` (for sudo operations).
- The user communicates casually, typos are normal, parse intent not spelling.
- Never use emdashes in text output, use commas instead.
- The user prefers deep technical analysis ("ultrathink") when asked.
- The user is highly technically skilled, knows kernel internals.

---

## 9. WHAT NEEDS TO HAPPEN NEXT

### Immediate
1. **Reboot** to load the new v2.0.0 module
2. **Test hibernate** to verify time sync fix (should see `time_sync: 'hwclock --hctosys --utc' ok` and `time_sync: 'chronyc makestep' ok` in dmesg)
3. **Test lid close/open** to confirm death sleep fix holds

### Future: Built-in kernel code (eliminates boot parameters)
The plan to bake boot parameter functionality into the linux-surface kernel as built-in code is designed but not yet implemented:

1. Add `surface_s2idle_early_init()` at `subsys_initcall_sync` (level 4s, runs microseconds after ACPI init)
2. DMI-gated: `acpi_mask_gpe(0x52)`, `acpi_nvs_nosave()`, `reboot_type = BOOT_ACPI`, `acpi_install_interface("Windows 2020")`
3. Wrapped in `#ifndef MODULE` guards (only compiles when built-in)
4. Kconfig entry for `CONFIG_SURFACE_S2IDLE_FIX` (tristate, default y)
5. Eliminates all 6 GRUB boot parameters for Surface Laptop 5 users
