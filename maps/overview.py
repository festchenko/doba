#!/usr/bin/env python3
"""Adds the overview zooms (z4-z7) to a merged base.dtp, made from its z8 tiles.

The regions are rendered from z8 down to z16, so the card had nothing coarser
than z8 (~200 km across the screen) and the map went white when zoomed out
further. Each coarser tile here is its four children shrunk 2x: the mean of
each 2x2 block, pulled a third of the way toward its darkest pixel so a
one-pixel road or shore survives the halving instead of fading to paper.

    overview.py BASE.dtp [--min-zoom 4]     rewrites BASE.dtp in place
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tilepack import BASE_CELL_Z, TILE, PackReader, PackWriter  # noqa: E402

WHITE = 15


def shrink(children):
    """Four (TILE, TILE) children [[nw, ne], [sw, se]] (None = paper) -> one tile."""
    full = np.full((2 * TILE, 2 * TILE), WHITE, np.float32)
    for r in range(2):
        for c in range(2):
            t = children[r][c]
            if t is not None:
                full[r * TILE:(r + 1) * TILE, c * TILE:(c + 1) * TILE] = t
    blocks = full.reshape(TILE, 2, TILE, 2)
    mean = blocks.mean(axis=(1, 3))
    dark = blocks.min(axis=(1, 3))
    return np.clip(np.rint(mean - (mean - dark) / 3.0), 0, WHITE).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("base")
    ap.add_argument("--min-zoom", type=int, default=4)
    a = ap.parse_args()

    r = PackReader(a.base)
    tiles = {k: r.get(*k) for k in r.keys}
    r.close()
    top = max(z for z, _, _ in tiles)
    if top != 8:
        print(f"!! {a.base}: deepest zoom is {top}, expected 8", file=sys.stderr)
    level = {(x, y): t for (z, x, y), t in tiles.items() if z == top}
    out = {k: t for k, t in tiles.items() if k[0] >= top}         # z8 as it is
    for z in range(top - 1, a.min_zoom - 1, -1):
        parents = {(x >> 1, y >> 1) for x, y in level}
        nxt = {}
        for px, py in parents:
            kids = [[level.get((2 * px + c, 2 * py + r)) for c in range(2)] for r in range(2)]
            nxt[(px, py)] = shrink(kids)
        for (x, y), t in nxt.items():
            out[(z, x, y)] = t
        print(f"z{z}: {len(nxt)} tiles", flush=True)
        level = nxt

    w = PackWriter(a.base + ".new", BASE_CELL_Z, 0, 0)
    for (z, x, y), t in sorted(out.items()):
        w.add(z, x, y, t)
    w.close()
    os.replace(a.base + ".new", a.base)
    print(f"done: {len(out)} tiles -> {a.base} ({os.path.getsize(a.base) / 1e6:.1f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
