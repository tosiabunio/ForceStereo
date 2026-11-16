#!/bin/bash
set -uo pipefail
LABEL=local.forcestereo.helper
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null
rm -f "$HOME/Library/LaunchAgents/$LABEL.plist"
rm -rf "$HOME/Library/Application Support/ForceStereo"
echo "==> Removing driver (sudo)"
sudo rm -rf /Library/Audio/Plug-Ins/HAL/ForceStereo.driver
sudo killall coreaudiod 2>/dev/null
echo "==> Uninstalled."
