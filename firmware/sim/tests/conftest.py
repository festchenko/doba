"""Fixtures for the firmware sim tests (sim/README.md).

Every test gets a factory-fresh device (its own NVS, bonds and SD card in a
temp directory, on free ports). On failure the device's console log and a
screenshot of the panel land in sim/build/test-artifacts/<test>/."""
import os
import shutil
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "client"))

from otpsim import LinkClient, SimDevice, SimPhone, pb  # noqa: E402

# Wall-clock bounds (boot time, throughput) are set on a developer Mac; a CI
# runner is slower, so it scales them (OTP_SIM_TIME_SCALE=3 in build.yml).
TIME_SCALE = float(os.environ.get("OTP_SIM_TIME_SCALE", "1"))

# Doba boots to its HOME screen; the suites inherited from OTP were written for
# a dashboard boot, so they get one. test_boot.py checks the real HOME boot.
os.environ.setdefault("DOBA_START", "dash")
# Doba defaults to EU_868 (one LongFast slot); OTP's mesh suites assume US.
os.environ.setdefault("DOBA_MESH_REGION", "US")

ARTIFACTS = os.path.join(os.path.dirname(__file__), "..", "build", "test-artifacts")


def pytest_addoption(parser):
    parser.addoption("--slow", action="store_true",
                     help="also run the tests marked slow (heavy SD card profiles, riding a card full)")


def pytest_configure(config):
    config.addinivalue_line("markers", "slow: minutes, not seconds; skipped unless --slow")


# DOBA: the bike computer's ride recording (tracks, FIT files, laps, the
# recovery prompt) was taken out of Doba; so was HOME -> map. Their tests stay
# in the tree for the code that still exists, skipped here, not deleted.
DOBA_REMOVED = {
    "test_app_sync.py::test_a_ride_deleted_while_recording_is_refused_and_kept",
    "test_cards.py::test_a_ride_records_saves_and_downloads[empty]",
    "test_cards.py::test_a_ride_records_saves_and_downloads[nearly_full]",
    "test_cards.py::test_a_ride_records_saves_and_downloads[typical]",
    "test_crash_recovery.py::test_a_crash_mid_ride_is_offered_back_and_continue_carries_on",
    "test_crash_recovery.py::test_discard_on_the_recovery_prompt_removes_the_ride",
    "test_crash_recovery.py::test_power_cuts_at_any_moment_of_a_ride_lose_at_most_the_last_checkpoint",
    "test_crash_recovery.py::test_the_crash_test_is_refused_while_a_ride_records",
    "test_dashboard.py::test_a_layout_sent_mid_ride_waits_for_the_ride_to_end",
    "test_dashboard.py::test_the_home_key_goes_home_and_on_to_the_map",
    "test_gps_edges.py::test_a_fix_that_jumps_across_the_map_adds_no_distance",
    "test_gps_edges.py::test_a_ride_across_utc_midnight_keeps_its_timestamps_in_order",
    "test_mesh.py::test_mesh_traffic_during_a_ride_leaves_the_recording_alone",
    "test_navigation.py::test_a_ride_recorded_while_navigating_saves_and_the_navigation_carries_on",
    "test_navigation.py::test_a_route_of_4096_points_is_followed_without_slowing_the_ride",
    "test_pois.py::test_pois_along_a_ride_on_a_route",
    "test_power.py::test_a_flat_battery_mid_ride_is_recovered_on_the_next_boot",
    "test_power.py::test_a_ride_in_progress_is_never_switched_off",
    "test_power.py::test_shut_down_mid_ride_keeps_the_ride_and_boots_clean",
    "test_regressions_gps.py::test_a_ride_records_the_phone_position_while_the_receiver_has_none",
    "test_regressions_rides.py::test_a_card_that_mounts_late_is_put_to_use",
    "test_regressions_rides.py::test_a_saved_ride_carries_a_valid_fit_crc",
    "test_regressions_rides.py::test_an_interrupted_ride_on_a_full_card_is_still_found",
    "test_regressions_rides.py::test_diag_flush_and_card_users_lock_in_one_order",
    "test_regressions_rides.py::test_ride_history_mid_ride_scans_the_card_under_the_lock",
    "test_regressions_rides.py::test_the_shutdown_tap_is_acknowledged_before_the_ride_saves",
    "test_regressions_ui.py::test_a_quick_boot_press_starts_the_ride",
    "test_regressions_ui.py::test_modal_holds_still_on_the_1hz_tick",
    "test_regressions_ui.py::test_the_auto_pause_band_leaving_is_scrubbed",
    "test_rides.py::test_a_ride_is_not_downloadable_while_recording",
    "test_rides.py::test_record_save_list_and_download_a_ride",
    "test_sensors.py::test_a_greyed_power_hero_comes_back_when_the_meter_does",
    "test_sensors.py::test_an_indoor_ride_on_a_wheel_sensor_records_distance_and_speed",
    "test_sensors.py::test_the_ride_file_carries_sensor_values_and_drops_stale_ones",
    "test_stopwatch.py::test_the_lap_table_is_drawn_once_there_are_laps",
}


def pytest_collection_modifyitems(config, items):
    gone = pytest.mark.skip(reason="Doba: feature removed (ride recording / HOME goes home)")
    for item in items:
        if item.nodeid.split("tests/")[-1] in DOBA_REMOVED or item.nodeid in DOBA_REMOVED:
            item.add_marker(gone)
    if config.getoption("--slow"):
        return
    skip = pytest.mark.skip(reason="slow: run with --slow")
    for item in items:
        if "slow" in item.keywords:
            item.add_marker(skip)


@pytest.hookimpl(tryfirst=True, hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    rep = outcome.get_result()
    setattr(item, "rep_" + rep.when, rep)


@pytest.fixture
def device(request):
    """A booted device. Its GPS module starts at the firmware's own baud
    (115200) so boot doesn't spend 4 s probing; tests that care pass
    `gps_baud` via indirect parametrisation or start their own."""
    args = getattr(request, "param", None) or ["--gps-baud", "115200"]
    dev = SimDevice.start(args=args)
    yield dev
    failed = getattr(request.node, "rep_call", None) is not None and request.node.rep_call.failed
    if failed:
        out = os.path.join(ARTIFACTS, request.node.name)
        os.makedirs(out, exist_ok=True)
        try:
            dev.screenshot(os.path.join(out, "panel.png"))
        except Exception:
            pass
    dev.stop()
    if failed:
        shutil.copy(dev.log_path, os.path.join(out, "console.log"))
    shutil.rmtree(dev.dir, ignore_errors=True)


def paired_phone(device, identity="d0:00:00:00:00:01", name="Sim iPhone"):
    phone = SimPhone(device.ble_port, identity=identity, name=name).connect()
    sec = phone.wait_security(10)
    assert sec["encrypted"] and sec["bonded"], sec
    return phone


@pytest.fixture
def phone(device):
    p = paired_phone(device)
    yield p
    p.disconnect()


@pytest.fixture
def link(phone):
    return LinkClient(phone).hello()


__all__ = ["paired_phone", "pb"]
