"""Regression tests for past panel, UI and navigation bugs.

Each test names the commit that fixed the bug and was checked against the
unfixed code (the fix reverted by hand in src/, the sim rebuilt): it fails
with the bug and passes with the fix. Assertions are on paint and scrub
counts, the status JSON and pixel regions of the glass, not golden images."""
import math
import os
import struct
import time
import zlib

import pytest


W, H = 540, 960
STATUS_H = 64
SHEET_TOP = H - 520                    # ui_render.cpp sheetShell()
POWER_SHUTDOWN = (24 + 246, 700 + 48)  # kPowerShutdown centre (START on the nav prompt)


# -- the glass --------------------------------------------------------------------------

class Glass:
    """A screenshot as rows of 0-255 grey levels (the sim writes 8-bit grey,
    filter 0 on every row)."""

    def __init__(self, path):
        data = open(path, "rb").read()
        pos, idat = 8, b""
        while pos < len(data):
            n, kind = struct.unpack_from(">I4s", data, pos)
            body = data[pos + 8:pos + 8 + n]
            if kind == b"IHDR":
                self.w, self.h = struct.unpack_from(">II", body)
            elif kind == b"IDAT":
                idat += body
            pos += 12 + n
        raw = zlib.decompress(idat)
        stride = self.w + 1
        self.rows = [raw[y * stride + 1:(y + 1) * stride] for y in range(self.h)]

    def px(self, x, y):
        return self.rows[y][x]

    def region(self, x0, y0, x1, y1):
        return bytes(b for y in range(y0, y1) for b in self.rows[y][x0:x1])

    def ink(self, x0, y0, x1, y1, level=0x40):
        """Fraction of pixels in the box at or darker than `level`."""
        r = self.region(x0, y0, x1, y1)
        return sum(1 for b in r if b <= level) / max(len(r), 1)

    def checker(self, x0, y0, x1, y1):
        """How strongly the box carries the TONE_50 scrim (ui_render.cpp
        fillTone: a 2 px checker, on where ((x>>1)+(y>>1)) is even): the ink
        fraction on the checker's 'on' cells minus that on its 'off' cells.
        ~1 under a scrim, ~0 over ordinary content."""
        on = off = n_on = n_off = 0
        for y in range(y0, y1):
            row = self.rows[y]
            for x in range(x0, x1):
                dark = row[x] <= 0x40
                if ((x >> 1) + (y >> 1)) % 2 == 0:
                    n_on += 1
                    on += dark
                else:
                    n_off += 1
                    off += dark
        return on / max(n_on, 1) - off / max(n_off, 1)

    def dots(self, x0, y0, x1, y1):
        """The same for TONE_25, the greyed-out cell's dot: one pixel in every
        2x2, on where x and y are both even."""
        on = off = n_on = n_off = 0
        for y in range(y0, y1):
            row = self.rows[y]
            for x in range(x0, x1):
                dark = row[x] <= 0x40
                if not (x & 1) and not (y & 1):
                    n_on += 1
                    on += dark
                else:
                    n_off += 1
                    off += dark
        return on / max(n_on, 1) - off / max(n_off, 1)


def glass(device, tmp_path, name):
    path = str(tmp_path / (name + ".png"))
    device.screenshot(path)
    return Glass(path)


# -- riding helpers ---------------------------------------------------------------------

def line_track(lat, lon, km, bearing_deg=90, step_m=20):
    pts = []
    r = math.radians(bearing_deg)
    for i in range(int(km * 1000 / step_m) + 1):
        d = i * step_m
        pts.append(offset(lat, lon, d * math.cos(r), d * math.sin(r)))
    return pts


def offset(lat, lon, north_m, east_m):
    return (lat + north_m / 111320.0,
            lon + east_m / (111320.0 * math.cos(math.radians(lat))))


def open_power_sheet(device, tmp_path):
    device.button("boot", ms=2200)           # hold → SHUT DOWN?
    deadline = time.time() + 5
    while time.time() < deadline:
        g = glass(device, tmp_path, "sheet")
        # The sheet's 6 px rule across the full width at its top edge.
        if g.ink(0, SHEET_TOP + 1, W, SHEET_TOP + 5) > 0.95:
            return g
        time.sleep(0.2)
    raise AssertionError("the power sheet never appeared")


def fix_at(device, lat, lon, settle_s=1.5):
    """Hold the GPS at one spot until the firmware has projected it."""
    device.gps(lat, lon, speed_kmh=12, course=90)
    time.sleep(settle_s)
    return device.status()["nav"]


# -- the tests --------------------------------------------------------------------------


def test_power_sheet_scrims_the_status_bar_too(device, tmp_path):
    """ce15d45 "Grey the status bar under the power-off overlay too" (#107).
    The power sheet's scrim started below the status band, so the clock and
    battery stayed crisp over a toned screen and the device looked half
    still-on while shutting down. Above the sheet, the band is toned like the
    body."""
    g = open_power_sheet(device, tmp_path)
    body = g.checker(0, STATUS_H, W, SHEET_TOP)
    assert body > 0.6, "the body is not scrimmed (%.2f)" % body
    # DOBA: the tactical band is white on black, so the scrim shows on its
    # light text: the checker's 'on' cells go dark, the 'off' cells keep it.
    on = off = 0
    for y in range(0, STATUS_H):
        row = g.rows[y]
        for x in range(W):
            if row[x] > 0x40:
                if ((x >> 1) + (y >> 1)) % 2 == 0:
                    on += 1
                else:
                    off += 1
    assert off > 50, "no text in the status band (%d light px)" % off
    assert on < 0.2 * off, "the status band is crisp over a scrimmed screen (%d vs %d)" % (on, off)


HERO_BODY = (40, 130, 500, 300)     # inside the default layout's POWER hero, above the value
HR_CELL_BODY = (36, 630, 250, 670)   # inside HEART RATE, between caption and value


def eventually(device, predicate, message, timeout=5):
    try:
        return device.wait_status(predicate, timeout=timeout)
    except TimeoutError as e:
        raise AssertionError("%s (%s)" % (message, str(e)[:300]))


def settle(device, quiet_s=1.5, timeout=10):
    """Wait until the panel has gone `quiet_s` without a paint."""
    deadline = time.time() + timeout
    last, since = device.status()["paints"], time.time()
    while time.time() < deadline:
        time.sleep(0.2)
        p = device.status()["paints"]
        if p != last:
            last, since = p, time.time()
        elif time.time() - since >= quiet_s:
            return device.status()
    raise AssertionError("the panel never settled")


def test_entering_the_map_scrubs_only_what_changed(device):
    """28be13f "Scrub only the dirty rectangles entering the map, not the whole
    panel" (#33). Entering the map drove a whole-panel clear — a full flash —
    where a scrub of the rectangles that differ resyncs the driver just as well.
    Entering the map must scrub, and must not clear the whole panel."""
    device.gps(37.77, -122.45)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    st0 = settle(device)
    device.console("screen map")
    st1 = device.wait_status(lambda s: s["screen"] == "map", timeout=5)
    st1 = settle(device)
    assert st1["clears"] == st0["clears"], "entering the map flashed the whole panel"
    assert st1["scrubs"] == st0["scrubs"] + 1, "entering the map: %d scrubs" % (st1["scrubs"] - st0["scrubs"])


def test_no_scrub_for_the_power_sheet_over_the_map_or_leaving_it(device, tmp_path):
    """ba72100, the map UI part: the map's scrub fired entering the map,
    leaving it, and when the power sheet opened over it (an overlay change
    counts as a screen change while the screen stays the map), so every popup
    and every exit flashed. Only entering earns it (and, since, closing a
    modal over the map — not tested here)."""
    device.gps(37.77, -122.45)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    device.console("screen map")
    device.wait_status(lambda s: s["screen"] == "map", timeout=5)
    st0 = settle(device)
    open_power_sheet(device, tmp_path)
    st1 = settle(device)
    assert st1["paints"] > st0["paints"]
    assert st1["scrubs"] == st0["scrubs"], "the power sheet opening over the map scrubbed the panel"
    device.tap(270, 250)                     # outside the sheet: closes it (that does scrub, by design)
    st2 = settle(device)
    assert st2["screen"] == "map"
    device.console("screen home")
    st3 = settle(device)
    assert st3["paints"] > st2["paints"]
    assert st3["scrubs"] == st2["scrubs"], "leaving the map for home scrubbed the panel"


# -- keys -------------------------------------------------------------------------------
