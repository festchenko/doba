"""GPS and clock bugs that shipped once, replayed against the sim.

Each test names the commit that fixed its bug, and was checked against the
unfixed behaviour re-created in src/: it fails with the bug and passes with
the fix."""
import math
import time

import pytest

from otpsim import SimDevice

LAT, LON = 37.7749, -122.4194

# Indoors after a reboot: the receiver has no fix and no UTC either.
indoors = pytest.mark.parametrize("device", [["--gps-baud", "115200", "--gps-cold"]], indirect=True,
                                  ids=["indoors"])


@pytest.mark.parametrize("module_baud", [38400, 57600])
def test_an_l76k_left_at_another_baud_still_gets_a_fix(module_baud):
    """24ba88c: an L76K left at a non-default baud (kept by its backup supply)
    never answered the PCAS06 probe, and any byte read at 38400 passed as a
    u-blox M10Q: the unit reported a u-blox, never moved the module and never
    parsed a sentence, so the rider never got a fix."""
    with SimDevice.start(args=["--gps-baud", str(module_baud)]) as dev:
        dev.gps(LAT, LON)
        st = dev.wait_status(lambda s: s["ride"]["gps_fix"], timeout=40)
        assert abs(st["ride"]["lat"] - LAT) < 1e-4
        assert not [l for l in dev.log() if "u-blox" in l]


def test_speed_drops_to_zero_when_the_fix_is_lost_or_the_receiver_goes_quiet(device):
    """d88990d (#99): TinyGPS++ never clears isValid(), so the last moving
    speed stayed on the dashboard (and in moving time / auto-pause) after the
    fix was lost; and fix and speed were only re-evaluated on a new sentence,
    so a receiver that went silent left 'fix, 30 km/h' frozen forever."""
    device.gps(LAT, LON, speed_kmh=30, course=90)
    device.wait_status(lambda s: s["ride"]["gps_fix"] and s["ride"]["speed_kmh"] > 25, timeout=10)

    # The sky goes away: RMC status V, GGA without a position.
    device.gps(LAT, LON, speed_kmh=30, fix=False, sats=3)
    device.wait_status(lambda s: not s["ride"]["gps_fix"] and s["ride"]["speed_kmh"] == 0, timeout=8)

    device.gps(LAT, LON, speed_kmh=30, course=90)
    device.wait_status(lambda s: s["ride"]["gps_fix"] and s["ride"]["speed_kmh"] > 25, timeout=10)

    # The receiver stops talking altogether.
    device.gps_silent(True)
    device.wait_status(lambda s: not s["ride"]["gps_fix"] and s["ride"]["speed_kmh"] == 0, timeout=8)


