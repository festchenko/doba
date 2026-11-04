#!/bin/bash
# Prepare a microSD card for Doba on a Mac: FAT32, then copy the map packs.
#
#   tools/prepare-sd-mac.sh list                         # find the card
#   tools/prepare-sd-mac.sh format disk4                 # ERASES disk4 -> FAT32 "DOBA"
#   tools/prepare-sd-mac.sh copy /path/to/tiles/groenlo  # copy packs to /Volumes/DOBA
#
# The device reads only FAT32 (not exFAT, which cards over 32 GB come with).
set -euo pipefail

case "${1:-}" in
  list)
    diskutil list external physical
    ;;
  format)
    DISK="${2:?usage: format diskN (see: list)}"
    diskutil info "$DISK" | grep -E "Device / Media Name|Disk Size|Removable Media|Protocol"
    echo
    read -r -p "ERASE everything on $DISK and format it FAT32? Type the disk name to confirm: " ok
    [ "$ok" = "$DISK" ] || { echo "cancelled"; exit 1; }
    diskutil eraseDisk FAT32 DOBA MBRFormat "/dev/$DISK"
    sudo mdutil -i off /Volumes/DOBA >/dev/null 2>&1 || true   # no Spotlight index on the card
    diskutil info /Volumes/DOBA | grep -E "File System Personality|Volume Total Space|Allocation Block Size"
    ;;
  copy)
    SRC="${2:?usage: copy <dir with base.dtp and 8/>}"
    DST="${3:-/Volumes/DOBA}"
    [ -f "$SRC/base.dtp" ] || [ -d "$SRC/8" ] || { echo "$SRC has no .dtp packs"; exit 1; }
    mkdir -p "$DST/doba/map"
    rsync -rt --progress --exclude '.*' "$SRC/" "$DST/doba/map/"
    dot_clean -m "$DST" 2>/dev/null || true       # remove macOS ._ files
    find "$DST" -name '._*' -delete 2>/dev/null || true
    echo "== on the card:"; du -sh "$DST/doba/map"
    ;;
  *)
    sed -n 2,9p "$0"; exit 1 ;;
esac
