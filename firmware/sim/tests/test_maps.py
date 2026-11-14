"""Maps on the device, with real tiles: the Hood River fixture
(sim/fixtures/tiles/, otpsim.tiles) sent over the link the way the apps send
it, listed back, stored, replaced, deleted and drawn.

What is drawn is checked on the glass: the amount of ink against the
"NO MAP HERE" page, and the fixture's own roads, projected the way
src/map_tiles.cpp projects them, must land on inked pixels (and must not when
the projection is moved 30 m). The firmware's `timing` console switch logs
every map frame (polys per class, tiles drawn/wanted, " cut" for a partial
frame); the tests read those too."""
import math
import os
import re
import struct
import time
import zlib

import pytest

from conftest import TIME_SCALE
from otpsim import SimDevice
from otpsim.tiles import (MAJOR, MINOR, PATH, PRIMARY, SECONDARY, TERTIARY, card_path, fixture_manifest,
                          fixture_pois, fixture_tiles, keep_roads, samples_along, screen_roads)
from test_regressions_ui import Glass

MANIFEST = fixture_manifest()
CENTRE = (MANIFEST["centre"]["lat"], MANIFEST["centre"]["lon"])
CENTRE_H3 = MANIFEST["centre"]["h3"]
TILES = fixture_tiles()
POIS = fixture_pois()
BLOBS = [d for _, d, _ in TILES]
STREETS = (MAJOR, PRIMARY, SECONDARY, TERTIARY, MINOR)

# The "NO MAP HERE" box (map_view.cpp ui_render_map: 500x96, centred, 30 px up).
NOMAP_TOP = (960 - 96) // 2 - 30


# -- the app's side -------------------------------------------------------------------------







# -- the map page ---------------------------------------------------------------------------

FRAME = re.compile(r"map: project=\d+ draw=\d+ polys=(\d+) \(tiles=(\d+) base=\d+\) "
                   r"cls\[maj=(\d+) pri=(\d+) sec=(\d+) ter=(\d+) min=(\d+) path=(\d+)\] "
                   r"ntiles=(\d+)/(\d+)( cut)? mpp=(\d+)")


def parse_frame(line):
    m = FRAME.search(line)
    if not m:
        return None
    g = [int(x) if x and x.strip().isdigit() else x for x in m.groups()]
    return dict(polys=g[0], tile_polys=g[1], cls=dict(zip(("maj", "pri", "sec", "ter", "min", "path"), g[2:8])),
                drawn=g[8], wanted=g[9], cut=bool(m.group(11)), mpp=g[11])


def frames(device, since):
    return [f for f in (parse_frame(l) for l in device.log(since)) if f]


def wait_frame(device, since, pred=lambda f: True, timeout=10):
    """The first complete map frame (not cut short) after `since` that
    matches `pred`."""
    deadline = time.time() + timeout
    while True:
        for f in frames(device, since):
            if not f["cut"] and pred(f):
                return f
        if time.time() > deadline:
            raise AssertionError("no complete map frame matching; last: %s" % (frames(device, since)[-3:],))
        time.sleep(0.2)


FRAME_AT = re.compile(r" at=(-?\d+\.\d+),(-?\d+\.\d+)")


def wait_painted(device, since, lat, lon, timeout=10):
    """A complete map frame centred on (lat, lon), logged after `since`, once
    it is on the glass. The frame line is logged before the panel is driven;
    the paint logs "refresh:" after it, from the same task, and the next
    frame line means that step is over too (an identical frame is not
    painted). A screenshot taken before either shows the frame before — the
    one for where the rider was, not where the test just put them."""
    deadline = time.time() + timeout
    while True:
        lines = device.log(since)
        at = None
        for i, l in enumerate(lines):
            f = parse_frame(l)
            m = FRAME_AT.search(l)
            if f and not f["cut"] and m and abs(float(m.group(1)) - lat) < 1e-4 and abs(float(m.group(2)) - lon) < 1e-4:
                at = i
                break
        if at is not None and any("refresh:" in l or parse_frame(l) for l in lines[at + 1:]):
            return parse_frame(lines[at])
        if time.time() > deadline:
            raise AssertionError("no painted map frame at %.5f,%.5f; last: %s" % (lat, lon, lines[-3:]))
        time.sleep(0.1)


def open_map(device, lat, lon):
    device.gps(lat, lon)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    device.console("timing")              # log every map frame
    seq = device.log_seq()
    device.console("screen map")
    device.wait_status(lambda s: s["screen"] == "map", timeout=5)
    return seq


def glass(device, tmp_path, name):
    path = str(tmp_path / (name + ".png"))
    device.screenshot(path)
    return Glass(path)


def map_ink(g):
    """Ink over the map viewport (status bar and footer strip left out)."""
    return g.ink(0, 70, 540, 805)


def shows_no_map(g):
    """The 'NO MAP HERE' box: its 2 px frame across the panel (left of the
    zoom buttons), white inside above the text."""
    return (g.ink(30, NOMAP_TOP - 1, 450, NOMAP_TOP + 1) > 0.95 and
            g.ink(30, NOMAP_TOP + 95, 450, NOMAP_TOP + 97) > 0.95 and
            g.ink(30, NOMAP_TOP + 4, 450, NOMAP_TOP + 20) < 0.02)


def safe(x, y):
    """Map pixels nothing else is drawn over: not the rider marker, the
    compass, the zoom/layers buttons or the scale bar."""
    return (10 <= x <= 450 and 150 <= y <= 790 and math.hypot(x - 270, y - 430) > 45
            and not (x < 140 and y > 740))


def road_hits(g, lat, lon, blobs=BLOBS, classes=STREETS, mpp=2):
    """Of the fixture's roads around (lat, lon), sampled every 6 px along
    each, the fraction with ink within 2 px. (fraction, samples)."""
    n = hit = 0
    for _, xy in screen_roads(blobs, lat, lon, mpp=mpp, classes=classes):
        for x, y in samples_along(xy):
            if not safe(x, y):
                continue
            n += 1
            xi, yi = int(round(x)), int(round(y))
            if any(g.px(xi + dx, yi + dy) <= 0x40 for dx in range(-2, 3) for dy in range(-2, 3)):
                hit += 1
    return hit / max(n, 1), n


def moved(lat, lon, north_m, east_m):
    return (lat + north_m / 110540.0, lon + east_m / (111320.0 * math.cos(math.radians(lat))))


def over_usb(device, edit):
    """A computer mounts the card, `edit(sd_root)` changes it, the computer
    lets go. Held until the device has noticed (its UI loop polls the host
    once per frame) and has rescanned its maps."""
    seq = device.log_seq()
    device.usb_host(True)
    device.wait_log(r"map: project=", since=seq, timeout=10)
    edit(device.sd)
    seq = device.log_seq()
    device.usb_host(False)
    device.wait_log(r"map: \d+ whole maps indexed", since=seq, timeout=10)


def alive(device):
    assert device.proc.poll() is None, "the sim exited (%s): see %s" % (device.proc.returncode, device.log_path)
    device.status()


# -- tests ----------------------------------------------------------------------------------


# -- big transfers ----------------------------------------------------------------------------

def newer_record(rec):
    """Another valid CDN record for the same cell ("cdn:" + 16 hex)."""
    return "cdn:" + rec[4:][::-1]




def report(what, n, size, secs, raw=None):
    rate = size / 1024.0 / max(secs, 1e-6)
    line = "%s: %d tiles, %d KB in %.1f s (%.1f KB/s%s)" % (
        what, n, size // 1024, secs, rate, ", link carries %.1f KB/s" % (raw / 1024.0) if raw else "")
    print("\n" + line)
    return line


# -- bad tiles ---------------------------------------------------------------------------------


# Damaged tiles, each way the reader can be misled. A tile's header names its
# grid and the grid sizes its index; the index names where each sub-tile's
# roads are; the sections after the roads carry their own counts. The link
# refuses all of them (map_tiles::tileValid); a tile already on the card (put
# there by firmware that only checked the magic, or by a computer) is read
# with every offset and count checked against the file, and skipped where
# they do not fit.

def _grid(nx, ny):
    def f(b):
        struct.pack_into("<ii", b, 28, nx, ny)
    return f


def _cell_deg(v):
    def f(b):
        struct.pack_into("<d", b, 20, v)
    return f


def _entry(off=None, ln=None):
    """The first sub-tile with roads: its offset (absolute, or from the end
    of the file when negative) and length."""
    def f(b):
        k, o, l = _first_entry(b)
        o = o if off is None else (len(b) + off if off < 0 else off)
        l = l if ln is None else ln
        struct.pack_into("<II", b, 36 + k * 8, o & 0xFFFFFFFF, l & 0xFFFFFFFF)
    return f


def _first_entry(b):
    nx, ny = struct.unpack_from("<ii", b, 28)
    for k in range(nx * ny):
        o, l = struct.unpack_from("<II", b, 36 + k * 8)
        if o:
            return k, o, l
    raise AssertionError("no roads")


def _records(count=None, points=None):
    """The first sub-tile's record count, or its first polyline's point count."""
    def f(b):
        _, o, _ = _first_entry(b)
        if count is not None:
            struct.pack_into("<H", b, o, count)
        if points is not None:
            struct.pack_into("<H", b, o + 3, points)
    return f


def _sections_at(b):
    nx, ny = struct.unpack_from("<ii", b, 28)
    end = 36 + nx * ny * 8
    for k in range(nx * ny):
        o, l = struct.unpack_from("<II", b, 36 + k * 8)
        if o:
            end = max(end, o + l)
    return end


def _elevation(gw, gh):
    def f(b):
        p = _sections_at(b)
        assert b[p:p + 4] == b"ELV1"
        struct.pack_into("<ii", b, p + 4, gw, gh)
    return f


def _water(polys=None, points=None):
    def f(b):
        p = _sections_at(b)
        if b[p:p + 4] == b"ELV1":
            gw, gh = struct.unpack_from("<ii", b, p + 4)
            p += 44 + gw * gh * 2
        assert b[p:p + 4] == b"WTR2", b[p:p + 4]
        if polys is not None:
            struct.pack_into("<H", b, p + 4, polys)
        if points is not None:
            struct.pack_into("<H", b, p + 6, points)
    return f


def _half(b):
    del b[len(b) // 2:]


def _noise(b):
    b[36:] = os.urandom(len(b) - 36)


DAMAGE = {
    "grid_overruns_the_file": _grid(30000, 30000),       # nx*ny*8 bytes of index past the end
    "grid_wraps_int32": _grid(65536, 65536),             # nx*ny overflows to 0
    "grid_negative": _grid(-6, 4),
    "grid_empty": _grid(0, 4),
    "cell_size_zero": _cell_deg(0.0),                    # sub-tiles in view: floor(x / 0)
    "cell_size_nan": _cell_deg(float("nan")),
    "cell_size_tiny": _cell_deg(1e-300),                 # billions of sub-tiles in view
    "entry_past_the_end": _entry(off=-1, ln=1000),
    "entry_wraps_uint32": _entry(off=0xFFFFFF00, ln=0x200),   # off + len wraps past 4 GB
    "entry_into_the_index": _entry(off=8),
    "entry_too_short": _entry(off=-1, ln=1),             # no room for the record count
    "record_count_overruns": _records(count=0xFFFF),
    "point_count_overruns": _records(points=0xFFFF),
    "elevation_grid_overruns": _elevation(0x7FFFFFFF, 0x7FFFFFFF),
    "elevation_grid_negative": _elevation(-1, 2),
    "elevation_grid_wraps": _elevation(0x10000, 0x8000),  # gw*gh*2 = 2^32: 0 in 32 bits
    "water_count_overruns": _water(polys=0xFFFF),
    "water_points_overrun": _water(points=0xFFFF),
    "truncated": _half,
    "noise": _noise,
}


def damaged_tile(kind, good):
    b = bytearray(good)
    DAMAGE[kind](b)
    return bytes(b)


def test_the_damage_cases_are_what_they_say():
    """Each case damages what it names, on the centre tile as it is (has
    elevation and water: the sections cases need them)."""
    good = dict((h, d) for h, d, _ in TILES)[CENTRE_H3]
    assert _sections_at(bytearray(good)) < len(good)
    for kind in DAMAGE:
        assert damaged_tile(kind, good) != good, kind


