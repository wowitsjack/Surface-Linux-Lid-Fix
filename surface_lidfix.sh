#!/bin/bash
#
# Surface Lid Fix
# (For Surface laptops, and potentially other devices)
#
# Fixes the "lid won't suspend after first resume" bug
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
VERSION="1.0"
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
    ║                                                           ║
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
    
    local required_files=("lid_suspend_monitor.py" "post_resume_lid_fix.py" "reload_gnome_session.py")
    for file in "${required_files[@]}"; do
        if [[ ! -f "$CURRENT_DIR/$file" ]]; then
            error_msg "Required file not found: $file"
            error_msg "Ensure all Python scripts are in the current directory: $CURRENT_DIR"
            return 1
        fi
    done
    
    step_msg "Creating installation directory: $INSTALL_DIR"
    sudo mkdir -p "$INSTALL_DIR"
    
    step_msg "Copying Python scripts..."
    sudo cp "$CURRENT_DIR/lid_suspend_monitor.py" "$INSTALL_DIR/"
    sudo cp "$CURRENT_DIR/post_resume_lid_fix.py" "$INSTALL_DIR/"
    sudo cp "$CURRENT_DIR/reload_gnome_session.py" "$INSTALL_DIR/"
    sudo chmod +x "$INSTALL_DIR"/*.py
    
    step_msg "Creating systemd service: ${SERVICE_NAME}.service"
    sudo tee "/etc/systemd/system/${SERVICE_NAME}.service" > /dev/null << EOF
[Unit]
Description=${SCRIPT_TITLE} - Lid Suspend Monitor
After=graphical-session.target
Wants=graphical-session.target

[Service]
Type=simple
User=$USER
Environment=DISPLAY=:0
Environment=XDG_RUNTIME_DIR=/run/user/$(id -u)
ExecStart=/usr/bin/python3 $INSTALL_DIR/lid_suspend_monitor.py
Restart=always
RestartSec=5

[Install]
WantedBy=default.target
EOF

    step_msg "Creating post-resume service: ${SERVICE_NAME}-resume.service"
    sudo tee "/etc/systemd/system/${SERVICE_NAME}-resume.service" > /dev/null << EOF
[Unit]
Description=${SCRIPT_TITLE} - Post Resume Fix
After=suspend.target sleep.target hybrid-sleep.target hibernate.target

[Service]
Type=oneshot
ExecStart=$INSTALL_DIR/post_resume_lid_fix.py
User=root

[Install]
WantedBy=suspend.target sleep.target hybrid-sleep.target hibernate.target
EOF

    step_msg "Creating helper commands..."
    sudo tee "/usr/local/bin/fix-lid" > /dev/null << EOF
#!/bin/bash
# Quick lid fix command
# Part of ${SCRIPT_TITLE}
sudo python3 $INSTALL_DIR/post_resume_lid_fix.py
EOF

    sudo tee "/usr/local/bin/fix-session" > /dev/null << EOF
#!/bin/bash
# Quick session fix command  
# Part of ${SCRIPT_TITLE}
python3 $INSTALL_DIR/reload_gnome_session.py
EOF

    sudo chmod +x /usr/local/bin/fix-lid
    sudo chmod +x /usr/local/bin/fix-session
    
    step_msg "Reloading systemd daemon..."
    sudo systemctl daemon-reload
    
    success_msg "Service installed successfully."
    info_msg "You can now use these commands globally:"
    echo -e "  ${GREEN}fix-lid${NC}     - Manually apply lid detection fix"
    echo -e "  ${GREEN}fix-session${NC} - Manually fix GNOME session issues"
    return 0
}

# Start and enable services
enable_service() {
    step_msg "Enabling and starting ${SCRIPT_TITLE} services..."
    
    sudo systemctl enable "${SERVICE_NAME}.service"
    sudo systemctl enable "${SERVICE_NAME}-resume.service"
    sudo systemctl start "${SERVICE_NAME}.service"
    
    success_msg "Services enabled and started."
    info_msg "The services will automatically start on boot and after resume."
}

# Stop and disable services
disable_service() {
    step_msg "Stopping and disabling ${SCRIPT_TITLE} services..."
    
    sudo systemctl stop "${SERVICE_NAME}.service" 2>/dev/null || true
    sudo systemctl disable "${SERVICE_NAME}.service" 2>/dev/null || true
    sudo systemctl disable "${SERVICE_NAME}-resume.service" 2>/dev/null || true
    
    success_msg "Services disabled."
}

# Complete uninstall
uninstall() {
    step_msg "Uninstalling ${SCRIPT_TITLE}..."
    
    disable_service # This already prints messages
    
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
    
    # Main service status
    if systemctl is-active --quiet "${SERVICE_NAME}.service"; then
        success_msg "Main service (${SERVICE_NAME}.service): RUNNING"
    else
        warning_msg "Main service (${SERVICE_NAME}.service): STOPPED"
    fi
    
    if systemctl is-enabled --quiet "${SERVICE_NAME}.service"; then
        success_msg "Main service auto-start: ENABLED"
    else
        warning_msg "Main service auto-start: DISABLED"
    fi
    echo ""

    # Post-resume service status
    if systemctl is-enabled --quiet "${SERVICE_NAME}-resume.service"; then
        success_msg "Post-resume service (${SERVICE_NAME}-resume.service): ENABLED (will run on resume)"
    else
        warning_msg "Post-resume service (${SERVICE_NAME}-resume.service): DISABLED"
    fi
    
    echo ""
    info_msg "Recent logs for ${SERVICE_NAME}.service (main service):"
    sudo journalctl -u "${SERVICE_NAME}.service" --no-pager -n 5 --output=cat 2>/dev/null || echo "No logs available or permission denied."
}

# Test the lid detection
test_lid() {
    echo ""
    step_msg "Testing lid detection..."
    echo ""
    
    local lid_path=""
    if [ -f "/proc/acpi/button/lid/LID0/state" ]; then
        lid_path="/proc/acpi/button/lid/LID0/state"
    elif [ -f "/proc/acpi/button/lid/LID/state" ]; then
        lid_path="/proc/acpi/button/lid/LID/state"
    fi

    local initial_lid_state=""
    if [ -n "$lid_path" ]; then
        initial_lid_state=$(cat "$lid_path")
        info_msg "Initial lid state from $lid_path: $initial_lid_state"
    else
        error_msg "Could not find ACPI lid state file (e.g., /proc/acpi/button/lid/LID0/state)."
        info_msg "This test may not be conclusive for your system."
        # Do not return 1, allow test to proceed if possible, or user can observe logs.
    fi
    
    echo ""
    info_msg "Please CLOSE and then OPEN your laptop lid now."
    echo -n "Monitoring for lid state changes for 10 seconds... "
    
    for i in {10..1}; do
        echo -ne "\rMonitoring for lid state changes for 10 seconds... $i "
        sleep 1
    done
    echo -e "\rMonitoring for lid state changes for 10 seconds... Done."
    
    if [ -n "$lid_path" ]; then
        local new_lid_state=$(cat "$lid_path")
        info_msg "New lid state from $lid_path: $new_lid_state"
        if [[ "$new_lid_state" != "$initial_lid_state" ]]; then
            success_msg "Lid state changed. ACPI level detection appears to be working."
        else
            warning_msg "Lid state did not change from '$initial_lid_state'. Ensure you operated the lid."
        fi
    else
        info_msg "Could not read ACPI lid state. Check service logs for detection activity."
    fi
    
    info_msg "Test complete. If the services are running, they should react to lid events."
    info_msg "You can also check with: journalctl -f -u ${SERVICE_NAME}.service"
}

# Main menu
show_menu() {
    echo ""
    echo -e "${WHITE}═══════════════════════════════════════════════════════════${NC}"
    echo -e "${WHITE}                        Main Menu                          ${NC}"
    echo -e "${WHITE}═══════════════════════════════════════════════════════════${NC}"
    echo ""
    echo -e "  ${GREEN}1)${NC} Install ${SCRIPT_TITLE}"
    echo -e "  ${GREEN}2)${NC} Start/Enable Services"
    echo -e "  ${GREEN}3)${NC} Stop/Disable Services"
    echo -e "  ${GREEN}4)${NC} Check Service Status"
    echo -e "  ${GREEN}5)${NC} Test Lid Detection"
    echo -e "  ${GREEN}6)${NC} Run Manual Lid Fix (fix-lid)"
    echo -e "  ${GREEN}7)${NC} Run Manual Session Fix (fix-session)"
    echo -e "  ${RED}8)${NC} Uninstall ${SCRIPT_TITLE}"
    echo -e "  ${RED}9)${NC} Exit"
    echo ""
    echo -e "${WHITE}═══════════════════════════════════════════════════════════${NC}"
    echo ""
}

# Manual fixes
manual_lid_fix() {
    echo ""
    step_msg "Attempting manual lid fix..."
    echo ""
    
    if [ -x "/usr/local/bin/fix-lid" ]; then
        sudo /usr/local/bin/fix-lid
        success_msg "Manual lid fix command executed."
    elif [ -f "$INSTALL_DIR/post_resume_lid_fix.py" ]; then
        info_msg "Helper command /usr/local/bin/fix-lid not found. Running script directly from $INSTALL_DIR."
        sudo python3 "$INSTALL_DIR/post_resume_lid_fix.py"
        success_msg "Manual lid fix script executed."
    elif [ -f "$CURRENT_DIR/post_resume_lid_fix.py" ]; then
        info_msg "${SCRIPT_TITLE} not installed. Attempting to run script from current directory: $CURRENT_DIR."
        sudo python3 "$CURRENT_DIR/post_resume_lid_fix.py"
        success_msg "Manual lid fix script (local) executed."
    else
        error_msg "Lid fix script (post_resume_lid_fix.py) not found."
        error_msg "Please install ${SCRIPT_TITLE} first, or ensure the script is in the current directory."
    fi
}

manual_gnome_session_fix() {
    echo ""
    step_msg "Attempting manual GNOME session fix..."
    echo ""
    
    if [ -x "/usr/local/bin/fix-session" ]; then
        /usr/local/bin/fix-session
        success_msg "Manual session fix command executed."
    elif [ -f "$INSTALL_DIR/reload_gnome_session.py" ]; then
        info_msg "Helper command /usr/local/bin/fix-session not found. Running script directly from $INSTALL_DIR."
        python3 "$INSTALL_DIR/reload_gnome_session.py"
        success_msg "Manual session fix script executed."
    elif [ -f "$CURRENT_DIR/reload_gnome_session.py" ]; then
        info_msg "${SCRIPT_TITLE} not installed. Attempting to run script from current directory: $CURRENT_DIR."
        python3 "$CURRENT_DIR/reload_gnome_session.py"
        success_msg "Manual session fix script (local) executed."
    else
        error_msg "Session fix script (reload_gnome_session.py) not found."
        error_msg "Please install ${SCRIPT_TITLE} first, or ensure the script is in the current directory."
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
    info_msg "This tool helps resolve lid suspend issues, primarily on Surface Linux devices."
    echo ""
    warning_msg "For installation, ensure the following Python scripts are in the current directory ($CURRENT_DIR):"
    echo "   • lid_suspend_monitor.py"
    echo "   • post_resume_lid_fix.py" 
    echo "   • reload_gnome_session.py"
    echo ""
    
    while true; do
        show_menu
        read -p "Choose an option (1-9): " choice
        
        case $choice in
            1)
                echo ""
                if install_service; then
                    echo ""
                    read -p "Installation successful. Enable and start services now? (Y/n): " enable_choice
                    if [[ ! "$enable_choice" =~ ^[Nn]$ ]]; then
                        enable_service
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
                test_lid
                ;;
            6)
                manual_lid_fix
                ;;
            7)
                manual_gnome_session_fix
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
