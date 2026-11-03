#!/usr/bin/env bash
# Download everything a region needs and build its vector tiles.
#
#   ./fetch.sh groenlo            # small test region, ~15 min on a MacBook Air
#   DOBA_BUILD=/Volumes/SSD/doba ./fetch.sh groenlo
#
# Needs: curl, Java 21+ (brew install openjdk@21), ~5 GB free for the test
# region (Planetiler's one-time water/coastline sources are ~2 GB of that).
set -euo pipefail
cd "$(dirname "$0")"

REGION="${1:?usage: ./fetch.sh <region>   (see regions/*.env)}"
# shellcheck disable=SC1090
source "regions/${REGION}.env"
BUILD="${DOBA_BUILD:-build}"
mkdir -p "$BUILD/osm" "$BUILD/dem" "$BUILD/sources"

echo "== region: $NAME  bbox: $BBOX  build dir: $BUILD"

# --- Planetiler -------------------------------------------------------------
JAR="$BUILD/planetiler.jar"
if [ ! -f "$JAR" ]; then
  echo "== downloading Planetiler"
  curl -fL -o "$JAR" \
    https://github.com/onthegomap/planetiler/releases/latest/download/planetiler.jar
fi

# --- OpenStreetMap extract --------------------------------------------------
PBF="$BUILD/osm/$(basename "$GEOFABRIK").osm.pbf"
if [ ! -f "$PBF" ]; then
  echo "== downloading OSM: $GEOFABRIK"
  curl -fL -C - -o "$PBF.part" "https://download.geofabrik.de/${GEOFABRIK}-latest.osm.pbf"
  mv "$PBF.part" "$PBF"
fi

# --- vector tiles ---------------------------------------------------------------
MBT="$BUILD/$NAME.mbtiles"
if [ ! -f "$MBT" ]; then
  echo "== building vector tiles (Planetiler)"
  java -Xmx"${PLANETILER_RAM:-4g}" -jar "$JAR" \
    --osm_path="$PBF" \
    --download --download_dir="$BUILD/sources" \
    --bounds="$BBOX" --maxzoom=14 \
    --output="$MBT" --force
fi

# --- elevation (Copernicus GLO-30, public on AWS) -------------------------------
echo "== downloading elevation tiles"
IFS=, read -r W S E N <<< "$BBOX"
lat0=$(python3 -c "import math;print(math.floor($S))")
lat1=$(python3 -c "import math;print(math.floor($N))")
lon0=$(python3 -c "import math;print(math.floor($W))")
lon1=$(python3 -c "import math;print(math.floor($E))")
for ((la = lat0; la <= lat1; la++)); do
  for ((lo = lon0; lo <= lon1; lo++)); do
    if [ "$la" -ge 0 ]; then ns=$(printf "N%02d" "$la"); else ns=$(printf "S%02d" $((-la))); fi
    if [ "$lo" -ge 0 ]; then ew=$(printf "E%03d" "$lo"); else ew=$(printf "W%03d" $((-lo))); fi
    name="Copernicus_DSM_COG_10_${ns}_00_${ew}_00_DEM"
    out="$BUILD/dem/$name.tif"
    [ -f "$out" ] && continue
    url="https://copernicus-dem-30m.s3.amazonaws.com/$name/$name.tif"
    if curl -fsL -o "$out.part" "$url"; then
      mv "$out.part" "$out"; echo "   $name"
    else
      rm -f "$out.part"; echo "   $name: none (sea)"
    fi
  done
done

echo
echo "== ready. Next:"
echo "python3 render.py --mbtiles $MBT --dem $BUILD/dem --bbox $BBOX --zoom $ZOOM --out $BUILD/tiles/$NAME"
