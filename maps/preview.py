#!/usr/bin/env python3
"""Show what the Doba screen (960x540, 16 greys) will display.

Reads tile packs exactly the way the firmware will, and writes PNGs.

    python3 preview.py build/tiles/groenlo --at 52.042,6.617 --zoom 12,14,16
"""
import argparse
import os

import numpy as np
from PIL import Image, ImageDraw

from render import lonlat_to_tile
from tilepack import TILE, PackLibrary

SCREEN_W, SCREEN_H = 960, 540
MISSING = 8  # mid grey for tiles that are not in the packs


def screen(lib, lat, lon, z, w=SCREEN_W, h=SCREEN_H):
    cx, cy = lonlat_to_tile(lon, lat, z)
    left = cx * TILE - w / 2
    top = cy * TILE - h / 2
    out = np.full((h, w), MISSING, dtype=np.uint8)
    tx0, ty0 = int(left // TILE), int(top // TILE)
    tx1, ty1 = int((left + w - 1) // TILE), int((top + h - 1) // TILE)
    for ty in range(ty0, ty1 + 1):
        for tx in range(tx0, tx1 + 1):
            if not (0 <= tx < 2 ** z and 0 <= ty < 2 ** z):
                continue
            t = lib.get(z, tx, ty)
            if t is None:
                continue
            ox, oy = int(tx * TILE - left), int(ty * TILE - top)
            sx0, sy0 = max(0, -ox), max(0, -oy)
            sx1, sy1 = min(TILE, w - ox), min(TILE, h - oy)
            out[oy + sy0:oy + sy1, ox + sx0:ox + sx1] = t[sy0:sy1, sx0:sx1]
    img = Image.fromarray(out * 17, "L")
    d = ImageDraw.Draw(img)  # position marker, as the device will draw it
    mx, my = w // 2, h // 2
    d.ellipse([mx - 7, my - 7, mx + 7, my + 7], outline=0, width=3)
    d.ellipse([mx - 2, my - 2, mx + 2, my + 2], fill=0)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("packs", help="directory with base.dtp and 8/ subfolder")
    ap.add_argument("--at", required=True, help="lat,lon")
    ap.add_argument("--zoom", default="12,14,16")
    ap.add_argument("--out", default="preview")
    a = ap.parse_args()
    lat, lon = (float(v) for v in a.at.split(","))
    lib = PackLibrary(a.packs)
    os.makedirs(a.out, exist_ok=True)
    shots = []
    for z in (int(v) for v in a.zoom.split(",")):
        img = screen(lib, lat, lon, z)
        p = os.path.join(a.out, f"z{z}.png")
        img.save(p)
        shots.append(img)
        print(p)
    # one sheet with all zooms stacked, handy for a quick look on a phone
    sheet = Image.new("L", (SCREEN_W, (SCREEN_H + 10) * len(shots) - 10), 255)
    for i, im in enumerate(shots):
        sheet.paste(im, (0, i * (SCREEN_H + 10)))
    sheet.save(os.path.join(a.out, "sheet.png"))
    print(os.path.join(a.out, "sheet.png"))


if __name__ == "__main__":
    main()
