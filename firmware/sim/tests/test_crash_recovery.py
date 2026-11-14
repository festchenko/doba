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
from power_util import FAST, console_text, uptime_s
from power_util import line_track

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


# ---- the ride across a crash ---------------------------------------------------------------

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


# ---- a tile torn by a reset mid-write ----------------------------------------------------------

