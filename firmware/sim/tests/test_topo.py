"""DOBA: the topo base layer (src/topo_layer.cpp).

Tile packs are written with maps/tilepack.py — the same code the map pipeline
uses — so these tests also hold the firmware reader to the file format. What is
checked is the glass: the grey levels the pack holds must be the grey levels on
the panel, the right way round, centred on the rider."""
import os
import sys
import time

import numpy as np
import pytest
from PIL import Image

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "maps"))
from tilepack import BASE_CELL_Z, PackWriter, pack_path  # noqa: E402

LAT, LON = 52.042, 6.617          # Groenlo
Z = 16                            # what the default 2 m/px snaps to here
MAP_TOP, MAP_BOTTOM = 70, 800     # panel rows between the status bar and the data strip


def tile_xy(lat, lon, z):
    import math
    n = 2 ** z
    x = (lon + 180) / 360 * n
    y = (1 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2 * n
    return x, y


def write_packs(sd, tiles):
    """tiles: {(z, x, y): (256, 256) uint8 levels}. Writes /doba/map packs."""
    root = os.path.join(sd, "doba", "map")
    writers = {}
    for (z, x, y), lv in tiles.items():
        if z <= 8:
            key = (BASE_CELL_Z, 0, 0)
        else:
            key = (8, x >> (z - 8), y >> (z - 8))
        if key not in writers:
            writers[key] = PackWriter(pack_path(root, *key), *key)
        writers[key].add(z, x, y, lv)
    for w in writers.values():
        w.close()


def quadrant_tile(level_tl, level_br):
    """A tile whose top-left quarter is one grey and the rest another."""
    t = np.full((256, 256), level_br, np.uint8)
    t[:128, :128] = level_tl
    return t


def glass(device, tmp_path, name="shot.png"):
    p = str(tmp_path / name)
    device.screenshot(p)
    return np.asarray(Image.open(p).convert("L"))


def show_map(device):
    device.gps(LAT, LON, speed_kmh=3, course=0)
    time.sleep(1.5)
    # Home: dash -> HOME -> map (the sim boots to the dashboard, conftest.py).
    for _ in range(3):
        if device.status()["screen"] == "map":
            break
        device.button("home")
        time.sleep(0.6)
    device.wait_status(lambda s: s["screen"] == "map", timeout=10)
    time.sleep(3)


def test_topo_tiles_are_drawn_level_for_level(device, tmp_path):
    fx, fy = tile_xy(LAT, LON, Z)
    tx, ty = int(fx), int(fy)
    tiles = {}
    for dy in range(-3, 4):
        for dx in range(-2, 3):
            tiles[(Z, tx + dx, ty + dy)] = quadrant_tile(3, 12)
    write_packs(device.sd, tiles)
    device.reboot()
    show_map(device)
    img = glass(device, tmp_path)
    band = img[MAP_TOP:MAP_BOTTOM, 0:440]          # left of the zoom buttons
    levels = set(np.unique(band // 17).tolist())
    # Only the two pack greys, plus the black/white of the rider marker and
    # the scale bar, and the dark grey (2) of the kilometre grid over them.
    assert {3, 12} <= levels, levels
    assert levels <= {0, 2, 3, 12, 15}, levels


def test_topo_is_north_up_and_centred(device, tmp_path):
    """The rider's own tile: its dark top-left quarter must be up and left of
    the rider by exactly where the rider sits inside the tile."""
    fx, fy = tile_xy(LAT, LON, Z)
    tx, ty = int(fx), int(fy)
    tiles = {(Z, tx + dx, ty + dy): np.full((256, 256), 12, np.uint8)
             for dy in range(-3, 4) for dx in range(-2, 3)}
    tiles[(Z, tx, ty)] = quadrant_tile(3, 12)
    write_packs(device.sd, tiles)
    device.reboot()
    show_map(device)
    img = glass(device, tmp_path)
    rider_x, rider_y = 270, 430                      # ui_dashboard buildMapScreenData
    left = rider_x - int((fx - tx) * 256)
    top = rider_y - int((fy - ty) * 256)
    # inside the dark quarter, away from its edges
    assert img[top + 64, left + 64] // 17 == 3
    # just outside it, to the right and below
    assert img[top + 64, left + 192] // 17 == 12
    assert img[top + 192, left + 64] // 17 == 12


def test_no_pack_falls_back_to_vector_maps(device, tmp_path):
    show_map(device)
    out = "\n".join(device.console("topo"))
    assert "active=0" in out, out


def test_topo_reports_active_and_decodes(device, tmp_path):
    fx, fy = tile_xy(LAT, LON, Z)
    tx, ty = int(fx), int(fy)
    rng = np.random.default_rng(3)
    tiles = {(Z, tx + dx, ty + dy): rng.integers(0, 16, (256, 256), dtype=np.uint8)
             for dy in range(-3, 4) for dx in range(-2, 3)}
    write_packs(device.sd, tiles)
    device.reboot()
    show_map(device)
    out = "\n".join(device.console("topo"))
    assert "active=1" in out, out
    decoded = int(out.split("decoded=")[1].split()[0])
    assert decoded >= 6, out


def test_position_readout_shows_mgrs(device, tmp_path):
    """The strip under the topo map is the MGRS readout, not the bike cells:
    it must be there and ink-heavy where the bike strip had its cell frames."""
    fx, fy = tile_xy(LAT, LON, Z)
    tx, ty = int(fx), int(fy)
    tiles = {(Z, tx + dx, ty + dy): np.full((256, 256), 12, np.uint8)
             for dy in range(-3, 4) for dx in range(-2, 3)}
    write_packs(device.sd, tiles)
    device.reboot()
    show_map(device)
    img = glass(device, tmp_path)
    strip = img[815:960, :]
    assert (strip < 128).mean() > 0.03          # three rows of text
    out = "\n".join(device.console("mils 6400"))
    assert "[mils] 6400" in out, out
