#!/usr/bin/env python3
"""Sea depths: EMODnet Bathymetry DTM 2024 (~115 m), European seas incl. the
Black Sea and the Sea of Azov.

Fetched per 1x1 degree from the EMODnet ERDDAP server (ESRI ASCII, true
metres; its GeoTIFF output is 8-bit) and cached as .npz:

    python3 bathy.py --bbox 3.2,50.7,7.3,53.6 --out build/bathy

Depths are elevations relative to Lowest Astronomical Tide: negative = below.
Land is NaN. Not for navigation (EMODnet's own words: not for safety at sea).
"""
import argparse
import math
import os
import subprocess
import sys

import numpy as np
from scipy.ndimage import map_coordinates

ERDDAP = "https://erddap.emodnet.eu/erddap/griddap/bathymetry_dtm_2024.esriAscii"
COVER = (-36.0, 15.0, 43.0, 90.0)   # west, south, east, north of the DTM


def cell_name(lat_i, lon_i):
    return f"{'N' if lat_i >= 0 else 'S'}{abs(lat_i):02d}{'E' if lon_i >= 0 else 'W'}{abs(lon_i):03d}"


def parse_asc(text):
    lines = text.splitlines()
    hdr = {}
    for ln in lines[:6]:
        k, v = ln.split()
        hdr[k.lower()] = float(v)
    a = np.array(" ".join(lines[6:]).split(), dtype=np.float32).reshape(int(hdr["nrows"]), int(hdr["ncols"]))
    a[a <= hdr.get("nodata_value", -9999999) + 1] = np.nan
    xll = hdr.get("xllcenter", hdr.get("xllcorner", 0) + hdr["cellsize"] / 2)
    yll = hdr.get("yllcenter", hdr.get("yllcorner", 0) + hdr["cellsize"] / 2)
    return a, xll, yll, hdr["cellsize"]


def fetch_cell(lat_i, lon_i, out):
    """Download one degree; writes NAME.npz, or NAME.none when it is all land."""
    name = cell_name(lat_i, lon_i)
    npz, none = os.path.join(out, name + ".npz"), os.path.join(out, name + ".none")
    if os.path.exists(npz) or os.path.exists(none):
        return "cached"
    w, s, e, n = COVER
    if not (w <= lon_i < e and s <= lat_i < n):
        open(none, "w").close()
        return "outside"
    url = f"{ERDDAP}?elevation%5B({lat_i}.0):1:({lat_i + 1}.0)%5D%5B({lon_i}.0):1:({lon_i + 1}.0)%5D"
    r = subprocess.run(["curl", "-fsL", "--retry", "4", "--retry-delay", "5", "-m", "300", url],
                       capture_output=True)
    if r.returncode != 0:
        return f"failed ({r.returncode})"
    a, xll, yll, cs = parse_asc(r.stdout.decode("ascii", "ignore"))
    if np.isnan(a).all():
        open(none, "w").close()
        return "land"
    np.savez_compressed(npz + ".part.npz", depth=a, xll=xll, yll=yll, cs=cs)
    os.replace(npz + ".part.npz", npz)
    return f"{np.nanmin(a):.0f}..{np.nanmax(a):.0f} m"


class Bathy:
    """Bilinear sea-floor elevation (m, negative below LAT) at lon/lat arrays; NaN on land."""

    def __init__(self, root):
        self.root = root
        self.cache = {}

    def available(self):
        return os.path.isdir(self.root) and any(f.endswith(".npz") for f in os.listdir(self.root))

    def _open(self, lat_i, lon_i):
        key = (lat_i, lon_i)
        if key not in self.cache:
            p = os.path.join(self.root, cell_name(lat_i, lon_i) + ".npz")
            if os.path.exists(p):
                z = np.load(p)
                self.cache[key] = (z["depth"], float(z["xll"]), float(z["yll"]), float(z["cs"]))
            else:
                self.cache[key] = None
        return self.cache[key]

    def sample(self, lon, lat):
        out = np.full(lon.shape, np.nan, dtype=np.float32)
        for la in range(math.floor(lat.min()), math.floor(lat.max()) + 1):
            for lo in range(math.floor(lon.min()), math.floor(lon.max()) + 1):
                sel = (lat >= la) & (lat < la + 1) & (lon >= lo) & (lon < lo + 1)
                if not sel.any():
                    continue
                c = self._open(la, lo)
                if c is None:
                    continue
                a, xll, yll, cs = c
                top = yll + (a.shape[0] - 1) * cs          # row 0 is the north edge
                rows = (top - lat[sel]) / cs
                cols = (lon[sel] - xll) / cs
                out[sel] = map_coordinates(a, [rows, cols], order=1, mode="nearest", cval=np.nan)
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bbox", required=True, help="west,south,east,north")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    w, s, e, n = (float(x) for x in a.bbox.split(","))
    os.makedirs(a.out, exist_ok=True)
    bad = 0
    for la in range(math.floor(s), math.ceil(n)):
        for lo in range(math.floor(w), math.ceil(e)):
            r = fetch_cell(la, lo, a.out)
            if r != "cached":
                print(f"   {cell_name(la, lo)}: {r}", flush=True)
            bad += r.startswith("failed")
    if bad:
        print(f"!! {bad} cells failed; rerun to retry", file=sys.stderr)


if __name__ == "__main__":
    main()
