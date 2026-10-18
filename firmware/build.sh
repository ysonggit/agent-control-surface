#!/bin/sh
# Build (and optionally flash) the firmware.
#   firmware/build.sh [xiao|amoled] [upload [PORT]]
#   xiao   = Seeed XIAO nRF52840 Plus + 1.47" LCD (default)
#   amoled = Waveshare ESP32-S3-Touch-AMOLED-1.64
set -e
cd "$(dirname "$0")"
TARGET=xiao
case "$1" in xiao|amoled) TARGET=$1; shift ;; esac
if [ "$TARGET" = amoled ]; then
  FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB"
  OUT=build-amoled
else
  FQBN=Seeeduino:nrf52:xiaonRF52840Plus
  OUT=build
fi
GFX_COMMIT=762e316
if [ ! -d lib/Seeed_GFX2 ]; then
  git clone -q https://github.com/Seeed-Studio/Seeed_GFX2.git lib/Seeed_GFX2
  git -C lib/Seeed_GFX2 checkout -q $GFX_COMMIT
fi
# The Seeed nRF52 core calls `python` and its packaging tool needs a UTF-8 locale.
export PATH="$PWD/tools/bin:$PATH" LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8
LIBS="--library lib/Seeed_GFX2"
[ "$TARGET" = amoled ] && LIBS=""
arduino-cli compile -b "$FQBN" $LIBS --output-dir $OUT acs 2>&1 | grep -E "Sketch uses|Global variables|error|Error" || true
ls $OUT/acs.ino.* >/dev/null 2>&1 || { echo "build failed"; exit 1; }
if [ "$1" = "upload" ]; then
  # The daemon holds the serial port; pause its LaunchAgent (KeepAlive would restart it) while flashing.
  JOB="gui/$(id -u)/com.agentcontrolsurface.acsd"
  PLIST="$HOME/Library/LaunchAgents/com.agentcontrolsurface.acsd.plist"
  launchctl bootout "$JOB" 2>/dev/null && RESTART=1 || true
  sleep 1
  PORT=${2:-$(ls /dev/cu.usbmodem* | head -1)}
  arduino-cli upload -b "$FQBN" -p "$PORT" --input-dir $OUT acs
  [ -n "$RESTART" ] && sleep 2 && launchctl bootstrap "gui/$(id -u)" "$PLIST"
fi
