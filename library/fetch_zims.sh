#!/bin/bash
# Download the newest Kiwix ZIM of each book Doba carries, then convert them.
#
#   DOBA_BUILD=/Volumes/SSD/doba library/fetch_zims.sh          # download all
#   DOBA_BUILD=/Volumes/SSD/doba library/fetch_zims.sh convert  # zim -> doba books
#
# Edit BOOKS to taste. Sizes are rough; the converted books are smaller (text only).
set -euo pipefail
cd "$(dirname "$0")"
BUILD="${DOBA_BUILD:-build}"
ZIMS="$BUILD/zim"; OUT="$BUILD/lib"
mkdir -p "$ZIMS" "$OUT"
MIRROR="https://download.kiwix.org/zim"

# kiwix dir | file prefix | book id on the card | name on the device
BOOKS=(
  "wikipedia|wikipedia_en_all_nopic_|wiki_en|Wikipedia EN"
  "wikipedia|wikipedia_ru_all_nopic_|wiki_ru|Википедия RU"
  "other|mdwiki_en_all_maxi_|med_en|WikiMed EN"
  "other|zimgit-medicine_en_|medicine|Medicine (field)"
  "other|zimgit-post-disaster_en_|disaster|Post-disaster"
  "other|zimgit-water_en_|water|Water"
  "other|zimgit-food-preparation_en_|food|Food preparation"
  "other|zimgit-knots_en_|knots|Knots"
  "other|appropedia_en_all_maxi_|appropedia|Appropedia"
  "ifixit|ifixit_en_all_|ifixit|iFixit"
  "stack_exchange|chemistry.stackexchange.com_en_all_|chem_se|Chemistry Q&A"
  "stack_exchange|diy.stackexchange.com_en_all_|diy_se|DIY Q&A"
  "stack_exchange|woodworking.stackexchange.com_en_all_|wood_se|Woodworking Q&A"
)

latest() {   # newest file in a kiwix directory listing with this prefix
  curl -fsL "$MIRROR/$1/" | grep -o "href=\"$2[^\"]*\.zim\"" | sed 's/href="//; s/"$//' | sort | tail -1
}

if [ "${1:-}" = "convert" ]; then
  for b in "${BOOKS[@]}"; do
    IFS='|' read -r dir prefix id name <<< "$b"
    zim=$(ls "$ZIMS"/"$prefix"*.zim 2>/dev/null | sort | tail -1 || true)
    [ -n "$zim" ] || { echo "skip $id: no zim"; continue; }
    [ -f "$OUT/$id/index.dti" ] && { echo "skip $id: done"; continue; }
    echo "== $id <- $(basename "$zim")"
    python3 zim2doba.py "$zim" "$OUT/$id" --name "$name"
  done
  echo "books in $OUT — copy to the card: tools/prepare-sd-mac.sh copy-lib $OUT"
  exit 0
fi

for b in "${BOOKS[@]}"; do
  IFS='|' read -r dir prefix id name <<< "$b"
  f=$(latest "$dir" "$prefix" || true)
  [ -n "$f" ] || { echo "!! nothing for $prefix"; continue; }
  if [ -f "$ZIMS/$f" ]; then echo "have $f"; continue; fi
  echo "== $f"
  curl -fL -C - -o "$ZIMS/$f.part" "$MIRROR/$dir/$f" && mv "$ZIMS/$f.part" "$ZIMS/$f"
done
