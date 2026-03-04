# Surface S2Idle Fix: Research Findings & Redesign Notes

## Root Cause (Confirmed via Windows RE)

Intel INTC1055 GPIO Community 4's VNN (VCCIO nanonode) power rail drops during C-state transitions in s2idle. When VNN drops, PADCFG registers lose state. When VNN returns, PADCFG0 on pin 213 (lid sensor) comes back with RXINV (bit 23) flipped. This phantom edge fires GPE 0x52 as an SCI, and `acpi_any_gpe_status_set()` promotes it to a full resume. `pm_system_cancel_wakeup()` poisons the wakeup framework permanently = death sleep.

**Proof from intelpep.sys**: The string `BTC_GPIO_COM4_VNN_ON` exists in intelpep's SLP_S0 blocker table, confirming Windows PEP actively tracks Community 4's power rail state as an S0ix entry blocker.

---

## Windows Driver Stack (4 drivers reverse-engineered via Binary Ninja)

### 1. iaLPSS2_GPIO2_ADL.sys (Intel GPIO hardware driver)

**PADCFG Save** (`GpioSaveBankHardwareContext` @ 0x140007dc0):
- Iterates ALL pins in bank (not just one)
- Bank stride: `bank_index << 5` (32 bytes per bank)
- Pin stride: 9 DWORDs (0x24 bytes) per pin in save buffer
- For each pin:
  - Checks PMODE field `(padcfg0 & 0x3C00) == 0` (only saves Mode 0 / native GPIO pins)
  - Stores: DW0 at `pin*9+8`, DW1 at `pin*9+0xC`, valid flag at `pin*9+7`
- After all pins: saves HOSTSW_OWN register at `bank+0x1C`
- Sets bank valid flag at `bank+0x18`
- Evaluates ACPI `_DSM` to get skip list (vendor-excluded pins)

**PADCFG Restore** (`GpioRestoreBankHardwareContext` @ 0x140007b30):
- Checks bank valid flag before restoring
- Per-pin: checks pin valid flag, then writes:
  - **DW1 FIRST** (pad tolerance/termination)
  - **DW0 SECOND** (mode, direction, RXINV)
  - Clears valid flag after restore
- Restores HOSTSW_OWN register
- Clears bank valid flag

### 2. msgpioclx.sys (GPIO class extension, ActiveBoth emulation)

**RXINV Flip** (`GpiopFlipPinPolarity` @ 0x1c000a848):
- Checks pin flag bit 0x100 (emulated ActiveBoth enabled)
- Toggles polarity, calls hardware ReconfigureInterrupt callback
- **5-retry loop** on hardware failure
- **KeBugCheckEx(GPIO_CONTROLLER_DRIVER_ERROR, 5)** after 5 failures = BSOD
- Updates pin polarity (offset 0xC) and state (offset 0x8) on success

**ISR Chain**:
```
GpiopInterruptBankIsr (hardware ISR)
  -> GpiopServiceInterrupt
       -> GpiopUpdateStatusAndMaskInformation (read/clear GPI_STS)
       -> GpiopServiceInterruptOnPin (per-pin)
            -> GpiopInvokeTargetIsr (call registered ISR)
                 -> if "not claimed" + ActiveBoth: GpiopFlipPinPolarity
```

**Key insight**: If no driver claims the interrupt AND the pin has ActiveBoth emulation, Windows assumes it was a phantom edge from wrong polarity and auto-corrects by flipping RXINV. This is the self-healing mechanism Linux lacks.

**D0 Transition**: `GpiopDevicePowerD0TransitionWorker` processes all pending ActiveBoth reconfiguration when controller exits D3, ensuring polarity consistency after power state changes.

### 3. SurfaceButton.sys (Button/lid state machine)

- Uses DMF (Driver Module Framework) + SAM/SSH communication
- 11 button slots, reset at D0Entry via `ButtonStateInitialize`
- Detects "button held during hibernate" at D0Entry (logs warning)
- D0Exit distinguishes hibernate (D3) from disable (D3.GE)
- `PowerSettingCallback`: registered at D0Entry, unregistered at D0Exit
- Long power button hold: 4-second timer, `ExNotifyCallback` notification
- Source: `REPOSITORYDIR\sys\ButtonState.c`

### 4. intelpep.sys (Platform Energy Processor)

**LPIT Parsing**:
- 0x38 bytes per LPIT entry, unique IDs, residency counters
- Supports frequency overrides per platform state
- Error: "INTELPEP: Cannot override NULL LPIT Address!"

**SLP_S0 Blocker Table** (100+ named blockers):
- `SWK_*`: Software Known (OTG_D3, SATA_D3, XHCI_D3, ADSP_D3, PMC_IDLE, FIVR_WAKE, CSME_POWER_GATING)
- `SDB_*`: Debug (Audio_DSP_Not_In_D3, USB_Not_In_D3, various Clock_Active)
- `BTC_*`: Block-To-Check (GPIO_COM0-5_VNN_ON, PMC_IDLE, ITSS, 70+ power domains)
- `PC0/PC2/PC2R_WAKE_*`: CPU package C-state wake reasons
- `PC2R_DEEP_WAKE_*`: Deep wake reasons (PCH_MEM, USB_LTR, TIMER, TYPEC, DISPLAY)
- `CTA_WAKE_REASON_*`: Per-core C2-C9 wake reasons (30+ per state)

**PCIe Constraint Management**:
- `PepPcieClearRpConstraint`: uses `ZwPowerInformation(PowerInformationInternal, 0x41)` to clear DRIPS device constraints
- `PepPciePostPchLtrOvrMailbox`: PMC mailbox for LTR overrides, 3 retries, `KeStallExecutionProcessor(10us)` between attempts
- `PepPciePreparePcieDevice`: `KeBugCheckEx(INTERNAL_POWER_ERROR, 0x100)` on allocation failure = BSOD

**Residency Counters** (`PepSubsystemReadResidencyCounter`):
- Multi-width: 8/16/32/64-bit with appropriate masks
- PMC mailbox commands for debug pkg/pch counter reads
- Writes command register BEFORE reading counter value

---

## Current Linux Module (v2.0.0) Architecture

### Hardware Target
- Pin 213, INTC1055 Community 4, physical 0xfd6a09a0
- GPE 0x52, SCI IRQ 9
- PADCFG0 bits: RXINV(23), GPIROUTSCI(19), GPIORXDIS(8), GPIORXSTATE(1)

### Callback Layers (Defense in Depth)

| Layer | When | What |
|-------|------|------|
| PM Notifier (INT_MAX) | PM_SUSPEND_PREPARE / PM_HIBERNATION_PREPARE | Save golden PADCFG, mask GPE 0x52 (`acpi_mask_gpe(TRUE)`), cancel work items |
| suspend_noirq | Last before s2idle | Force-disable GPE 0x52 |
| ACPI wakeup handler | Every C-state exit, BEFORE acpi_ec_dispatch_gpe | Fix PADCFG corruption, clear GPE 0x52 status |
| LPS0 prepare | s2idle loop entry | Fix PADCFG, start 500ms hrtimer, GPE stays masked |
| LPS0 check | Every s2idle iteration | Fix PADCFG, poll RXSTATE for lid-open, only pm_system_wakeup() if genuine |
| LPS0 restore | s2idle loop exit | Cancel hrtimer, clear flags |
| resume_noirq | First after wake | Fix PADCFG before GPE re-enablement |
| resume_early | Second after wake | Double-check PADCFG, unmask GPE 0x52 |
| PM Notifier | PM_POST_SUSPEND / PM_POST_HIBERNATION | Final check, unmask fallback, schedule failsafe, restart polling |
| Hibernate variants | freeze/freeze_noirq/thaw_noirq/thaw_early/poweroff/restore | All covered with equivalent callbacks |

### GPE 0x52 Unmask at Init (v2.0.0 fix)

The `acpi_mask_gpe=0x52` boot parameter masks GPE 0x52 at ACPI init (0.36s) to close the 1.26s timing gap before the module loads. Previous versions only called `acpi_set_gpe(ENABLE)` at init, which does NOT clear the hardware mask bit. v2.0.0 adds `acpi_mask_gpe(NULL, LID_GPE, FALSE)` at init to properly unmask after the wakeup handler is registered. Without this, the ACPI button driver (PNP0C0D, event0) was deaf to lid events, causing delayed suspend via the module's 2s poller on event2 instead.

### Pre-sleep GPE Masking (v2.0.0 simplification)

v2.0.0 simplified `fix_pre_sleep_common()` from calling `gpe52_force_disable()` (mask + disable + clear) to just `acpi_mask_gpe(TRUE)` + `gpe52_was_enabled = true`. The `suspend_noirq` handler still does `gpe52_force_disable()` as belt-and-suspenders at the noirq level.

### Failsafe System
- **2-second delay** after wake, checks power_button_seen + RXSTATE
- Real wake: power button OR lid open -> stay awake
- Spurious wake: no power button AND lid closed -> re-suspend
- **Exponential backoff**: 2s, 4s, 8s, 15s cap for rapid-wake storms
- **Max 10 retries** before giving up

### Background Polling
- **lid_poll_work** (2s interval): continuous RXSTATE polling, emits SW_LID input events
- **lid_resync_work** (1s interval, 120 max): settles GPIO after power-button wake with lid closed
- **s2idle_poll_timer** (500ms hrtimer): periodic CPU wakeup during s2idle for lid polling

### Time Sync (Post-Hibernate "32Hz Of Death" Fix)

v2.0.0 replaced the old 3-tier kernel-space approach with a simple userspace strategy:

1. **3s after hibernate resume:** `hwclock --hctosys --utc` via `call_usermodehelper` (force system clock from hardware RTC)
2. **Immediately after:** `chronyc makestep` (force NTP step-correct, not slew)

The old `do_settimeofday64()` kernel-space approach was removed because it fails silently in PM notifier context. The 3s delay ensures userspace (`/bin/sh`, `chronyc`) is thawed and available. Only runs for `PM_POST_HIBERNATION` and `PM_POST_RESTORE` (not suspend, which doesn't need it).

Generic `run_cmd()` helper handles all userspace command execution with proper PATH and error logging.

### Boot Parameter Dependencies
```
acpi_mask_gpe=0x52    # Closes 1.26s timing gap at ACPI init
acpi_sleep=nonvs      # Skips NVS restore confusion
reboot=acpi           # Forces ACPI reboot method
acpi_osi="Windows 2020"  # Enables Windows firmware paths
acpi=force            # Probably unnecessary
pci=hpiosize=0        # Probably unnecessary
```

---

## What Windows Does Better (Gaps in Our Module)

### 1. Full-Bank PADCFG Save/Restore
Windows saves ALL pins in Community 4, we only save pin 213. Other pins could have corruption we're not catching. Low risk for the death sleep bug specifically, but not defense-in-depth.

### 2. HOSTSW_OWN Register
Windows saves/restores the host-software-ownership register per bank. If HOSTSW_OWN corruption occurred, our GPIO reads could silently fail. We don't check this.

### 3. ActiveBoth Emulation / Dynamic RXINV Flip
Windows detects phantom edges at ISR level and self-corrects by flipping RXINV back. We restore from a saved snapshot instead. Our approach is simpler but can't handle the case where firmware intentionally re-flips RXINV after our restore.

### 4. VNN Power Rail Awareness
intelpep tracks `BTC_GPIO_COM4_VNN_ON`. Could pre-emptively save PADCFG before VNN drops instead of fixing after corruption. Would require PMC register reading.

### 5. PMC Debug Integration
intelpep reads PMC debug counters for SLP_S0 blocker identification. We could add diagnostics showing which subsystems block S0ix entry.

### 6. Boot Parameters as Built-in Code
The 4 meaningful boot params could be applied programmatically at `subsys_initcall_sync` time instead of requiring GRUB editing:
- `acpi_mask_gpe=0x52` -> `acpi_mask_gpe(NULL, 0x52, TRUE)` at early init
- `acpi_sleep=nonvs` -> could be set via kernel API
- `reboot=acpi` -> `reboot_type = BOOT_ACPI`
- `acpi_osi="Windows 2020"` -> `acpi_install_interface("Windows 2020")`

### 7. Coordinated Power Transitions
Windows has a full stack: intelpep coordinates platform state, tells GPIO driver when to save, GPIO CLX manages per-pin state, hardware driver does the MMIO. Our module is a standalone workaround that doesn't integrate into the pinctrl-intel driver's normal save/restore flow.

---

## Key Questions for Redesign

1. Should we extend to save/restore multiple pins or the entire Community 4 bank?
2. Should we integrate with pinctrl-intel's existing save/restore instead of doing raw ioremap?
3. Should we add PMC SLP_S0 debug reading for diagnostics?
4. Should we try to eliminate boot parameters by doing early init?
5. Should we add HOSTSW_OWN monitoring?
6. How much of the Windows PEP coordination model can we replicate in a standalone module?
7. Should we implement real ActiveBoth emulation (dynamic RXINV flip) vs snapshot restore?

---

## Linux Kernel Source Analysis (pinctrl-intel + ACPI s2idle)

### pinctrl-intel.c: How Linux Saves/Restores GPIO State

**Data structures**:
```c
struct intel_pad_context {
    u32 padcfg0;    // PADCFG0 register value
    u32 padcfg1;    // PADCFG1 register value (pull-up/down)
    u32 padcfg2;    // PADCFG2 register value (debounce)
};

struct intel_community_context {
    u32 *intmask;   // GPI_IE interrupt enable masks per GPP
    u32 *hostown;   // HOSTSW_OWN ownership per GPP
};
```

**`intel_pinctrl_suspend_noirq()`**: Iterates ALL pins, calls `intel_pinctrl_should_save()` filter, masks GPIORXSTATE, saves PADCFG0/1/2, then saves GPI_IE and HOSTSW_OWN per community per GPP.

**`intel_pinctrl_resume_noirq()`**: Calls `intel_gpio_irq_init()` FIRST (mask all interrupts, clear all status), then restores pads, then restores intmask and hostown.

**`intel_pinctrl_should_save()`**: Returns true if pad is usable AND has (mux_owner OR gpio_owner OR irq_line OR direct_irq).

**`intel_restore_padcfg()`**: Uses `intel_gpio_update_reg()` to only write if value differs (avoids unnecessary MMIO writes).

**`intel_restore_hostown()`**: Only restores bits for pads actually requested by GPIO driver.

**Critical gap**: pinctrl-intel's suspend_noirq/resume_noirq ONLY runs during explicit system suspend/resume, NOT during s2idle C-state transitions where VNN cycling happens continuously. There are NO s2idle-specific hooks in pinctrl-intel. No VNN/power-gating awareness. No ActiveBoth emulation (that's Windows-only via msgpioclx).

### ACPI s2idle Wake Decision Flow

From `acpi_s2idle_wake()` in drivers/acpi/sleep.c:

```
1. SCI Validity Check
   -> If SCI IRQ invalid, return pm_wakeup_pending()
2. IRQD_WAKEUP_ARMED Check
   -> If SCI not armed, wakeup came from different source -> resume
3. Fixed Event Status (power/sleep button, RTC)
   -> acpi_any_fixed_event_status_set() -> resume if set
4. Custom Wakeup Handlers  <-- OUR HANDLER RUNS HERE
   -> acpi_check_wakeup_handlers() iterates registered list
   -> Return true = promote to full resume
5. EC GPE Dispatch          <-- GPE 0x52 STATUS CHECKED HERE
   -> acpi_ec_dispatch_gpe()
   -> When first_ec==NULL: acpi_any_gpe_status_set(U32_MAX) checks ALL GPEs!
6. Event Drain + Rearm Loop
   -> Clear wakeup, rearm SCI, loop back
```

**Key insight**: Our wakeup handler (step 4) runs BEFORE `acpi_ec_dispatch_gpe()` (step 5). By clearing GPE 0x52 status in our handler, `acpi_any_gpe_status_set()` never sees it, preventing spurious full-resume promotion.

**`acpi_ec_dispatch_gpe()` with no EC**: When `first_ec==NULL` (our Surface), it calls `acpi_any_gpe_status_set(U32_MAX)` which checks ALL GPEs indiscriminately. This is why GPE 0x52 phantom edges cause death sleep.

### LPS0 Device Ops Infrastructure

```c
struct acpi_s2idle_dev_ops {
    struct list_head list_node;
    void (*prepare)(void);   // Before platform deep sleep
    void (*check)(void);     // During wake validation
    void (*restore)(void);   // On resume
};
```

Registration via `acpi_register_lps0_dev()` / `acpi_unregister_lps0_dev()`. Callbacks called at:
- **begin**: Load device constraints
- **prepare_late**: All registered ->prepare() callbacks
- **check**: All registered ->check() callbacks during wake validation
- **restore_early**: All registered ->restore() callbacks

LPS0 DSM UUIDs:
- Intel: `c4eb40a0-6cd2-11e2-bcfd-0800200c9a66`
- Microsoft: `11e00d56-ce64-47ce-837b-1f898f9aa461`
- AMD: `e3f32452-febc-43ce-9039-932122d37721`

### Wakeup Handler Registration

```c
int acpi_register_wakeup_handler(int wake_irq,
    bool (*wakeup)(void *context), void *context);
```

Handler must share the SCI IRQ. If `wake_irq != acpi_sci_irq`, registration is silently skipped.

### Design Implications

1. **pinctrl-intel doesn't help us during s2idle**: Its save/restore only fires on suspend_noirq/resume_noirq, not during continuous C-state VNN cycling. We MUST handle PADCFG corruption ourselves.
2. **Our wakeup handler position is perfect**: Step 4 (before GPE dispatch at step 5) lets us clear GPE 0x52 before it's checked.
3. **LPS0 ops give us the s2idle loop hooks**: prepare/check/restore run inside the s2idle idle loop, exactly where we need to poll RXSTATE.
4. **No EC means ALL GPEs checked**: The `acpi_any_gpe_status_set(U32_MAX)` path when `first_ec==NULL` is the root cause of phantom wake promotion.

---

## Reference: Key Addresses and Constants

```
Pin 213 PADCFG0:     0xfd6a09a0
Pin 213 PADCFG1:     0xfd6a09a4
GPE:                  0x52
SCI IRQ:              9
PADCFG0 RXINV:        bit 23 (0x00800000)
PADCFG0 GPIROUTSCI:   bit 19 (0x00080000)
PADCFG0 GPIORXDIS:    bit 8  (0x00000100)
PADCFG0 GPIORXSTATE:  bit 1  (0x00000002)
PMODE mask:           0x00003C00 (bits 13:10)
```
