#!/bin/bash
# Flash the Doba firmware onto a LilyGo T5 E-Paper S3 Pro from a Mac.
#
#   tools/flash-mac.sh            # backup the factory firmware, then flash
#   tools/flash-mac.sh --no-backup
#   tools/flash-mac.sh --app-only # update: keep settings, write the app only
#
# Before running: connect the device by USB-C. If the port is not found, put
# it in download mode: hold BOOT, press and release RST, release BOOT.
set -euo pipefail
cd "$(dirname "$0")/.."

BACKUP=1; APP_ONLY=0
for a in "$@"; do
  case "$a" in
    --no-backup) BACKUP=0 ;;
    --app-only) APP_ONLY=1; BACKUP=0 ;;
    *) echo "unknown option: $a"; exit 1 ;;
  esac
done

VENV=.venv-flash
if [ ! -x "$VENV/bin/esptool.py" ]; then
  echo "== installing esptool (into $VENV)"
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install --quiet "esptool>=4.5,<5"
fi
ESPTOOL=("$VENV/bin/esptool.py" --chip esp32s3 --baud 921600)

PORT="${DOBA_PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)}"
if [ -z "$PORT" ]; then
  echo "No device found on /dev/cu.usbmodem*."
  echo "Hold BOOT, press RST, release BOOT, then run this again."
  exit 1
fi
echo "== device on $PORT"
"${ESPTOOL[@]}" --port "$PORT" chip_id

if [ "$BACKUP" = 1 ]; then
  mkdir -p backup
  OUT="backup/factory-$(date +%Y%m%d-%H%M%S).bin"
  echo "== saving the factory firmware to $OUT (16 MB, ~4 min)"
  "${ESPTOOL[@]}" --port "$PORT" read_flash 0 0x1000000 "$OUT"
  echo "   to restore it later: $VENV/bin/esptool.py --chip esp32s3 --port $PORT write_flash 0 $OUT"
fi

if [ "$APP_ONLY" = 1 ]; then
  echo "== writing the app (settings kept)"
  "${ESPTOOL[@]}" --port "$PORT" write_flash 0x10000 release/doba-app.bin
else
  echo "== erasing and writing the full image"
  "${ESPTOOL[@]}" --port "$PORT" erase_flash
  "${ESPTOOL[@]}" --port "$PORT" write_flash 0x0 release/doba-full.bin
fi
echo "== done. Press RST (or unplug and replug) to start Doba."
