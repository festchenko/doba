# Doba firmware simulator

The real firmware on a Mac or Linux box, so Doba can be tested without the
board: every page, the map, the library, the mesh and the power paths.

```sh
sim/build.sh                      # -> sim/build/otp-sim
sim/test.sh                       # the test suite (sim/tests), arguments go to pytest
python3 sim/simview.py --data /Volumes/festchenko/doba-build/data   # the panel in a browser tab
```

`otp-sim` compiles `src/` as it is: the UI and renderers, the library, the topo
map, points, the mesh (`mesh_*.cpp`), GPS parsing and baud detection
(`gps_service.cpp`), settings, crash reports and diag. Only `src/main.cpp` is
replaced, by `sim/host/sim_main.cpp`, which mirrors its `setup()`/`loop()`
minus the board. `sim/shim/` stands in for Arduino, FreeRTOS, ESP-IDF, the SD
card and NVS, and `sim/host/` for the hardware:

| Hardware | In the sim |
|---|---|
| Tasks, mutexes, queues | threads ([rtos_sim.cpp](host/rtos_sim.cpp)); the clock can run N× faster |
| E-paper panel | a framebuffer; every paint, scrub and clear is counted, screenshots are PNGs of what's on the glass |
| Touch, keys | injected from the control channel; a press is held until the firmware has read it |
| SD card | a directory (`<dir>/sd`), removable, with a size (`--sd-size-mb`, default 32 GB); optionally slow (`--sd-latency-ms`), slow to come up (`--sd-ready-ms`), or holding a file whose reading resets the device (`--sd-crash-on-read`); a computer can hold it over USB (`usb host`) while a test edits the directory |
| NVS | files under `<dir>/nvs`, kept across reboots |
| L76K GPS on UART2 | an emulated module: obeys PCAS commands (baud, output, version, save), keeps its baud across restarts, streams NMEA for a fix or a track |
| SX1262 LoRa radio | with `--lora`: [lora_sim.cpp](host/lora_sim.cpp) — tuning, time on air, half-duplex, a one-packet FIFO, CRC errors, channel-activity detection; its air is the control channel |
| Resets, RTC memory | a panic, watchdog or brownout (`reset`), `esp_restart()` and `abort()` re-exec the sim with `esp_reset_reason()` set and `RTC_NOINIT_ATTR` memory kept; deep sleep exits it and the next launch is the wake |
| Fuel gauge | set from the control channel |
| Qwiic sensors, RTC chip, the USB stack, light sleep, Memfault | absent (the firmware's "not fitted" paths) |

The control channel is in [PROTOCOL.md](PROTOCOL.md). The Python client
([client/otpsim](client/otpsim)) drives it:

```python
from otpsim import SimDevice
dev = SimDevice(args=["--lora"])
dev.launch()
dev.gps(52.0421, 6.6170)
dev.console("screen map")
dev.screenshot("map.png")
```

`otpsim.mesh` joins several sims, and a test, over one LoRa air.

`sim/tools/tour_i18n.py uk OUT BOOK` walks every page in Ukrainian and saves a
screenshot of each, with every string drawn (`OUT/i18n.txt`).

A build with AddressSanitizer and UBSan, and the tests against it:

```sh
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer" OUT=sim/build-san sim/build.sh
OTP_SIM_BINARY=$PWD/sim/build-san/otp-sim sim/test.sh
```
