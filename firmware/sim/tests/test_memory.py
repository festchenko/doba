"""DOBA: PSRAM between the pages (doba_mem.cpp).

On the board (17 Nov) the map kept ~2.5 MB of PSRAM as caches after you left
it; the library's pictures and the home screen's topo lines then found no room
and were quietly left out. The sim counts PSRAM (OTP_SIM_PSRAM_KB caps it), so
these run the same walk — map, a picture in the library, home — with too
little PSRAM to keep everything, and the pictures must still arrive."""
import os
import re
import shutil
import time

import numpy as np

from test_doba import TOP, boot, book, doba  # noqa: F401
from test_map_canvas import open_z16, pattern, topo_stats
from test_topo import LAT, LON, Z, tile_xy, write_packs

MEM = re.compile(r"psram (\d+) free \(largest \d+, lowest (\d+)\)")


def psram(dev):
    """(free now, lowest free since boot)."""
    m = MEM.search("\n".join(dev.console("mem")))
    assert m, "no [mem] line"
    return int(m[1]), int(m[2])


def setup_card(dev, book):
    fx, fy = tile_xy(LAT, LON, Z)
    tx, ty = int(fx), int(fy)
    write_packs(dev.sd, {(Z, tx + dx, ty + dy): pattern(tx + dx, ty + dy)
                         for dy in range(-6, 7) for dx in range(-4, 5)})
    lib = os.path.join(dev.sd, "doba", "lib")
    shutil.copytree(book, os.path.join(lib, "field"))
    with open(os.path.join(lib, "sections.txt"), "w") as f:
        f.write("X|KNOTS|knots|field|Knots\nS|k|KNOTS|knots|field\n")


def walk(dev):
    """Map (canvas filled), then a picture in the library, then home."""
    open_z16(dev)
    deadline = time.time() + 30
    while topo_stats(dev)["blocks"] < 64 and time.time() < deadline:
        time.sleep(0.5)
    after_map = psram(dev)[0]
    dev.console("screen library", 800)
    time.sleep(0.8)
    since = dev.log_seq()
    dev.tap(150, TOP + 96)                                  # Topics tab
    time.sleep(0.6)
    dev.tap(96, TOP + 194 + 28 + 54)
    dev.wait_log(r"\[lib\] picture [0-9a-f]{10} 480x320", since=since, timeout=15)
    dev.console("screen home", 800)
    time.sleep(1.5)
    return after_map, since


def test_the_map_gives_its_caches_up_for_a_picture(doba, book, monkeypatch):
    setup_card(doba, book)
    # With room to spare: what the walk needs at most, and what the map keeps.
    boot(doba)
    base = psram(doba)[0]
    after_map, _ = walk(doba)
    peak = (8 << 20) - psram(doba)[1]
    kept = base - after_map
    doba.stop()
    assert kept > 1 << 20, "the map kept only %d KB: the test squeezes nothing" % (kept >> 10)

    # Again with half the map's caches' worth less than that: the picture and
    # the home screen get their room only if the map lets go.
    monkeypatch.setenv("OTP_SIM_PSRAM_KB", str((peak - kept // 2) >> 10))
    boot(doba)
    try:
        _, since = walk(doba)
    except Exception:
        print("\n".join(l for l in doba.log(0) if re.search(r"\[(mem|lib|pois|topo)\]|alloc|fail", l))[-6000:])
        raise
    log = "\n".join(doba.log(since))
    assert "letting the caches go" in log, "the squeeze never happened: the test proves nothing"
    assert "STILL NO ROOM" not in log
