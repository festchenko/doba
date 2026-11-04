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

## Military-style map
- `src/milgeo.{h,cpp}` — UTM (WGS84, Norway/Svalbard zones), MGRS at 1 m,
  mils (6000 as `05-33`, or NATO 6400). Checked against the `mgrs` and
  `pyproj` libraries by `tools/milgeo_test/run.py` (509 points, 0 mismatches).
- `src/milspec.{h,cpp}` — on the topo map: UTM kilometre grid (1 km up to
  ~20 m/px, then 10 km), lines labelled with two-digit kilometres at the top and
  left edges, every 10th line heavier; the bottom strip is a monospace readout:
  MGRS, latitude/longitude, GPS altitude, satellites, course in mils + degrees.
  Console `mils 6000|6400` switches the system (kept in NVS).
- `src/fonts/doba_mono_{s,m,l}.h` — DejaVu Sans Mono Bold, epdiy format.
- `src/gps_service.cpp` — course over ground from 2 km/h (walking) instead of 5.
- `src/map_view.cpp` — no bike-layers button on topo; scale bar rungs to 20 km.

## Tactical restyle (all screens)
- `src/vfont_faces.h` — every face is JetBrains Mono (OFL): 800 for values,
  700 for labels (`src/fonts/vf/jbmono_700.h`, built with tools/vfont).
- `src/ui_render.cpp` — black status band in white monospace (`14:25 SAT 10
  REC … BAT 87%`); menu: Record Track / Routes / Phone Link / Tracks /
  Settings; settings row 0 is MILS (60-00 / 6400) instead of FTP; track wording
  on the summary and sheets; a reticle mark and "D O B A" on boot and power-off.
- `src/dash_layout.*` — new fields `mgrs`, `latlon`, `course` (mils + degrees);
  altitude falls back to GPS; the default page is MGRS / lat-lon / azimuth /
  altitude + distance / time, and the vector-map strip course / altitude / sats.
- `greytest` console command — a card of the 16 framebuffer levels, to see on
  the real panel which greys it tells apart (any touch closes it).

## Map style and the panel's greys
The shipping display driver (EPD_Painter, fast mode) shows four greys:
levels 0-2 black, 3-7 dark, 8-12 light, 13-15 white (`epd_compat.cpp`
`buildLut()`). `maps/style.py` therefore draws with BLACK/DARK/LIGHT/WHITE only
and tells areas apart by pattern — forest dots, built-up hatch, wetland dashes —
like a one-colour military sheet. Upstream comments say light greys may show as
white on the real panel: check with `greytest` on day one.

## Simulator
- `sim/` builds and runs on Linux as well as macOS (`sim/linux/linux_compat.h`,
  `fopencookie`, `/proc/self/exe`, ELF section for RTC memory).
- `sim/tests/test_topo.py` — grey levels, orientation and centring checked on
  the simulated panel, with packs written by `maps/tilepack.py`.

## Not changed (yet)
- BLE name, FIT product name and the version string are still OpenTrailPaper's,
  so the OpenTrailPaper phone app keeps working with the device (mesh chat,
  settings) until Doba has its own on-device keyboard and settings.
