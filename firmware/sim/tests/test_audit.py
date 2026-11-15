"""DOBA: the November 2026 audit. Each test pins a bug that was found and fixed,
or a feature nothing checked end to end before."""
import os
import time

import numpy as np
from PIL import Image

from test_doba import TOP, boot, doba  # noqa: F401
from test_topo import LAT, LON, Z, quadrant_tile, tile_xy, write_packs


def shot(dev, tmp_path, name):
    p = str(tmp_path / name)
    dev.screenshot(p)
    return np.asarray(Image.open(p).convert("L"))


def packs_around(sd):
    fx, fy = tile_xy(LAT, LON, Z)
    tx, ty = int(fx), int(fy)
    write_packs(sd, {(Z, tx + dx, ty + dy): quadrant_tile(3, 12)
                     for dy in range(-3, 4) for dx in range(-2, 3)})


def open_topo(dev):
    dev.gps(LAT, LON, speed_kmh=0)
    time.sleep(1.5)
    dev.console("screen map", 600)
    dev.wait_status(lambda s: s["screen"] == "map", timeout=10)
    time.sleep(1.5)
    for _ in range(2):          # the map opens at z14, the packs are z16
        dev.console("zoom in")
        time.sleep(1)
    time.sleep(2)


def test_a_topo_view_from_the_cache_keeps_its_greys(doba, tmp_path):
    """The frame cache packed the base layer to one bit a pixel: going back to
    a view already seen drew the topo thresholded, without its greys."""
    packs_around(doba.sd)
    boot(doba)
    open_topo(doba)
    fresh = shot(doba, tmp_path, "fresh.png")
    doba.console("screen home", 600)
    time.sleep(1.5)
    doba.console("screen map", 600)
    doba.wait_status(lambda s: s["screen"] == "map", timeout=10)
    time.sleep(2.5)
    again = shot(doba, tmp_path, "again.png")
    band = slice(140, 700), slice(0, 400)
    levels = set(np.unique(again[band] // 17).tolist())
    assert {3, 12} <= levels, levels
    assert (fresh[band] == again[band]).mean() > 0.995


def test_maps_copied_over_usb_show_without_a_restart(doba, tmp_path):
    """After a computer wrote to the card, the topo layer still remembered that
    there was no pack here; only books, points and heights were looked at again."""
    boot(doba)
    open_topo(doba)
    assert "active=0" in "\n".join(doba.console("topo"))
    since = doba.log_seq()
    doba.usb_host(True)
    doba.wait_log(r"sd: lent to the USB host", since=since, timeout=10)
    packs_around(doba.sd)
    since = doba.log_seq()
    doba.usb_host(False)
    doba.wait_log(r"sd: remount \(usb host released\) -> ok", since=since, timeout=10)
    time.sleep(2)
    doba.console("zoom out")
    time.sleep(1)
    doba.console("zoom in")
    time.sleep(2)
    assert "active=1" in "\n".join(doba.console("topo"))


def test_ground_height_stays_after_a_while(doba):
    """The UI overwrote the card's ground height with the vector tiles' (none)
    every 2.5 s, until the next fix put it back: it flickered off and on."""
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "..", "maps"))
    import dempack
    n = 1200
    h = np.full((n, n), 123.0, np.float32)
    os.makedirs(os.path.join(doba.sd, "doba", "dem"), exist_ok=True)
    with open(os.path.join(doba.sd, "doba", "dem", "N52E006.dem"), "wb") as f:
        f.write(dempack.encode(h, 52, 6))
    boot(doba)
    doba.gps(52.0421, 6.6170, alt=999)
    doba.wait_status(lambda s: s["ride"].get("ground_m", -99999) > -99999, timeout=8)
    seen = []
    for _ in range(40):              # 8 s, past several 2.5 s overwrites
        seen.append(doba.status()["ride"]["ground_m"])
        time.sleep(0.2)
    assert all(abs(g - 123) < 1 for g in seen), seen


def test_the_gps_page_goes_back_from_its_top(doba):
    """A tap in the top 150 px of the GPS page, its Back corner included, ran a
    panel-drive experiment (2 s of dark and light) and stayed on the page."""
    boot(doba)
    doba.console("screen more", 600)
    time.sleep(1.5)
    more = doba.status()["page"]
    doba.console("screen gps", 600)
    time.sleep(1.5)
    gps = doba.status()["page"]
    assert gps != more
    doba.tap(50, 30)                 # Back
    doba.wait_status(lambda s: s["page"] == more, timeout=5)


def test_a_point_dropped_by_holding_the_map_lands_in_the_file(doba):
    os.makedirs(os.path.join(doba.sd, "doba"), exist_ok=True)
    boot(doba)
    doba.gps(52.0421, 6.6170)
    time.sleep(1.5)
    doba.console("screen map", 600)
    doba.wait_status(lambda s: s["screen"] == "map", timeout=10)
    time.sleep(1.5)
    doba.tap(270, 430, ms=1400)       # hold on the position: a point there
    time.sleep(2)
    path = os.path.join(doba.sd, "doba", "points.txt")
    assert os.path.exists(path), "no points file"
    lines = open(path).read().splitlines()
    assert lines and ";52.04" in lines[-1], lines
