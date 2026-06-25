#!/usr/bin/env bash
# Flash a prebuilt claudemon image to a connected device — no toolchain needed
# beyond esptool (`pip install esptool`).
#
#   ./scripts/flash.sh [path-to-bin] [port]
#
# Defaults: dist/claudemon-full.bin, auto-detected port.
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="${1:-dist/claudemon-full.bin}"
[ -f "$BIN" ] || { echo "image not found: $BIN  (download claudemon-full.bin from Releases)"; exit 1; }

PORT="${2:-}"
if [ -z "$PORT" ]; then
  PORT=$(ls /dev/cu.usbserial-* /dev/cu.SLAB_USBtoUART /dev/cu.wchusbserial* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -1 || true)
fi
[ -n "$PORT" ] || { echo "no serial port found; pass it explicitly: ./scripts/flash.sh $BIN /dev/cu.usbserial-XX"; exit 1; }

command -v esptool.py >/dev/null 2>&1 || ESPTOOL="python3 -m esptool"
ESPTOOL="${ESPTOOL:-esptool.py}"

echo "flashing $BIN -> $PORT"
$ESPTOOL --chip esp32s3 --port "$PORT" --baud 460800 write_flash 0x0 "$BIN"
echo "done. Join WiFi 'claudemon-XXXX' (password claudemon) to finish setup."
