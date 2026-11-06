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

# kiwix dir | file prefix | book id on the card | name on the device | source line
BOOKS=(
  "wikipedia|wikipedia_en_all_nopic_|wiki_en|Wikipedia EN|Wikipedia EN · CC BY-SA"
  "wikipedia|wikipedia_uk_all_nopic_|wiki_uk|Вікіпедія UA|Вікіпедія · CC BY-SA"
  "other|mdwiki_en_all_maxi_|med_en|WikiMed EN|WikiProject Medicine · CC BY-SA"
  "other|zimgit-medicine_en_|medicine|Medicine (field)|zimgit · public sources"
  "other|zimgit-post-disaster_en_|disaster|Post-disaster|zimgit · public sources"
  "other|zimgit-water_en_|water|Water|zimgit · public sources"
  "other|zimgit-food-preparation_en_|food|Food preparation|zimgit · public sources"
  "other|zimgit-knots_en_|knots|Knots|zimgit · public sources"
  "other|appropedia_en_all_maxi_|appropedia|Appropedia|Appropedia · CC BY-SA"
  "ifixit|ifixit_en_all_|ifixit|iFixit|iFixit · CC BY-NC-SA"
  "stack_exchange|chemistry.stackexchange.com_en_all_|chem_se|Chemistry Q&A|Chemistry SE · CC BY-SA"
  "stack_exchange|diy.stackexchange.com_en_all_|diy_se|DIY Q&A|DIY SE · CC BY-SA"
  "stack_exchange|woodworking.stackexchange.com_en_all_|wood_se|Woodworking Q&A|Woodworking SE · CC BY-SA"
  "stack_exchange|gardening.stackexchange.com_en_all_|garden_se|Gardening Q&A|Gardening SE · CC BY-SA"
  "gutenberg|gutenberg_en_all_|shelf_en|Gutenberg EN|Project Gutenberg · public domain"
  "gutenberg|gutenberg_ru_all_|shelf_ru|Gutenberg RU|Project Gutenberg · public domain"
)

latest() {   # newest file in a kiwix directory listing with this prefix
  curl -fsL "$MIRROR/$1/" | grep -o "href=\"$2[^\"]*\.zim\"" | sed 's/href="//; s/"$//' | sort | tail -1
}

if [ "${1:-}" = "convert" ]; then
  for b in "${BOOKS[@]}"; do
    IFS='|' read -r dir prefix id name source <<< "$b"
    zim=$(ls "$ZIMS"/"$prefix"*.zim 2>/dev/null | sort | tail -1 || true)
    [ -n "$zim" ] || { echo "skip $id: no zim"; continue; }
    [ -f "$OUT/$id/index.dti" ] && { echo "skip $id: done"; continue; }
    echo "== $id <- $(basename "$zim")"
    # zimgit-* collections are PDFs behind a little web app: unpack, then docs2doba.
    tmp="$OUT/.src-$id"
    rm -rf "$tmp"
    if python3 nautilus2dir.py "$zim" "$tmp" >/dev/null 2>&1; then
      python3 docs2doba.py "$tmp" "$OUT/$id" --name "$name" --source "$source" --titles-from-filenames
      rm -rf "$tmp"
    else
      python3 zim2doba.py "$zim" "$OUT/$id" --name "$name" --source "$source"
    fi
  done
  echo "books in $OUT — copy to the card: tools/prepare-sd-mac.sh copy-lib $OUT"
  exit 0
fi

for b in "${BOOKS[@]}"; do
  IFS='|' read -r dir prefix id name source <<< "$b"
  f=$(latest "$dir" "$prefix" || true)
  [ -n "$f" ] || { echo "!! nothing for $prefix"; continue; }
  if [ -f "$ZIMS/$f" ]; then echo "have $f"; continue; fi
  echo "== $f"
  curl -fL -C - -o "$ZIMS/$f.part" "$MIRROR/$dir/$f" && mv "$ZIMS/$f.part" "$ZIMS/$f"
done
