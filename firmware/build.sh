#!/bin/sh
# Build (and optionally flash) the firmware.   usage: firmware/build.sh [upload [PORT]]
set -e
cd "$(dirname "$0")"
FQBN=Seeeduino:nrf52:xiaonRF52840Plus
GFX_COMMIT=762e316
if [ ! -d lib/Seeed_GFX2 ]; then
  git clone -q https://github.com/Seeed-Studio/Seeed_GFX2.git lib/Seeed_GFX2
  git -C lib/Seeed_GFX2 checkout -q $GFX_COMMIT
fi
# The Seeed nRF52 core calls `python` and its packaging tool needs a UTF-8 locale.
export PATH="$PWD/tools/bin:$PATH" LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8
arduino-cli compile -b $FQBN --library lib/Seeed_GFX2 --output-dir build acs 2>&1 | grep -E "Sketch uses|Global variables|error|Error" || true
[ -f build/acs.ino.zip ] || { echo "build failed"; exit 1; }
if [ "$1" = "upload" ]; then
  # The daemon holds the serial port; pause its LaunchAgent (KeepAlive would restart it) while flashing.
  JOB="gui/$(id -u)/com.agentcontrolsurface.acsd"
  PLIST="$HOME/Library/LaunchAgents/com.agentcontrolsurface.acsd.plist"
  launchctl bootout "$JOB" 2>/dev/null && RESTART=1 || true
  sleep 1
  PORT=${2:-$(ls /dev/cu.usbmodem* | head -1)}
  arduino-cli upload -b $FQBN -p "$PORT" --input-dir build acs
  [ -n "$RESTART" ] && sleep 2 && launchctl bootstrap "gui/$(id -u)" "$PLIST"
fi
