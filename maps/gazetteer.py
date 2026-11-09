#!/usr/bin/env python3
"""Place names for the device: "where am I" without a network.

Reads OSM extracts (.osm.pbf) and writes one file the map header uses to
name the nearest town or village: /doba/map/places.dgz.

    python3 gazetteer.py out/places.dgz data/osm/*.osm.pbf

Format (little-endian):
    "DGZ1", u32 count, u32 cells
    cells x (i16 lat_cell, i16 lon_cell, u32 first, u32 n)   sorted by (lat, lon)
    count x record (36 bytes): f32 lat, f32 lon, u8 rank, name[27] UTF-8, NUL padded
Cells are 0.25 degree; records are grouped by cell, in cell order. The device
reads the cell table (a few thousand entries) and the 3x3 cells round it.
rank: 5 city, 4 town, 3 village, 2 suburb / quarter, 1 hamlet / locality.
"""
import math
import struct
import sys

import osmium

RANK = {"city": 5, "town": 4, "village": 3, "suburb": 2, "quarter": 2, "neighbourhood": 1,
        "hamlet": 1, "isolated_dwelling": 0, "locality": 1}
CELL = 0.25
REC = struct.Struct("<ffB27s")


class Places(osmium.SimpleHandler):
    def __init__(self):
        super().__init__()
        self.found = {}

    def node(self, n):
        kind = n.tags.get("place")
        if kind not in RANK or RANK[kind] < 1:
            return
        name = n.tags.get("name")
        if not name:
            return
        lat, lon = n.location.lat, n.location.lon
        key = (name, round(lat, 2), round(lon, 2))   # the same place in two extracts
        self.found[key] = (lat, lon, RANK[kind], name)


def clip(name, n=27):
    b = name.encode("utf-8")
    while len(b) > n:
        name = name[:-1]
        b = name.encode("utf-8")
    return b


def main():
    out, pbfs = sys.argv[1], sys.argv[2:]
    h = Places()
    for p in pbfs:
        print(f"  reading {p}", flush=True)
        h.apply_file(p, locations=False)
    places = list(h.found.values())
    cells = {}
    for lat, lon, rank, name in places:
        c = (math.floor(lat / CELL), math.floor(lon / CELL))
        cells.setdefault(c, []).append((lat, lon, rank, name))
    order = sorted(cells)
    with open(out, "wb") as f:
        f.write(b"DGZ1" + struct.pack("<II", len(places), len(order)))
        first = 0
        for c in order:
            f.write(struct.pack("<hhII", c[0], c[1], first, len(cells[c])))
            first += len(cells[c])
        for c in order:
            for lat, lon, rank, name in sorted(cells[c], key=lambda p: -p[2]):
                f.write(REC.pack(lat, lon, rank, clip(name)))
    print(f"{len(places)} places in {len(order)} cells -> {out}")


if __name__ == "__main__":
    main()
