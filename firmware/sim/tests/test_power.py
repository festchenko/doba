"""Power: the idle auto-off, what holds it off and what restarts its
countdown, the power dialog, and the battery as the rider and the app see it.

A bike computer left on a desk must switch itself off (ui_dashboard.cpp,
AUTO_SLEEP_MS: 10 minutes without input), and must never do it under a
rider. Deep sleep exits the sim; `power_on()` is BOOT pressed afterwards.
The clock runs fast (power_util.run_to) and slows down near the moment the
test is about, so each switch-off is timed to about a second of device time."""
import os
import time

import pytest


from power_util import (FAST, IDLE_S, card_logs, console_text,
                        run_to, uptime_s)
from power_util import line_track

LAT, LON = 37.77, -122.45
SHUT_DOWN = (24 + 246, 700 + 48)     # ui_render.cpp kPowerShutdown centre
CANCEL = (24 + 246, 816 + 48)        # kPowerCancel centre
TOLERANCE_S = 30                     # the UI loop's pass at a fast clock, the poll's resolution


def assert_switched_off_after_idle(off, since):
    assert off is not None, "still on %d s after the last input" % (IDLE_S + 100)
    assert off >= since + IDLE_S - 2, "switched off %.0f s after the last input, not %d" % (off - since, IDLE_S)
    assert off <= since + IDLE_S + TOLERANCE_S, "switched off only %.0f s after the last input" % (off - since)


def stays_on(dev, minutes):
    """Run the clock `minutes` past now; the device must still be on."""
    t = uptime_s(dev)
    off = run_to(dev, t + minutes * 60)
    assert off is None, "switched itself off at %.0f s (%.0f s after the check began)" % (off, off - t)


# ---- the idle countdown ------------------------------------------------------------------

def test_an_idle_device_switches_itself_off_after_ten_minutes(device):
    """Left on a desk, the device must switch off on its own — and not
    before ten minutes. It says why in the card's log before it goes, so the
    next boot's log tells an auto-off from a crash or a power cut, and BOOT
    brings it back (a wake from deep sleep, not a crash)."""
    off = run_to(device, IDLE_S + 120, fine_from=IDLE_S - 30)
    assert_switched_off_after_idle(off, 0)
    assert "shutdown: auto-sleep (idle timeout)" in console_text(device)
    assert "shutdown: auto-sleep (idle timeout)" in card_logs(device), "the reason never reached the card"

    device.power_on()
    st = device.status()
    assert st["reset_reason"] == 8          # ESP_RST_DEEPSLEEP
    assert st["screen"] == "other"   # HOME
    assert not [n for n in os.listdir(os.path.join(device.sd, "logs")) if n.startswith("crash-")]
    # A fresh countdown on the new boot.
    assert run_to(device, IDLE_S - 30) is None


@pytest.mark.parametrize("touch", ["tap", "side key", "home key"])
def test_any_input_restarts_the_idle_countdown(device, touch):
    """A tap or a key is the rider using the device: the ten minutes start
    again from there. (A BOOT press starts a ride, which holds it on anyway.)"""
    assert run_to(device, 480) is None
    if touch == "tap":
        device.tap(270, 480)
    elif touch == "side key":
        device.button("side")
    else:
        device.button("home")
    t = uptime_s(device)
    off = run_to(device, t + IDLE_S + 120, fine_from=t + IDLE_S - 30)
    assert_switched_off_after_idle(off, t)


def test_autosleep_off_on_the_console_keeps_it_on_until_switched_back(device):
    """The bench override (`autosleep off`) holds the device on; `autosleep
    on` starts a full countdown from then, not from the last tap an hour ago."""
    assert any("OFF" in l for l in device.console("autosleep off"))
    stays_on(device, 25)
    device.console("autosleep on")
    t = uptime_s(device)
    off = run_to(device, t + IDLE_S + 120, fine_from=t + IDLE_S - 30)
    assert_switched_off_after_idle(off, t)


# ---- what holds it off -----------------------------------------------------------------------

def test_riding_without_recording_keeps_it_on_and_stopping_starts_the_countdown(device):
    """Riding along with the dashboard up but no ride recording: speed is
    activity, so it stays on; once the bike stops, ten minutes later it
    switches off."""
    device.gps_track(line_track(LAT, LON, 3), speed_kmh=20, loop=True)
    device.wait_status(lambda s: s["ride"]["gps_fix"] and s["ride"]["speed_kmh"] > 10, timeout=15)
    stays_on(device, 25)
    st = device.status()
    device.gps(st["ride"]["lat"], st["ride"]["lon"], speed_kmh=0)
    device.wait_status(lambda s: s["ride"]["speed_kmh"] < 1, timeout=10)
    t = uptime_s(device)
    off = run_to(device, t + IDLE_S + 120, fine_from=t + IDLE_S - 30)
    assert_switched_off_after_idle(off, t)


def test_a_computer_holding_the_card_keeps_it_on(device):
    device.usb_host(True)
    stays_on(device, 25)


# ---- the power dialog --------------------------------------------------------------------------

def test_cancel_on_the_power_dialog_keeps_it_on(device):
    """Holding BOOT opens the power dialog; CANCEL closes it and nothing else
    happens (and it counts as a touch: the countdown starts again)."""
    paints = device.status()["paints"]
    device.button("boot", ms=2200)
    device.wait_status(lambda s: s["paints"] > paints, timeout=5)   # the sheet drawn
    time.sleep(0.5)
    device.tap(*CANCEL)
    time.sleep(1.5)
    assert device.running
    assert "shutdown:" not in console_text(device)


# ---- the battery -------------------------------------------------------------------------------

