"""Riding: record with GPS and sensors, save on the panel, list and download
the FIT over the link — and the transfers around it (routes, firmware)."""
import math
import os
import struct
import time

import pytest

from conftest import TIME_SCALE
from otpsim import SimDevice

SAVE_BUTTON = (24 + 120, 700 + 48)   # ui_render.cpp kSaveButton centre


def line_track(lat, lon, km, bearing_deg=90, step_m=50):
    pts = []
    r = math.radians(bearing_deg)
    for i in range(int(km * 1000 / step_m) + 1):
        d = i * step_m
        pts.append((lat + d * math.cos(r) / 111320.0,
                    lon + d * math.sin(r) / (111320.0 * math.cos(math.radians(lat)))))
    return pts


def record_ride(device, seconds=90, speed_kmh=30):
    device.gps_track(line_track(37.77, -122.45, 3), speed_kmh=speed_kmh)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    device.set_speed(6)
    device.button("boot")
    device.wait_status(lambda s: s["ride"]["recording"], timeout=10)
    device.wait_status(lambda s: s["ride"]["elapsed_s"] >= seconds, timeout=seconds / 3 + 20)
    device.button("boot")                     # stop: the save/discard summary
    device.wait_status(lambda s: s["screen"] == "other", timeout=10)
    device.tap(*SAVE_BUTTON)
    st = device.wait_status(lambda s: not s["ride"]["recording"], timeout=20)
    device.set_speed(1)
    return st


def check_fit(data):
    assert len(data) > 100
    hdr = data[0]
    assert hdr in (12, 14)
    assert data[8:12] == b".FIT"
    data_size = struct.unpack_from("<I", data, 4)[0]
    assert len(data) == hdr + data_size + 2   # header + records + file CRC


@pytest.mark.xfail(strict=True, reason=(
    "BUG: ride_recorder::task (src/ride_recorder.cpp) counts the ride timer (timerS, and movingS/pausedS "
    "with it) once per loop, and paces the loop with vTaskDelay(RECORD_INTERVAL_MS): a second AFTER the "
    "tick's work, not a 1 s period (vTaskDelayUntil). The work — the FIT write under sdLock, waiting behind "
    "the map for the card — is time the timer never counts, so on a slow card the ride clock runs slow, "
    "records come at under 1 Hz and the average speed reads high"))
def test_the_ride_timer_keeps_time_on_a_slow_card(device):
    """A card that takes 100 ms an operation (a slow card, or one the map is
    busy with): the ride's elapsed time still counts every second ridden."""
    device.gps_track(line_track(37.77, -122.45, 3), speed_kmh=30)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    device.button("boot")
    device.wait_status(lambda s: s["ride"]["recording"], timeout=10)
    device.set_sd_latency(100)
    device.set_speed(2)
    st0 = device.status()
    st = device.wait_status(lambda s: s["uptime_ms"] - st0["uptime_ms"] >= 40000, timeout=40 * TIME_SCALE)
    device.set_speed(1)
    device.set_sd_latency(0)
    ridden = (st["uptime_ms"] - st0["uptime_ms"]) / 1000.0
    counted = st["ride"]["elapsed_s"] - st0["ride"]["elapsed_s"]
    assert abs(counted - ridden) <= 1.5, "the ride timer counted %d s of %.1f s ridden" % (counted, ridden)


GPX = """<?xml version="1.0"?>
<gpx version="1.1" creator="sim"><trk><name>Loop</name><trkseg>
{}
</trkseg></trk></gpx>"""


