#!/bin/bash
set -e

MODULE_NAME="surface_s2idle_fix"
MODULES_LOAD_CONF="/etc/modules-load.d/surface-s2idle-fix.conf"

echo "Surface Laptop 5 s2idle fix - installer"
echo "========================================="
echo

# Check if running on a Surface Laptop 5
if ! grep -qi "Surface Laptop 5" /sys/class/dmi/id/product_name 2>/dev/null; then
    echo "WARNING: This doesn't appear to be a Surface Laptop 5."
    echo "Detected: $(cat /sys/class/dmi/id/product_name 2>/dev/null || echo 'unknown')"
    read -p "Continue anyway? [y/N] " -n 1 -r
    echo
    [[ $REPLY =~ ^[Yy]$ ]] || exit 1
fi

# Check for kernel headers
KDIR="/lib/modules/$(uname -r)/build"
if [ ! -d "$KDIR" ]; then
    echo "ERROR: Kernel headers not found at $KDIR"
    echo "Install them with:"
    echo "  sudo apt install linux-headers-$(uname -r)"
    exit 1
fi

# Build
echo "[1/4] Building module..."
make clean 2>/dev/null || true
make
echo

# Install
echo "[2/4] Installing module..."
sudo make install
echo

# Load
echo "[3/4] Loading module..."
if lsmod | grep -q "$MODULE_NAME"; then
    echo "Module already loaded, reloading..."
    sudo modprobe -r "$MODULE_NAME"
fi
sudo modprobe "$MODULE_NAME"
echo

# Persist across reboots
echo "[4/4] Enabling on boot..."
echo "$MODULE_NAME" | sudo tee "$MODULES_LOAD_CONF" > /dev/null
echo

# Verify
if lsmod | grep -q "$MODULE_NAME"; then
    echo "SUCCESS! Module is loaded and will persist across reboots."
    echo
    echo "Check status with: dmesg | grep surface_s2idle_fix"
else
    echo "ERROR: Module failed to load. Check dmesg for details."
    exit 1
fi
