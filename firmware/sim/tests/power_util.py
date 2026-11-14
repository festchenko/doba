"""Helpers for the power and robustness tests (test_power.py,
test_crash_recovery.py, test_gps_edges.py): running the device clock to a
moment while watching for it to power itself off, and reading FIT records."""
import math
import os
import struct
import time

from conftest import TIME_SCALE
from otpsim.device import ControlError

FAST = ["--gps-baud", "115200"]
IDLE_S = 600            # ui_dashboard.cpp AUTO_SLEEP_MS
FAST_SPEED = 60         # device seconds per real second while nothing is due
FINE_SPEED = 10         # ... and near the moment that matters


def uptime_s(dev):
    return dev.status()["uptime_ms"] / 1000.0


def run_to(dev, until_s, fine_from=None):
    """Run the device clock fast until its uptime reaches `until_s` s.

    Returns None if the device is still on then (the clock goes back to 1x),
    else the last uptime seen before it powered itself off. From `fine_from`
    on the clock runs at FINE_SPEED, so that moment is resolved to ~1 s of
    device time."""
    last = uptime_s(dev)
    fine = fine_from is not None and last >= fine_from
    dev.set_speed(FINE_SPEED if fine else FAST_SPEED)
    deadline = time.time() + ((until_s - last) / FINE_SPEED + 30) * TIME_SCALE
    while time.time() < deadline:
        try:
            st = dev.status()
        except (ControlError, OSError, ValueError):
            if not dev.running or _exits(dev):
                dev.wait_powered_off(5)
                return last
            raise
        last = st["uptime_ms"] / 1000.0
        if last >= until_s:
            dev.set_speed(1)
            return None
        if not fine and fine_from is not None and last >= fine_from:
            fine = True
            dev.set_speed(FINE_SPEED)
        time.sleep(0.05)
    raise TimeoutError("the device clock never reached %.0f s (at %.0f s)" % (until_s, last))


def _exits(dev, wait=3.0):
    try:
        dev.proc.wait(timeout=wait)
        return True
    except Exception:
        return False


def console_text(dev):
    with open(dev.log_path, "r", errors="replace") as f:
        return f.read()


def card_logs(dev):
    """Everything in the card's /logs/*.log, concatenated."""
    d = os.path.join(dev.sd, "logs")
    out = ""
    for n in sorted(os.listdir(d)) if os.path.isdir(d) else []:
        if n.endswith(".log"):
            out += open(os.path.join(d, n), errors="replace").read()
    return out


# ---- FIT --------------------------------------------------------------------------------

FIT_EPOCH = 631065600   # 1989-12-31T00:00:00Z


def line_track(lat, lon, km, bearing_deg=90, step_m=50):
    """Points along a straight line, step_m apart."""
    pts = []
    r = math.radians(bearing_deg)
    for i in range(int(km * 1000 / step_m) + 1):
        d = i * step_m
        pts.append((lat + d * math.cos(r) / 111320.0,
                    lon + d * math.sin(r) / (111320.0 * math.cos(math.radians(lat)))))
    return pts
