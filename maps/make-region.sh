#!/bin/bash
# One command for a region: download, build vector tiles, render the e-ink
# packs and the screen previews.
#
#   DOBA_BUILD=/Volumes/SSD/doba maps/make-region.sh groenlo
set -euo pipefail
cd "$(dirname "$0")"
REGION="${1:?usage: make-region.sh <region>   (see regions/*.env)}"
# shellcheck disable=SC1090
source "regions/${REGION}.env"
BUILD="${DOBA_BUILD:-build}"
./fetch.sh "$REGION"
python3 render.py --mbtiles "$BUILD/$NAME.mbtiles" --dem "$BUILD/dem" \
  --bbox "$BBOX" --zoom "$ZOOM" --out "$BUILD/tiles/$NAME"
python3 dempack.py --dem "$BUILD/dem" --bbox "$BBOX" --out "$BUILD/dem-pack"
IFS=, read -r W S E N <<< "$BBOX"
CLAT=$(python3 -c "print(($S+$N)/2)"); CLON=$(python3 -c "print(($W+$E)/2)")
python3 preview.py "$BUILD/tiles/$NAME" --at "$CLAT,$CLON" --zoom 10,12,14,16 \
  --out "$BUILD/preview/$NAME"
echo
echo "== packs: $BUILD/tiles/$NAME   previews: $BUILD/preview/$NAME/sheet.png"
echo "   to the card: tools/prepare-sd-mac.sh copy $BUILD/tiles/$NAME"
echo "   heights:     tools/prepare-sd-mac.sh copy-dem $BUILD/dem-pack"
