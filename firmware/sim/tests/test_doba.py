"""DOBA's own pages: the library (search, links, back), waypoints, settings.

The sim boots to HOME here (DOBA_START=home) and the radio band is Doba's own
default (EU_868), unlike the inherited OTP suites (conftest.py)."""
import os
import re
import shutil
import subprocess
import sys
import time

import pytest

from otpsim import SimDevice

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(HERE, "..", "..", "..", "library")
W, TOP, KTOP = 540, 64, 960 - 336
RES_TOP = TOP + 142   # first search result (doba_library.cpp)
ROWS = ["qwertyuiop", "asdfghjkl", "zxcvbnm"]


def key(ch):
    """Centre of a letter on the EN keyboard (doba_keyboard.cpp geometry)."""
    for r, row in enumerate(ROWS):
        if ch in row:
            # doba_keyboard.cpp: one key width (the longest row) over the span
            # inside 6 px margins; shorter rows centred; row 3 between SHIFT and DEL.
            span = W - 12
            kw = span // max(len(x) for x in ROWS)
            if r == 2:
                side = (span - len(row) * kw) // 2
                x = 6 + side
            else:
                x = 6 + (span - len(row) * kw) // 2
            return x + row.index(ch) * kw + kw // 2, KTOP + r * 84 + 42
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
    doba.tap(270, TOP + 150)                                # "Search all books", under the tabs
    time.sleep(0.6)
    for c in "wat":
        doba.tap(*key(c))
        time.sleep(0.2)
    time.sleep(0.6)
    since = doba.log_seq()
    doba.tap(200, RES_TOP + 30)
    doba.wait_log(r"\[lib\] open 'Water purification' pages=\d+ links=2", since=since)
    time.sleep(0.6)
    since = doba.log_seq()
    doba.tap(420, 239)                                      # "knots", underlined
    doba.wait_log(r"\[lib\] open 'Knots'", since=since)
    time.sleep(0.6)
    since = doba.log_seq()
    doba.tap(50, 30)                                        # BACK: back along the link
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
    doba.tap(270, TOP + 436 + 40)                           # CAMP
    time.sleep(0.8)
    doba.tap(270, 960 - 196 + 40)                           # GO TO
    doba.wait_status(lambda s: s["screen"] == "map", timeout=5)
    # MARK HERE appends a point to the card's file.
    doba.console("screen points", 800)
    time.sleep(0.8)
    doba.tap(130, TOP + 306 + 30)
    time.sleep(1.5)
    with open(os.path.join(doba.sd, "doba", "points.txt")) as f:
        lines = f.read().splitlines()
    assert len(lines) == 3 and lines[2].startswith("PT03;52.0421")


def test_settings_change_the_band_on_the_device(doba):
    boot(doba)
    doba.console("screen setup", 800)
    time.sleep(0.6)
    # Rows sit under section headers: RADIO row at TOP+4+38, 84 px each.
    doba.tap(500, TOP + 4 + 34 + 78 + 39)                   # RADIO: the ON half

    def freq(ok):
        end = time.time() + 8
        while time.time() < end:
            f = doba.lora().get("freq_mhz") or 0
            if ok(f):
                return f
            time.sleep(0.2)
        raise AssertionError(doba.lora())
    freq(lambda f: abs(f - 869.525) < 1e-3)
    doba.tap(500, TOP + 4 + 34 + 2 * 78 + 39)               # COUNTRY: ">" AUTO -> EU_868 by hand
    time.sleep(0.6)
    doba.tap(500, TOP + 4 + 34 + 2 * 78 + 39)               # ">" EU_868 -> UA_868
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


def test_a_manual_pdf_becomes_chapters_found_by_their_own_words(doba, tmp_path):
    """library/docs2doba.py: a PDF with bookmarks -> contents + one article a
    chapter, each also indexed by its bare name ("Shelter")."""
    pymupdf = pytest.importorskip("pymupdf")
    src = tmp_path / "in"
    src.mkdir()
    doc = pymupdf.open()
    toc = []
    for i, (h, body) in enumerate([("Chapter 1 Basics", "Plan and stay calm."),
                                   ("Chapter 2 Shelter", "Build it before dark, out of the wind."),
                                   ("Chapter 3 Water", "Boil it for one minute.")]):
        p = doc.new_page()
        p.insert_text((72, 90), h, fontsize=18)
        for k in range(5):
            p.insert_text((72, 130 + 16 * k), body, fontsize=10)
        toc.append([1, h, i + 1])
    doc.set_toc(toc)
    doc.set_metadata({"title": "FM 21-76 Survival"})
    doc.save(str(src / "fm.pdf"))
    out = os.path.join(doba.sd, "doba", "lib", "manuals")
    subprocess.run([sys.executable, os.path.join(LIB, "docs2doba.py"), str(src), out, "--name", "Manuals",
                    "--source", "US Army · public domain"], check=True, capture_output=True)
    boot(doba)
    doba.console("screen library", 800)
    time.sleep(0.6)
    doba.tap(270, TOP + 150)                                # "Search all books", under the tabs
    time.sleep(0.6)
    for c in "shel":
        doba.tap(*key(c))
        time.sleep(0.2)
    time.sleep(0.6)
    since = doba.log_seq()
    doba.tap(200, RES_TOP + 30)
    doba.wait_log(r"\[lib\] open 'Shelter'", since=since)


def test_on_water_the_card_gives_depth_not_height(doba):
    """dempack --bathy codes sea as WATER_BASE - depth; the device reads it as
    depth, and land below sea level (polders) still as height."""
    sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "maps"))
    import numpy as np
    import dempack
    n = 1200
    h = np.full((n, n), 5.0, dtype=np.float32)
    h[:, :400] = -6.0
    h[:, 800:] = 0.0
    b = np.full_like(h, np.nan)
    b[:, 800:] = -23.0
    dempack.merge_water(h, b)
    os.makedirs(os.path.join(doba.sd, "doba", "dem"), exist_ok=True)
    with open(os.path.join(doba.sd, "doba", "dem", "N52E004.dem"), "wb") as f:
        f.write(dempack.encode(h, 52, 4))
    boot(doba)
    for lon, want in [(4.9, r"\[dem\] water 23\.0"), (4.1, r"\[dem\] land -6\.0"), (4.5, r"\[dem\] land 5\.0")]:
        doba.gps(52.5, lon)
        time.sleep(1.5)
        since = doba.log_seq()
        doba.console("dem", 500)
        doba.wait_log(want, since=since)


def test_library_sections_open_their_guides_and_emergency_buttons_jump(doba, book):
    """library/sections.py resolves sections.src against the built books; the
    device shows EMERGENCY buttons and icon sections that open guides."""
    lib = os.path.join(doba.sd, "doba", "lib")
    shutil.copytree(book, os.path.join(lib, "field"))
    src = os.path.join(os.path.dirname(book), "t.src")
    with open(src, "w") as f:
        f.write("emergency BLEEDING | bleed | nope:X ; field:Tourniquet\n"
                "section water | WATER | water | field\n"
                "  * Make water safe | boil, filter | field:Water purification\n"
                "  - Missing | gone | field:No such article\n")
    r = subprocess.run([sys.executable, os.path.join(LIB, "sections.py"), lib, "--src", src],
                       check=True, capture_output=True, text=True)
    assert "1 sections, 1 guides, 1 emergency buttons (1 not on this card)" in r.stdout
    boot(doba)
    doba.console("screen library", 800)
    time.sleep(0.8)
    since = doba.log_seq()
    doba.tap(150, TOP + 96)                                 # Topics tab
    time.sleep(0.6)
    doba.tap(96, TOP + 194 + 28 + 54)                        # BLEEDING
    doba.wait_log(r"\[lib\] open 'Tourniquet'", since=since)
    time.sleep(0.6)
    doba.tap(50, 30)                                        # BACK: to the sections
    time.sleep(0.8)
    doba.tap(96, TOP + 194 + 28 + 108 + 18 + 40)             # WATER, first cell
    time.sleep(0.8)
    since = doba.log_seq()
    doba.tap(270, TOP + 120 + 50)                           # START HERE card
    doba.wait_log(r"\[lib\] open 'Water purification'", since=since)


def test_a_figure_in_a_manual_is_shown_as_a_picture_in_its_article(doba, tmp_path):
    """docs2doba renders a PDF figure (vector drawing) to <book>/img/*.dgi and
    puts it in the text; the article draws it from the card."""
    pymupdf = pytest.importorskip("pymupdf")
    pytest.importorskip("PIL")
    src = tmp_path / "in"
    src.mkdir()
    doc = pymupdf.open()
    p = doc.new_page()
    p.insert_text((72, 90), "Square knot", fontsize=18)
    p.insert_text((72, 130), "Tie right over left, then left over right.", fontsize=10)
    p.draw_circle((300, 300), 80, color=(0, 0, 0), width=4)
    p.draw_line((180, 300), (420, 300), color=(0, 0, 0), width=4)
    p.insert_text((72, 420), "Figure 1. The knot.", fontsize=10)
    doc.save(str(src / "Knots (US Army).pdf"))
    out = os.path.join(doba.sd, "doba", "lib", "manuals")
    subprocess.run([sys.executable, os.path.join(LIB, "docs2doba.py"), str(src), out, "--name", "Manuals",
                    "--titles-from-filenames"], check=True, capture_output=True)
    assert os.listdir(os.path.join(out, "img"))
    lib = os.path.join(doba.sd, "doba", "lib")
    with open(os.path.join(lib, "sections.txt"), "w") as f:
        f.write("X|KNOT|knots|manuals|Knots (US Army)\nS|k|KNOTS|knots|manuals\n")
    boot(doba)
    doba.console("screen library", 800)
    time.sleep(0.8)
    since = doba.log_seq()
    doba.tap(150, TOP + 96)                                 # Topics tab
    time.sleep(0.6)
    doba.tap(96, TOP + 194 + 28 + 54)
    doba.wait_log(r"\[lib\] open 'Knots \(US Army\)'", since=since)
    doba.wait_log(r"\[lib\] picture \d+ \d+x\d+", since=since)


def test_a_picture_in_a_zim_article_reaches_the_screen_from_its_pack(doba, book):
    """zim2doba packs ZIM pictures into img/NN.dgp + img/index.dgx (not a file
    each: a cluster apiece on the card); icons stay out."""
    import struct
    lib = os.path.join(doba.sd, "doba", "lib")
    shutil.copytree(book, os.path.join(lib, "field"))
    ix = os.path.join(lib, "field", "img", "index.dgx")
    if not os.path.exists(ix):
        pytest.skip("no Pillow for the converter")
    with open(ix, "rb") as f:
        assert f.read(4) == b"DGX1"
        assert struct.unpack("<I", f.read(4))[0] == 1
    assert not [f for f in os.listdir(os.path.join(lib, "field", "img")) if f.endswith(".dgi")]
    with open(os.path.join(lib, "sections.txt"), "w") as f:
        f.write("X|KNOTS|knots|field|Knots\nS|k|KNOTS|knots|field\n")
    boot(doba)
    doba.console("screen library", 800)
    time.sleep(0.8)
    since = doba.log_seq()
    doba.tap(150, TOP + 96)                                 # Topics tab
    time.sleep(0.6)
    doba.tap(96, TOP + 194 + 28 + 54)
    doba.wait_log(r"\[lib\] picture [0-9a-f]{10} 480x320", since=since)


def test_stealth_from_the_home_bar_goes_silent_and_needs_a_5s_hold_to_end(doba):
    """One tap on HOME's stealth bar: no transmit, no BT advertising, no light.
    SILENT also puts the LoRa radio to sleep; ending stealth takes a finger held
    5 s and brings the radio back as it was."""
    boot(doba)
    since = doba.log_seq()
    doba.console("stealth", 800)
    before = doba.wait_log(r"\[stealth\] state off radio=\d", since=since)[-1]
    since = doba.log_seq()
    doba.tap(270, 960 - 10 - 41)                          # STEALTH bar
    doba.wait_log(r"\[stealth\] on \(listening\)", since=since)
    doba.wait_log(r"ble: stealth", since=since)
    since = doba.log_seq()
    doba.console("stealth silent", 800)
    doba.wait_log(r"\[stealth\] state silent radio=0", since=since)
    since = doba.log_seq()
    doba.console("screen home", 800)
    time.sleep(0.6)
    doba.tap(270, 960 - 10 - 41)                          # a tap only says to hold
    time.sleep(1.0)
    doba.console("stealth", 800)
    doba.wait_log(r"\[stealth\] state silent", since=since)
    doba.tap(270, 960 - 10 - 41, ms=5300)                 # held 5 s: that ends it
    doba.wait_log(r"\[stealth\] off", since=since)
    time.sleep(1.0)                                       # the mesh task applies it
    since = doba.log_seq()
    doba.console("stealth", 800)
    doba.wait_log(r"\[stealth\] state off radio=" + before, since=since)   # as it was


def test_reader_text_size_and_contents(doba, book):
    """Aa steps the text size (and keeps it); the page counter opens the contents."""
    shutil.copytree(book, os.path.join(doba.sd, "doba", "lib", "field"))
    boot(doba)
    doba.console("screen library", 800)
    time.sleep(0.6)
    doba.tap(270, TOP + 150)
    time.sleep(0.6)
    for c in "wat":
        doba.tap(*key(c))
        time.sleep(0.2)
    time.sleep(0.6)
    since = doba.log_seq()
    doba.tap(200, RES_TOP + 30)
    doba.wait_log(r"\[lib\] open 'Water purification'", since=since)
    time.sleep(0.6)
    since = doba.log_seq()
    doba.tap(500, TOP + 26)                                 # Aa
    doba.wait_log(r"\[lib\] text size 2 pages=\d+", since=since)
    time.sleep(0.6)
    since = doba.log_seq()
    doba.tap(270, 960 - 38)                                 # "1 / n  CONTENTS"
    doba.wait_log(r"\[lib\] contents [1-9]", since=since)
    time.sleep(0.6)
    since = doba.log_seq()
    doba.tap(200, TOP + 66 - 8 + 30)                        # the first heading
    doba.wait_log(r"\[lib\] contents -> page \d+", since=since)


def test_the_home_key_goes_home_from_anywhere(doba):
    """HOME is the main screen, from a sub-page and from the map, in one press."""
    boot(doba)
    for where in ("setup", "map", "comms", "library"):
        doba.console("screen " + where, 800)
        time.sleep(0.8)
        since = doba.log_seq()
        doba.button("home")
        doba.wait_log(r"\[ui\] home key", since=since)
        time.sleep(0.6)
        doba.wait_status(lambda s: s["screen"] == "other", timeout=3)


def test_the_library_opens_on_its_books_and_a_book_opens_its_documents(doba, book):
    """BOOKS: a cover per book; a book's page lists what it holds; a row opens it."""
    shutil.copytree(book, os.path.join(doba.sd, "doba", "lib", "field"))
    boot(doba)
    doba.console("screen library", 800)
    time.sleep(0.8)
    since = doba.log_seq()
    doba.tap(100, TOP + 204 + 75)                            # the first cover
    doba.wait_log(r"\[lib\] book field", since=since)
    time.sleep(0.8)
    since = doba.log_seq()
    doba.tap(200, TOP + 216 + 40 + 30)                      # its first document
    doba.wait_log(r"\[lib\] open '", since=since)


def big_book(book, out, n=60000):
    """The test book, its index padded with made-up titles to Wikipedia's shape:
    far too many to page through."""
    import random
    import struct
    sys.path.insert(0, LIB)
    from dobafold import fold
    rec, hdr = struct.Struct("<56s56sHHIII"), struct.Struct("<8sIII12s")
    shutil.copytree(book, out)
    raw = open(os.path.join(book, "index.dti"), "rb").read()
    h = hdr.unpack(raw[:32])
    recs = [rec.unpack(raw[32 + i * 128:160 + i * 128]) for i in range(h[1])]
    rows = {r[1].rstrip(b"\0").decode(): r for r in recs}
    for t in ["A*", "A+", "A-", "A1", "A2 road", "A 1", "/a/", "Ab!"]:
        rows.setdefault(t, recs[0][:1] + (t.encode(),) + recs[0][2:])
    rnd = random.Random(1)
    syl = ["ab", "al", "an", "ar", "ba", "ca", "da", "el", "ga", "ka", "ma", "na", "pa", "ra", "sa", "ta"]
    while len(rows) < n:
        t = "".join(rnd.choice(syl) for _ in range(rnd.randint(2, 4))).capitalize()
        rows.setdefault(t, recs[0][:1] + (t.encode(),) + recs[0][2:])
    out_rows = sorted((fold(t)[:55], t.encode()[:55]) + r[2:] for t, r in rows.items())
    with open(os.path.join(out, "index.dti"), "wb") as f:
        f.write(hdr.pack(h[0], len(out_rows), h[2], h[3], h[4]))
        for r in out_rows:
            f.write(rec.pack(*r))


def test_a_big_book_opens_on_its_picks_and_a_z_splits_until_it_lists(doba, book):
    """Wikipedia: picks first; A-Z narrows "A" -> "Ab"... until a short list."""
    lib = os.path.join(doba.sd, "doba", "lib")
    big_book(book, os.path.join(lib, "field"))
    src = os.path.join(doba.sd, "t.src")
    with open(src, "w") as f:
        f.write("section water | WATER | water | field\n"
                "  * Make water safe | boil, filter | field:Water purification\n"
                "  - Knots | tie | field:Knots\n")
    subprocess.run([sys.executable, os.path.join(LIB, "sections.py"), lib, "--src", src],
                   check=True, capture_output=True, text=True)
    picks = os.path.join(doba.sd, "picks.src")
    with open(picks, "w") as f:
        f.write("No such page|Knots\nMissing too\n")
    r = subprocess.run([sys.executable, os.path.join(LIB, "picks.py"), os.path.join(lib, "field"), picks],
                       check=True, capture_output=True, text=True)
    assert "1 picks, 1 missing" in r.stdout          # its own picks before the topics'
    boot(doba)
    doba.console("screen library", 800)
    time.sleep(0.8)
    since = doba.log_seq()
    doba.tap(100, TOP + 204 + 75)                            # the cover
    doba.wait_log(r"\[lib\] book field", since=since)
    time.sleep(0.8)
    doba.screenshot(os.path.join(doba.dir, "big.png"))
    since = doba.log_seq()
    doba.tap(200, TOP + 300)                                # the first pick
    doba.wait_log(r"\[lib\] open 'Knots'", since=since)
    time.sleep(0.6)
    doba.tap(50, 30)                                        # BACK to the book
    time.sleep(0.8)
    doba.tap(100, TOP + 186)                                # Browse A-Z
    time.sleep(0.8)
    since = doba.log_seq()
    doba.tap(50, TOP + 250)                                 # A
    doba.wait_log(r"\[lib\] index 'A': \d+ entries, \d+ tabs", since=since)
    time.sleep(0.8)
    doba.screenshot(os.path.join(doba.dir, "tabs.png"))
    for _ in range(4):                                      # first tab until it lists
        since = doba.log_seq()
        doba.tap(70, TOP + 216 - 22 + 30)
        line = doba.wait_log(r"\[lib\] index '[^']+': \d+ entries, (\d+) tabs", since=since)
        time.sleep(0.6)
        if " 0 tabs" in str(line):
            break
    doba.screenshot(os.path.join(doba.dir, "list.png"))
    since = doba.log_seq()
    doba.tap(200, TOP + 216 - 30 + 30)                      # the first title
    doba.wait_log(r"\[lib\] open '", since=since)


def test_a_book_opens_on_its_menu_and_a_row_opens_its_page(doba, book):
    """menu.txt (library/bookmenu.py): groups, a line under each title."""
    out = os.path.join(doba.sd, "doba", "lib", "field")
    shutil.copytree(book, out)
    with open(os.path.join(out, "menu.txt"), "w") as f:
        f.write("# Start here\nKnots|tie them, untie them\n# Water\nWater purification|boil, filter\n")
    with open(os.path.join(out, "info.txt"), "a") as f:
        f.write("about=A test book\nunit=guides\n")
    boot(doba)
    doba.console("screen library", 800)
    time.sleep(0.8)
    since = doba.log_seq()
    doba.tap(100, TOP + 204 + 75)                            # the cover
    doba.wait_log(r"\[lib\] book field", since=since)
    time.sleep(0.8)
    doba.screenshot(os.path.join(doba.dir, "menu.png"))
    since = doba.log_seq()
    doba.tap(200, TOP + 176 + 50 + 40)                      # under START HERE: Knots
    doba.wait_log(r"\[lib\] open 'Knots'", since=since)


def test_recent_lists_what_was_read_and_opens_it_again(doba, book):
    """RECENT (the third tab): newest first, kept on the card."""
    shutil.copytree(book, os.path.join(doba.sd, "doba", "lib", "field"))
    boot(doba)
    doba.console("screen library", 800)
    time.sleep(0.6)
    doba.tap(270, TOP + 150)                                 # search all books
    time.sleep(0.6)
    for c in "kno":
        doba.tap(*key(c))
        time.sleep(0.2)
    time.sleep(0.6)
    since = doba.log_seq()
    doba.tap(200, RES_TOP + 30)
    doba.wait_log(r"\[lib\] open 'Knots'", since=since)
    with open(os.path.join(doba.sd, "doba", "recent.txt")) as f:
        assert f.read().startswith("field|Knots")
    doba.tap(50, 30)                                        # BACK: the search
    time.sleep(0.6)
    doba.tap(50, 30)                                        # BACK: the shelf
    time.sleep(0.6)
    doba.tap(240, TOP + 96)                                 # Recent tab
    time.sleep(0.6)
    since = doba.log_seq()
    doba.tap(200, TOP + 196 + 40)                           # the first row
    doba.wait_log(r"\[lib\] open 'Knots'", since=since)


def test_sos_is_sent_only_by_a_5s_hold(doba):
    """A tap on the SOS button only says how; a finger held 5 s sends it."""
    boot(doba)
    doba.console("screen sos", 800)
    time.sleep(0.8)
    since = doba.log_seq()
    doba.tap(270, 470)                                    # a tap: nothing goes out
    with pytest.raises(Exception):
        doba.wait_log(r"\[sos\] sent", since=since, timeout=1.5)
    doba.tap(270, 470, ms=5300)                           # held: sent
    doba.wait_log(r"\[sos\] sent #1", since=since, timeout=8)


def write_pois(path, pois):
    """A pois.dgp (maps/pois.py) from (lat, lon, kind, name)."""
    import math
    import struct
    cells = {}
    for p in pois:
        cells.setdefault((math.floor(p[0] / 0.25), math.floor(p[1] / 0.25)), []).append(p)
    order = sorted(cells)
    with open(path, "wb") as f:
        f.write(b"DGP1" + struct.pack("<II", len(pois), len(order)))
        first = 0
        for c in order:
            f.write(struct.pack("<hhII", c[0], c[1], first, len(cells[c])))
            first += len(cells[c])
        for c in order:
            for lat, lon, k, name in cells[c]:
                f.write(struct.pack("<ffBB34s", lat, lon, k, 0, name.encode()))


def test_nearby_lists_the_nearest_of_each_kind_and_a_tap_goes_there(doba):
    """Position > Nearby places: kinds with the nearest; a kind lists its places;
    a place becomes a point to go to."""
    os.makedirs(os.path.join(doba.sd, "doba", "map"), exist_ok=True)
    write_pois(os.path.join(doba.sd, "doba", "map", "pois.dgp"), [
        (52.0500, 6.6300, 1, "Far hospital"), (52.0430, 6.6200, 1, "Near hospital"),
        (52.0410, 6.6150, 4, ""), (52.0450, 6.6100, 5, "Spar")])
    boot(doba)
    doba.gps(52.0421, 6.6170)
    time.sleep(2)
    doba.console("screen points", 800)
    time.sleep(0.8)
    doba.tap(440, TOP + 436 - 24)                           # Nearby places ›
    time.sleep(1.0)
    doba.screenshot(os.path.join(doba.dir, "nearby.png"))
    since = doba.log_seq()
    doba.tap(200, TOP + 112 + 40)                           # Hospital, first kind
    doba.wait_log(r"\[pois\] kind Hospital", since=since)
    time.sleep(0.8)
    since = doba.log_seq()
    doba.tap(200, TOP + 112 + 40)                           # the nearest one
    doba.wait_log(r"\[pois\] point \d+ 'Near hospital'", since=since)


def test_long_cyrillic_chapter_titles_stay_apart_and_links_reach_each(doba, book):
    """Wikisource chapters fold to the same first 55 bytes: the key keeps a
    hash of the whole title (dobafold.key), so each chapter is found, and the
    list shows the short end of the path ("Захар Беркут / II")."""
    shutil.copytree(book, os.path.join(doba.sd, "doba", "lib", "field"))
    boot(doba)
    for t, want in [("Твори (Франко, 1956–1962)/13/Захар Беркут/II", "Захар Беркут / II"),
                    ("Твори (Франко, 1956–1962)/13/Захар Беркут/I", "Захар Беркут / I"),
                    ("Твори (Франко, 1956–1962)/13/Захар Беркут", "Захар Беркут")]:
        since = doba.log_seq()
        doba.console("libopen " + t, 800)
        doba.wait_log(r"\[lib\] open '%s' " % re.escape(want), since=since)


def test_ukrainian_interface_leaves_no_english_on_the_main_pages(doba):
    """Language: Українська. Every page the simulator draws is checked for
    words still in English (sim builds log them as '[i18n] drawn ...')."""
    boot(doba)
    doba.console("lang uk", 500)
    allowed = re.compile(r"Doba \d+|LongFast|English|Language|MGRS|GPS|USB|UTC|SOS|abc|\d{4}")
    since = doba.log_seq()
    for s in ("home", "setup", "points", "mapopts", "sun", "sos", "more", "comms", "library"):
        doba.console("screen " + s, 800)
        time.sleep(1.0)
    left = [l for l in doba.log(since) if "[i18n] drawn" in l and not allowed.search(l)
            and not re.search("[\u0400-\u04ff]", l)]   # mixed lines (a callsign in Ukrainian) are fine
    doba.console("lang en", 500)
    assert not left, left
