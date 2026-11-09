#!/usr/bin/env python3
"""Useful places for the device: hospitals, water, food, shelter, fuel...

Reads OSM extracts (.osm.pbf) and writes /doba/map/pois.dgp, the list the
device's NEARBY page searches: the nearest of each kind, with distance and
azimuth, offline.

    python3 pois.py out/pois.dgp data/osm/*.osm.pbf

Format (little-endian), like places.dgz:
    "DGP1", u32 count, u32 cells
    cells x (i16 lat_cell, i16 lon_cell, u32 first, u32 n)   sorted by (lat, lon)
    count x record (44 bytes): f32 lat, f32 lon, u8 kind, u8 flags, name[34] UTF-8, NUL padded
Cells are 0.25 degree; records grouped by cell, in cell order. A building
(a closed way) is placed at the mean of its nodes. Unnamed places are kept
where the kind says enough (a spring, a bunker); the device names them by kind.
"""
import math
import os
import struct
import sys
import tempfile

import osmium

# kind id -> (name). The device has the same table (doba_pois.cpp).
KINDS = {
    1: "Hospital", 2: "Doctor", 3: "Pharmacy", 4: "Water", 5: "Food shop", 6: "Farm",
    7: "Shelter", 8: "Bunker", 9: "Fuel", 10: "Police", 11: "Fire station",
    12: "Hunting & arms", 13: "Hardware", 14: "Outdoor shop", 15: "Campsite",
    16: "Assembly point",
}
NEED_NAME = {6}   # a farmyard without a name is just a yard


def kind_of(t):
    a, shop = t.get("amenity"), t.get("shop")
    hc = t.get("healthcare")
    if a == "hospital" or hc == "hospital":
        return 1
    if a in ("clinic", "doctors") or hc in ("clinic", "doctor", "centre"):
        return 2
    if a == "pharmacy" or hc == "pharmacy":
        return 3
    if a in ("drinking_water", "water_point") or t.get("man_made") in ("water_well", "water_tap") \
            or t.get("natural") == "spring":
        if t.get("drinking_water") == "no" or t.get("pump") == "no" and t.get("man_made") == "water_well":
            return 0
        return 4
    if shop in ("supermarket", "convenience", "greengrocer", "butcher", "bakery", "general", "deli",
                "food", "frozen_food"):
        return 5
    if shop == "farm" or t.get("place") == "farm" or t.get("landuse") == "farmyard":
        return 6
    if (a == "shelter" and t.get("shelter_type") not in ("public_transport", "bicycle_parking")) \
            or t.get("tourism") in ("wilderness_hut", "alpine_hut"):
        return 7
    if t.get("military") == "bunker" or t.get("building") == "bunker" or t.get("bunker_type"):
        return 8
    if a == "fuel":
        return 9
    if a == "police":
        return 10
    if a == "fire_station":
        return 11
    if shop in ("weapons", "hunting"):
        return 12
    if shop in ("hardware", "doityourself", "trade"):
        return 13
    if shop == "outdoor":
        return 14
    if t.get("tourism") == "camp_site":
        return 15
    if t.get("emergency") == "assembly_point":
        return 16
    return 0


class Pois(osmium.SimpleHandler):
    def __init__(self):
        super().__init__()
        self.found = {}

    def add(self, k, lat, lon, tags):
        name = tags.get("name") or ""
        if k in NEED_NAME and not name:
            return
        key = (k, round(lat, 4), round(lon, 4))   # the same place in two extracts
        self.found[key] = (lat, lon, k, name)

    def node(self, n):
        if not n.tags:
            return
        k = kind_of(n.tags)
        if k and n.location.valid():
            self.add(k, n.location.lat, n.location.lon, n.tags)

    def way(self, w):
        if not w.tags:
            return
        k = kind_of(w.tags)
        if not k:
            return
        la = lo = 0.0
        m = 0
        for nd in w.nodes:
            if nd.location.valid():
                la += nd.location.lat
                lo += nd.location.lon
                m += 1
        if m:
            self.add(k, la / m, lo / m, w.tags)


CELL = 0.25
REC = struct.Struct("<ffBB34s")


def clip(name, n=34):
    b = name.encode("utf-8")
    while len(b) > n:
        name = name[:-1]
        b = name.encode("utf-8")
    return b


def main():
    out, pbfs = sys.argv[1], sys.argv[2:]
    h = Pois()
    for p in pbfs:
        print(f"  reading {p}", flush=True)
        with tempfile.TemporaryDirectory(dir=os.path.dirname(os.path.abspath(out))) as tmp:
            h.apply_file(p, locations=True, idx=f"sparse_file_array,{tmp}/nodes.idx")
        print(f"    {len(h.found)} so far", flush=True)
    pois = list(h.found.values())
    cells = {}
    for lat, lon, k, name in pois:
        c = (math.floor(lat / CELL), math.floor(lon / CELL))
        cells.setdefault(c, []).append((lat, lon, k, name))
    order = sorted(cells)
    with open(out + ".part", "wb") as f:
        f.write(b"DGP1" + struct.pack("<II", len(pois), len(order)))
        first = 0
        for c in order:
            f.write(struct.pack("<hhII", c[0], c[1], first, len(cells[c])))
            first += len(cells[c])
        for c in order:
            for lat, lon, k, name in sorted(cells[c], key=lambda p: (p[2], p[3])):
                f.write(REC.pack(lat, lon, k, 0, clip(name)))
    os.replace(out + ".part", out)
    counts = {}
    for _, _, k, _ in pois:
        counts[k] = counts.get(k, 0) + 1
    print(f"{len(pois)} places in {len(order)} cells -> {out}")
    for k in sorted(counts):
        print(f"  {KINDS[k]:16} {counts[k]}")


if __name__ == "__main__":
    main()
