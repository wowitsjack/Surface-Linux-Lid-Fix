#!/bin/bash
#
# Fix audio crackling on Surface Laptop 5 (and likely other Intel Alder Lake devices)
#
# Three causes of audio crackling on these machines:
#   1. HDA codec power management suspends the codec after 1s of silence,
#      causing pops/crackles on resume
#   2. PipeWire's default ALSA headroom of 0 leaves no buffer margin,
#      any scheduling delay = instant underrun = crackle
#   3. Deep C-states (C8/C10) have 200-230us wakeup latency, which can
#      cause the CPU to miss audio IRQ deadlines
#
set -e

echo "Surface audio crackling fix"
echo "==========================="
echo

# ─── Fix 1: HDA power save ────────────────────────────────────────────

echo "[1/4] Disabling HDA codec power management..."

MODPROBE_CONF="/etc/modprobe.d/audio-no-powersave.conf"

# Immediate effect
if [ -f /sys/module/snd_hda_intel/parameters/power_save ]; then
    echo 0 | sudo tee /sys/module/snd_hda_intel/parameters/power_save > /dev/null
    echo "  power_save = 0 (immediate)"
fi
if [ -f /sys/module/snd_hda_intel/parameters/power_save_controller ]; then
    echo N | sudo tee /sys/module/snd_hda_intel/parameters/power_save_controller > /dev/null
    echo "  power_save_controller = N (immediate)"
fi

# Persistent
echo "options snd_hda_intel power_save=0 power_save_controller=N" | sudo tee "$MODPROBE_CONF" > /dev/null
echo "  Saved to $MODPROBE_CONF"
echo

# ─── Fix 2: PipeWire headroom ─────────────────────────────────────────

echo "[2/4] Adding PipeWire ALSA headroom..."

WP_CONF_DIR="$HOME/.config/wireplumber/wireplumber.conf.d"
WP_CONF="$WP_CONF_DIR/51-alsa-headroom.conf"

mkdir -p "$WP_CONF_DIR"
cat > "$WP_CONF" << 'EOF'
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
echo "  Saved to $WP_CONF"
echo

# ─── Fix 3: Disable deep C-states ─────────────────────────────────────

echo "[3/4] Disabling deep C-states (C8, C10)..."

# Immediate effect
for cpu in /sys/devices/system/cpu/cpu*/cpuidle/state3/disable; do
    [ -f "$cpu" ] && echo 1 | sudo tee "$cpu" > /dev/null
done
for cpu in /sys/devices/system/cpu/cpu*/cpuidle/state4/disable; do
    [ -f "$cpu" ] && echo 1 | sudo tee "$cpu" > /dev/null
done
echo "  C8 and C10 disabled (immediate)"

# Persistent via systemd service
CSTATE_SERVICE="/etc/systemd/system/disable-deep-cstates.service"
sudo tee "$CSTATE_SERVICE" > /dev/null << 'EOF'
[Unit]
Description=Disable C8 and C10 to prevent audio glitches
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/bin/bash -c "for cpu in /sys/devices/system/cpu/cpu*/cpuidle/state3/disable; do echo 1 > $cpu; done; for cpu in /sys/devices/system/cpu/cpu*/cpuidle/state4/disable; do echo 1 > $cpu; done"
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable disable-deep-cstates.service 2>/dev/null
echo "  Persistent service enabled"
echo

# ─── Restart PipeWire ──────────────────────────────────────────────────

echo "[4/4] Restarting PipeWire..."
systemctl --user restart pipewire pipewire-pulse wireplumber 2>/dev/null || true
sleep 1
echo "  PipeWire restarted"
echo

# ─── Verify ────────────────────────────────────────────────────────────

echo "Done! Current state:"
echo "  power_save = $(cat /sys/module/snd_hda_intel/parameters/power_save 2>/dev/null || echo 'N/A')"
echo "  power_save_controller = $(cat /sys/module/snd_hda_intel/parameters/power_save_controller 2>/dev/null || echo 'N/A')"

HEADROOM=$(pw-dump 2>/dev/null | python3 -c "
import json, sys
try:
    data = json.load(sys.stdin)
    for obj in data:
        props = obj.get('info', {}).get('props', {})
        if 'alsa_output' in props.get('node.name', ''):
            print(props.get('api.alsa.headroom', 'not set'))
            break
except: print('unknown')
" 2>/dev/null || echo "unknown")
echo "  ALSA headroom = $HEADROOM"

C8=$(cat /sys/devices/system/cpu/cpu0/cpuidle/state3/disable 2>/dev/null || echo "?")
C10=$(cat /sys/devices/system/cpu/cpu0/cpuidle/state4/disable 2>/dev/null || echo "?")
echo "  C8 disabled = $C8"
echo "  C10 disabled = $C10"
echo
echo "Audio crackling should be fixed. If it persists, try logging out and back in."
