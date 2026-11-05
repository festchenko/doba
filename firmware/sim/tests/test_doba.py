"""DOBA's own pages: the library (search, links, back), waypoints, settings.

The sim boots to HOME here (DOBA_START=home) and the radio band is Doba's own
default (EU_868), unlike the inherited OTP suites (conftest.py)."""
import os
import shutil
import subprocess
import sys
import time

import pytest

from otpsim import SimDevice

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(HERE, "..", "..", "..", "library")
W, TOP, KTOP = 540, 64, 960 - 336
ROWS = ["qwertyuiop", "asdfghjkl", "zxcvbnm"]


def key(ch):
    """Centre of a letter on the EN keyboard (doba_keyboard.cpp geometry)."""
    for r, row in enumerate(ROWS):
        if ch in row:
            units = len(row) * 2 + (6 if r == 2 else 0)
            u = W // max(units, 24)
            x = (W - units * u) // 2 + (u * 3 if r == 2 else 0)
            return x + row.index(ch) * 2 * u + u, KTOP + r * 84 + 42
    raise KeyError(ch)


@pytest.fixture
def doba(monkeypatch, tmp_path):
    monkeypatch.setenv("DOBA_START", "home")
    monkeypatch.setenv("DOBA_MESH_REGION", "")
    dev = SimDevice(args=["--gps-baud", "115200", "--lora"])
    yield dev
    dev.stop()


def boot(dev):
    dev.launch(30)
    dev.wait_status(lambda s: s["screen"] == "other" and s["paints"] > 0)


@pytest.fixture(scope="module")
def book(tmp_path_factory):
    pytest.importorskip("libzim")
    d = tmp_path_factory.mktemp("lib")
    zim = str(d / "test.zim")
    subprocess.run([sys.executable, os.path.join(LIB, "test", "make_test_zim.py"), zim],
                   check=True, capture_output=True)
    out = str(d / "field")
    subprocess.run([sys.executable, os.path.join(LIB, "zim2doba.py"), zim, out, "--name", "Field",
                    "--source", "Test book · CC0"], check=True, capture_output=True)
    return out


def test_an_article_link_opens_its_article_and_back_returns(doba, book):
    shutil.copytree(book, os.path.join(doba.sd, "doba", "lib", "field"))
    boot(doba)
    doba.console("screen library", 800)
    time.sleep(0.6)
    for c in "wat":
        doba.tap(*key(c))
        time.sleep(0.2)
    time.sleep(0.6)
    since = doba.log_seq()
    doba.tap(200, TOP + 146 + 29)
    doba.wait_log(r"\[lib\] open 'Water purification' pages=\d+ links=2", since=since)
    time.sleep(0.6)
    since = doba.log_seq()
    doba.tap(50, 243)                                       # "knots", underlined
    doba.wait_log(r"\[lib\] open 'Knots'", since=since)
    time.sleep(0.6)
    since = doba.log_seq()
    doba.button("home")                                     # back along the link
    doba.wait_log(r"\[lib\] open 'Water purification'", since=since)


def test_points_from_the_card_show_azimuth_and_go_to_sets_the_map(doba):
    os.makedirs(os.path.join(doba.sd, "doba"), exist_ok=True)
    with open(os.path.join(doba.sd, "doba", "points.txt"), "w") as f:
        f.write("CAMP;52.0610;6.6440\nBRIDGE;52.0300;6.5900\n")
    boot(doba)
    doba.gps(52.0421, 6.6170)
    time.sleep(2)
    doba.console("screen points", 800)
    time.sleep(0.8)
    doba.tap(270, TOP + 262 + 40)                           # CAMP
    time.sleep(0.8)
    doba.tap(270, 960 - 196 + 40)                           # GO TO
    doba.wait_status(lambda s: s["screen"] == "map", timeout=5)
    # MARK HERE appends a point to the card's file.
    doba.console("screen points", 800)
    time.sleep(0.8)
    doba.tap(130, TOP + 158 + 40)
    time.sleep(1.5)
    with open(os.path.join(doba.sd, "doba", "points.txt")) as f:
        lines = f.read().splitlines()
    assert len(lines) == 3 and lines[2].startswith("PT03;52.0421")


def test_settings_change_the_band_on_the_device(doba):
    boot(doba)
    doba.console("screen setup", 800)
    time.sleep(0.6)
    doba.tap(270, TOP + 8 + 86 * 0 + 40)                    # RADIO on

    def freq(ok):
        end = time.time() + 8
        while time.time() < end:
            f = doba.lora().get("freq_mhz") or 0
            if ok(f):
                return f
            time.sleep(0.2)
        raise AssertionError(doba.lora())
    freq(lambda f: abs(f - 869.525) < 1e-3)
    doba.tap(270, TOP + 8 + 86 * 1 + 40)                    # BAND: EU_868 -> UA_868
    freq(lambda f: 868.0 <= f <= 868.6)


def test_ground_height_comes_from_the_card_dem(doba):
    """maps/dempack.py file on the card -> the state's map elevation at the fix."""
    sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "maps"))
    import numpy as np
    import dempack
    n = 1200
    k = np.linspace(0, 1, n)
    x, y = np.meshgrid(k, k)
    h = (40 + 300 * np.exp(-((x - 0.4) ** 2 + (y - 0.6) ** 2) * 12)).astype(np.float32)
    os.makedirs(os.path.join(doba.sd, "doba", "dem"), exist_ok=True)
    with open(os.path.join(doba.sd, "doba", "dem", "N52E006.dem"), "wb") as f:
        f.write(dempack.encode(h, 52, 6))
    boot(doba)
    lat, lon = 52.0421, 6.6170
    doba.gps(lat, lon, alt=999)
    fi, fj = (53 - lat) * n - 0.5, (lon - 6) * n - 0.5
    i0, j0 = int(fi), int(fj)
    di, dj = fi - i0, fj - j0
    r = np.rint(h)
    want = ((r[i0, j0] * (1 - dj) + r[i0, j0 + 1] * dj) * (1 - di)
            + (r[i0 + 1, j0] * (1 - dj) + r[i0 + 1, j0 + 1] * dj) * di)
    st = doba.wait_status(lambda s: s["ride"].get("ground_m", -99999) > -99999, timeout=8)
    assert abs(st["ride"]["ground_m"] - want) < 0.6, (st["ride"]["ground_m"], want)
    # Off the card's ground: no height (and GPS altitude remains the fallback).
    doba.gps(50.5, 6.6, alt=999)
    doba.wait_status(lambda s: s["ride"].get("ground_m") == -99999, timeout=8)
