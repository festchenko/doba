# Doba changes to the OpenTrailPaper firmware

Every change is marked `// DOBA:` in the sources.

## Topographic base layer
- `src/topo_layer.{h,cpp}` — reads Doba tile packs (`/doba/map/base.dtp`,
  `/doba/map/8/<x>/<y>.dtp`, format in `maps/tilepack.py`), keeps 32 decoded
  tiles (1 MB) in PSRAM, inflates with the ESP32-S3 ROM `tinfl` (zlib in the
  sim), and blits the 16-level tiles 1:1 into the 4bpp framebuffer.
- `src/map_view.{h,cpp}` — `MapScreenData::topo`; `ui_render_map_base()` draws
  the topo tiles instead of the vector roads when it is set.
- `src/ui_dashboard.cpp` — a pack under the rider switches the map to topo:
  zoom snaps to tile zooms (z16 ≈ 1.5 m/px at 52°N), north-up, no vector
  tiles read; zoom-out goes to ~200 m/px instead of 32; console command `topo`
  prints the layer's state and counters.

## Simulator
- `sim/` builds and runs on Linux as well as macOS (`sim/linux/linux_compat.h`,
  `fopencookie`, `/proc/self/exe`, ELF section for RTC memory).
- `sim/tests/test_topo.py` — grey levels, orientation and centring checked on
  the simulated panel, with packs written by `maps/tilepack.py`.

## Not changed (yet)
- BLE name, FIT product name and the version string are still OpenTrailPaper's,
  so the OpenTrailPaper phone app keeps working with the device (mesh chat,
  settings) until Doba has its own on-device keyboard and settings.
