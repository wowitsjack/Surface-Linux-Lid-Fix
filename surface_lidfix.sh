#!/bin/bash
#
# Surface Lid Fix
# (For Surface laptops, and potentially other devices)
#
# Fixes the "lid won't suspend after first resume" bug
# and provides tools to monitor lid state and reload GNOME session.
#
# Usage: ./surface_lidfix.sh
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
WHITE='\033[1;37m'
NC='\033[0m' # No Color

# Script info
SCRIPT_TITLE="Surface Lid Fix"
VERSION="1.2" # Version updated: gsd-media-keys is never killed.
INSTALL_DIR="/opt/surface-lidfix"
SERVICE_NAME="surface-lidfix"
CURRENT_DIR="$(pwd)"

# ASCII Art Banner
show_banner() {
    echo -e "${CYAN}"
    cat << EOF
    ╔═══════════════════════════════════════════════════════════╗
    ║                                                           ║
    ║             ${SCRIPT_TITLE} v${VERSION}                   ║
    ║                                                           ║
    ║             Lid Suspend Fix for Surface Linux             ║
    ║                                                           ║
    ║          ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~    ║
    ║                                                           ║
    ╚═══════════════════════════════════════════════════════════╝
EOF
    echo -e "${NC}"
}

step_msg() {
    echo -e "${BLUE}==>${NC} $1"
}

success_msg() {
    echo -e "${GREEN}[OK]${NC} $1"
}

error_msg() {
    echo -e "${RED}[ERROR]${NC} $1"
}

warning_msg() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

info_msg() {
    echo -e "${CYAN}[INFO]${NC} $1"
}

# Check if running as root
check_root() {
    if [[ $EUID -eq 0 ]]; then
        error_msg "This script should not be run as root. Run as your regular user."
        error_msg "Sudo privileges will be requested when necessary."
        exit 1
    fi
}

# Get sudo access early and keep it alive
setup_sudo() {
    step_msg "Requesting sudo privileges..."
    if ! sudo -v; then
        error_msg "Sudo privileges not granted. Exiting."
        exit 1
    fi

    # Keep sudo alive in background
    while true; do sudo -n true; sleep 60; kill -0 "$$" || exit; done 2>/dev/null &

    success_msg "Sudo privileges acquired and will be kept alive."
}

# Check if this is a Surface device
check_surface() {
    info_msg "Detecting device type..."

    local is_surface=false
    local product_info="Unknown"

    if [ -f "/sys/class/dmi/id/product_name" ]; then
        product_info=$(cat /sys/class/dmi/id/product_name 2>/dev/null)
        local product_name_lower=$(echo "$product_info" | tr '[:upper:]' '[:lower:]')
        if [[ "$product_name_lower" == *"surface"* ]]; then
            is_surface=true
            success_msg "Detected Surface device: $product_info"
        fi
    fi

    if ! $is_surface; then
        if sudo dmesg 2>/dev/null | grep -qi surface; then
            is_surface=true
            success_msg "Surface device detected in kernel messages."
        fi
    fi

    if ! $is_surface; then
        if lsmod | grep -qi surface; then
            is_surface=true
            success_msg "Surface kernel modules detected."
        fi
    fi

    if ! $is_surface; then
        warning_msg "This does not appear to be a Surface device."
        info_msg "Detected system: $product_info"
        echo -e "${YELLOW}This tool is primarily for Surface Linux, but may work on other laptops.${NC}"
        echo ""
        read -p "Continue anyway? (y/N): " choice
        if [[ ! "$choice" =~ ^[Yy]$ ]]; then
            info_msg "Exiting."
            exit 0
        fi
    fi
}

# Install the service files
install_service() {
    step_msg "Installing ${SCRIPT_TITLE} service..."

    step_msg "Creating installation directory: $INSTALL_DIR"
    sudo mkdir -p "$INSTALL_DIR"

    step_msg "Creating Python scripts in $INSTALL_DIR..."

    # Create lid_suspend_monitor.py
    sudo tee "$INSTALL_DIR/lid_suspend_monitor.py" > /dev/null << 'EOF_LID_MONITOR'
#!/usr/bin/env python3
"""
Lid Suspend Monitor - A script to force suspend when lid is closed
"""

import time
import subprocess
import sys
import os

def get_lid_state():
    """Check if the lid is open or closed"""
    try:
        with open('/proc/acpi/button/lid/LID0/state', 'r') as f:
            state = f.read().strip()
            return 'closed' in state.lower()
    except FileNotFoundError:
        try:
            with open('/proc/acpi/button/lid/LID/state', 'r') as f:
                state = f.read().strip()
                return 'closed' in state.lower()
        except FileNotFoundError:
            print("LID_MONITOR: Error - Could not find lid state file!", flush=True)
            return None # Indicate error

def force_suspend():
    """Force the system to suspend"""
    try:
        print("LID_MONITOR: Forcing system suspend...", flush=True)
        # Use -i flag to ignore inhibitors that might be blocking suspend
        # This command requires root privileges or specific sudo configuration.
        # If run as a user service without sudo, this will likely fail.
        subprocess.run(['systemctl', 'suspend', '-i'], check=True)
        return True
    except subprocess.CalledProcessError as e:
        print(f"LID_MONITOR: Error - Failed to suspend: {e}", flush=True)
        return False
    except FileNotFoundError:
        print("LID_MONITOR: Error - systemctl command not found.", flush=True)
        return False


def main():
    print("LID_MONITOR: Lid Suspend Monitor Started.", flush=True)
    print("LID_MONITOR: Monitoring lid state. Close lid to test suspend action.", flush=True)

    lid_was_open = True
    consecutive_closed_checks = 0
    required_closed_checks = 2 # Require 2 consecutive checks (1 second total)

    while True:
        try:
            lid_is_closed = get_lid_state()

            if lid_is_closed is None: # Error reading lid state
                time.sleep(5) # Wait longer before retrying if file not found
                continue

            if lid_is_closed:
                if lid_was_open:
                    print("LID_MONITOR: Lid closed detected. Starting check countdown...", flush=True)
                    consecutive_closed_checks = 1
                    lid_was_open = False
                else: # Lid was already considered closed
                    consecutive_closed_checks += 1
                    print(f"LID_MONITOR: Lid remains closed. Check {consecutive_closed_checks}/{required_closed_checks}.", flush=True)

                if consecutive_closed_checks >= required_closed_checks:
                    print("LID_MONITOR: Lid confirmed closed. Attempting suspend...", flush=True)
                    if force_suspend():
                        print("LID_MONITOR: Suspend command issued. Monitor will pause until resume.", flush=True)
                        # After suspend, the script might be restarted by systemd, or continue.
                        # Assume systemd restarts it on resume if it's a service.
                        # For now, just reset state for next cycle.
                        time.sleep(10) # Give system time to actually suspend
                        lid_was_open = True # Assume lid will be open on resume
                        consecutive_closed_checks = 0
                    else:
                        print("LID_MONITOR: Suspend attempt failed. Will retry on next lid close cycle.", flush=True)
                        # Reset to re-detect lid close event properly
                        lid_was_open = True
                        consecutive_closed_checks = 0
                        time.sleep(5) # Wait before retrying
            else: # Lid is open
                if not lid_was_open:
                    print("LID_MONITOR: Lid opened.", flush=True)
                lid_was_open = True
                consecutive_closed_checks = 0

            time.sleep(0.5) # Polling interval

        except KeyboardInterrupt:
            print("\nLID_MONITOR: Monitor stopped by user.", flush=True)
            break
        except Exception as e:
            print(f"LID_MONITOR: Error in main loop: {e}", flush=True)
            # traceback.print_exc() # For more detailed debugging
            time.sleep(5) # Wait before continuing after an unexpected error

if __name__ == "__main__":
    main()
EOF_LID_MONITOR

    # Create post_resume_lid_fix.py
    sudo tee "$INSTALL_DIR/post_resume_lid_fix.py" > /dev/null << 'EOF_POST_RESUME_FIX'
#!/usr/bin/env python3
"""
Post-Resume Lid Fix Script
This script attempts to fix lid detection after resume from suspend.
It will NOT terminate gsd-media-keys.
"""

import subprocess
import time
import sys
import os

def log_msg(message):
    print(f"POST_RESUME_FIX: {message}", flush=True)

def run_cmd(command_list, check=False, capture=True, as_root=True):
    cmd_to_run = command_list
    if as_root and os.geteuid() != 0:
        cmd_to_run = ['sudo'] + command_list
    
    log_msg(f"Executing: {' '.join(cmd_to_run)}")
    try:
        result = subprocess.run(cmd_to_run, capture_output=capture, text=True, check=check, timeout=15)
        if capture:
            if result.stdout:
                log_msg(f"Stdout: {result.stdout.strip()}")
            if result.stderr:
                log_msg(f"Stderr: {result.stderr.strip()}")
        return result
    except subprocess.CalledProcessError as e:
        log_msg(f"Error executing command: {e}")
        if capture:
            log_msg(f"Stdout: {e.stdout.strip() if e.stdout else 'N/A'}")
            log_msg(f"Stderr: {e.stderr.strip() if e.stderr else 'N/A'}")
    except subprocess.TimeoutExpired:
        log_msg(f"Timeout executing: {' '.join(command_list)}")
    except FileNotFoundError:
        log_msg(f"Command not found: {command_list[0]}")
    except Exception as e_gen:
        log_msg(f"Unexpected error running command {' '.join(command_list)}: {e_gen}")
    return None


def check_for_blocking_inhibitors():
    log_msg("Checking for systemd inhibitors...")
    result = run_cmd(['systemd-inhibit', '--list', '--no-pager'], as_root=False) # systemd-inhibit list doesn't need root
    if result and result.returncode == 0:
        lines = result.stdout.strip().split('\n')
        blocking_inhibitors = []
        for line in lines[1:]:  # Skip header
            # Looking for 'gsd-media-keys' and 'gsd-power' specifically related to 'sleep' and 'block'
            if ('gsd-media-keys' in line or 'gsd-power' in line) and 'sleep' in line and 'block' in line:
                blocking_inhibitors.append(line.strip())
        
        if blocking_inhibitors:
            log_msg(f"Found {len(blocking_inhibitors)} potentially problematic gsd-media-keys/gsd-power sleep inhibitors:")
            for inhibitor in blocking_inhibitors:
                log_msg(f"  - {inhibitor}")
        return blocking_inhibitors
    return []

def kill_problematic_gsd_services():
    """Kill problematic GNOME services that might interfere.
    IMPORTANT: gsd-media-keys is intentionally NOT targeted by this function."""
    services_to_kill = ['gsd-power'] # Only gsd-power is targeted if necessary
    killed_any = False
    
    if not services_to_kill:
        log_msg("No gsd services are currently targeted for termination.")
        return

    for service in services_to_kill:
        log_msg(f"Attempting to terminate process: {service}")
        result = run_cmd(['pkill', '-TERM', service], check=False, capture=False)
        if result and result.returncode == 0:
            log_msg(f"{service} processes sent SIGTERM.")
            killed_any = True
        else:
            check_running = run_cmd(['pgrep', service], capture=True, as_root=False)
            if not (check_running and check_running.stdout.strip()):
                 log_msg(f"{service} was not running or no processes found.")
            else:
                log_msg(f"Could not terminate {service} with SIGTERM or it was not found (pkill returned non-zero).")
    if killed_any:
        log_msg("Waiting briefly for services to terminate...")
        time.sleep(2)


def reload_acpi_modules():
    """Reload ACPI modules to fix hardware detection."""
    modules = ['button'] 
    log_msg("Reloading ACPI kernel modules...")
    for module in modules:
        full_module_name = f'acpi_{module}'
        log_msg(f"Attempting to reload module: {full_module_name}")
        run_cmd(['modprobe', '-r', full_module_name], check=False) 
        time.sleep(0.5)
        run_cmd(['modprobe', full_module_name], check=True)

def check_lid_state():
    """Check current ACPI lid state."""
    log_msg("Checking current ACPI lid state...")
    path1 = '/proc/acpi/button/lid/LID0/state'
    path2 = '/proc/acpi/button/lid/LID/state'
    lid_path_to_use = None

    if os.path.exists(path1):
        lid_path_to_use = path1
    elif os.path.exists(path2):
        lid_path_to_use = path2
    
    if lid_path_to_use:
        try:
            with open(lid_path_to_use, 'r') as f:
                state = f.read().strip()
                log_msg(f"Current lid state from {lid_path_to_use}: {state}")
                return True
        except Exception as e:
            log_msg(f"Error reading lid state from {lid_path_to_use}: {e}")
            return False
    else:
        log_msg("Error: Could not find standard ACPI lid state file.")
        return False

def main():
    log_msg("Post-Resume Lid Fix script started.")
    if os.geteuid() != 0:
        log_msg("Error: This script must be run as root.")
        sys.exit(1)

    log_msg("Initial system state check:")
    check_lid_state()
    time.sleep(1)

    initial_inhibitors = check_for_blocking_inhibitors()
    gsd_media_keys_is_inhibiting = False
    gsd_power_is_inhibiting = False

    if initial_inhibitors:
        log_msg("Analyzing initial inhibitors...")
        for inhibitor_line in initial_inhibitors:
            if 'gsd-media-keys' in inhibitor_line:
                gsd_media_keys_is_inhibiting = True
                log_msg("  INFO: 'gsd-media-keys' is listed as a sleep inhibitor.")
                log_msg("        This service will NOT be terminated by this script.")
            if 'gsd-power' in inhibitor_line:
                gsd_power_is_inhibiting = True
                log_msg("  INFO: 'gsd-power' is listed as a sleep inhibitor.")
        
        if gsd_power_is_inhibiting:
            log_msg("Attempting to resolve 'gsd-power' inhibition as it was found in inhibitors...")
            kill_problematic_gsd_services() # This function now only targets 'gsd-power'
            # A short sleep is already in kill_problematic_gsd_services if it acts.
        elif gsd_media_keys_is_inhibiting and not gsd_power_is_inhibiting:
             log_msg("'gsd-media-keys' is inhibiting but will not be touched. 'gsd-power' is not inhibiting or not found.")
        elif initial_inhibitors: # Other inhibitors found, but not the ones we specifically check for action
             log_msg("Other inhibitors might be present, but 'gsd-power' (targeted for action) was not found among them.")

    else:
        log_msg("No gsd-media-keys or gsd-power sleep inhibitors found initially.")

    log_msg("Applying hardware-level fixes...")
    reload_acpi_modules()
    time.sleep(1)

    log_msg("Post-resume fixes applied.")
    log_msg("Final system state check:")
    check_lid_state()

    final_inhibitors = check_for_blocking_inhibitors()
    if final_inhibitors:
        log_msg(f"Warning: Still found {len(final_inhibitors)} gsd-media-keys/gsd-power sleep inhibitors after fixes:")
        for inhibitor in final_inhibitors:
            log_msg(f"  - {inhibitor}")
            if 'gsd-media-keys' in inhibitor:
                log_msg("    (Note: 'gsd-media-keys' was not targeted for termination by this script.)")
    else:
        log_msg("No gsd-media-keys or gsd-power sleep inhibitors found after fixes.")

    log_msg("Post-Resume Lid Fix script finished.")

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log_msg("\nFix script cancelled by user.")
    except Exception as e:
        log_msg(f"\nUnexpected error in Post-Resume Lid Fix script: {e}")
        sys.exit(1)
EOF_POST_RESUME_FIX

    # Create reload_gnome_session.py
    sudo tee "$INSTALL_DIR/reload_gnome_session.py" > /dev/null << 'EOF_RELOAD_GNOME'
#!/usr/bin/env python3
"""
GNOME Session Reloader
This script restarts GNOME Shell to address potential UI issues.
"""

import subprocess
import time
import sys
import os

def log_msg(message):
    print(f"RELOAD_GNOME: {message}", flush=True)

def restart_gnome_shell_gdbus():
    """Restart GNOME Shell using gdbus (preferred method)."""
    log_msg("Attempting to restart GNOME Shell via gdbus...")
    try:
        if not os.getenv('DISPLAY') or not os.getenv('XDG_RUNTIME_DIR'):
            log_msg("Warning: DISPLAY or XDG_RUNTIME_DIR not set. gdbus call might fail.")
            if not os.getenv('XDG_RUNTIME_DIR'):
                uid = os.geteuid()
                runtime_dir_path = f"/run/user/{uid}"
                if os.path.isdir(runtime_dir_path):
                    os.environ['XDG_RUNTIME_DIR'] = runtime_dir_path
                    log_msg(f"Set XDG_RUNTIME_DIR to {runtime_dir_path}")

        command = [
            'gdbus', 'call', '--session',
            '--dest', 'org.gnome.Shell',
            '--object-path', '/org/gnome/Shell',
            '--method', 'org.gnome.Shell.Eval',
            'global.reexec_self()'
        ]
        result = subprocess.run(command, capture_output=True, text=True, timeout=5)

        if result.returncode == 0:
            log_msg("GNOME Shell restart initiated (gdbus returned 0).")
            return True
        elif "NoReply" in result.stderr or "timeout" in result.stderr.lower():
            log_msg("GNOME Shell restart likely initiated (gdbus timed out or got no reply - this is often normal).")
            return True 
        else:
            log_msg(f"gdbus method failed. Return code: {result.returncode}")
            log_msg(f"Stderr: {result.stderr.strip()}")
            return False

    except subprocess.TimeoutExpired:
        log_msg("gdbus command timed out (this is often normal for shell restart). Assuming success.")
        return True
    except FileNotFoundError:
        log_msg("Error: gdbus command not found.")
        return False
    except Exception as e:
        log_msg(f"Error restarting GNOME Shell with gdbus: {e}")
        return False

def alternative_restart_killall():
    """Alternative method using killall -HUP gnome-shell."""
    log_msg("Attempting alternative GNOME Shell restart via killall -HUP...")
    try:
        subprocess.run(['killall', '-HUP', 'gnome-shell'], capture_output=True, check=True, text=True)
        log_msg("Signal HUP sent to gnome-shell. It should restart if running.")
        return True
    except subprocess.CalledProcessError as e:
        if "no process found" in e.stderr.lower():
            log_msg("killall: gnome-shell process not found.")
        else:
            log_msg(f"killall method failed: {e.stderr.strip()}")
        return False
    except FileNotFoundError:
        log_msg("Error: killall command not found.")
        return False
    except Exception as e_gen:
        log_msg(f"Unexpected error with killall method: {e_gen}")
        return False

def check_gnome_session_vars():
    """Check environment variables to guess if in a GNOME session."""
    desktop = os.environ.get('XDG_CURRENT_DESKTOP', '').lower()
    session = os.environ.get('DESKTOP_SESSION', '').lower()
    is_gnome = 'gnome' in desktop or 'gnome' in session or 'gnome-shell' in session
    
    if is_gnome:
        log_msg(f"Detected GNOME-related session (XDG_CURRENT_DESKTOP='{desktop}', DESKTOP_SESSION='{session}').")
        return True
    else:
        log_msg(f"Non-GNOME or undetermined session (XDG_CURRENT_DESKTOP='{desktop}', DESKTOP_SESSION='{session}').")
        log_msg("This script is primarily for GNOME. Proceeding with caution.")
        return False

def main():
    log_msg("GNOME Session Reloader script started.")
    log_msg("Attempting to refresh the GNOME Shell session.")

    if not check_gnome_session_vars():
        log_msg("Warning: Not definitively a GNOME session. Results may vary.")

    log_msg("Checking session status...")
    time.sleep(0.5) 

    if restart_gnome_shell_gdbus():
        log_msg("GNOME Shell reload (via gdbus) initiated. Your desktop should refresh shortly.")
    else:
        log_msg("Primary gdbus method for GNOME Shell restart did not confirm success. Trying alternative (killall -HUP)...")
        if alternative_restart_killall():
            log_msg("Alternative GNOME Shell restart (killall -HUP) signal sent.")
        else:
            log_msg("Both gdbus and killall methods for GNOME Shell restart failed or could not confirm success.")
            log_msg("No further automatic actions taken to restart GNOME Shell.")

    log_msg("GNOME Session Reloader script finished.")
    log_msg("If UI issues persist, consider logging out and back in, or restarting the display manager (e.g., sudo systemctl restart gdm).")


if __name__ == "__main__":
    if os.geteuid() == 0:
        log_msg("Warning: This script is typically run as a regular user, not root, to interact with the user's GNOME session.")
    
    try:
        main()
    except KeyboardInterrupt:
        log_msg("\nReload script cancelled by user.")
    except Exception as e:
        log_msg(f"\nUnexpected error in GNOME Session Reloader: {e}")
        sys.exit(1)
EOF_RELOAD_GNOME

    sudo chmod +x "$INSTALL_DIR"/*.py
    success_msg "Python scripts created in $INSTALL_DIR."

    step_msg "Creating systemd service: ${SERVICE_NAME}.service (Lid Monitor)"
    sudo tee "/etc/systemd/system/${SERVICE_NAME}.service" > /dev/null << EOF
[Unit]
Description=${SCRIPT_TITLE} - Lid Suspend Monitor
After=graphical-session.target
Wants=graphical-session.target

[Service]
Type=simple
User=$USER
Environment=DISPLAY=:0
Environment=XDG_RUNTIME_DIR=/run/user/$(id -u $USER)
ExecStart=/usr/bin/python3 $INSTALL_DIR/lid_suspend_monitor.py
Restart=always
RestartSec=5

[Install]
WantedBy=graphical-session.target
EOF

    step_msg "Creating post-resume service: ${SERVICE_NAME}-resume.service (Post Resume Fix)"
    sudo tee "/etc/systemd/system/${SERVICE_NAME}-resume.service" > /dev/null << EOF
[Unit]
Description=${SCRIPT_TITLE} - Post Resume Fix
After=suspend.target sleep.target hybrid-sleep.target hibernate.target

[Service]
Type=oneshot
ExecStart=/usr/bin/python3 $INSTALL_DIR/post_resume_lid_fix.py
User=root

[Install]
WantedBy=suspend.target sleep.target hybrid-sleep.target hibernate.target
EOF

    step_msg "Creating helper commands..."
    sudo tee "/usr/local/bin/fix-lid" > /dev/null << EOF
#!/bin/bash
# Quick lid fix command
# Part of ${SCRIPT_TITLE}
echo "Manually running Post-Resume Lid Fix script..."
sudo /usr/bin/python3 $INSTALL_DIR/post_resume_lid_fix.py
EOF

    sudo tee "/usr/local/bin/fix-session" > /dev/null << EOF
#!/bin/bash
# Quick session fix command
# Part of ${SCRIPT_TITLE}
echo "Manually running GNOME Session Reloader script..."
/usr/bin/python3 $INSTALL_DIR/reload_gnome_session.py
EOF

    sudo chmod +x /usr/local/bin/fix-lid
    sudo chmod +x /usr/local/bin/fix-session

    step_msg "Reloading systemd daemon..."
    sudo systemctl daemon-reload

    success_msg "Service installation routines complete."
    info_msg "You can now use these commands globally:"
    echo -e "  ${GREEN}fix-lid${NC}     - Manually apply post-resume lid fix (runs as root)"
    echo -e "  ${GREEN}fix-session${NC} - Manually attempt to reload GNOME session (runs as user)"
    return 0
}

# Start and enable services
enable_service() {
    step_msg "Enabling and starting ${SCRIPT_TITLE} services..."

    sudo systemctl enable "${SERVICE_NAME}.service"
    sudo systemctl enable "${SERVICE_NAME}-resume.service"
    sudo systemctl restart "${SERVICE_NAME}.service" 

    success_msg "Services enabled and ${SERVICE_NAME}.service (Lid Monitor) started/restarted."
    info_msg "The Lid Monitor service will attempt to run continuously."
    info_msg "The Post Resume Fix service will run automatically after system resume."
}

# Stop and disable services
disable_service() {
    step_msg "Stopping and disabling ${SCRIPT_TITLE} services..."

    sudo systemctl stop "${SERVICE_NAME}.service" 2>/dev/null || true
    sudo systemctl disable "${SERVICE_NAME}.service" 2>/dev/null || true
    sudo systemctl disable "${SERVICE_NAME}-resume.service" 2>/dev/null || true

    success_msg "Services stopped and disabled."
}

# Complete uninstall
uninstall() {
    step_msg "Uninstalling ${SCRIPT_TITLE}..."

    disable_service 

    step_msg "Removing service files..."
    sudo rm -f "/etc/systemd/system/${SERVICE_NAME}.service"
    sudo rm -f "/etc/systemd/system/${SERVICE_NAME}-resume.service"

    step_msg "Removing installation directory: $INSTALL_DIR"
    sudo rm -rf "$INSTALL_DIR"

    step_msg "Removing helper commands..."
    sudo rm -f /usr/local/bin/fix-lid
    sudo rm -f /usr/local/bin/fix-session

    step_msg "Reloading systemd daemon..."
    sudo systemctl daemon-reload

    success_msg "${SCRIPT_TITLE} has been completely uninstalled."
    info_msg "Thank you for using this tool."
}

# Check service status
check_status() {
    echo ""
    step_msg "${SCRIPT_TITLE} Status:"
    echo ""

    info_msg "Status for ${SERVICE_NAME}.service (Lid Monitor):"
    if systemctl is-active --quiet "${SERVICE_NAME}.service"; then
        success_msg "  Service State: RUNNING"
    else
        warning_msg "  Service State: STOPPED / INACTIVE"
    fi

    if systemctl is-enabled --quiet "${SERVICE_NAME}.service"; then
        success_msg "  Auto-start on boot: ENABLED"
    else
        warning_msg "  Auto-start on boot: DISABLED"
    fi
    echo ""

    info_msg "Status for ${SERVICE_NAME}-resume.service (Post Resume Fix):"
    if systemctl is-enabled --quiet "${SERVICE_NAME}-resume.service"; then
        success_msg "  Auto-run on resume: ENABLED"
    else
        warning_msg "  Auto-run on resume: DISABLED"
    fi
    echo "  (This is a oneshot service, typically inactive until resume)"
    echo ""


    info_msg "Recent logs for ${SERVICE_NAME}.service (Lid Monitor - 5 lines):"
    sudo journalctl -u "${SERVICE_NAME}.service" --no-pager -n 5 --output=cat 2>/dev/null || echo "  No logs available or permission denied."
    echo ""
    info_msg "Recent logs for ${SERVICE_NAME}-resume.service (Post Resume Fix - 5 lines, last run):"
    sudo journalctl -u "${SERVICE_NAME}-resume.service" --no-pager -n 5 --output=cat 2>/dev/null || echo "  No logs available or permission denied."
}

# Test the lid detection (ACPI level)
test_lid_acpi() {
    echo ""
    step_msg "Testing ACPI Lid State Detection..."
    echo ""

    local lid_path=""
    if [ -f "/proc/acpi/button/lid/LID0/state" ]; then
        lid_path="/proc/acpi/button/lid/LID0/state"
    elif [ -f "/proc/acpi/button/lid/LID/state" ]; then
        lid_path="/proc/acpi/button/lid/LID/state"
    fi

    local initial_lid_state="unknown"
    if [ -n "$lid_path" ]; then
        initial_lid_state=$(cat "$lid_path")
        info_msg "Initial ACPI lid state from $lid_path: $initial_lid_state"
    else
        error_msg "Could not find standard ACPI lid state file (e.g., /proc/acpi/button/lid/LID0/state)."
        info_msg "This test checks raw ACPI state. The monitor service uses similar paths."
        return 1
    fi

    echo ""
    info_msg "Please CLOSE and then OPEN your laptop lid now."
    echo -n "Monitoring ACPI lid state for 10 seconds... "

    local state_changed=false
    for i in {10..1}; do
        echo -ne "\rMonitoring ACPI lid state for 10 seconds... $i "
        local current_state=$(cat "$lid_path" 2>/dev/null || echo "$initial_lid_state")
        if [[ "$current_state" != "$initial_lid_state" ]]; then
            state_changed=true
        fi
        sleep 1
    done
    echo -e "\rMonitoring ACPI lid state for 10 seconds... Done."

    local final_lid_state=$(cat "$lid_path" 2>/dev/null || echo "$initial_lid_state")
    info_msg "Final ACPI lid state from $lid_path: $final_lid_state"

    if $state_changed || [[ "$final_lid_state" != "$initial_lid_state" ]]; then
        success_msg "ACPI lid state changed during the test. Raw detection appears functional."
    else
        warning_msg "ACPI lid state did not change from '$initial_lid_state'."
        warning_msg "Ensure you operated the lid. If so, there might be an issue with ACPI reporting."
    fi

    info_msg "Test complete. The Lid Monitor service polls this state."
    info_msg "You can also check monitor logs: journalctl -f -u ${SERVICE_NAME}.service"
}

# Main menu
show_menu() {
    echo ""
    echo -e "${WHITE}═══════════════════════════════════════════════════════════${NC}"
    echo -e "${WHITE}                        Main Menu                          ${NC}"
    echo -e "${WHITE}═══════════════════════════════════════════════════════════${NC}"
    echo ""
    echo -e "  ${GREEN}1)${NC} Install/Reinstall ${SCRIPT_TITLE}"
    echo -e "  ${GREEN}2)${NC} Start/Enable Services"
    echo -e "  ${GREEN}3)${NC} Stop/Disable Services"
    echo -e "  ${GREEN}4)${NC} Check Service Status & Logs"
    echo -e "  ${GREEN}5)${NC} Test ACPI Lid Detection"
    echo -e "  ${GREEN}6)${NC} Run Manual Lid Fix (via fix-lid)"
    echo -e "  ${GREEN}7)${NC} Run Manual Session Reload (via fix-session)"
    echo -e "  ${RED}8)${NC} Uninstall ${SCRIPT_TITLE}"
    echo -e "  ${RED}9)${NC} Exit"
    echo ""
    echo -e "${WHITE}═══════════════════════════════════════════════════════════${NC}"
    echo ""
}

# Manual fixes callers
manual_lid_fix_caller() {
    echo ""
    step_msg "Attempting manual lid fix execution..."
    if [ -x "/usr/local/bin/fix-lid" ]; then
        /usr/local/bin/fix-lid
    else
        error_msg "Command /usr/local/bin/fix-lid not found. Is ${SCRIPT_TITLE} installed?"
    fi
}

manual_gnome_session_fix_caller() {
    echo ""
    step_msg "Attempting manual GNOME session reload execution..."
    if [ -x "/usr/local/bin/fix-session" ]; then
        /usr/local/bin/fix-session
    else
        error_msg "Command /usr/local/bin/fix-session not found. Is ${SCRIPT_TITLE} installed?"
    fi
}

# Main function
main() {
    clear
    show_banner

    check_root
    setup_sudo
    check_surface

    echo ""
    step_msg "Welcome to ${SCRIPT_TITLE}!"
    info_msg "This tool helps resolve lid suspend issues and manage related services."
    echo ""

    while true; do
        show_menu
        read -p "Choose an option (1-9): " choice

        case $choice in
            1)
                echo ""
                if install_service; then
                    echo ""
                    read -p "Installation routines complete. Enable and start services now? (Y/n): " enable_choice
                    if [[ ! "$enable_choice" =~ ^[Nn]$ ]]; then
                        enable_service
                    else
                        info_msg "Services not started. You can start them via option 2."
                    fi
                else
                    error_msg "Installation failed. Please check messages above."
                fi
                ;;
            2)
                enable_service
                ;;
            3)
                disable_service
                ;;
            4)
                check_status
                ;;
            5)
                test_lid_acpi
                ;;
            6)
                manual_lid_fix_caller
                ;;
            7)
                manual_gnome_session_fix_caller
                ;;
            8)
                echo ""
                warning_msg "This will completely remove ${SCRIPT_TITLE} and its services!"
                read -p "Are you sure you want to uninstall? (y/N): " uninstall_choice
                if [[ "$uninstall_choice" =~ ^[Yy]$ ]]; then
                    uninstall
                    info_msg "Exiting now."
                    exit 0
                else
                    info_msg "Uninstall cancelled."
                fi
                ;;
            9)
                echo ""
                info_msg "Exiting ${SCRIPT_TITLE}."
                SUDO_KEEPALIVE_PID=$(pgrep -f "sudo -n true.*sleep 60")
                if [ -n "$SUDO_KEEPALIVE_PID" ]; then
                    # Silently try to kill, don't error if already gone
                    sudo kill "$SUDO_KEEPALIVE_PID" 2>/dev/null || true
                fi
                echo ""
                exit 0
                ;;
            *)
                error_msg "Invalid option: '$choice'. Please choose a number from 1 to 9."
                ;;
        esac

        echo ""
        read -p "Press Enter to return to the menu..."
        clear
        show_banner
    done
}

# Run the main function
main "$@"
