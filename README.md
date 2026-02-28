# Surface Series s2idle Fix

A kernel module that fixes the "death sleep" bug on the Microsoft Surface series, where the laptop enters s2idle suspend and never wakes up again, requiring a hard reset.

Also includes a bonus script to fix audio crackling common on Intel Alder Lake devices running Linux.

## Symptoms

- Closing the lid suspends the laptop, but it **never wakes up**
- Power button, keyboard, trackpad do nothing
- Only a hard reset (hold power 10+ seconds) recovers the machine
- `dmesg` may show `GPE 0x52` spam or `pm_system_cancel_wakeup` after resume
- Happens specifically with s2idle (`cat /sys/power/mem_sleep` shows `[s2idle]`)

## Quick Install

```bash
git clone https://github.com/wowitsjack/Surface-Linux-Lid-Fix.git
cd Surface-Linux-Lid-Fix
sudo ./install.sh
```

That's it. The module builds against your running kernel, installs itself, and persists across reboots.

## What It Does

Intel's INTC1055 GPIO pinctrl power-gates during s2idle, which corrupts the PADCFG0 register for pin 213 (the lid sensor). The RXINV bit flips, causing a phantom edge that fires a spurious SCI on GPE 0x52. The ACPI handler calls `pm_system_cancel_wakeup()`, which permanently poisons the wakeup framework. The system never wakes.

This module:

- **Saves and restores PADCFG0/PADCFG1** at every suspend stage to fix the corruption before anything sees it
- **Masks GPE 0x52** during suspend, only unmasking it inside the s2idle loop for genuine lid-open detection
- **Registers a wakeup handler** that runs before `acpi_ec_dispatch_gpe()` to fix corruption and clear GPE status before it can promote a spurious full resume
- **LPS0 check callback** polls RXSTATE for genuine lid-open events, only calling `pm_system_wakeup()` when the lid is actually open
- **Post-resume failsafe** detects spurious wakes (lid still closed, no power button) and re-suspends automatically, with exponential backoff to prevent rapid sleep-wake storms
- **SW_LID input device** emits lid open/close events so your desktop environment handles suspend policy natively (the module does NOT force suspend on lid close)

## Uninstall

```bash
sudo ./uninstall.sh
```

## Bonus: Fix Audio Crackling

Intel Alder Lake laptops (including Surface Laptop 5) often have audio crackling/popping under Linux, caused by aggressive HDA codec power management, zero PipeWire buffer headroom, and deep C-states (C8/C10) missing audio IRQ deadlines.

```bash
./utils/fix-audio-crackling.sh
```

This script:
1. Disables HDA codec power save (`power_save=0`, `power_save_controller=N`)
2. Adds PipeWire ALSA headroom (1024 samples via WirePlumber rule)
3. Disables C8 and C10 deep sleep states (keeps C1E and C6 for battery life)
4. Restarts PipeWire

All changes are persistent across reboots.

## Manual Installation

If you prefer to do everything by hand:

```bash
# 1. Install kernel headers
sudo apt install linux-headers-$(uname -r)

# 2. Build the module
make

# 3. Copy to updates directory
sudo cp surface_s2idle_fix.ko /lib/modules/$(uname -r)/updates/
sudo depmod -a

# 4. Load it
sudo modprobe surface_s2idle_fix

# 5. Make it load on boot
echo "surface_s2idle_fix" | sudo tee /etc/modules-load.d/surface-s2idle-fix.conf

# 6. Verify
dmesg | grep surface_s2idle_fix
```

## Manual Audio Fix

```bash
# HDA power save off
echo 0 | sudo tee /sys/module/snd_hda_intel/parameters/power_save
echo N | sudo tee /sys/module/snd_hda_intel/parameters/power_save_controller
echo "options snd_hda_intel power_save=0 power_save_controller=N" | sudo tee /etc/modprobe.d/audio-no-powersave.conf

# PipeWire headroom
mkdir -p ~/.config/wireplumber/wireplumber.conf.d
cat > ~/.config/wireplumber/wireplumber.conf.d/51-alsa-headroom.conf << 'EOF'
monitor.alsa.rules = [
  {
    matches = [
      { node.name = "~alsa_output.*" }
    ]
    actions = {
      update-props = {
        api.alsa.headroom = 1024
      }
    }
  }
]
EOF

# Disable deep C-states (C8 = state3, C10 = state4)
for cpu in /sys/devices/system/cpu/cpu*/cpuidle/state3/disable; do echo 1 | sudo tee $cpu > /dev/null; done
for cpu in /sys/devices/system/cpu/cpu*/cpuidle/state4/disable; do echo 1 | sudo tee $cpu > /dev/null; done

# Restart PipeWire
systemctl --user restart pipewire pipewire-pulse wireplumber
```

## Tested On

- Microsoft Surface Laptop 5 (Intel i7-1265U / i5-1245U)
- Kernel 6.18.x (linux-surface)
- Ubuntu 25.10, Ubuntu 24.04
- GNOME, KDE Plasma

## Related

- [linux-surface PR #2011](https://github.com/linux-surface/linux-surface/pull/2011) - upstream patch submission
- [linux-surface](https://github.com/linux-surface/linux-surface) - Linux on Surface devices

## License

GPL-2.0 - see [LICENSE](LICENSE)
