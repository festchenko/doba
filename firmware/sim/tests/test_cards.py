"""The device against realistic SD cards (otpsim/cards.py): an empty card, a
season's worth (`typical`), years of it (`heavy`), and a card with only a few
MB left (`nearly_full`) — booting, listing to the app, paging on the panel,
recording — and against a card that fills up: mid-ride, and under uploads.

Card images are built once per session and cloned into each device. The
`heavy` profile and the ride that fills the card are marked slow (--slow).

Where the firmware misbehaves the test is a strict xfail naming the bug: it
documents today's behaviour, and turns into a failure the day it's fixed."""
import hashlib
import os
import random
import re
import shutil
import struct
import tempfile
import time

import pytest

from conftest import ARTIFACTS, TIME_SCALE
from otpsim import SimDevice
from test_regressions_rides import HISTORY_ROW, fit_crc, read_png
from test_rides import SAVE_BUTTON, check_fit, line_track, record_ride

FAST = ["--gps-baud", "115200"]
SLOW = pytest.mark.slow
PROFILES = ["empty", "typical", "nearly_full", pytest.param("heavy", marks=SLOW)]

# Boot timing runs with every card operation costing 2 ms of device time, so
# card work that grows with the card shows up (the sim's card is otherwise
# free). Measured (2026-10-26, M-series Mac), power-on to a painted dashboard:
# empty 1.2 s, typical 1.9 s, nearly_full 1.9 s, heavy 4.5-5.3 s — of which
# ~3.4 s is ride recovery walking all 1000 rides with openNextFile (an open per
# entry) before the dashboard's first paint. Bounds have ~2x headroom.
CARD_OP_MS = 2
BOOT_BOUND_S = {"empty": 4, "typical": 5, "nearly_full": 5, "heavy": 10}

LIST_MAX = 8192       # link.cpp: files per FileList walk (past it, rides keep the newest)
TILE_LIST_MAX = 4096  # ble_server.cpp lockTileList
HISTORY_MAX = 256     # ui_dashboard.cpp kRideListMax: the newest 256, by design
ROWS = 5              # kMenuRowCount
PAGER_DOWN = (150 + 56, 866 + 40)   # ui_render.h kPagerDown centre


# ---- fixtures ------------------------------------------------------------------------------------

@pytest.fixture(scope="session")
def card_image(tmp_path_factory):
    """card_image(profile) -> cards.Card, built on first use in the session."""
    root = tmp_path_factory.mktemp("cards")
    built = {}

    def get(profile):
        if profile not in built:
            built[profile] = cards.build(profile, str(root / profile))
        return built[profile]
    return get


@pytest.fixture
def boot_card(request, card_image):
    """boot_card(profile, extra_args=()) -> (SimDevice, Card): a device whose
    card is a fresh clone of the profile's image."""
    devs = []

    def start(profile, extra=()):
        card = card_image(profile)
        d = tempfile.mkdtemp(prefix="otp-card-")
        card.install(d)
        dev = SimDevice(dir=d, args=FAST + card.sim_args + list(extra))
        devs.append(dev)
        dev.launch(60)
        return dev, card

    yield start
    failed = getattr(request.node, "rep_call", None) is not None and request.node.rep_call.failed
    for dev in devs:
        if failed:
            out = os.path.join(ARTIFACTS, request.node.name)
            os.makedirs(out, exist_ok=True)
            try:
                dev.screenshot(os.path.join(out, "panel.png"))
            except Exception:
                pass
        dev.stop()
        if failed and os.path.exists(dev.log_path):
            shutil.copy(dev.log_path, os.path.join(out, "console.log"))
        shutil.rmtree(dev.dir, ignore_errors=True)


def link_to(dev):
    return LinkClient(paired_phone(dev)).hello(timeout=15)


def listed(link, kind, timeout=10):
    pages = link.request(list=pb.ListRequest(kind=kind), all_pages=True, timeout=timeout)
    for p in pages:
        assert p.WhichOneof("body") in ("file_list", "tile_list"), p
    if pages[0].WhichOneof("body") == "tile_list":
        return [t.h3 for p in pages for t in p.tile_list.tiles]
    return {f.name: f.size for p in pages for f in p.file_list.files}


def digest(path):
    return hashlib.sha1(open(path, "rb").read()).hexdigest()


def rides_on(dev):
    return set(os.listdir(os.path.join(dev.sd, "rides")))


def fit_problems(data):
    """Why `data` isn't a whole FIT activity (None if it is): the header's
    data_size must account for the file, and the trailing CRC must match."""
    if len(data) < 14 or data[8:12] != b".FIT":
        return "no FIT header (%d bytes)" % len(data)
    size = struct.unpack_from("<I", data, 4)[0]
    if len(data) != data[0] + size + 2:
        return "header says %d data bytes, the file holds %d" % (size, len(data) - data[0] - 2)
    if struct.unpack("<H", data[-2:])[0] != fit_crc(data[:-2]):
        return "file CRC doesn't match"
    return None


def fit_messages(data):
    """Walk a FIT file's messages: [(global message number, {field: bytes})].
    Every message must decode, and the last must end where data_size says."""
    end = data[0] + struct.unpack_from("<I", data, 4)[0]
    defs, out, i = {}, [], data[0]
    while i < end:
        h = data[i]
        assert not h & 0xA0, "header 0x%02x at %d" % (h, i)   # no compressed time / dev data
        if h & 0x40:
            gnum, n = struct.unpack_from("<HB", data, i + 3)
            defs[h & 0xF] = (gnum, [tuple(data[i + 6 + 3 * k:i + 9 + 3 * k]) for k in range(n)])
            i += 6 + 3 * n
        else:
            gnum, fields = defs[h & 0xF]
            i += 1
            vals = {}
            for num, size, _ in fields:
                vals[num] = data[i:i + size]
                i += size
            out.append((gnum, vals))
    assert i == end, "messages run to %d, data ends at %d" % (i, end)
    return out


# ---- booting ------------------------------------------------------------------------------------

@pytest.mark.parametrize("profile", PROFILES)
def test_boots_to_the_dashboard(boot_card, card_image, profile, record_property):
    """From power-on to a painted dashboard, with each card operation
    costing CARD_OP_MS. Also: boot's free-space figure is the card's."""
    card_image(profile)                   # built outside the timing
    t0 = time.time()
    dev, card = boot_card(profile, ["--sd-latency-ms", str(CARD_OP_MS)])
    tasks = time.time() - t0
    dev.wait_status(lambda s: s["screen"] == "dash" and s["paints"] > 0, timeout=60, interval=0.02)
    dash = time.time() - t0
    record_property("boot_s", round(dash, 2))
    print("\n%s: tasks started %.2f s, dashboard %.2f s" % (profile, tasks, dash))
    assert dash < BOOT_BOUND_S[profile] * TIME_SCALE, "%s card: %.1f s to the dashboard" % (profile, dash)

    line = dev.wait_log(r"sd: ready, \d+ MB free", since=0)
    said = int(re.search(r"(\d+) MB free", line).group(1))
    u = dev.sd_usage()
    assert abs(said - (u["total_bytes"] - u["used_bytes"]) // (1 << 20)) <= 1, (line, u)
    time.sleep(2)   # the UI task's start-up card work (recovery, sd update)
    assert not [l for l in dev.log() if "SD ACCESS WITHOUT sdLock" in l]
    st = dev.status()
    assert st["screen"] == "dash" and not st["ride"]["recording"]


# ---- the app's lists ----------------------------------------------------------------------------


# ---- the panel ------------------------------------------------------------------------------------

def list_rows(dev, path):
    """The list rows of a screenshot (between the status bar and the pager)."""
    dev.screenshot(path)
    px = read_png(path)
    return px[64 * 540:852 * 540]


@pytest.mark.parametrize("profile", PROFILES)
def test_ride_history_pages_through_the_card(boot_card, profile, tmp_path):
    """Ride History on the panel: the pager steps through every page of the
    newest HISTORY_MAX rides, and no further."""
    dev, card = boot_card(profile)
    want_pages = max(1, -(-min(len(card.rides), HISTORY_MAX) // ROWS))
    dev.console("screen menu")
    time.sleep(0.5)
    dev.tap(*HISTORY_ROW)
    time.sleep(1)
    assert dev.status()["screen"] == "other"
    shot = str(tmp_path / "page.png")
    rows = list_rows(dev, shot)
    pages = 1
    while pages <= want_pages + 1:
        dev.tap(*PAGER_DOWN)
        # The tap paints the pressed button first and the next page after it,
        # so wait for the rows themselves to change; past the last page they
        # never do.
        deadline = time.time() + 2.0 * TIME_SCALE
        nxt = list_rows(dev, shot)
        while nxt == rows and time.time() < deadline:
            time.sleep(0.1)
            nxt = list_rows(dev, shot)
        if nxt == rows:
            break
        rows = nxt
        pages += 1
    assert pages == want_pages, "%d pages for %d rides" % (pages, len(card.rides))


# ---- recording ------------------------------------------------------------------------------------


# ---- a full card ----------------------------------------------------------------------------------

def ride_until_full(dev, seconds):
    """Start a ride, fill the card behind it (the ride file keeps the one
    cluster it has), ride on for `seconds` of device time and save."""
    dev.gps_track(line_track(37.77, -122.45, 30), speed_kmh=30)
    dev.wait_status(lambda s: s["ride"]["gps_fix"] and s["ride"]["time_valid"], timeout=15)
    before = rides_on(dev)
    dev.set_speed(10)
    dev.button("boot")
    dev.wait_status(lambda s: s["ride"]["recording"], timeout=10)
    seq = dev.log_seq()
    assert dev.fill_card(0)["free_bytes"] == 0
    st = dev.wait_status(lambda s: s["ride"]["elapsed_s"] >= seconds, timeout=seconds / 10 * 3 + 30, interval=0.5)
    dev.screenshot(os.path.join(dev.dir, "riding.png"))
    dev.button("boot")
    dev.wait_status(lambda s: s["screen"] == "other", timeout=10)
    time.sleep(1)
    dev.screenshot(os.path.join(dev.dir, "sheet.png"))
    dev.tap(*SAVE_BUTTON)
    dev.wait_status(lambda s: not s["ride"]["recording"], timeout=20)
    dev.set_speed(1)
    new = rides_on(dev) - before
    assert len(new) == 1, new
    return new.pop(), seq, st


@pytest.fixture(scope="module")
def full_ride(request, card_image):
    """One ride on the nearly-full card (4 KB clusters) that runs out of
    space ~150 s in and carries on to 300 s. Shared by the tests below."""
    if not request.config.getoption("--slow"):
        pytest.skip("slow: run with --slow")
    card = card_image("nearly_full")
    d = tempfile.mkdtemp(prefix="otp-card-")
    card.install(d)
    dev = SimDevice(dir=d, args=FAST + card.sim_args)
    dev.launch(60)
    try:
        name, seq, st = ride_until_full(dev, 300)
        out = dict(dev=dev, card=card, name=name, log=dev.log(since=seq), status=st,
                   data=open(os.path.join(dev.sd, "rides", name), "rb").read())
        yield out
    finally:
        dev.stop()
        shutil.rmtree(d, ignore_errors=True)


@SLOW
def test_a_card_filling_mid_ride_keeps_the_device_and_earlier_rides(full_ride):
    """No crash, no hang: the ride runs to the end and stops when asked,
    the earlier rides are untouched, and the device boots again."""
    dev, card = full_ride["dev"], full_ride["card"]
    assert not [l for l in full_ride["log"] if "[sim] reboot" in l]
    for n in card.rides:
        assert digest(os.path.join(dev.sd, "rides", n)) == digest(os.path.join(card.sd, "rides", n)), n
    dev.reboot()
    dev.wait_status(lambda s: s["screen"] in ("dash", "other"), timeout=20)
    assert dev.sd_usage()["free_bytes"] == 0


@SLOW
def test_a_ride_that_fills_the_card_is_saved_as_a_valid_fit(full_ride):
    """Cut back to the records that leave room for the closing messages in
    the space the file already had: a whole activity whose session describes
    the records kept, and which the next boot leaves alone."""
    data = full_ride["data"]
    assert fit_problems(data) is None, fit_problems(data)
    msgs = fit_messages(data)
    records = [v for g, v in msgs if g == 20]
    session = [v for g, v in msgs if g == 18]
    assert 100 < len(records) < 300, len(records)        # filled ~150 s in, rode 300 s
    assert len(session) == 1 and [g for g, _ in msgs][-1] == 34   # ... then activity
    stamps = [struct.unpack("<I", r[253])[0] for r in records]
    assert stamps == sorted(stamps)
    assert session[0][9] == records[-1][5]               # total_distance = the last record's
    assert session[0][253] == records[-1][253]           # and it ends where they do
    dev = full_ride["dev"]
    dev.reboot()
    dev.wait_status(lambda s: s["screen"] in ("dash", "other"), timeout=20)
    time.sleep(2)
    assert open(os.path.join(dev.sd, "rides", full_ride["name"]), "rb").read() == data
    assert not [l for l in dev.log() if "interrupted" in l and full_ride["name"] in l]


@SLOW
def test_the_rider_is_told_when_the_card_fills_mid_ride(full_ride):
    """The status bar says SD FULL while the ride runs on, the end-of-ride
    sheet says the ride is saved only up to then, and the log never calls it
    plainly 'ride saved'."""
    told = [l for l in full_ride["log"]
            if re.search(r"(rec|ride).*(card full|no space|write fail|not saved|ENOSPC)", l, re.I)]
    assert told, "no recorder message about the full card"
    assert full_ride["status"]["ride"]["card_full"]
    assert not [l for l in full_ride["log"] if re.search(r"\bride saved:", l)]


def full_device(boot_card):
    dev, card = boot_card("nearly_full")
    link = link_to(dev)
    assert dev.fill_card(0)["free_bytes"] == 0
    return dev, card, link


def a_new_tile():
    cell, box = cards.h3_cells((37.0, -121.0, 37.05, -120.95))[0]   # a prefix no profile uses
    return cell, cards.sample_tile(cell, box)


