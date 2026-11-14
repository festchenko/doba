"""Settings kept across a reboot that reach the hardware (the side key's backlight)."""
import time

PWM = {0: 0, 1: 12, 2: 90, 3: 230}     # kBacklightPWM (ui_dashboard.cpp)

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
