#!/usr/bin/env python3
"""Render 4-bit e-ink map tiles for Doba.

Input:  OpenMapTiles vector tiles (.mbtiles from Planetiler, max z14)
        + optional Copernicus GLO-30 DEM tiles for relief.
Output: Doba tile packs (.dtp), one per z8 cell plus base.dtp for z0-8.

Example:
    python3 render.py --mbtiles build/groenlo.mbtiles --dem build/dem \
        --bbox 6.50,51.97,6.75,52.12 --zoom 8-16 --out build/tiles/groenlo
"""
import argparse
import gzip
import math
import multiprocessing as mp
import os
import sqlite3
import sys
import time

import mapbox_vector_tile
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy.ndimage import binary_erosion, gaussian_filter
from skimage.measure import find_contours

import style as S
from dem import Dem
from tilepack import BASE_CELL_Z, DETAIL_Z, TILE, PackWriter, pack_path

HERE = os.path.dirname(os.path.abspath(__file__))
SS = 2           # supersampling factor
BUFFER = 64      # px around a metatile, rendered then cropped (1x)
META = 8         # metatile = META x META tiles
CELL_Z = 8
VECTOR_MAX_Z = 14

# ---------------------------------------------------------------------------
# tile maths (Web Mercator, XYZ)


def lonlat_to_tile(lon, lat, z):
    n = 2 ** z
    x = (lon + 180.0) / 360.0 * n
    lat_r = math.radians(max(min(lat, 85.0511), -85.0511))
    y = (1.0 - math.asinh(math.tan(lat_r)) / math.pi) / 2.0 * n
    return x, y


def tile_to_lonlat(x, y, z):
    n = 2 ** z
    lon = x / n * 360.0 - 180.0
    lat = np.degrees(np.arctan(np.sinh(np.pi * (1 - 2 * np.asarray(y) / n))))
    return lon, lat


def tile_range(bbox, z):
    w, s, e, n = bbox
    x0, y0 = lonlat_to_tile(w, n, z)
    x1, y1 = lonlat_to_tile(e, s, z)
    lim = 2 ** z - 1
    return (max(int(x0), 0), max(int(y0), 0),
            min(int(x1), lim), min(int(y1), lim))


# ---------------------------------------------------------------------------
# vector tile source


class VectorSource:
    def __init__(self, path):
        self.db = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        self.cache = {}

    def get(self, z, x, y):
        key = (z, x, y)
        if key in self.cache:
            return self.cache[key]
        row = self.db.execute(
            "SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? "
            "AND tile_row=?", (z, x, (2 ** z - 1) - y)).fetchone()
        layers = None
        if row:
            data = row[0]
            if data[:2] == b"\x1f\x8b":
                data = gzip.decompress(data)
            layers = mapbox_vector_tile.decode(
                data, default_options={"y_coord_down": True})
        if len(self.cache) > 256:
            self.cache.clear()
        self.cache[key] = layers
        return layers


# ---------------------------------------------------------------------------
# drawing helpers


def iter_lines(geom):
    t, c = geom["type"], geom["coordinates"]
    if t == "LineString":
        yield c
    elif t == "MultiLineString":
        yield from c
    elif t == "Polygon":
        yield from c
    elif t == "MultiPolygon":
        for poly in c:
            yield from poly


def iter_polygons(geom):
    t, c = geom["type"], geom["coordinates"]
    if t == "Polygon":
        yield c
    elif t == "MultiPolygon":
        yield from c


def iter_points(geom):
    t, c = geom["type"], geom["coordinates"]
    if t == "Point":
        yield c
    elif t == "MultiPoint":
        yield from c


def dashed(points, pattern):
    """Split a polyline into the 'on' parts of a dash pattern (canvas px).
    pattern = (on, off, on, off, ...), cycled along the line."""
    out, cur = [], []
    k, left = 0, pattern[0]
    for (x0, y0), (x1, y1) in zip(points, points[1:]):
        seg = math.hypot(x1 - x0, y1 - y0)
        pos = 0.0
        while seg - pos > 1e-6:
            step = min(left, seg - pos)
            t0, t1 = pos / seg, (pos + step) / seg
            a = (x0 + (x1 - x0) * t0, y0 + (y1 - y0) * t0)
            b = (x0 + (x1 - x0) * t1, y0 + (y1 - y0) * t1)
            if k % 2 == 0:
                if not cur:
                    cur.append(a)
                cur.append(b)
            pos += step
            left -= step
            if left <= 1e-6:
                if k % 2 == 0 and len(cur) > 1:
                    out.append(cur)
                cur = []
                k = (k + 1) % len(pattern)
                left = pattern[k]
    if k % 2 == 0 and len(cur) > 1:
        out.append(cur)
    return out


def px(w):
    """1x width -> integer canvas width (at least 1)."""
    return max(1, int(round(w * SS)))


class Canvas:
    """One metatile being drawn at SS x resolution with a buffer."""

    def __init__(self, z, mx0, my0, size_tiles):
        self.z = z
        self.mx0, self.my0 = mx0, my0
        self.n = size_tiles
        self.core = size_tiles * TILE * SS
        self.buf = BUFFER * SS
        self.size = self.core + 2 * self.buf
        self.fill = np.full((self.size, self.size), S.BACKGROUND * 17,
                            dtype=np.uint8)
        self.img = None
        self.draw = None
        self.labels = []  # placed label boxes

    # --- coordinate transforms -------------------------------------------
    def setup_vector(self, vz):
        self.vz = vz
        self.vscale = TILE * SS * 2 ** (self.z - vz)  # canvas px per vz tile
        self.ox = self.mx0 / 2 ** (self.z - vz)
        self.oy = self.my0 / 2 ** (self.z - vz)

    def to_canvas(self, tx, ty, extent, pts):
        k = self.vscale / extent
        bx = (tx - self.ox) * self.vscale + self.buf
        by = (ty - self.oy) * self.vscale + self.buf
        return [(bx + p[0] * k, by + p[1] * k) for p in pts]

    def canvas_lonlat(self, step):
        """lon/lat grids sampled every `step` canvas px."""
        n = self.size // step + 1
        idx = np.arange(n) * step
        tx = self.mx0 + (idx - self.buf) / (TILE * SS)
        ty = self.my0 + (idx - self.buf) / (TILE * SS)
        lon, _ = tile_to_lonlat(tx, 0, self.z)
        _, lat = tile_to_lonlat(0, ty, self.z)
        return np.meshgrid(lon, lat)

    # --- stages -------------------------------------------------------------
    def start_lines(self):
        self.img = Image.fromarray(self.fill, "L")
        self.draw = ImageDraw.Draw(self.img)

    def line(self, pts, width, level, dash=None):
        if len(pts) < 2:
            return
        w = px(width)
        col = level * 17
        if dash:
            for part in dashed(pts, [d * SS for d in dash]):
                self.draw.line(part, fill=col, width=w)
        else:
            self.draw.line(pts, fill=col, width=w, joint="curve")

    def place_label(self, x, y, text, font, level=0, halo=S.LABEL_HALO):
        hs = px(halo)
        box = self.draw.textbbox((x, y), text, font=font, anchor="mm",
                                 stroke_width=hs)
        lo, hi = self.buf, self.buf + self.core
        if box[0] < lo or box[1] < lo or box[2] > hi or box[3] > hi:
            return False  # only labels fully inside the core: no cut text
        pad = 3 * SS
        for b in self.labels:
            if not (box[2] + pad < b[0] or box[0] - pad > b[2] or
                    box[3] + pad < b[1] or box[1] - pad > b[3]):
                return False
        self.draw.text((x, y), text, font=font, fill=level * 17, anchor="mm",
                       stroke_width=hs, stroke_fill=S.WHITE * 17)
        self.labels.append(box)
        return True

    def finish(self):
        """Downsample, crop buffer, quantize -> dict {(x, y): levels}."""
        img = self.img if self.img is not None else Image.fromarray(self.fill)
        small = img.resize((self.size // SS, self.size // SS), Image.BOX)
        arr = np.asarray(small, dtype=np.float32)
        b = BUFFER
        arr = arr[b:b + self.n * TILE, b:b + self.n * TILE]
        # Snap to the greys the panel can show (style.PANEL_LEVELS).
        panel = np.array(S.PANEL_LEVELS, dtype=np.float32) * 17.0
        idx = np.abs(arr[..., None] - panel[None, None, :]).argmin(axis=-1)
        levels = np.array(S.PANEL_LEVELS, dtype=np.uint8)[idx]
        out = {}
        for j in range(self.n):
            for i in range(self.n):
                out[(self.mx0 + i, self.my0 + j)] = levels[
                    j * TILE:(j + 1) * TILE, i * TILE:(i + 1) * TILE]
        return out


# ---------------------------------------------------------------------------
# fonts

_fonts = {}


def font(name, size):
    key = (name, size)
    if key not in _fonts:
        _fonts[key] = ImageFont.truetype(
            os.path.join(HERE, "fonts", name), int(round(size * SS)))
    return _fonts[key]


# ---------------------------------------------------------------------------
# street names: along the road, upright, on its longest straight stretch


def _runs(pts, max_turn=0.35):
    """Straight-ish runs of a polyline: (x0, y0, x1, y1, length)."""
    out, i = [], 0
    while i < len(pts) - 1:
        a = math.atan2(pts[i + 1][1] - pts[i][1], pts[i + 1][0] - pts[i][0])
        j = i + 1
        while j < len(pts) - 1:
            b = math.atan2(pts[j + 1][1] - pts[j][1], pts[j + 1][0] - pts[j][0])
            d = abs((b - a + math.pi) % (2 * math.pi) - math.pi)
            if d > max_turn:
                break
            j += 1
        x0, y0 = pts[i]
        x1, y1 = pts[j]
        out.append((x0, y0, x1, y1, math.hypot(x1 - x0, y1 - y0)))
        i = j
    return out


def along_label(cv, text, fnt, x0, y0, x1, y1, level=0):
    """Text centred on a straight run, turned to its angle, kept upright;
    False when it would cut the edge or touch another label."""
    hs = px(S.LABEL_HALO)
    tw = cv.draw.textlength(text, font=fnt)
    mx, my = (x0 + x1) / 2, (y0 + y1) / 2
    ang = math.degrees(math.atan2(y1 - y0, x1 - x0))
    if ang > 90:
        ang -= 180
    if ang < -90:
        ang += 180
    asc, desc = fnt.getmetrics()
    w, h = int(tw) + 2 * hs + 4, asc + desc + 2 * hs + 4
    halo = Image.new("L", (w, h), 0)
    ink = Image.new("L", (w, h), 0)
    ImageDraw.Draw(halo).text((w / 2, h / 2), text, font=fnt, fill=255, anchor="mm",
                              stroke_width=hs, stroke_fill=255)
    ImageDraw.Draw(ink).text((w / 2, h / 2), text, font=fnt, fill=255, anchor="mm")
    halo = halo.rotate(-ang, resample=Image.BICUBIC, expand=True)
    ink = ink.rotate(-ang, resample=Image.BICUBIC, expand=True)
    bx, by = int(mx - halo.width / 2), int(my - halo.height / 2)
    box = (bx, by, bx + halo.width, by + halo.height)
    lo, hi = cv.buf, cv.buf + cv.core
    if box[0] < lo or box[1] < lo or box[2] > hi or box[3] > hi:
        return False
    sh = 0.3 if 20 < abs(ang) < 70 else 0.0
    cb = (box[0] + halo.width * sh / 2, box[1] + halo.height * sh / 2,
          box[2] - halo.width * sh / 2, box[3] - halo.height * sh / 2)
    pad = 4 * SS
    if any(not (cb[2] + pad < b[0] or cb[0] - pad > b[2] or cb[3] + pad < b[1] or cb[1] - pad > b[3])
           for b in cv.labels):
        return False
    cv.img.paste(S.WHITE * 17, box, halo)
    cv.img.paste(level * 17, box, ink)
    cv.labels.append(cb)
    return True


def contour_labels(cv, contours):
    rule = S.CONTOUR_LABEL
    fnt = font(S.FONT_REGULAR, rule["font"])
    placed = []
    for is_index, pts, level in contours:
        if not is_index or len(pts) < 2:
            continue
        text = ("%g" % level)
        tw = cv.draw.textlength(text, font=fnt)
        for x0, y0, x1, y1, length in sorted(_runs(pts, 0.25), key=lambda r: -r[4]):
            if length < tw + 16 * SS:
                break
            mx, my = (x0 + x1) / 2, (y0 + y1) / 2
            if any(math.hypot(mx - a, my - b) < rule["every_px"] * SS for a, b in placed):
                continue
            if along_label(cv, text, fnt, x0, y0, x1, y1, S.DARK):
                placed.append((mx, my))
                break


def street_names(cv, feats, z):
    rule = S.STREET_NAME
    fnt = font(S.FONT_REGULAR, rule["font"])
    hs = px(S.LABEL_HALO)
    rank = {c: i for i, c in enumerate(rule["classes"])}
    cands = []
    for tx, ty, ext, f in feats.get("transportation_name", []):
        p = f["properties"]
        name, cls = p.get("name"), p.get("class")
        if not name or cls not in rank:
            continue
        for ln in iter_lines(f["geometry"]):
            pts = cv.to_canvas(tx, ty, ext, ln)
            for run in _runs(pts):
                cands.append((rank[cls], -run[4], name, run))
    cands.sort()
    lo, hi = cv.buf, cv.buf + cv.core
    pad = 4 * SS
    placed_names = {}
    for _, _, name, (x0, y0, x1, y1, length) in cands:
        tw = cv.draw.textlength(name, font=fnt)
        if length < tw + 24 * SS:
            continue
        mx, my = (x0 + x1) / 2, (y0 + y1) / 2
        # one copy of a name per stretch of screen
        far = all(math.hypot(mx - a, my - b) > rule["every_px"] * SS
                  for a, b in placed_names.get(name, []))
        if not far:
            continue
        if not along_label(cv, name, fnt, x0, y0, x1, y1):
            continue
        placed_names.setdefault(name, []).append((mx, my))


# ---------------------------------------------------------------------------
# metatile renderer


def collect_features(src, cv):
    """Gather decoded features from all vector tiles touching the canvas."""
    z = cv.z
    vz = min(z, VECTOR_MAX_Z)
    cv.setup_vector(vz)
    span = cv.n / 2 ** (z - vz)
    bufv = BUFFER / TILE / 2 ** (z - vz)
    x0 = int(math.floor(cv.ox - bufv))
    y0 = int(math.floor(cv.oy - bufv))
    x1 = int(math.floor(cv.ox + span + bufv - 1e-9))
    y1 = int(math.floor(cv.oy + span + bufv - 1e-9))
    lim = 2 ** vz - 1
    feats = {}
    for ty in range(max(y0, 0), min(y1, lim) + 1):
        for tx in range(max(x0, 0), min(x1, lim) + 1):
            layers = src.get(vz, tx, ty)
            if not layers:
                continue
            for lname, layer in layers.items():
                ext = layer.get("extent", 4096)
                for f in layer["features"]:
                    feats.setdefault(lname, []).append((tx, ty, ext, f))
    return feats


def fill_mask(cv, items):
    """Rasterise polygons to a boolean mask at canvas resolution."""
    m = Image.new("1", (cv.size, cv.size), 0)
    d = ImageDraw.Draw(m)
    for tx, ty, ext, f in items:
        for poly in iter_polygons(f["geometry"]):
            rings = [cv.to_canvas(tx, ty, ext, r) for r in poly]
            if len(rings[0]) >= 3:
                d.polygon(rings[0], fill=1)
            for hole in rings[1:]:
                if len(hole) >= 3:
                    d.polygon(hole, fill=0)
    return np.asarray(m, dtype=bool)


def apply_fill(cv, mask, level):
    cv.fill[mask] = level * 17


def apply_edge(cv, mask, level, width=1.0):
    it = max(1, int(round(width * SS)))
    edge = mask & ~binary_erosion(mask, iterations=it)
    cv.fill[edge] = level * 17


def pattern_mask(cv, kind):
    """Boolean texture for the canvas, aligned to WORLD pixels so it runs on
    seamlessly from one metatile (and one tile) to the next."""
    sp = S.PATTERN_SPACING[kind] * SS
    ox = (cv.mx0 * TILE - BUFFER) * SS
    oy = (cv.my0 * TILE - BUFFER) * SS
    x = (np.arange(cv.size) + ox)[None, :]
    y = (np.arange(cv.size) + oy)[:, None]
    row = (y // sp) % 2
    if kind == "dots":      # staggered 2x2 (1x) dots
        return ((y % sp) < SS) & (((x + row * (sp // 2)) % sp) < SS)
    if kind == "hatch":     # 45 degree lines, 1 px (1x) wide
        return ((x + y) % sp) < SS
    if kind == "dash":      # short horizontal dashes, staggered rows
        return ((y % sp) < SS) & (((x + row * (sp // 2)) % (2 * sp)) < 3 * SS)
    if kind == "lines":     # horizontal water lining, like an engraved sheet
        return (y % sp) < SS
    if kind == "grain":     # sparse fine dots
        return ((y % (2 * sp)) < SS) & (((x + row * sp) % (2 * sp)) < SS)
    raise ValueError(kind)


def relief(cv, dem):
    z = cv.z
    step = 4 * SS
    lon, lat = cv.canvas_lonlat(step)
    elev = dem.sample(lon, lat)
    lat_c = float(np.mean(lat))
    m_per_px = 156543.03 * math.cos(math.radians(lat_c)) / 2 ** z / SS
    m_per_cell = m_per_px * step
    flat_dem = os.environ.get("DOBA_CONTOURS") == "flat"
    sigma = min(max((S.DEM_SMOOTH_M_FLAT if flat_dem else S.DEM_SMOOTH_M) / m_per_cell, 0.5), 20)
    elev = gaussian_filter(elev, sigma)
    out = {}
    if z >= S.HILLSHADE["min_zoom"]:
        gy, gx = np.gradient(elev, m_per_cell)
        slope = np.arctan(np.hypot(gx, gy))
        aspect = np.arctan2(-gx, gy)
        az, alt = math.radians(315), math.radians(45)
        shade = (math.sin(alt) * np.cos(slope) +
                 math.cos(alt) * np.sin(slope) * np.cos(az - aspect))
        factor = np.clip(shade / math.sin(alt), 0, 1)
        strength = (S.HILLSHADE["strength_low"] if z <= 12
                    else S.HILLSHADE["strength_high"])
        mult = 1 - strength * (1 - factor)
        img = Image.fromarray((mult * 255).astype(np.uint8), "L").resize(
            (cv.size, cv.size), Image.BILINEAR)
        m = np.asarray(img, dtype=np.float32) / 255.0
        cv.fill = (cv.fill.astype(np.float32) * m).astype(np.uint8)
    flat = os.environ.get("DOBA_CONTOURS") == "flat"
    iv = S.size_for_zoom(S.CONTOUR_INTERVALS_FLAT if flat else S.CONTOUR_INTERVALS, z)
    if iv:
        interval, index = iv
        lo = math.ceil(elev.min() / interval) * interval
        hi = math.floor(elev.max() / interval) * interval
        lines = []
        min_loop = (S.CONTOUR_MIN_LOOP_PX_FLAT if flat else S.CONTOUR_MIN_LOOP_PX) * SS
        for level in np.arange(lo, hi + 1, interval):
            for c in find_contours(elev, level):
                pts = c[:, ::-1] * step
                closed = np.allclose(pts[0], pts[-1])
                if closed:
                    length = np.hypot(*np.diff(pts, axis=0).T).sum()
                    if length < min_loop:
                        continue  # DSM noise: trees, houses, tiny bumps
                lines.append((abs(level % index) < 1e-6, [tuple(p) for p in pts], float(level)))
        out["contours"] = lines
    return out


def sea(cv, bathy):
    """Depth grid (m, positive; NaN on land) every 4 canvas px, smoothed."""
    step = 4 * SS
    lon, lat = cv.canvas_lonlat(step)
    d = -bathy.sample(lon, lat)
    if np.isnan(d).all() or np.nanmax(d) <= 0:
        return None
    filled = np.where(np.isnan(d), 0.0, np.maximum(d, 0.0)).astype(np.float32)
    return step, gaussian_filter(filled, 0.8)


def isobaths(cv, depth):
    """[(bold, depth, pts)] for this zoom's isobath levels."""
    step, d = depth
    levels = S.size_for_zoom(S.BATHY_LEVELS, cv.z) or ()
    out = []
    for lv in levels:
        if lv > d.max():
            continue
        for c in find_contours(d, lv):
            pts = c[:, ::-1] * step
            if len(pts) < 3:
                continue
            out.append((lv in S.BATHY_BOLD, lv, [tuple(p) for p in pts]))
    return out


def render_metatile(src, dem, z, mx0, my0, n, bathy=None):
    cv = Canvas(z, mx0, my0, n)
    feats = collect_features(src, cv)

    # 1. area fills, then textured areas ------------------------------------
    def items_of(layer, cls):
        return [it for it in feats.get(layer, [])
                if it[3]["properties"].get("class") == cls]

    for layer, cls, minz, level in S.AREA_FILLS:
        maxz = S.AREA_PATTERN_MAX_Z.get(cls, 99)
        items = items_of(layer, cls) if minz <= z <= maxz else []
        if items:
            apply_fill(cv, fill_mask(cv, items), level)
    textures = {}
    for layer, cls, minz, kind, level in S.AREA_PATTERNS:
        maxz = S.AREA_PATTERN_MAX_Z.get(cls, 99)
        items = items_of(layer, cls) if minz <= z <= maxz else []
        if not items:
            continue
        mask = fill_mask(cv, items)
        if kind not in textures:
            textures[kind] = pattern_mask(cv, kind)
        cv.fill[mask & textures[kind]] = level * 17
        edge = S.PATTERN_EDGE.get(kind)
        if edge is not None and z >= 11:
            apply_edge(cv, mask, edge, 1.0)

    # 2. relief --------------------------------------------------------------
    rel = relief(cv, dem) if dem is not None else {}

    # 3. water areas (mask edge = shoreline without tile seams) -------------
    depth = sea(cv, bathy) if bathy is not None else None
    seamask = None
    if depth is not None:
        step, d = depth
        img = Image.fromarray(((d > S.SEA_FROM_DEPTH_M) * 255).astype(np.uint8), "L")
        seamask = np.asarray(img.resize((cv.size, cv.size), Image.BILINEAR)) > 127
    if z >= S.WATER_MIN_ZOOM and (feats.get("water") or seamask is not None):
        wmask = fill_mask(cv, feats["water"]) if feats.get("water") else np.zeros((cv.size, cv.size), bool)
        if seamask is not None:
            wmask = wmask | seamask
        apply_fill(cv, wmask, S.WATER_FILL)
        if z >= S.WATER_LINING_FROM:   # lined, so water never reads as a roof
            apply_fill(cv, wmask & pattern_mask(cv, "lines"), S.WATER_LINING)
        if z >= 9:
            apply_edge(cv, wmask, S.WATER_EDGE, 1.0)

    # 4. buildings -------------------------------------------------------------
    if z >= S.BUILDING_MIN_ZOOM and feats.get("building"):
        bmask = fill_mask(cv, feats["building"])
        apply_fill(cv, bmask, S.BUILDING_FILL)
        apply_edge(cv, bmask, S.BUILDING_EDGE, 0.6)

    cv.start_lines()

    # 5. contours ------------------------------------------------------------
    for is_index, pts, _ in rel.get("contours", []):
        st = S.CONTOUR_INDEX if is_index else S.CONTOUR
        cv.line(pts, st["width"], st["level"])

    # 5b. isobaths ----------------------------------------------------------
    bathy_labels = []
    if depth is not None:
        for bold, lv, pts in isobaths(cv, depth):
            st = S.ISOBATH_BOLD if bold else S.ISOBATH
            cv.line(pts, st["width"], st["level"], st["dash"])
            if z >= S.ISOBATH_LABEL["min_zoom"]:
                every = S.ISOBATH_LABEL["every_px"] * SS
                for k in range(len(pts) // 2, len(pts), max(1, int(every / (4 * SS)))):
                    bathy_labels.append((pts[k], str(lv)))

    # 6. waterways ------------------------------------------------------------
    for tx, ty, ext, f in feats.get("waterway", []):
        cls = f["properties"].get("class")
        if cls not in S.WATERWAYS or z < S.WATERWAYS[cls][0]:
            continue
        w = S.WATERWAYS[cls][1] * (1.0 if z >= 12 else 0.7)
        for ln in iter_lines(f["geometry"]):
            cv.line(cv.to_canvas(tx, ty, ext, ln), w, S.WATERWAY_LEVEL)

    # 7. boundaries ---------------------------------------------------------
    for tx, ty, ext, f in feats.get("boundary", []):
        p = f["properties"]
        if p.get("maritime"):
            continue
        rule = S.BOUNDARIES.get(p.get("admin_level"))
        if not rule or z < rule[0]:
            continue
        for ln in iter_lines(f["geometry"]):
            cv.line(cv.to_canvas(tx, ty, ext, ln), rule[1], rule[2], rule[3])

    # 8. transport ------------------------------------------------------------
    roads = {}   # class -> list of canvas polylines
    trails, rails = [], []
    for tx, ty, ext, f in feats.get("transportation", []):
        p = f["properties"]
        cls = p.get("class")
        lines = [cv.to_canvas(tx, ty, ext, ln) for ln in iter_lines(f["geometry"])]
        if cls in S.ROADS:
            if z >= S.ROADS[cls][0]:
                roads.setdefault(cls, []).extend(
                    (ln, p.get("brunnel")) for ln in lines)
        elif cls in S.TRAILS:
            if z >= S.TRAILS[cls][0]:
                trails.extend((cls, ln) for ln in lines)
        elif cls == "rail" and z >= S.RAIL["min_zoom"]:
            rails.extend(lines)

    for cls, ln in trails:
        _, w, level, dash = S.TRAILS[cls]
        cv.line(ln, w, level, dash)

    order = list(S.ROADS)[::-1]  # minor first, motorway last (on top)
    if z >= S.ROAD_CASED_FROM:
        for cls in order:          # all casings first ...
            w = S.road_width(cls, z)
            for ln, brunnel in roads.get(cls, []):
                dash = (4, 3) if brunnel == "tunnel" else None
                cv.line(ln, w + 2.0, S.ROAD_CASING, dash)
        for cls in order:          # ... then fills, so junctions merge
            w = S.road_width(cls, z)
            fill_level = S.ROADS[cls][2]
            for ln, _ in roads.get(cls, []):
                cv.line(ln, w, fill_level)
    else:
        for cls in order:
            w = S.ROAD_THIN.get(cls, 0.6)
            for ln, _ in roads.get(cls, []):
                cv.line(ln, w, S.BLACK)

    for ln in rails:
        cv.line(ln, S.RAIL["width"] + (1.0 if z >= 13 else 0), S.RAIL["level"])
        if z >= 13:
            cv.line(ln, S.RAIL["width"] - 0.6, S.WHITE, S.RAIL["tie_dash"])

    # 9. labels -----------------------------------------------------------------
    seen = set()
    places = []
    for tx, ty, ext, f in feats.get("place", []):
        p = f["properties"]
        cls, name = p.get("class"), p.get("name")
        if cls not in S.PLACES or not name or z < S.PLACES[cls][0]:
            continue
        for pt in iter_points(f["geometry"]):
            (x, y), = cv.to_canvas(tx, ty, ext, [pt])
            key = (name, int(x // (20 * SS)), int(y // (20 * SS)))
            if key in seen:
                continue
            seen.add(key)
            places.append((S.PLACE_ORDER.index(cls), p.get("rank", 99),
                           cls, name, x, y))
    for _, _, cls, name, x, y in sorted(places, key=lambda t: (t[0], t[1])):
        _, sizes, bold = S.PLACES[cls]
        size = S.size_for_zoom(sizes, z)
        if not size:
            continue
        fnt = font(S.FONT_BOLD if bold else S.FONT_REGULAR, size)
        if cls in ("suburb", "neighbourhood"):
            fnt = font(S.FONT_ITALIC, size)
        cv.place_label(x, y, name, fnt)

    if z >= S.STREET_NAME["min_zoom"]:
        street_names(cv, feats, z)
    if z >= S.CONTOUR_LABEL["min_zoom"]:
        contour_labels(cv, rel.get("contours", []))

    if z >= S.PEAK["min_zoom"]:
        m = S.PEAK["marker"] * SS
        for tx, ty, ext, f in feats.get("mountain_peak", []):
            p = f["properties"]
            for pt in iter_points(f["geometry"]):
                (x, y), = cv.to_canvas(tx, ty, ext, [pt])
                text = p.get("name") or ""
                ele = p.get("ele")
                if ele is not None:
                    text = f"{text} {int(ele)}".strip()
                fnt = font(S.FONT_REGULAR, S.PEAK["font"])
                if cv.place_label(x, y - m - 8 * SS, text, fnt) if text else True:
                    cv.draw.polygon([(x, y - m), (x - m, y + m * 0.7),
                                     (x + m, y + m * 0.7)], fill=0)

    if z >= S.WATER_NAME["min_zoom"]:
        fnt_b = font(S.FONT_ITALIC, S.ISOBATH_LABEL["font"])
        for (x, y), text in bathy_labels:
            cv.place_label(x, y, text, fnt_b, level=S.DARK)
        for tx, ty, ext, f in feats.get("water_name", []):
            name = f["properties"].get("name")
            if not name:
                continue
            for pt in iter_points(f["geometry"]):
                (x, y), = cv.to_canvas(tx, ty, ext, [pt])
                cv.place_label(x, y, name, font(S.FONT_ITALIC, S.WATER_NAME["font"]),
                               S.WATER_NAME["level"])

    return cv.finish()


# ---------------------------------------------------------------------------
# jobs: base pack (z <= 8) and one pack per z8 cell


def render_job(args):
    job, opts = args
    src = VectorSource(opts["mbtiles"])
    dem = Dem(opts["dem"]) if opts.get("dem") else None
    if dem is not None and not dem.available():
        dem = None
    bathy = None
    if opts.get("bathy"):
        from bathy import Bathy
        bathy = Bathy(opts["bathy"])
        if not bathy.available():
            bathy = None
    bbox, zmin, zmax = opts["bbox"], opts["zmin"], opts["zmax"]
    if job == "base":
        cz, cx, cy = BASE_CELL_Z, 0, 0
        zooms = range(zmin, min(zmax, CELL_Z) + 1)
    else:
        cz, (cx, cy) = CELL_Z, job
        zooms = range(max(zmin, CELL_Z + 1), zmax + 1)
    detail = job != "base" and zmin >= DETAIL_Z   # z17: a pack of its own per cell
    final = pack_path(opts["out"], cz, cx, cy, detail)
    # Contours: 2.5 m in flat country, the standard 10 m elsewhere. Decided per
    # z8 cell from the heights there, unless the region says (DOBA_CONTOURS).
    if opts.get("contours") in ("flat", "normal"):
        os.environ["DOBA_CONTOURS"] = opts["contours"]
    elif dem is not None and job != "base":
        lon0, lat1 = tile_to_lonlat(cx, cy, cz)
        lon1, lat0 = tile_to_lonlat(cx + 1, cy + 1, cz)
        glon, glat = np.meshgrid(np.linspace(lon0, lon1, 60), np.linspace(lat0, lat1, 60))
        h = dem.sample(glon, glat)
        h = h[np.isfinite(h)] if h is not None else np.array([])
        relief = float(np.percentile(h, 98) - np.percentile(h, 2)) if h.size > 100 else 999.0
        os.environ["DOBA_CONTOURS"] = "flat" if relief < S.FLAT_RELIEF_M else "normal"
    else:
        os.environ["DOBA_CONTOURS"] = "normal"
    if os.path.exists(final):          # resumable: finished packs are kept
        return job, 0
    os.makedirs(os.path.dirname(final), exist_ok=True)
    writer = PackWriter(final + ".part", cz, cx, cy, relative=detail)
    count = 0
    for z in zooms:
        bx0, by0, bx1, by1 = tile_range(bbox, z)
        if job == "base":
            n = min(META, 2 ** z)
            gx0, gy0, gx1, gy1 = 0, 0, 2 ** z - 1, 2 ** z - 1
        else:
            s = 2 ** (z - CELL_Z)
            n = min(META, s)
            gx0, gy0 = cx * s, cy * s
            gx1, gy1 = gx0 + s - 1, gy0 + s - 1
        x0, y0 = max(bx0, gx0), max(by0, gy0)
        x1, y1 = min(bx1, gx1), min(by1, gy1)
        if x0 > x1 or y0 > y1:
            continue
        for my in range((y0 // n) * n, y1 + 1, n):
            for mx in range((x0 // n) * n, x1 + 1, n):
                tiles = render_metatile(src, dem, z, mx, my, n, bathy)
                for (x, y), lv in tiles.items():
                    if x0 <= x <= x1 and y0 <= y <= y1:
                        writer.add(z, x, y, lv)
                        count += 1
    writer.close()
    os.replace(final + ".part", final)
    return job, count


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mbtiles", required=True)
    ap.add_argument("--dem", help="directory with Copernicus GLO-30 .tif files")
    ap.add_argument("--bathy", help="directory of bathy.py cells (sea depths, isobaths)")
    ap.add_argument("--bbox", required=True, help="west,south,east,north")
    ap.add_argument("--zoom", default="8-16", help="e.g. 8-16")
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--contours", choices=("auto", "flat", "normal"), default="auto",
                    help="2.5 m (flat) or 10 m contours; auto decides per z8 cell")
    a = ap.parse_args()

    bbox = tuple(float(v) for v in a.bbox.split(","))
    zmin, zmax = (int(v) for v in a.zoom.split("-"))
    opts = {"mbtiles": a.mbtiles, "dem": a.dem, "bathy": a.bathy, "bbox": bbox,
            "zmin": zmin, "zmax": zmax, "out": a.out, "contours": a.contours}

    jobs = []
    if zmin <= CELL_Z:
        jobs.append("base")
    if zmax > CELL_Z:
        cx0, cy0, cx1, cy1 = tile_range(bbox, CELL_Z)
        jobs += [(x, y) for y in range(cy0, cy1 + 1) for x in range(cx0, cx1 + 1)]

    t0 = time.time()
    total = 0
    print(f"{len(jobs)} packs, zoom {zmin}-{zmax}, {a.jobs} workers")
    with mp.Pool(min(a.jobs, len(jobs))) as pool:
        for i, (job, cnt) in enumerate(
                pool.imap_unordered(render_job, [(j, opts) for j in jobs]), 1):
            total += cnt
            print(f"  [{i}/{len(jobs)}] {job}: {cnt} tiles "
                  f"({time.time() - t0:.0f}s)", flush=True)
    print(f"done: {total} tiles in {time.time() - t0:.0f}s -> {a.out}")


if __name__ == "__main__":
    sys.exit(main())
