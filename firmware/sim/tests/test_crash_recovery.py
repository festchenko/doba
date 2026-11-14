"""Crashes, watchdogs and power cuts, and what the next boot makes of them:
the ride that was recording, the crash report the app collects, the card's
files.

`device.reset(reason)` resets the way the chip does after a panic, a watchdog
or a brownout: esp_reset_reason() says so and RTC memory (the crash
reporter's breadcrumbs) survives. `device.reboot()` is a power cut: RTC
memory is lost and the boot is a power-on. Either way nothing the firmware
hadn't flushed reaches the card."""
import os
import re
import threading
import time

import pytest

from conftest import TIME_SCALE

from otpsim.tiles import card_path, fixture_manifest, fixture_tiles
from power_util import FAST, check_finished_fit, console_text, fit_records, only_ride, uptime_s
from test_rides import SAVE_BUTTON, line_track

LAT, LON = 37.77, -122.45
CONTINUE = (24 + 246, 816 + 48)      # ui_render.cpp kResumeButton centre ("CONTINUE")
DISCARD = (276 + 120, 700 + 48)      # kDiscardButton centre
CHECKPOINT_S = 15                    # config.h FIT_FLUSH_EVERY_S
CRASHES = ["panic", "task_wdt", "int_wdt", "wdt", "brownout"]
REASON_TEXT = {"panic": "crash (panic)", "task_wdt": "task watchdog", "int_wdt": "interrupt watchdog",
               "wdt": "watchdog", "brownout": "brownout"}


def crash_files(dev):
    d = os.path.join(dev.sd, "logs")
    return sorted(n for n in os.listdir(d) if n.startswith("crash-")) if os.path.isdir(d) else []


def report_ids(dev, since=0):
    """The ids of the crash reports this device created, oldest first."""
    return [re.search(r"CRASH report ([0-9a-f]{8})", l).group(1) for l in dev.log(since)
            if re.search(r"CRASH report [0-9a-f]{8}", l)]


def ride_for(dev, seconds, km=5, speed=6):
    dev.gps_track(line_track(LAT, LON, km), speed_kmh=30)
    dev.wait_status(lambda s: s["ride"]["gps_fix"] and s["ride"]["time_valid"], timeout=15)
    dev.set_speed(speed)
    if not dev.status()["ride"]["recording"]:
        dev.button("boot")
        dev.wait_status(lambda s: s["ride"]["recording"], timeout=10)
    return dev.wait_status(lambda s: s["ride"]["elapsed_s"] >= seconds, timeout=seconds / 3 + 20)


def stop_and_save(dev):
    dev.button("boot")
    dev.wait_status(lambda s: s["screen"] == "other", timeout=10)
    dev.tap(*SAVE_BUTTON)
    dev.wait_status(lambda s: not s["ride"]["recording"], timeout=20)
    dev.set_speed(1)


def wait_recovery_prompt(dev):
    line = dev.wait_log(r"ride interrupted: /rides/", since=0, timeout=20)
    dev.wait_status(lambda s: s["screen"] == "other", timeout=10)
    return line


# ---- the ride across a crash ---------------------------------------------------------------

def test_a_crash_mid_ride_is_offered_back_and_continue_carries_on(device):
    """The firmware panics 75 s into a ride. The next boot puts RIDE
    INTERRUPTED on the screen; CONTINUE takes the ride back where its
    records end — distance and time restored, not zero — and the ride saved
    at the end is ONE valid FIT holding both halves, in order."""
    st = ride_for(device, 75)
    before_km = st["ride"]["distance_m"]
    name = sorted(os.listdir(os.path.join(device.sd, "rides")))[0]
    device.reset("panic")
    line = wait_recovery_prompt(device)
    assert "/rides/" + name in line
    device.tap(*CONTINUE)
    st = device.wait_status(lambda s: s["ride"]["recording"], timeout=20)
    device.wait_log(r"ride continued after reset: /rides/" + re.escape(name), since=0, timeout=5)
    # Restored from the file: everything up to the last checkpoint.
    assert st["ride"]["elapsed_s"] >= 75 - CHECKPOINT_S - 2, st["ride"]
    assert st["ride"]["distance_m"] >= before_km * 0.6, (st["ride"]["distance_m"], before_km)

    resumed_at = st["ride"]["elapsed_s"]
    ride_for(device, resumed_at + 45)
    stop_and_save(device)
    got, data = only_ride(device)
    assert got == name
    check_finished_fit(data)
    recs = fit_records(data)
    assert len(recs) >= resumed_at + 40, (len(recs), resumed_at)
    times = [r["utc"] for r in recs]
    assert times == sorted(times), "record timestamps go backwards across the crash"
    dists = [r["distance_m"] for r in recs if r["distance_m"] is not None]
    assert dists == sorted(dists), "record distance goes backwards across the crash"
    assert dists[-1] > before_km


def test_discard_on_the_recovery_prompt_removes_the_ride(device):
    """DISCARD means it: the interrupted file is gone and the next boot asks
    nothing."""
    ride_for(device, 40)
    device.reset("task_wdt")
    wait_recovery_prompt(device)
    device.tap(*DISCARD)
    device.wait_log(r"interrupted ride discarded", since=0, timeout=10)
    assert not [n for n in os.listdir(os.path.join(device.sd, "rides")) if n.endswith(".fit")]
    device.reboot()
    time.sleep(2)
    assert not [l for l in device.log() if "ride interrupted" in l]
    assert device.status()["screen"] == "dash"


def test_power_cuts_at_any_moment_of_a_ride_lose_at_most_the_last_checkpoint(device):
    """Five power cuts at uneven moments of five rides (one landing just
    after a checkpoint, one just before). Each time the next boot offers the
    ride, SAVE turns it into a valid FIT, and no more than the seconds since
    the last checkpoint are missing. Older rides on the card are untouched."""
    kept = {}
    for i, secs in enumerate([17, 29, 46, 61, 88]):
        device.gps_track(line_track(LAT + i * 0.01, LON, 5), speed_kmh=30)
        st = ride_for(device, secs, speed=3)
        rides = os.path.join(device.sd, "rides")
        name = max(n for n in os.listdir(rides) if n not in kept)
        device.reboot()                       # the power cut
        wait_recovery_prompt(device)
        device.tap(*SAVE_BUTTON)
        device.wait_log(r"interrupted ride saved: /rides/%s \(ok\)" % re.escape(name), since=0, timeout=20)
        data = open(os.path.join(rides, name), "rb").read()
        check_finished_fit(data)
        n = len(fit_records(data))
        # A record a second while there is a fix; a slow host can leave a
        # few seconds fix-less at a fast clock, hence the slack.
        assert n >= (secs // CHECKPOINT_S) * CHECKPOINT_S - 5, "%s: %d records after %d s" % (name, n, secs)
        kept[name] = data
        for old, d in kept.items():
            assert open(os.path.join(rides, old), "rb").read() == d, "%s changed" % old
        device.set_speed(1)
        time.sleep(1.5)                       # the next ride's file name is a new second


# ---- the crash report ------------------------------------------------------------------------


@pytest.mark.parametrize("how", ["power cut", "restart", "wake from sleep"])
def test_no_crash_report_without_a_crash(device, how):
    """A power cut, a software restart (OTA, the console's reboot) and a wake
    from deep sleep are not crashes: no report, nothing queued."""
    if how == "power cut":
        device.reboot()
    elif how == "restart":
        device.reset("sw")
    else:
        device.console("autosleep off")
        device.button("boot", ms=2200)
        time.sleep(1.5)
        device.tap(24 + 246, 700 + 48)       # SHUT DOWN
        device.wait_powered_off(30)
        device.power_on()
    time.sleep(2)
    assert not report_ids(device)
    assert crash_files(device) == []


def test_a_crash_report_waits_in_flash_for_a_card(device):
    """Crashing with no card in: the report must survive in NVS (across a
    further power cut, even) and land on the card when one goes in."""
    device.set_sd(False)
    device.reset("panic")
    line = device.wait_log(r"CRASH report [0-9a-f]{8}: .*durable_NVS=1", since=0, timeout=10)
    rid = re.search(r"CRASH report ([0-9a-f]{8})", line).group(1)
    device.reboot()                           # and then the battery runs out
    time.sleep(1)
    assert crash_files(device) == []
    device.set_sd(True)
    device.set_speed(10)                      # the card is retried every 30 s
    device.wait_log(r"crash report %s delivered to SD" % rid, since=0, timeout=20 * TIME_SCALE)
    device.set_speed(1)
    assert crash_files(device) == ["crash-%s.log" % rid]


def test_a_crash_loop_without_a_card_keeps_the_first_report(device):
    """Six crashes in a row with no card (a boot loop in the field): NVS holds
    only a few, and the one that matters — the first, which usually shows the
    root cause — must be among those delivered when a card goes in."""
    device.set_sd(False)
    ids = []
    for i in range(6):
        seq = device.log_seq()
        device.reset("panic" if i % 2 == 0 else "task_wdt")
        line = device.wait_log(r"CRASH report [0-9a-f]{8}", since=0, timeout=10)
        ids.append(re.search(r"CRASH report ([0-9a-f]{8})", line).group(1))
    assert len(set(ids)) == 6, ids
    device.set_sd(True)
    device.set_speed(10)
    device.wait_log(r"crash report %s delivered to SD" % ids[0], since=0, timeout=20 * TIME_SCALE)
    time.sleep(1)
    device.set_speed(1)
    on_card = [f[6:14] for f in crash_files(device)]
    assert ids[0] in on_card, (ids, on_card)
    assert ids[-1] in on_card, "the latest crash was dropped: %s" % on_card
    assert 2 <= len(on_card) <= 5, on_card


def test_the_crash_test_is_refused_while_a_ride_records(device):
    """`crashlog panic` (the deliberate crash for testing the reporter) must
    never cost a rider their ride: refused while recording, honoured after."""
    ride_for(device, 10)
    device.set_speed(1)
    device.console("crashlog panic", wait_ms=1500)
    device.wait_log(r"crash report test refused: a ride is recording", since=0, timeout=5)
    assert device.running and device.status()["ride"]["recording"]
    stop_and_save(device)
    seq = device.log_seq()
    device.cmd("console", line="crashlog panic", wait_ms=0)
    device.wait_log(r"\[sim\] reboot: panic: abort\(\)", since=seq, timeout=10)
    time.sleep(0.3)
    device.reconnect_control()
    device.wait_log(r"boot: all tasks started", since=0, timeout=30)
    assert device.status()["reset_reason"] == 4           # ESP_RST_PANIC
    device.wait_log(r"crash report saved and verified", since=0, timeout=10)
    text = open(os.path.join(device.sd, "logs", crash_files(device)[0])).read()
    assert "CRASH TEST: deliberate panic" in text


# ---- a tile torn by a reset mid-write ----------------------------------------------------------


def rejoin(dev, phone):
    """The same (bonded) phone reconnecting after the device came back."""
    again = SimPhone(dev.ble_port, identity=phone.identity, bonded=True).connect()
    assert again.wait_security(10)["encrypted"]
    return again


def tile_centre(h3):
    s, w, n, e = fixture_manifest()["cells"][h3]["bbox"]
    return (s + n) / 2, (w + e) / 2
