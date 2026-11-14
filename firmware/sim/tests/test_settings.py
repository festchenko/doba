"""Settings (src/settings.cpp, the link's Settings, the Settings screen, the
side key) and the idle auto-shutdown: every value the app can set, what the
device does with values out of range, that each one is kept across a reboot
and reaches the hardware or the panel, that an edit on the device reaches
the app, and when the device may switch itself off."""
import os
import time

import pytest

from conftest import TIME_SCALE

from test_regressions_ui import glass, pair_sensor

PWM = {0: 0, 1: 12, 2: 90, 3: 230}     # kBacklightPWM (ui_dashboard.cpp)

# The Settings screen (ui_render.h): 108 px rows from y 64; the stepper's
# -/+ targets and the switches' hit rects.
ROW = lambda r: 64 + r * 108 + 54
MINUS_X, PLUS_X = 192 + 44, 412 + 44
TOGGLE_X, UNITS_TOGGLE_X = 380 + 60, 332 + 84


def reconnect(device, phone):
    phone.wait_disconnected(10)
    phone.connect()
    assert phone.wait_security(10)["encrypted"]
    return LinkClient(phone).hello()


def tap(device, x, y):
    device.tap(x, y)
    time.sleep(0.45)


# -- over the link ------------------------------------------------------------------------


def test_the_side_key_steps_the_backlight_and_keeps_it(device):
    """Off → low → medium → bright → off, and the level is kept."""
    assert device.status()["backlight_pwm"] == 0
    for level in (1, 2, 3, 0, 1):
        device.button("side")
        st = device.wait_status(lambda s: s["backlight_pwm"] == PWM[level], timeout=3)
        assert st["settings"]["backlight"] == level
        time.sleep(0.3)
    device.reboot()
    st = device.wait_status(lambda s: s["backlight_pwm"] == PWM[1], timeout=5)


# -- what the panel shows ----------------------------------------------------------------------


# -- auto-sleep ------------------------------------------------------------------------------

IDLE_S = 600   # AUTO_SLEEP_MS


def idle_for(device, seconds, factor=60):
    """Let `seconds` of device time pass, quickly."""
    device.set_speed(factor)
    time.sleep(seconds / factor * TIME_SCALE)
    if not device.powered_off():
        device.set_speed(1)


def _trainer_workout_without_a_phone(device, phone, link):
    pair_sensor(device, link, "power", watts=200, cadence=90)
    device.wait_status(lambda s: s["ride"]["power"] == 200, timeout=30)
    os.makedirs(os.path.join(device.sd, "workouts"), exist_ok=True)
    with open(os.path.join(device.sd, "workouts", "long.erg"), "w") as f:
        f.write("[COURSE HEADER]\nMINUTES WATTS\n[END COURSE HEADER]\n[COURSE DATA]\n0 200\n60 200\n"
                "[END COURSE DATA]\n")
    assert any("loaded LONG" in l for l in device.console("workout load long.erg"))
    device.console("workout start")
    phone.disconnect()
    device.wait_status(lambda s: not s["phone_connected"], timeout=5)


