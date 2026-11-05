#!/bin/bash
# Doba: maps for a region (default: the Groenlo test region), heights included.
# Double-click to start; double-click again to resume after a stop.
source "$(dirname "$0")/doba_env.sh"
REGION="${1:-groenlo}"
exec > >(tee -a "$LOGS/maps-$REGION.log") 2>&1
echo "=== $(date)  maps: $REGION"
echo "running" > "$LOGS/maps-$REGION.status"
if bash "$SRC/maps/make-region.sh" "$REGION"; then
  echo "done $(date)" > "$LOGS/maps-$REGION.status"
else
  echo "FAILED $(date)" > "$LOGS/maps-$REGION.status"
fi
echo "=== finished $(date). This window can be closed."
