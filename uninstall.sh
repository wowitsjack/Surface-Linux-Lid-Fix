#!/bin/bash
set -e

MODULE_NAME="surface_s2idle_fix"
MODULES_LOAD_CONF="/etc/modules-load.d/surface-s2idle-fix.conf"

echo "Surface Laptop 5 s2idle fix - uninstaller"
echo "==========================================="
echo

# Unload if running
if lsmod | grep -q "$MODULE_NAME"; then
    echo "[1/3] Unloading module..."
    sudo modprobe -r "$MODULE_NAME"
else
    echo "[1/3] Module not loaded, skipping."
fi

# Remove installed .ko files
echo "[2/3] Removing installed module files..."
for dir in /lib/modules/*/updates; do
    if [ -f "$dir/${MODULE_NAME}.ko" ]; then
        sudo rm -f "$dir/${MODULE_NAME}.ko"
        echo "  Removed $dir/${MODULE_NAME}.ko"
    fi
    if [ -f "$dir/${MODULE_NAME}.ko.zst" ]; then
        sudo rm -f "$dir/${MODULE_NAME}.ko.zst"
        echo "  Removed $dir/${MODULE_NAME}.ko.zst"
    fi
    if [ -f "$dir/${MODULE_NAME}.ko.xz" ]; then
        sudo rm -f "$dir/${MODULE_NAME}.ko.xz"
        echo "  Removed $dir/${MODULE_NAME}.ko.xz"
    fi
done
sudo depmod -a

# Remove boot persistence
echo "[3/3] Removing boot persistence..."
if [ -f "$MODULES_LOAD_CONF" ]; then
    sudo rm -f "$MODULES_LOAD_CONF"
    echo "  Removed $MODULES_LOAD_CONF"
else
    echo "  No boot config found, skipping."
fi

echo
echo "Uninstalled. The module will no longer load on boot."
echo "You can safely delete this directory if you want."
