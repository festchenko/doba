"""DOBA: the topo canvas (topo_layer.cpp) — the map drawn half a screen beyond
each edge of the view, so a drag or a walk is a copy instead of a redraw.
What it puts on the glass must be what the tiles give, pixel for pixel."""
import re
import time

import numpy as np

from test_doba import boot, doba  # noqa: F401
from test_topo import LAT, LON, Z, tile_xy, write_packs
from test_audit import shot

TOPO = re.compile(r"blocks=(\d+) blits=(\d+) anchors=(\d+)")


def pattern(tx, ty):
    """A tile no two pixels apart can be mistaken in: diagonal stripes 3 px wide
    whose phase depends on where the tile is."""
    y, x = np.mgrid[0:256, 0:256]
    return (((tx * 256 + x) // 3 + (ty * 256 + y) // 5) % 14 + 1).astype(np.uint8)


def topo_stats(dev):
    out = "\n".join(dev.console("topo"))
    m = TOPO.search(out)
    return dict(blocks=int(m[1]), blits=int(m[2]), anchors=int(m[3]), out=out)


def open_z16(dev):
    dev.gps(LAT, LON, speed_kmh=0)
    time.sleep(1.5)
    dev.console("screen map", 600)
    dev.wait_status(lambda s: s["screen"] == "map", timeout=10)
    time.sleep(1.5)
    for _ in range(2):
        dev.console("zoom in")
        time.sleep(1)
    time.sleep(2)


def band(img):
    # under the header band, over the scale bar; left of the zoom buttons
    return img[150:700, 0:400]


def test_a_drag_onto_the_canvas_is_a_copy_and_matches_the_tiles(doba, tmp_path):
    fx, fy = tile_xy(LAT, LON, Z)
    tx, ty = int(fx), int(fy)
    write_packs(doba.sd, {(Z, tx + dx, ty + dy): pattern(tx + dx, ty + dy)
                          for dy in range(-6, 7) for dx in range(-4, 5)})
    boot(doba)
    open_z16(doba)
    # the idle time fills the canvas round the view
    deadline = time.time() + 30
    while topo_stats(doba)["blocks"] < 64 and time.time() < deadline:
        time.sleep(0.5)
    before = topo_stats(doba)
    assert before["blocks"] >= 64, before["out"]

    for i, (x0, y0, x1, y1) in enumerate([(300, 500, 117, 381), (200, 300, 331, 523)]):
        doba.swipe(x0, y0, x1, y1)
        time.sleep(2.5)
        after = topo_stats(doba)
        assert after["anchors"] == before["anchors"], "the drag left the canvas"
        assert after["blits"] > before["blits"]
        canvas = band(shot(doba, tmp_path, "canvas%d.png" % i))
        doba.console("topo canvas off")
        time.sleep(2.5)
        direct = band(shot(doba, tmp_path, "direct%d.png" % i))
        doba.console("topo canvas on")
        time.sleep(2)
        same = (canvas == direct).mean()
        assert same > 0.999, "canvas and tiles differ on %.2f%% of the map" % (100 * (1 - same))
        before = topo_stats(doba)


def fill_canvas(dev):
    deadline = time.time() + 30
    while topo_stats(dev)["blocks"] < 64 and time.time() < deadline:
        time.sleep(0.5)


def test_the_map_follows_a_finger_while_it_drags(doba, tmp_path):
    """Frames go out while the finger is still moving, each a copy off the
    canvas; after it lets go the map is where the finger left it."""
    fx, fy = tile_xy(LAT, LON, Z)
    tx, ty = int(fx), int(fy)
    write_packs(doba.sd, {(Z, tx + dx, ty + dy): pattern(tx + dx, ty + dy)
                          for dy in range(-6, 7) for dx in range(-4, 5)})
    boot(doba)
    doba.console("timing")
    open_z16(doba)
    fill_canvas(doba)
    since = doba.log_seq()
    decoded = int(re.search(r"decoded=(\d+)", topo_stats(doba)["out"])[1])
    doba.swipe(300, 600, 150, 380, ms=1500)
    # while the finger moved nothing came off the card (after it lets go, the
    # canvas slides back round the view and draws its new edges: that may read)
    during = topo_stats(doba)
    time.sleep(2.5)
    live = [l for l in doba.log(since) if "map: live drag" in l]
    assert len(live) >= 3, "the map did not move under the finger: %s" % live
    assert int(re.search(r"decoded=(\d+)", during["out"])[1]) == decoded, "tiles were read during the drag"
    canvas = band(shot(doba, tmp_path, "after.png"))
    doba.console("topo canvas off")
    time.sleep(2.5)
    direct = band(shot(doba, tmp_path, "direct.png"))
    assert (canvas == direct).mean() > 0.999


def test_a_zoom_shows_the_enlarged_map_at_once_then_the_sharp_one(doba, tmp_path):
    fx, fy = tile_xy(LAT, LON, 15)
    tx, ty = int(fx), int(fy)
    tiles = {(15, tx + dx, ty + dy): pattern(tx + dx, ty + dy)
             for dy in range(-4, 5) for dx in range(-3, 4)}
    fx, fy = tile_xy(LAT, LON, Z)
    tx, ty = int(fx), int(fy)
    tiles.update({(Z, tx + dx, ty + dy): pattern(tx + dx, ty + dy)
                  for dy in range(-6, 7) for dx in range(-4, 5)})
    write_packs(doba.sd, tiles)
    boot(doba)
    doba.console("timing")
    doba.gps(LAT, LON, speed_kmh=0)
    time.sleep(1.5)
    doba.console("screen map", 600)
    doba.wait_status(lambda s: s["screen"] == "map", timeout=10)
    time.sleep(1.5)
    doba.console("zoom in")          # z15
    time.sleep(2)
    fill_canvas(doba)
    since = doba.log_seq()
    doba.tap(500, 598)               # the + button: z16
    time.sleep(3)
    assert any("map: zoom preview in" in l for l in doba.log(since))
    sharp = band(shot(doba, tmp_path, "sharp.png"))
    levels = set(np.unique(sharp // 17).tolist())
    assert len(levels) > 8, levels   # the z16 tiles' own stripes, not a blank or stale frame


def test_drags_one_after_another_the_same_way_stay_on_the_canvas(doba, tmp_path):
    """After a drag the idle time slides the canvas back round the view, so a
    second long drag the same way still finds the map drawn: no redraw from
    scratch (no anchor), no spinner, and what shows is still the tiles."""
    fx, fy = tile_xy(LAT, LON, Z)
    tx, ty = int(fx), int(fy)
    write_packs(doba.sd, {(Z, tx + dx, ty + dy): pattern(tx + dx, ty + dy)
                          for dy in range(-9, 10) for dx in range(-6, 7)})
    boot(doba)
    open_z16(doba)
    fill_canvas(doba)
    before = topo_stats(doba)
    for i in range(3):
        doba.swipe(250, 700, 250, 360, ms=900)      # 340 px up each time: 1020 in all
        time.sleep(1.0)
        fill_canvas(doba)
        time.sleep(1.0)
    after = topo_stats(doba)
    assert after["anchors"] == before["anchors"], "a drag fell off the canvas: " + after["out"]
    assert int(re.search(r"shifts=(\d+)", after["out"])[1]) > 0, after["out"]
    canvas = band(shot(doba, tmp_path, "after.png"))
    doba.console("topo canvas off")
    time.sleep(2.5)
    direct = band(shot(doba, tmp_path, "direct.png"))
    same = (canvas == direct).mean()
    assert same > 0.999, "canvas and tiles differ on %.2f%% of the map" % (100 * (1 - same))
