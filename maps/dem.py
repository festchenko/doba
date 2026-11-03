"""Elevation sampling from Copernicus GLO-30 1°x1° GeoTIFF tiles.

Files are expected under one directory with their original names, e.g.
Copernicus_DSM_COG_10_N52_00_E006_00_DEM.tif. Missing tiles (open sea)
read as 0 m.
"""
import math
import os

import numpy as np
import rasterio
from rasterio.windows import Window
from scipy.ndimage import map_coordinates


def tile_name(lat_i, lon_i):
    ns = "N" if lat_i >= 0 else "S"
    ew = "E" if lon_i >= 0 else "W"
    return (f"Copernicus_DSM_COG_10_{ns}{abs(lat_i):02d}_00_"
            f"{ew}{abs(lon_i):03d}_00_DEM")


class Dem:
    def __init__(self, root):
        self.root = root
        self.cache = {}

    def _open(self, lat_i, lon_i):
        key = (lat_i, lon_i)
        if key not in self.cache:
            name = tile_name(lat_i, lon_i)
            path = os.path.join(self.root, name + ".tif")
            if not os.path.exists(path):
                path = os.path.join(self.root, name, name + ".tif")
            self.cache[key] = rasterio.open(path) if os.path.exists(path) else None
        return self.cache[key]

    def available(self):
        return os.path.isdir(self.root) and any(
            f.endswith(".tif") for _, _, fs in os.walk(self.root) for f in fs)

    def sample(self, lon, lat):
        """Bilinear elevation (m) at arrays of lon/lat (same shape)."""
        out = np.zeros(lon.shape, dtype=np.float32)
        lat_cells = range(math.floor(lat.min()), math.floor(lat.max()) + 1)
        lon_cells = range(math.floor(lon.min()), math.floor(lon.max()) + 1)
        for la in lat_cells:
            for lo in lon_cells:
                sel = ((lat >= la) & (lat < la + 1) &
                       (lon >= lo) & (lon < lo + 1))
                if not sel.any():
                    continue
                ds = self._open(la, lo)
                if ds is None:
                    continue
                inv = ~ds.transform
                cols, rows = inv * (lon[sel], lat[sel])
                # pixel centres are at +0.5
                cols = np.asarray(cols) - 0.5
                rows = np.asarray(rows) - 0.5
                c0 = max(int(np.floor(cols.min())) - 1, 0)
                r0 = max(int(np.floor(rows.min())) - 1, 0)
                c1 = min(int(np.ceil(cols.max())) + 2, ds.width)
                r1 = min(int(np.ceil(rows.max())) + 2, ds.height)
                if c1 <= c0 or r1 <= r0:
                    continue
                win = ds.read(1, window=Window(c0, r0, c1 - c0, r1 - r0),
                              out_dtype="float32")
                nod = ds.nodata
                if nod is not None:
                    win[win == nod] = 0
                out[sel] = map_coordinates(win, [rows - r0, cols - c0],
                                           order=1, mode="nearest")
        return out
