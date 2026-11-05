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

# --- Planetiler's own sources (water, lakes, Natural Earth) ---------------------
# Fetched here with resumable curl: Planetiler's built-in downloader gives up
# after 30 s on a slow or shared line.
getsrc() {   # url file
  [ -f "$BUILD/sources/$2" ] && return 0
  echo "== downloading $2"
  for i in 1 2 3; do
    curl -fL --retry 5 --retry-delay 10 -C - -o "$BUILD/sources/$2.part" "$1" && \
      mv "$BUILD/sources/$2.part" "$BUILD/sources/$2" && return 0
    sleep 15
  done
  return 1
}
if ! getsrc https://osmdata.openstreetmap.de/download/water-polygons-split-3857.zip water-polygons-split-3857.zip; then
  echo "!! osmdata.openstreetmap.de unreachable: using placeholder ocean polygons (fine inland;"
  echo "   for coasts delete $BUILD/sources/water-polygons-split-3857.zip and rerun later)"
  python3 -c "import shapefile" 2>/dev/null || python3 -m pip install -q pyshp
  python3 placeholder_water.py "$BUILD/sources/water-polygons-split-3857.zip"
fi
getsrc https://naciscdn.org/naturalearth/packages/natural_earth_vector.sqlite.zip natural_earth_vector.sqlite.zip
getsrc https://github.com/acalcutt/osm-lakelines/releases/download/v12/lake_centerline.shp.zip lake_centerline.shp.zip

# --- vector tiles ---------------------------------------------------------------
MBT="$BUILD/$NAME.mbtiles"
if [ ! -f "$MBT" ]; then
  echo "== building vector tiles (Planetiler)"
  # Its temp files are memory-mapped: keep them off exFAT/FAT external disks.
  mkdir -p "${PLANETILER_TMP:-$HOME/.doba-tools/planetiler-tmp}"
  java -Xmx"${PLANETILER_RAM:-4g}" -jar "$JAR" \
    --osm_path="$PBF" \
    --download --download_dir="$BUILD/sources" --http_timeout=300s \
    --tmpdir="${PLANETILER_TMP:-$HOME/.doba-tools/planetiler-tmp}" \
    --download_osm_tile_weights=false \
    --bounds="$BBOX" --maxzoom=14 \
    --output="$BUILD/$NAME.part.mbtiles" --force
  mv "$BUILD/$NAME.part.mbtiles" "$MBT"   # only a finished build counts
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

# --- sea depths (EMODnet Bathymetry DTM 2024, European seas) --------------------
echo "== downloading sea depths (EMODnet)"
python3 bathy.py --bbox "$BBOX" --out "$BUILD/bathy" || echo "!! some depth cells failed; rerun to retry"

echo
echo "== ready. Next:"
echo "python3 render.py --mbtiles $MBT --dem $BUILD/dem --bbox $BBOX --zoom $ZOOM --out $BUILD/tiles/$NAME"
