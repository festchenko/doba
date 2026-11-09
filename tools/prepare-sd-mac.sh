#!/bin/bash
# Prepare a microSD card for Doba on a Mac: FAT32, then copy the map packs.
#
#   tools/prepare-sd-mac.sh list                         # find the card
#   tools/prepare-sd-mac.sh format disk4                 # ERASES disk4 -> FAT32 "DOBA"
#   tools/prepare-sd-mac.sh copy /path/to/tiles/groenlo  # copy packs to /Volumes/DOBA
#   tools/prepare-sd-mac.sh copy-lib /path/to/lib        # copy library books
#   tools/prepare-sd-mac.sh copy-dem /path/to/dem-pack   # copy ground heights
#   tools/prepare-sd-mac.sh all /path/to/doba-build/data  # everything: maps of every
#                                                          # region merged, books, heights
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
    fs=$(diskutil info "$DST" | awk -F: '/File System Personality/{gsub(/^ +/,"",$2); print $2}')
    case "$fs" in *FAT32*|*MS-DOS*) ;; *) echo "$DST is '$fs', not FAT32: format it first"; exit 1;; esac
    # FAT32 holds files up to 4 GB: nothing we copy may be bigger
    big=$(find "$DATA/lib" "$DATA/tiles" -type f -size +4000M 2>/dev/null | head -3)
    [ -z "$big" ] || { echo "files over 4 GB (FAT32 can't hold them):"; echo "$big"; exit 1; }
    regions=$(find "$DATA/tiles" -mindepth 1 -maxdepth 1 -type d ! -name '_*' ! -name '*.v3' ! -name '*.old' ! -name groenlo)
    echo "== merging maps: $(echo $regions | xargs -n1 basename | tr '\n' ' ')"
    python3 "$HERE/maps/mergepacks.py" --out "$DATA/tiles/_card" $regions
    [ -f "$DATA/places.dgz" ] && cp "$DATA/places.dgz" "$DATA/tiles/_card/places.dgz"
    "$0" copy "$DATA/tiles/_card" "$DST"
    "$0" copy-dem "$DATA/dem-pack" "$DST"
    "$0" copy-lib "$DATA/lib" "$DST"
    echo "== card: $(df -h "$DST" | awk 'NR==2{print $3" used, "$4" free"}')"
    ;;
  *)
    sed -n 2,13p "$0"; exit 1 ;;
esac
