"""The GPS receiver misbehaving, and the clock it sets: a tunnel, a fix that
jumps across the map, line noise and bad checksums, a module whose date is a
GPS week rollover behind, UTC midnight in the middle of a ride.

`gps_raw` puts bytes on the UART as they are; `gps_time` sets the module's
own clock; `gps_paced` makes sentences arrive one by one at the wire's byte
rate (as on the board) instead of a whole second's batch at once."""
import calendar
import math
import os
import random
import re
import time

import pytest


from power_util import line_track

LAT, LON = 37.77, -122.45
WEEK_ROLLOVER_S = 1024 * 7 * 86400


def nmea(body):
    ck = 0
    for c in body:
        ck ^= ord(c)
    return "$%s*%02X\r\n" % (body, ck)


def ddmm(deg, is_lat):
    a = abs(deg)
    d = int(a)
    m = (a - d) * 60
    if is_lat:
        return "%02d%08.5f,%s" % (d, m, "N" if deg >= 0 else "S")
    return "%03d%08.5f,%s" % (d, m, "E" if deg >= 0 else "W")


def fix_sentences(lat, lon, utc, speed_kmh=30.0, course=90.0):
    """A GGA + RMC pair for a fix at (lat, lon) at `utc`."""
    t = time.gmtime(utc)
    hms = "%02d%02d%02d.00" % (t.tm_hour, t.tm_min, t.tm_sec)
    date = "%02d%02d%02d" % (t.tm_mday, t.tm_mon, t.tm_year % 100)
    gga = "GNGGA,%s,%s,%s,1,10,0.9,50,M,0.0,M,," % (hms, ddmm(lat, True), ddmm(lon, False))
    rmc = "GNRMC,%s,A,%s,%s,%.2f,%.1f,%s,,,A,V" % (hms, ddmm(lat, True), ddmm(lon, False), speed_kmh / 1.852,
                                                   course, date)
    return nmea(gga) + nmea(rmc)


def metres(a, b):
    r = math.radians
    dla, dlo = r(b[0] - a[0]), r(b[1] - a[1])
    h = math.sin(dla / 2) ** 2 + math.cos(r(a[0])) * math.cos(r(b[0])) * math.sin(dlo / 2) ** 2
    return 2 * 6371000 * math.asin(math.sqrt(h))


# ---- position --------------------------------------------------------------------------------


def test_line_noise_and_bad_checksums_do_not_move_the_rider(device):
    """A noisy UART (a loose connector, the radio rail browning out): random
    bytes, torn sentences and well-formed sentences with the wrong checksum
    claiming a fix 10 km away. None of it may move the rider or drop the
    fix the module keeps reporting."""
    device.gps(LAT, LON, speed_kmh=0)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    rnd = random.Random(7)
    for i in range(40):
        bad = re.sub(r"\*[0-9A-F]{2}", lambda m: "*%02X" % (int(m.group(0)[1:], 16) ^ 0x5A),
                     fix_sentences(LAT + 0.09, LON, time.time()))              # checksums wrong
        torn = fix_sentences(LAT - 0.09, LON, time.time())[:30]               # cut off mid-sentence
        noise = "".join(chr(rnd.randrange(1, 128)) for _ in range(rnd.randrange(5, 60)))
        device.gps_raw(noise + bad + torn + noise)
        st = device.status()["ride"]
        assert abs(st["lat"] - LAT) < 1e-4 and abs(st["lon"] - LON) < 1e-4, st
        time.sleep(0.05)
    st = device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=5)
    assert abs(st["ride"]["lat"] - LAT) < 1e-4


# ---- time ------------------------------------------------------------------------------------

cold = pytest.mark.parametrize("device", [["--gps-baud", "115200", "--gps-cold"]], indirect=True, ids=["cold"])


def fix_from_the_past(dev):
    """A fix whose date is a GPS week rollover (1024 weeks) behind, held
    past the 60 s the GPS clock-set waits for."""
    now = int(time.time())
    dev.gps_time(now - WEEK_ROLLOVER_S)
    dev.gps(LAT, LON)
    dev.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    dev.set_speed(10)
    dev.wait_status(lambda s: s["uptime_ms"] > 80000, timeout=20)
    dev.set_speed(1)
    return now


@cold
def test_a_module_a_gps_week_rollover_behind_never_sets_the_clock(device):
    """An older receiver past its week-number rollover reports a date 19.6
    years ago. The device must not take it: no 2007 clock, no 2007 log file
    or ride timestamps."""
    now = fix_from_the_past(device)
    st = device.status()
    assert not st["ride"]["time_valid"], st["ride"]
    assert st["wall_unix"] < 1_000_000_000, st["wall_unix"]     # still unset, not 2007
    old = time.strftime("%Y%m%d.log", time.gmtime(now - WEEK_ROLLOVER_S))
    assert not os.path.exists(os.path.join(device.sd, "logs", old))


def test_the_clock_never_reads_a_day_early_at_utc_midnight(device):
    """The same rollover, watched on the device's clock directly rather than
    through whichever records the recorder happened to take: between the new
    day's first GGA and the RMC that brings its date, the parser holds
    00:00:00 against yesterday's date, and the clock must not show it."""
    device.gps_paced(True)
    midnight = (int(time.time()) // 86400 + 1) * 86400
    device.gps_time(midnight - 4)
    device.gps(LAT, LON)
    device.wait_status(lambda s: s["ride"]["time_valid"], timeout=15)
    seen = []
    deadline = time.time() + 60
    while time.time() < deadline:
        u = device.status()["ride"]["utc"]
        if u < midnight - 4:            # the clock before gps_time took hold
            continue
        if not seen or u != seen[-1]:
            seen.append(u)
        if u >= midnight + 4:
            break
        time.sleep(0.01)
    assert seen[0] < midnight <= seen[-1], (seen[0] - midnight, seen[-1] - midnight)
    assert all(0 < b - a <= 3 for a, b in zip(seen, seen[1:])), [u - midnight for u in seen]

