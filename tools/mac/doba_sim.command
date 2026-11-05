#!/bin/bash
# Doba simulator: the real firmware on this Mac, in a browser tab, with the
# maps, heights and books from this SSD. Double-click; close the window to stop.
source "$(dirname "$0")/doba_env.sh"
FW="$SSD/firmware"
mkdir -p "$FW"
if [ ! -f "$FW/.src-ok" ] || [ "$SSD/doba-fw-src.tgz" -nt "$FW/.src-ok" ]; then
  tar xzf "$SSD/doba-fw-src.tgz" -C "$FW" && touch "$FW/.src-ok"
fi
python3 -c "import google.protobuf" 2>/dev/null || python3 -m pip install -q "protobuf>=5,<7"
# Build where the compiler can work fast and with real file modes: the internal disk.
BUILD="$HOME/.doba-tools/sim-src"
mkdir -p "$BUILD"
rsync -a --delete --exclude 'sim/build' --exclude '._*' "$FW/" "$BUILD/"
cd "$BUILD" && sh sim/build.sh || { echo "!! sim build failed"; exit 1; }
REGION="${1:-netherlands}"
(sleep 4; open "http://127.0.0.1:8090") &
python3 sim/simview.py --data "$DOBA_BUILD" --region "$REGION"
