#!/usr/bin/env python3
"""Build a small fake dataset around Groenlo to test the pipeline offline.

Creates test/build/synthetic.mbtiles (OpenMapTiles layer names, z8-14) and
test/build/dem/*.tif (Copernicus file names, invented hills). The geography
is made up; only the formats are real.
"""
import gzip
import math
import os
import sqlite3
import sys

import mapbox_vector_tile
import numpy as np
import rasterio
from rasterio.transform import from_origin
from shapely.geometry import LineString, Point, Polygon, box, mapping
from shapely import affinity

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from render import lonlat_to_tile, tile_range  # noqa: E402

OUT = os.path.join(os.path.dirname(__file__), "build")
BBOX = (6.50, 51.97, 6.75, 52.12)
EXTENT = 4096
rng = np.random.default_rng(7)


def ll(lon, lat):
    return (lon, lat)


def features():
    f = []

    def add(layer, geom, **props):
        f.append((layer, geom, props))

    # places
    add("place", Point(6.617, 52.042), name="Groenlo", **{"class": "town"}, rank=10)
    add("place", Point(6.566, 51.990), name="Lichtenvoorde", **{"class": "town"}, rank=11)
    add("place", Point(6.650, 52.100), name="Eibergen", **{"class": "village"}, rank=12)
    add("place", Point(6.560, 52.070), name="Beltrum", **{"class": "village"}, rank=13)
    add("place", Point(6.525, 52.005), name="Zieuwent", **{"class": "hamlet"}, rank=14)
    add("place", Point(6.700, 52.030), name="Київ-тест", **{"class": "hamlet"}, rank=14)
    add("place", Point(6.608, 52.047), name="Oost", **{"class": "suburb"}, rank=16)
    add("mountain_peak", Point(6.690, 52.075), name="Testberg", ele=118)

    # residential area + buildings
    add("landuse", Point(6.617, 52.042).buffer(0.012).simplify(0.001),
        **{"class": "residential"})
    add("landuse", Point(6.566, 51.990).buffer(0.010).simplify(0.001),
        **{"class": "residential"})
    add("landuse", box(6.625, 52.030, 6.640, 52.036), **{"class": "industrial"})
    for i in range(-9, 10):
        for j in range(-6, 7):
            if rng.random() < 0.55:
                cx = 6.617 + i * 0.0011 + rng.normal(0, 0.0001)
                cy = 52.042 + j * 0.0007 + rng.normal(0, 0.00007)
                b = box(cx - 0.00022, cy - 0.00012, cx + 0.00022, cy + 0.00012)
                add("building", affinity.rotate(b, rng.uniform(-30, 30)))

    # forests
    for (cx, cy, r) in [(6.54, 52.10, 0.02), (6.70, 51.99, 0.018),
                        (6.66, 52.075, 0.012), (6.52, 52.03, 0.01)]:
        pts = [(cx + r * (1 + 0.3 * math.sin(5 * a)) * math.cos(a) * 1.6,
                cy + r * (1 + 0.3 * math.cos(3 * a)) * math.sin(a))
               for a in np.linspace(0, 2 * math.pi, 40)]
        add("landcover", Polygon(pts), **{"class": "wood"})
    add("landcover", box(6.58, 52.08, 6.60, 52.09), **{"class": "wetland"})

    # water
    lake = Polygon([(6.595, 52.005), (6.610, 52.008), (6.615, 52.000),
                    (6.600, 51.996), (6.590, 52.000)])
    add("water", lake, **{"class": "lake"})
    add("water_name", Point(6.602, 52.002), name="Testmeer", **{"class": "lake"})
    slinge = LineString([(6.50, 52.015), (6.55, 52.025), (6.60, 52.036),
                         (6.62, 52.045), (6.66, 52.06), (6.75, 52.07)])
    add("waterway", slinge, **{"class": "river"}, name="Slinge")
    for k in range(5):
        x = 6.52 + k * 0.045
        add("waterway", LineString([(x, 51.97), (x + 0.01, 52.00), (x + 0.005, 52.03)]),
            **{"class": "stream"})

    # roads
    add("transportation", LineString([(6.50, 52.06), (6.58, 52.045), (6.617, 52.035),
                                      (6.68, 52.03), (6.75, 52.025)]),
        **{"class": "trunk"})
    add("transportation", LineString([(6.566, 51.97), (6.566, 51.99), (6.60, 52.02),
                                      (6.617, 52.042), (6.65, 52.10), (6.66, 52.12)]),
        **{"class": "primary"})
    add("transportation", LineString([(6.50, 52.10), (6.56, 52.07), (6.617, 52.042)]),
        **{"class": "secondary"})
    add("transportation", LineString([(6.52, 51.97), (6.525, 52.005), (6.56, 52.07)]),
        **{"class": "tertiary"})
    for i in range(-6, 7):
        add("transportation", LineString([(6.617 + i * 0.002, 52.034),
                                          (6.617 + i * 0.002, 52.050)]),
            **{"class": "minor"})
    for j in range(-4, 5):
        add("transportation", LineString([(6.605, 52.042 + j * 0.002),
                                          (6.630, 52.042 + j * 0.002)]),
            **{"class": "minor"})
    add("transportation", LineString([(6.50, 52.12), (6.75, 51.97)]),
        **{"class": "motorway"}, brunnel="")
    add("transportation", LineString([(6.66, 52.06), (6.68, 52.07), (6.69, 52.075)]),
        **{"class": "track"})
    add("transportation", LineString([(6.53, 52.09), (6.545, 52.10), (6.56, 52.11)]),
        **{"class": "path"}, subclass="footway")
    add("transportation", LineString([(6.50, 52.0), (6.62, 52.03), (6.75, 52.08)]),
        **{"class": "rail"})

    add("boundary", LineString([(6.735, 51.97), (6.72, 52.04), (6.74, 52.12)]),
        admin_level=2, maritime=0)
    return f


def tile_bounds(z, x, y):
    n = 2 ** z

    def lon(xx):
        return xx / n * 360 - 180

    def lat(yy):
        return math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * yy / n))))
    return lon(x), lat(y + 1), lon(x + 1), lat(y)


def to_tile_coords(geom, z, x, y):
    from shapely.ops import transform
    n = 2 ** z

    def f(xs, ys):
        xs, ys = np.asarray(xs, float), np.asarray(ys, float)
        fx = (xs + 180) / 360 * n
        fy = (1 - np.arcsinh(np.tan(np.radians(ys))) / np.pi) / 2 * n
        return (fx - x) * EXTENT, (fy - y) * EXTENT
    return transform(f, geom)


def build_mbtiles(path, feats):
    if os.path.exists(path):
        os.remove(path)
    db = sqlite3.connect(path)
    db.execute("CREATE TABLE metadata (name text, value text)")
    db.execute("CREATE TABLE tiles (zoom_level integer, tile_column integer, "
               "tile_row integer, tile_data blob)")
    db.executemany("INSERT INTO metadata VALUES (?,?)",
                   [("name", "synthetic"), ("format", "pbf")])
    n = 0
    for z in range(0, 15):
        x0, y0, x1, y1 = tile_range(BBOX, z)
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                w, s, e, nn = tile_bounds(z, x, y)
                pad = (e - w) * 64 / 4096
                clip = box(w - pad, s - pad, e + pad, nn + pad)
                layers = {}
                for lname, g, props in feats:
                    if not g.intersects(clip):
                        continue
                    gc = g if g.geom_type == "Point" else g.intersection(clip)
                    if gc.is_empty:
                        continue
                    tg = to_tile_coords(gc, z, x, y)
                    layers.setdefault(lname, []).append(
                        {"geometry": tg.wkt, "properties": props})
                if not layers:
                    continue
                data = mapbox_vector_tile.encode(
                    [{"name": k, "features": v} for k, v in layers.items()],
                    default_options={"y_coord_down": True, "extents": EXTENT})
                db.execute("INSERT INTO tiles VALUES (?,?,?,?)",
                           (z, x, 2 ** z - 1 - y, gzip.compress(data)))
                n += 1
    db.commit()
    db.close()
    return n


def build_dem(root):
    os.makedirs(root, exist_ok=True)
    for lat_i in (51, 52):
        name = f"Copernicus_DSM_COG_10_N{lat_i:02d}_00_E006_00_DEM"
        rows, cols = 1800, 1200  # coarser than the real 3600x2400
        lons = 6 + (np.arange(cols) + 0.5) / cols
        lats = lat_i + 1 - (np.arange(rows) + 0.5) / rows
        LON, LAT = np.meshgrid(lons, lats)
        e = 20 + 8 * (LAT - 51.9) * 10
        for cx, cy, h, r in [(6.69, 52.075, 95, 0.018), (6.53, 52.10, 50, 0.03),
                             (6.70, 51.99, 40, 0.02)]:
            e += h * np.exp(-(((LON - cx) / (r * 1.6)) ** 2 + ((LAT - cy) / r) ** 2))
        e += rng.normal(0, 0.8, e.shape)  # "trees": DSM noise
        prof = dict(driver="GTiff", height=rows, width=cols, count=1,
                    dtype="float32", crs="EPSG:4326",
                    transform=from_origin(6, lat_i + 1, 1 / cols, 1 / rows))
        with rasterio.open(os.path.join(root, name + ".tif"), "w", **prof) as ds:
            ds.write(e.astype("float32"), 1)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    n = build_mbtiles(os.path.join(OUT, "synthetic.mbtiles"), features())
    build_dem(os.path.join(OUT, "dem"))
    print(f"synthetic.mbtiles: {n} tiles; dem: 2 files")
