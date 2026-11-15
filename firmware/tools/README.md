# Firmware tools

What is in here and what each is for. The maps, books and card contents are
built by the scripts at the top of the repository (`maps/`, `library/`,
`tools/doba_card.py`), not here.

## Building

| | |
|---|---|
| `apply-pm-framework.sh`, `check-framework.sh`, `revert-pm.sh` | the pinned Arduino-ESP32 framework with power management on |
| `apply-exfat.sh` | rebuilds the framework's FatFs with exFAT (the 1 TB card) |
| `epd_hybrid_patch.py`, `epd_idle_patch.py` | panel-driver patches applied at build time |
| `memfault_build.py` | pre-build step: compiles the pinned Memfault SDK (crash dumps to the card) |
| `pio_upload.py`, `flash.py` | flashing helpers |
| `i18n/` | `uk.tsv` (every Ukrainian string) and `build.py`, which makes `src/doba_i18n_uk.inc` and checks format specifiers |
| `vfont/vfont_build.py` | makes `src/vfont_faces.h`, the vector fonts with Cyrillic; see `vfont/DESIGN.md` |
| `declination/`, `tzgrid/` | make the magnetic-declination and time-zone tables in `src/` |
| `preview/shim/` | ESP-IDF header stand-ins, used by the simulator build |

## Host tests (no device needed)

| | |
|---|---|
| `map_cache_test/run.sh` | the map frame cache (`src/map_base_cache.h`) |
| `scan_test/run.sh` | the panel scan path's tables |
| `aux_test/run_aux_test.sh` | the optional I2C sensors' maths |
| `milgeo_test/run.py` | MGRS and mils conversions (`src/milgeo.cpp`) against a reference |
| `mesh_test/run_mesh_test.sh` | the Meshtastic packet code (`src/mesh_*.cpp`) |
| `crash_test/run_tests.py` | crash reports: flash, card, retries (`src/crash_report.cpp`) |
| `epd_hybrid_bench/run_core_test.sh` | the panel driver's frame scheduler |

The whole device runs in the simulator: `sim/build.sh`, then `sim/test.sh`.

## On a device

| | |
|---|---|
| `memfault_export.py` | checks a Memfault export taken off the card before it goes to Memfault's tools |
| `power_capture.py`, `power_logging.py`, `power_sd_decode.py`, `power_framework_report.py` | power measurements: capture, decode, report |
| `epdpainter_test/`, `epd_hybrid_bench/` | panel experiments flashed as their own firmware |
