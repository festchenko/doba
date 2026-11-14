#!/bin/bash
# Prepare a microSD card for Doba on a Mac: exFAT, then everything onto it.
#
#   tools/prepare-sd-mac.sh list                         # find the card
#   tools/prepare-sd-mac.sh format disk4                 # ERASES disk4 -> exFAT "DOBA"
#   tools/prepare-sd-mac.sh copy /path/to/tiles/groenlo  # copy packs to /Volumes/DOBA
#   tools/prepare-sd-mac.sh copy-lib /path/to/lib        # copy library books
#   tools/prepare-sd-mac.sh copy-dem /path/to/dem-pack   # copy ground heights
#   tools/prepare-sd-mac.sh all /path/to/doba-build/data  # everything: maps of every
#                                                          # region merged, books, heights,
#                                                          # a manifest, then a check
#
# The device reads exFAT (big files seek straight to the place) and FAT32.
set -euo pipefail

case "${1:-}" in
  list)
    diskutil list external physical
    ;;
  format)
    DISK="${2:?usage: format diskN (see: list)}"
    diskutil info "$DISK" | grep -E "Device / Media Name|Disk Size|Removable Media|Protocol"
    echo
    read -r -p "ERASE everything on $DISK and format it exFAT? Type the disk name to confirm: " ok
    [ "$ok" = "$DISK" ] || { echo "cancelled"; exit 1; }
    diskutil eraseDisk ExFAT DOBA MBRFormat "/dev/$DISK"
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
  copy-dem)
    SRC="${2:?usage: copy-dem <dir of .dem files>}"
    DST="${3:-/Volumes/DOBA}"
    mkdir -p "$DST/doba/dem"
    rsync -rt --progress --include '*.dem' --exclude '*' "$SRC/" "$DST/doba/dem/"
    dot_clean -m "$DST" 2>/dev/null || true
    find "$DST" -name '._*' -delete 2>/dev/null || true
    echo "== on the card: $(ls "$DST/doba/dem" | wc -l | tr -d ' ') degree files, $(du -sh "$DST/doba/dem" | cut -f1)"
    ;;
  copy-lib)
    SRC="${2:?usage: copy-lib <dir with one folder per book>}"
    DST="${3:-/Volumes/DOBA}"
    mkdir -p "$DST/doba/lib"
    rsync -rt --progress --exclude '.*' --exclude '*.part' "$SRC/" "$DST/doba/lib/"
    dot_clean -m "$DST" 2>/dev/null || true
    find "$DST" -name '._*' -delete 2>/dev/null || true
    echo "== on the card:"; du -sh "$DST/doba/lib"/*
    ;;
  all)
    DATA="${2:?usage: all <doba-build/data>}"
    DST="${3:-/Volumes/DOBA}"
    HERE="$(cd "$(dirname "$0")/.." && pwd)"
    [ -d "$DST" ] || { echo "$DST not mounted"; exit 1; }
    python3 "$HERE/tools/doba_card.py" write "$DATA" "$DST" --jobs 3
    echo "== card: $(df -h "$DST" | awk 'NR==2{print $3" used, "$4" free"}')"
    ;;
  *)
    sed -n 2,13p "$0"; exit 1 ;;
esac
