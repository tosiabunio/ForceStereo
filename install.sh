#!/bin/bash
# Builds and installs the ForceStereo driver (needs sudo) and the forwarding helper (per user).
set -euo pipefail
cd "$(dirname "$0")"

LABEL=local.forcestereo.helper
HAL=/Library/Audio/Plug-Ins/HAL
APPDIR="$HOME/Library/Application Support/ForceStereo"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/ForceStereo.log"

make

echo "==> Installing driver into $HAL (sudo)"
sudo rm -rf "$HAL/ForceStereo.driver"
sudo cp -R build/ForceStereo.driver "$HAL/"
sudo chown -R root:wheel "$HAL/ForceStereo.driver"
echo "==> Restarting coreaudiod (audio drops for a second)"
sudo killall coreaudiod 2>/dev/null || sudo launchctl kickstart -k system/com.apple.audio.coreaudiod

echo "==> Installing helper"
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
mkdir -p "$APPDIR" "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
cp build/forcestereo "$APPDIR/forcestereo"
cat > "$PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$LABEL</string>
	<key>ProgramArguments</key>
	<array>
		<string>$APPDIR/forcestereo</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>KeepAlive</key>
	<true/>
	<key>ProcessType</key>
	<string>Interactive</string>
	<key>StandardErrorPath</key>
	<string>$LOG</string>
</dict>
</plist>
PLIST
sleep 2
launchctl bootstrap "gui/$(id -u)" "$PLIST"

echo "==> Done. Pick \"ForceStereo\" as the sound output. Log: $LOG"
