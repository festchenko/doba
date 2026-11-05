#!/bin/bash
# Doba build environment on this Mac: own Python and Java under ~/.doba-tools
# (no Homebrew needed), sources and data on this SSD. Sourced by the .command files.
SSD="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$SSD/doba"
export DOBA_BUILD="$SSD/data"
LOGS="$SSD/logs"
TOOLS="$HOME/.doba-tools"
mkdir -p "$LOGS" "$TOOLS" "$DOBA_BUILD"
[ -d "$SRC" ] || tar xzf "$SSD/doba-src.tgz" -C "$SSD"

PYDIR="$TOOLS/python"
if [ ! -x "$PYDIR/bin/python3" ]; then
  echo "== Python 3.12 (standalone)"
  curl -fL -o "$TOOLS/py.tgz" "https://github.com/astral-sh/python-build-standalone/releases/download/20241016/cpython-3.12.7%2B20241016-aarch64-apple-darwin-install_only.tar.gz"
  tar xzf "$TOOLS/py.tgz" -C "$TOOLS" && rm -f "$TOOLS/py.tgz"
fi
JH="$TOOLS/jdk-21.0.5+11/Contents/Home"
if [ ! -x "$JH/bin/java" ]; then
  echo "== Java 21 (Temurin)"
  curl -fL -o "$TOOLS/jdk.tgz" "https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.5%2B11/OpenJDK21U-jdk_aarch64_mac_hotspot_21.0.5_11.tar.gz"
  tar xzf "$TOOLS/jdk.tgz" -C "$TOOLS" && rm -f "$TOOLS/jdk.tgz"
fi
export JAVA_HOME="$JH"
export PATH="$JH/bin:$PYDIR/bin:$PATH"
if [ ! -f "$TOOLS/.pip-ok" ]; then
  echo "== Python packages"
  python3 -m pip install -q --upgrade pip
  python3 -m pip install -q -r "$SRC/maps/requirements.txt" libzim pymupdf && touch "$TOOLS/.pip-ok"
fi
# Don't let the Mac sleep while this runs (the lid may close: plug it in).
caffeinate -dimsu -w $$ &
