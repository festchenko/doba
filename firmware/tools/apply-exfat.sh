#!/bin/sh
# Rebuild the framework's FAT driver (ESP-IDF v4.4.6 components/fatfs) with
# exFAT on, and overlay it: tools/sdk/esp32s3/lib/libfatfs.a and the matching
# ffconf.h, so every user of ff.h (the SD library, usb_storage.cpp) sees the
# same structures. FAT32 cards keep working; exFAT cards mount too.
#
# Why: a big FAT32 card walks the cluster chain from the start of a file on
# every seek; exFAT stores a big file contiguously and seeks straight to the
# place (the map packs and book chunks are big files read at random places).
#
# Run after tools/apply-pm-framework.sh (it replaces the SDK libraries).
# Idempotent. Needs network (raw.githubusercontent.com) the first time.
set -e
FW=${FRAMEWORK:-$HOME/.platformio/packages/framework-arduinoespressif32}
SDK=$FW/tools/sdk/esp32s3
CONF=$SDK/include/fatfs/src/ffconf.h
if grep -q "^#define FF_FS_EXFAT[[:space:]]*1" "$CONF"; then
  echo "apply-exfat: framework already has exFAT"; exit 0
fi
CC=$(ls "$HOME"/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-gcc)
W=$(mktemp -d)
IDF=https://raw.githubusercontent.com/espressif/esp-idf/v4.4.6/components/fatfs
SRC="src/ff.c src/ff.h src/ffconf.h src/ffunicode.c src/diskio.h port/freertos/ffsystem.c
  diskio/diskio.c diskio/diskio_impl.h diskio/diskio_rawflash.c diskio/diskio_rawflash.h
  diskio/diskio_sdmmc.c diskio/diskio_sdmmc.h diskio/diskio_wl.c diskio/diskio_wl.h
  vfs/esp_vfs_fat.h vfs/vfs_fat.c vfs/vfs_fat_internal.h vfs/vfs_fat_sdmmc.c vfs/vfs_fat_spiflash.c"
for f in $SRC; do
  mkdir -p "$W/$(dirname "$f")"
  curl -sSf -o "$W/$f" "$IDF/$f"
done
sed -i.bak 's/^#define FF_FS_EXFAT\t\t0/#define FF_FS_EXFAT\t\t1/' "$W/src/ffconf.h"
grep -q "^#define FF_FS_EXFAT[[:space:]]*1" "$W/src/ffconf.h" || { echo "apply-exfat: could not switch exFAT on" >&2; exit 1; }
# The framework's own include paths, as its build script lists them.
INC=$(grep -o 'join(FRAMEWORK_DIR, "tools", "sdk", "esp32s3", "include"[^)]*)' "$FW/tools/platformio-build-esp32s3.py" |
      sed 's/join(FRAMEWORK_DIR, //; s/)$//; s/", "/\//g; s/"//g' | sed "s#^#-I$FW/#" | tr '\n' ' ')
FLAGS="-std=gnu99 -Os -mlongcalls -ffunction-sections -fdata-sections -fstrict-volatile-bitfields
  -fno-jump-tables -fno-tree-switch-conversion -DESP_PLATFORM -D_GNU_SOURCE -Wno-old-style-declaration"
mkdir -p "$W/out"
for f in src/ff.c src/ffunicode.c port/freertos/ffsystem.c diskio/diskio.c diskio/diskio_rawflash.c \
         diskio/diskio_sdmmc.c diskio/diskio_wl.c vfs/vfs_fat.c vfs/vfs_fat_sdmmc.c vfs/vfs_fat_spiflash.c; do
  # shellcheck disable=SC2086
  "$CC" $FLAGS -I"$W/src" -I"$W/diskio" -I"$W/vfs" $INC -I"$SDK/qio_opi/include" -c "$W/$f" -o "$W/out/$(basename "$f").obj"
done
cp "$SDK/lib/libfatfs.a" "$SDK/lib/libfatfs.a.noexfat"
rm -f "$W/libfatfs.a"
"$(dirname "$CC")/xtensa-esp32s3-elf-ar" rcs "$W/libfatfs.a" "$W"/out/*.obj
cp "$W/libfatfs.a" "$SDK/lib/libfatfs.a"
cp "$CONF" "$CONF.noexfat"
cp "$W/src/ffconf.h" "$CONF"
rm -rf "$W"
echo "apply-exfat: libfatfs.a rebuilt with exFAT (backups: *.noexfat)"
