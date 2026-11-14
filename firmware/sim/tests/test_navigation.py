"""Routes and turn-by-turn navigation: what the app sends, what the device
keeps on its card, what it shows a rider following a route.

Routes arrive over the link as a GPX upload (routes.cpp loadFromMemory: parsed
and written to /routes/<name>), followed by the turn cues (RouteManeuvers) and
RouteFinish, which raises the NAVIGATE prompt. The same files can be picked
from the device's own NAVIGATE list (routes.cpp load: streamed from the card),
which is also how a rider rides a .gpx copied onto the card over USB.

The rider is the GPS module (`gps` / `gps track`); what the turn banner says
is the status JSON's `nav` (the same routes:: calls the banner draws from)."""
import math
import os
import re
import time

import pytest

from conftest import TIME_SCALE

from test_regressions_ui import POWER_SHUTDOWN, accept_route, glass, line_track, offset, send_route

W, H = 540, 960
LAT0, LON0 = 37.77, -122.45

MENU_NAVIGATE = (270, 64 + 1 * 148 + 74)      # Menu row 1, NAVIGATE


def row_xy(r):
    """Centre of row `r` of a menu-style list (kMenuRowTop 64, ROW_H 148)."""
    return (270, 64 + r * 148 + 74)


def along(m, north_m=0.0):
    """A point `m` metres east of LAT0/LON0 (the routes below run east)."""
    return offset(LAT0, LON0, north_m, m)


def trkpts(pts, fmt='<trkpt lat="%.7f" lon="%.7f"/>'):
    return "\n".join(fmt % p for p in pts)


def gpx(pts, name="Test", before_trk="", fmt='<trkpt lat="%.7f" lon="%.7f"/>'):
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<gpx version="1.1" creator="sim">%s'
            '<trk><name>%s</name><trkseg>\n%s\n</trkseg></trk></gpx>\n'
            % (before_trk, name, trkpts(pts, fmt))).encode()


# Metres per degree on the firmware's sphere (routes.cpp haversineM, R = 6371 km): the
# distances it reports are on this scale, 0.11% short of offset()'s 111320.
M_PER_DEG = 6371000.0 * math.pi / 180


def length_m(pts):
    tot = 0.0
    for (a, b), (c, d) in zip(pts, pts[1:]):
        dy = (c - a) * M_PER_DEG
        dx = (d - b) * M_PER_DEG * math.cos(math.radians(a))
        tot += math.hypot(dx, dy)
    return tot


ROUTE_LINE = re.compile(r"\[route\] (?:uploaded )?(.+?): (\d+) pts, ([\d.]+) km")


def route_lines(device, since=0):
    """Every `[route] <name>: N pts, K km` the device printed (load and upload)."""
    out = []
    for l in device.log(since):
        m = ROUTE_LINE.search(l)
        if m:
            out.append((m.group(1), int(m.group(2)), float(m.group(3)), "uploaded" in l))
    return out


def open_navigate(device):
    """The device's own NAVIGATE list, from the menu."""
    device.console("screen menu")
    device.wait_status(lambda s: s["screen"] == "other", timeout=5 * TIME_SCALE)
    time.sleep(0.5)
    device.tap(*MENU_NAVIGATE)
    time.sleep(1.0)


def pick_route(device, row):
    """Tap row `row` of the NAVIGATE list (row 0 is "Clear route" while a
    route is loaded); returns the `[route]` line the load printed."""
    seq = device.log_seq()
    device.tap(*row_xy(row))
    line = device.wait_log(r"\[route\] ", timeout=5 * TIME_SCALE, since=seq)
    m = ROUTE_LINE.search(line)
    return (m.group(1), int(m.group(2)), float(m.group(3))) if m else line


def upload(link, name, data):
    return link.upload(pb.TARGET_ROUTE, name, data)


# -- storage: what the app sends is what the card keeps ----------------------------------------


@pytest.mark.xfail(strict=True, reason=(
    "BUG: routes.cpp list() copies each name into char[routes::MAX_NAME] (32 B), so a .gpx with a "
    "longer name is listed cut short in NAVIGATE and tapping it fails ('[route] can't open "
    "/routes/2026-10-28 Morning coast loop.g'): nothing loads, no message on the panel."))
def test_a_long_file_name_copied_onto_the_card_can_be_ridden(device):
    """A rider copies a .gpx onto the card over USB under whatever name the
    planner exported — FAT keeps long names, and 33+ characters is ordinary
    ("2026-10-28 Morning loop to the coast.gpx"). It must be listed whole and
    load when tapped."""
    name = "2026-10-28 Morning coast loop.gpx"         # 33 bytes
    pts = line_track(LAT0, LON0, 2, step_m=25)
    device.usb_host(True)
    with open(os.path.join(device.sd, "routes", name), "wb") as f:
        f.write(gpx(pts, "Morning"))
    device.usb_host(False)
    open_navigate(device)
    picked = pick_route(device, 0)
    assert device.status()["nav"]["route"], "tapping %r loaded nothing: %r" % (name, picked)


# -- following a route ---------------------------------------------------------------------------

def polyline(*legs, step_m=20):
    """A route from LAT0/LON0 along `legs` [(bearing_deg, metres)], a point
    every `step_m`; returns (points, corner along-distances)."""
    pts, corners = [(LAT0, LON0)], []
    done = 0.0
    for bearing, metres in legs:
        r = math.radians(bearing)
        la, lo = pts[-1]
        n = int(round(metres / step_m))
        for i in range(1, n + 1):
            d = metres * i / n
            pts.append(offset(la, lo, d * math.cos(r), d * math.sin(r)))
        done += metres
        corners.append(done)
    return pts, corners


def at_along(pts, m):
    """The point `m` metres along the polyline `pts`."""
    for a, b in zip(pts, pts[1:]):
        seg = length_m([a, b])
        if m <= seg:
            f = m / seg if seg else 0
            return (a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f)
        m -= seg
    return pts[-1]


def along_of(pts, lat, lon):
    """Where (lat, lon) projects onto the polyline, in metres along it."""
    best, best_m, done = 1e18, 0.0, 0.0
    cosl = math.cos(math.radians(lat))
    for a, b in zip(pts, pts[1:]):
        ax, ay = (a[1] - lon) * M_PER_DEG * cosl, (a[0] - lat) * M_PER_DEG
        bx, by = (b[1] - lon) * M_PER_DEG * cosl, (b[0] - lat) * M_PER_DEG
        vx, vy = bx - ax, by - ay
        l2 = vx * vx + vy * vy
        t = max(0.0, min(1.0, -(ax * vx + ay * vy) / l2)) if l2 else 0.0
        d2 = (ax + vx * t) ** 2 + (ay + vy * t) ** 2
        seg = length_m([a, b])
        if d2 < best:
            best, best_m = d2, done + t * seg
        done += seg
    return best_m


def navigate(device, link, pts, turns, start=None, name="test.gpx"):
    """Put the rider at `start` (default: the route's first point), send the
    route with `turns` [(metres along, text)] and accept the NAVIGATE prompt."""
    la, lo = start or pts[0]
    device.gps(la, lo, speed_kmh=0)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    send_route(link, name, pts, [at_along(pts, m) + (t,) for m, t in turns])
    # A rider already past the route's first point has the prompt grabbed
    # away within a second (see test_a_rider_standing_on_the_route_keeps_the_
    # prompt_until_they_answer): START only if it is still up.
    st = device.wait_status(lambda s: s["nav"]["pending"] or s["nav"]["active"], timeout=10)
    if st["nav"]["active"]:
        return st
    time.sleep(0.9)                           # taps are ignored for 700 ms after it appears
    if device.status()["nav"]["pending"]:
        device.tap(*POWER_SHUTDOWN)           # START
    return device.wait_status(lambda s: s["nav"]["active"], timeout=10)


def hold(device, lat, lon, settle_s=1.3):
    """The rider stands at (lat, lon) until the firmware has projected it."""
    device.gps(lat, lon, speed_kmh=0)
    device.wait_status(lambda s: abs(s["ride"]["lat"] - lat) < 2e-6 and abs(s["ride"]["lon"] - lon) < 2e-6,
                       timeout=5 * TIME_SCALE)
    time.sleep(settle_s)
    return device.status()["nav"]


# An out-and-back: 1 km east on the north side of the road, a U-turn, and back
# west on the south side (8 m apart, the two carriageways of a road).
OUT_M, LANES_M = 1000.0, 8.0


def out_and_back():
    out = [offset(LAT0, LON0, LANES_M / 2, x) for x in range(0, int(OUT_M) + 1, 20)]
    back = [offset(LAT0, LON0, -LANES_M / 2, x) for x in range(int(OUT_M), -1, -20)]
    pts = out + back
    turns = [(600, "Keep left at the fork"),                    # outbound, 600 m east
             (OUT_M + LANES_M + 0.5, "Make a U-turn"),
             (OUT_M + LANES_M + 400, "Turn right onto Elm St")]   # return leg, 600 m east
    return pts, turns


# -- the NAVIGATE prompt ------------------------------------------------------------------------

def prompt_route(device, link, at_m=0.0):
    """The rider stands `at_m` along a 2 km route east when it arrives."""
    pts = line_track(LAT0, LON0, 2, step_m=20)
    device.gps(*along(at_m), speed_kmh=0)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    time.sleep(1.2)
    send_route(link, "t.gpx", pts, [along(800) + ("Turn left",)])
    device.wait_status(lambda s: s["nav"]["pending"], timeout=10)
    return pts


# -- the route on the map -----------------------------------------------------------------------

RIDER_X = 270
AHEAD = (215, 395)       # above the rider (north-up): below the turn banner, above "NO MAP HERE"
BEHIND = (505, 575)      # below it, down to the route's start 300 m (150 px) south


def column_runs(g, x, y0, y1):
    """Dark/light runs down the route's centre line, as (dark?, length)."""
    runs = []
    for y in range(y0, y1):
        dark = g.px(x, y) <= 0x40
        if runs and runs[-1][0] == dark:
            runs[-1][1] += 1
        else:
            runs.append([dark, 1])
    return runs


def route_north_on_the_map(device, link, tmp_path, step_m):
    """A 2 km route due north, the rider 300 m along it, the map at 2 m/px:
    the route runs straight up the panel's centre column from the rider."""
    pts = line_track(LAT0, LON0, 2, bearing_deg=0, step_m=step_m)
    rider = offset(LAT0, LON0, 300, 0)
    navigate(device, link, pts, [(1500, "Turn left")], start=rider)
    hold(device, *rider)
    device.console("screen map")
    device.wait_status(lambda s: s["screen"] == "map", timeout=5 * TIME_SCALE)
    time.sleep(2.5)
    return glass(device, tmp_path, "map_%d" % step_m)


# -- long routes ---------------------------------------------------------------------------------

@pytest.mark.xfail(strict=True, reason=(
    "BUG: routes.cpp GpxScanner thins a track over 4096 points by keeping every stride-th point "
    "(stride doubling at each halving) and never keeps the last one, so the route ends up to "
    "stride-1 points short of its finish (7 at 16k+ points): this 20,005-point GPX (a 200 km "
    "track at 10 m) ends 60 m before the destination, and 100 m out the device says 40 m are "
    "left. (The apps' importers keep the last point when they thin; the device doesn't. The "
    "chunk re-scan bug above shifts which points survive, so the shortfall varies 0-70 m.)"))
def test_a_very_long_track_from_the_card_still_ends_at_its_finish(device):
    """A 200 km track recorded at 10 m (20,000 points, ~0.9 MB) copied onto
    the card. The device keeps 4096 points at most and thins the rest; the
    route must still run to the file's last point: a rider 60 m from the
    finish has 100 m left, not 40."""
    pts = line_track(LAT0, LON0, 201, step_m=10)[:20005]
    with open(os.path.join(device.sd, "routes", "big.gpx"), "wb") as f:
        f.write(gpx(pts, "Big"))
    open_navigate(device)
    t0 = time.time()
    name, n, km = pick_route(device, 0)
    assert time.time() - t0 < 5 * TIME_SCALE, "loading a 0.9 MB route took %.1f s" % (time.time() - t0)
    assert name == "big.gpx" and 2048 <= n <= 4096, (name, n)
    near_end = at_along(pts, length_m(pts) - 100)
    for _ in range(6):                     # joined far from where it started: re-acquired
        nav = hold(device, *near_end, settle_s=0.8)
    assert abs(nav["remaining_km"] * 1000 - 100) < 12, "100 m from the finish, %.0f m left" % (
        nav["remaining_km"] * 1000)


# -- navigating while recording ------------------------------------------------------------------

