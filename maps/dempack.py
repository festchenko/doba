#!/usr/bin/env python3
"""Elevation for the device: Copernicus GLO-30 cut into 1x1 degree .dem files.

The device reads altitude from these when GPS height is poor or missing
(firmware src/doba_dem.*), and the height of waypoints.

    python3 dempack.py --dem build/dem --bbox 6.0,51.5,7.5,52.5 --out build/dem-pack
    -> build/dem-pack/N51E006.dem ...  copy to /doba/dem/ on the card

File layout (little-endian):
  Header, 32 bytes
    0   8s  magic       b"DOBADEM1"
    8   i16 lat         south edge (degrees)
    10  i16 lon         west edge (degrees)
    12  u16 n           samples per side (3600 / arcsec)
    14  u16 block       samples per block side
    16  u16 blocks      blocks per side (ceil(n / block))
    18  u16 reserved
    20  i16 min_m, i16 max_m
    24  8 bytes reserved
  Index, blocks*blocks entries of (u32 offset, u32 length), row-major from
  the north-west block.
  Data: per block a zlib stream of block*block int16 metres, rows north to
  south; each row is delta-coded (first sample, then differences), which
  zlib packs far better than raw heights. Edge blocks are padded by
  repeating the last row/column.

Sample (i, j) is the height at lat = south + 1 - (i + 0.5) / n,
lon = west + (j + 0.5) / n.
"""
import argparse
import math
import os
import struct
import sys
import zlib

import numpy as np

MAGIC = b"DOBADEM1"
HEADER = struct.Struct("<8shhHHHHhh8s")
assert HEADER.size == 32


def name_for(lat, lon):
    return f"{'N' if lat >= 0 else 'S'}{abs(lat):02d}{'E' if lon >= 0 else 'W'}{abs(lon):03d}.dem"


def encode(heights, lat, lon, block=100):
    """heights: (n, n) array of metres, north row first."""
    n = heights.shape[0]
    nb = math.ceil(n / block)
    h = np.clip(np.rint(heights), -32000, 32000).astype(np.int32)
    pad = np.pad(h, ((0, nb * block - n), (0, nb * block - n)), mode="edge")
    blobs = []
    for bi in range(nb):
        for bj in range(nb):
            b = pad[bi * block:(bi + 1) * block, bj * block:(bj + 1) * block]
            d = np.empty_like(b)
            d[:, 0] = b[:, 0]
            d[:, 1:] = np.diff(b, axis=1)
            blobs.append(zlib.compress(np.clip(d, -32768, 32767).astype("<i2").tobytes(), 9))
    head = HEADER.pack(MAGIC, lat, lon, n, block, nb, 0, int(h.min()), int(h.max()), b"\0" * 8)
    index_size = nb * nb * 8
    out = bytearray(head)
    off = len(head) + index_size
    idx = bytearray()
    for b in blobs:
        idx += struct.pack("<II", off, len(b))
        off += len(b)
    out += idx
    for b in blobs:
        out += b
    return bytes(out)


def decode_sample(data, i, j):
    """One sample, the way the firmware reads it (for tests)."""
    _, lat, lon, n, block, nb, _, _, _, _ = HEADER.unpack_from(data, 0)
    bi, bj = i // block, j // block
    off, ln = struct.unpack_from("<II", data, HEADER.size + (bi * nb + bj) * 8)
    d = np.frombuffer(zlib.decompress(data[off:off + ln]), dtype="<i2").reshape(block, block)
    row = d[i % block].astype(np.int32)
    return int(np.cumsum(row)[j % block])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dem", required=True, help="directory of Copernicus GLO-30 GeoTIFFs")
    ap.add_argument("--bbox", required=True, help="west,south,east,north")
    ap.add_argument("--out", required=True)
    ap.add_argument("--arcsec", type=int, default=3, choices=(1, 2, 3, 4, 6),
                    help="sample spacing (3 = ~90 m, ~1 MB/degree)")
    a = ap.parse_args()
    from dem import Dem
    dem = Dem(a.dem)
    if not dem.available():
        sys.exit(f"no DEM GeoTIFFs under {a.dem}")
    w, s, e, n_ = (float(x) for x in a.bbox.split(","))
    os.makedirs(a.out, exist_ok=True)
    n = 3600 // a.arcsec
    total = 0
    for lat in range(math.floor(s), math.ceil(n_)):
        for lon in range(math.floor(w), math.ceil(e)):
            path = os.path.join(a.out, name_for(lat, lon))
            if os.path.exists(path):
                continue
            k = (np.arange(n) + 0.5) / n
            lons, lats = np.meshgrid(lon + k, lat + 1 - k)
            hts = np.zeros((n, n), dtype=np.float32)
            step = 300   # rows at a time keeps memory flat
            for r in range(0, n, step):
                hts[r:r + step] = dem.sample(lons[r:r + step], lats[r:r + step])
            if not hts.any():
                continue   # open sea: no file; the device reads "unknown"
            data = encode(hts, lat, lon)
            with open(path, "wb") as f:
                f.write(data)
            total += len(data)
            print(f"{name_for(lat, lon)}  {len(data) / 1e6:.1f} MB  {hts.min():.0f}..{hts.max():.0f} m")
    print(f"done: {total / 1e6:.0f} MB -> {a.out}   (to the card: /doba/dem/)")


if __name__ == "__main__":
    main()
