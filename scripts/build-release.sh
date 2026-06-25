#!/usr/bin/env bash
# Build the firmware and produce a single flashable image (offset 0x0).
# Output: dist/claudemon-full.bin and docs/firmware/claudemon-full.bin (web flasher).
set -euo pipefail
cd "$(dirname "$0")/.."

ENV=esp32-4848S040CIY1
B=".pio/build/$ENV"

echo "== building $ENV =="
pio run -e "$ENV"

ESPTOOL=$(find "$HOME/.platformio/packages" -name esptool.py | head -1)
BOOT0=$(find "$HOME/.platformio" -name boot_app0.bin | head -1)
# Use PlatformIO's bundled python (has esptool's deps); fall back to python3.
PY=$(ls /opt/homebrew/Cellar/platformio/*/libexec/bin/python 2>/dev/null | head -1)
PY=${PY:-python3}

[ -n "$ESPTOOL" ] || { echo "esptool.py not found under ~/.platformio"; exit 1; }
[ -n "$BOOT0" ]   || { echo "boot_app0.bin not found under ~/.platformio"; exit 1; }

mkdir -p dist docs/firmware
"$PY" "$ESPTOOL" --chip esp32s3 merge_bin -o dist/claudemon-full.bin \
  --flash_mode keep --flash_freq keep --flash_size 16MB \
  0x0     "$B/bootloader.bin" \
  0x8000  "$B/partitions.bin" \
  0xe000  "$BOOT0" \
  0x10000 "$B/firmware.bin"

cp dist/claudemon-full.bin docs/firmware/claudemon-full.bin
echo
echo "wrote dist/claudemon-full.bin  ($(du -h dist/claudemon-full.bin | cut -f1))"
echo "  flash:   esptool.py --chip esp32s3 write_flash 0x0 dist/claudemon-full.bin"
echo "  web:     docs/firmware/claudemon-full.bin updated for the web installer"
