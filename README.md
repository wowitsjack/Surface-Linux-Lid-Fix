==========Fixes Surface series laptops failing to enter sleep mode/suspend when the lid is closed.

To resume, open lid and tap power button, or press any keyboard key (model depending).==========

Tested on Pop!_OS 22.04 w/Surface Linux kernel.


*   **1) Install Surface Lid Fix:** Installs the services and helper scripts.
*   **2) Start/Enable Services:** Enables the services to start on boot and starts them immediately.
*   **3) Stop/Disable Services:** Stops the services and disables them from starting on boot.
*   **4) Check Service Status:** Shows the current status (running, enabled) of the services and recent log entries.
*   **5) Test Lid Detection:** Guides you through a manual lid open/close test and reports ACPI state.
*   **6) Run Manual Lid Fix (`fix-lid`):** Manually executes the `post_resume_lid_fix.py` script. This is also available globally as `sudo fix-lid` after installation.
*   **7) Run Manual Session Fix (`fix-session`):** Manually executes `reload_gnome_session.py`. This is also available globally as `fix-session` after installation.
*   **8) Uninstall Surface Lid Fix:** Removes all installed files, services, and helper commands.
*   **9) Exit:** Exits the script.

### Global Helper Commands

After installation, two helper commands are available system-wide:

*   `sudo fix-lid`: Manually triggers the lid fix mechanism (runs `post_resume_lid_fix.py`).
*   `fix-session`: Manually reloads the GNOME session (runs `reload_gnome_session.py`).

## Uninstallation

1.  Run `./surface_lidfix.sh`.
2.  Choose option `8) Uninstall Surface Lid Fix`.
3.  Confirm the uninstallation.

This will stop and disable the services, remove the systemd service files, delete the installation directory (`/opt/surface-lidfix`), and remove the global helper commands.

## Troubleshooting

IF YOU EXPERIENCE SCALING RESETS ON POP!_OS, DISABLE HiDPI AGENT IN DIPLSAY SETTINGS

*   **Python Scripts Not Found:** Ensure `lid_suspend_monitor.py`, `post_resume_lid_fix.py`, and `reload_gnome_session.py` are in the same directory as `surface_lidfix.sh` *before running the installation option*.
*   **Permissions:** The script requires `sudo` for installation and some operations. It will prompt when needed. Do not run the main script itself with `sudo`.
*   **Service Status:** If you suspect issues, use option `4) Check Service Status` from the menu. You can also check logs directly:
    ```bash
    systemctl status surface-lidfix.service
    systemctl status surface-lidfix-resume.service
    journalctl -u surface-lidfix.service -f
    journalctl -u surface-lidfix-resume.service
    ```
*   **"Device not detected as Surface":** The script tries to detect if it's running on a Surface device. If not, it will warn you but allow you to proceed. The fix might still work on other laptops with similar ACPI/lid issues.

## Contributing

Contributions, bug reports, and feature requests are welcome! Please feel free to open an issue or submit a pull request on the [GitHub repository](https://github.com/wowitsjack/Surface-Linux-Lid-Fix/).

## License

This project is licensed under the MIT License.
