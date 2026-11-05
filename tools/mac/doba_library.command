#!/bin/bash
# Doba: download the library ZIMs from Kiwix, then convert them to Doba books.
# Double-click to start; double-click again to resume (downloads continue,
# finished books are skipped).
source "$(dirname "$0")/doba_env.sh"
exec > >(tee -a "$LOGS/library.log") 2>&1
echo "=== $(date)  library"
echo "downloading" > "$LOGS/library.status"
bash "$SRC/library/fetch_zims.sh"
echo "converting" > "$LOGS/library.status"
cd "$SRC/library" && bash ./fetch_zims.sh convert
echo "done $(date)" > "$LOGS/library.status"
echo "=== finished $(date). This window can be closed."
