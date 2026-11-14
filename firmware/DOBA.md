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
  on the summary and sheets; the boxed "D.O.B.A" wordmark and "DURABLE OFFLINE
  BACKCOUNTRY ASSISTANT" on boot and power-off (`drawDobaMark`).
- `src/dash_layout.*` — new fields `mgrs`, `latlon`, `course` (mils + degrees);
  altitude falls back to GPS; the default page is MGRS / lat-lon / azimuth /
  altitude + distance / time, and the vector-map strip course / altitude / sats.
- `greytest` console command — a card of the 16 framebuffer levels, to see on
  the real panel which greys it tells apart (any touch closes it).

## HOME, COMMS and LIBRARY (`src/doba_ui.*`, `doba_keyboard.*`, `doba_library.*`)
- Boot lands on HOME: eight tiles — MAP, POSITION (MGRS), LIBRARY, COMMS (unread
  badge, nodes heard), TRACKS, ROUTES, SETTINGS, RECORD/STOP. The Home key goes
  HOME from any page and from HOME to the map; a tap on the status band goes
  HOME. (OTP's Home key stepped the dashboard carousel.)
- COMMS: radio on/off, the Meshtastic channel's messages, WRITE opens COMPOSE —
  an on-device keyboard (EN / RU / UA / symbols, 200 bytes) that broadcasts with
  `mesh_service::queueText`. No phone needed.
- LIBRARY: books in `/doba/lib/<book>/` (`info.txt`, `index.dti`, `aNNNN.dta`,
  made by `library/zim2doba.py`). Prefix search over folded titles (case, ё→е,
  accents; table generated from `library/dobafold.py`), article text unpacked to
  PSRAM, word-wrapped and paged; tap left/right half to turn pages.
- Console: `screen home|comms|compose|library`.

## SETTINGS and POSITION (`doba_ui.cpp`, `doba_points.*`, `milgeo` parse)
- SETTINGS (the HOME tile): radio, band (cycles EU_868, UA_868, EU_433,
  UA_433, US, RU), mode, callsign typed on the device (short name = first four
  letters), 60-00 / 6400 mils, light, time zone. MORE opens OTP's settings.
- POSITION: own MGRS / lat-lon / height, MARK HERE, ADD GRID (MGRS typed in,
  zone and square prefilled from the fix, at least 4 digits), the points with
  azimuth (mils) and distance. A point: north-up dial, azimuth, distance, its
  height and the climb (DEM), GO TO / DELETE. GO TO: a dashed line on the map,
  a name tag where it leaves the screen, AZ and distance in the strip.
- Points live in `/doba/points.txt` (`name;lat;lon` per line).
- Defaults: TZ UTC+1, callsign "Doba xxxx".

## Ground height (`doba_dem.*`, `maps/dempack.py`)
- `/doba/dem/N52E006.dem`: 1x1 degree, ~90 m samples, delta-coded zlib blocks
  of 100x100. Every GPS fix looks it up and sets `mapElevation`, which the
  altitude field, the strip and POSITION prefer over GPS height.

## Library links and documents
- `zim2doba.py` keeps in-book links; the reader underlines them, follows by
  exact title, Home goes back (16 deep). PREV / NEXT footer pages.
- Every article ends "Source: …" (`source=` in info.txt).
- `library/docs2doba.py`: PDF (split per bookmark chapter, contents article
  with links, chapters indexed by their bare names), EPUB, HTML, MD, TXT.

## Contour background (`src/doba_topo_bg.*`)
- A made-up landscape contoured like a topo sheet (every 5th line 2 px) behind
  HOME, COMMS, the LIBRARY search and below the boot log. Drawn after the
  content, only on white, 3 px clear of anything drawn — text keeps a halo.
  Tone `topo_bg::kLine` (0xAA, the LUT's light grey): confirm with `greytest`.

## Radio region
- `config.h` MESH_REGION_DEFAULT is `EU_868` (869.4–869.65 MHz; OTP shipped
  `US`, 902–928 MHz — wrong band for an 868 MHz board in Europe). `UA_868` is
  in the table for use in Ukraine.

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
- `otpsim/mesh.py` falls back to `cryptography` for AES off macOS.
- The inherited OTP suites start the sim on the dashboard (`DOBA_START=dash`)
  and the US band plan (`DOBA_MESH_REGION=US`, both
  set in `sim/tests/conftest.py`); `test_boot.py` checks the real HOME boot.
  Tests that encoded OTP's Home-key carousel were rewritten for Doba's.
- `sim/tests/test_topo.py` — grey levels, orientation and centring checked on
  the simulated panel, with packs written by `maps/tilepack.py`.

## Not changed (yet)
- BLE name, FIT product name and the version string are still OpenTrailPaper's,
  so the OpenTrailPaper phone app keeps working with the device (mesh chat,
  settings). Mesh chat now also works on the device itself (COMMS).
- Links, images and tables inside library articles are dropped.

## Bike-computer features removed (November 2026)
Doba grew out of the OpenTrailPaper bike computer. What Doba never used is
gone, about 27 000 lines: structured workouts, Strava segments, phone music
control, the bike radar, Bluetooth sensors (heart rate, power, cadence, wheel
speed), the Bluetooth companion link (pairing, protobuf link, firmware over
Bluetooth) and the NimBLE stack itself, with their screens, settings, tools
and sim suites. The firmware is ~400 KB smaller and has no Bluetooth or Wi-Fi
at all: the LoRa radio is the only transmitter. Track recording (FIT), routes,
the ride page and the mesh stay. The sim reads the mesh through its `mesh`
control command, and Doba's console gained `mesh tx|preset|share|chan`.
Auto-sleep is held while the mesh is on (a listening radio is a pager).
