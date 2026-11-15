# Firmware sim protocols

The sim (`sim/build/otp-sim`) runs the real firmware on the host. It listens on
one TCP port on 127.0.0.1 (default 7879, `--ctl-port`): the control channel,
for the test or viewer driving the device, in JSON lines.

## control — driving the device

Newline-delimited JSON. Every request may carry `"id"`; the response echoes it.
Responses are `{"id":…, "ok":true, …}` or `{"id":…, "ok":false, "error":"…"}`.
Asynchronous events (log lines, when subscribed) come as `{"event":…}`.

| cmd | arguments | does |
|-----|-----------|------|
| `status` | — | `screen` (`map` or `other`), `page` (the firmware's Screen number: tells Doba's pages apart), settings, the position and fix (`ride`, with `ground_m` from the card's DEM), the mesh, the card's size and use (`sd`, as last counted), panel counts (`paints`, `scrubs` — the scoped pre-paint resync, `clears` — whole-panel), clocks |
| `console` | `line`, `wait_ms` | runs a serial console command; returns its output |
| `screenshot` | `path` | writes the panel as a 540×960 PNG |
| `tap` | `x`, `y`, `ms` | a touch at panel coordinates (portrait, 540×960) |
| `button` | `name` (`boot`, `home`, `side`), `ms` | presses a key |
| `gps` | `lat`, `lon`, `alt`, `speed_kmh`, `course`, `fix`, `sats`, `time` (no fix: still report UTC; default true); or `track` [[lat,lon],…] with `speed_kmh` | sets what the GPS module reports |
| `gps_silent` | `on` | the GPS module goes quiet (sends and hears nothing) until `on: false` |
| `battery` | `pct`, `charging` | the fuel gauge's reading |
| `wallclock` | `unix` | sets the device's clock (as the RTC would) |
| `sd` | `present`; or `latency_ms`; or `size_mb` (+ `cluster_kb`); or `usage` | inserts or removes the card; or makes every card open/read/write cost that much device time (a slow card); or swaps in a card of that size with the same files; or just reports. Answers `sd`: `card_bytes`, `total_bytes`, `used_bytes`, `free_bytes`, `cluster_bytes` (usage recounted from `<dir>/sd`) |
| `usb` | `host` | a computer mounts the card over USB (`true`: the firmware keeps off it) or lets it go (`false`: the firmware mounts it again and looks at its maps, books, points, heights and places again) |
| `speed` | `factor` | runs the device clock N× real time |
| `log` | `since` | log lines after sequence number `since` |
| `wait_log` | `pattern` (regex), `timeout_ms`, `since` | waits for a log line |
| `subscribe_log` | `on` | streams log lines as events |
| `reboot` | — | restarts the firmware (NVS and card kept): a power cut — RTC memory lost, the next boot is a power-on |
| `lora` | `loss_pct`, `crc_error_pct`, `latency_ms`, `rssi`, `snr`, `busy`, `overrun`, `seed` (all optional) | the LoRa radio (`--lora`, see below): sets the link's impairments, then reports `lora`: `fitted`, `up`, `mode` (off/sleep/standby/rx/tx), its tuning (`freq_mhz`, `bw_khz`, `sf`, `cr`, `sync_word`, `power_dbm`), the impairments, and counters — `queued`, `lost` (to `loss_pct`), `mistuned` (sent on another frequency/modem), `deaf` (arrived while it was asleep or transmitting), `overrun` (overwritten in the one-packet FIFO before it was read), `crc`, `read`, `tx`, `cad_busy`, `begins`, `tx_seq` — and for diagnosis `listening`, `deaf_spans` (recent [from, to] device-ms spans it was not receiving), `last_deaf_frame`, `now_ms`. With `airtime_len` N: just the time on air of N bytes at the current tuning |
| `lora_rx` | `hex`; `rssi`, `snr`, `delay_ms`, `elapsed_ms`, `crc_error`; `freq_mhz`, `bw_khz`, `sf`, `cr`, `sync_word` | puts a raw frame (1–255 bytes, a Meshtastic packet as sent over the air) on the air for the radio, by default on the tuning it has now. It is on the air for its LoRa time on air after the link latency, and after any frame before it (frames don't overlap); `elapsed_ms`: it started that long ago. Answers `lost` (dropped by `loss_pct`), `start_ms`, `end_ms` (device time) |
| `lora_tx` | `since` | frames the radio transmitted after sequence `since`, logged as each starts: `frames` [{`seq`, `start_ms`, `end_ms`, tuning, `power_dbm`, `aborted`, `hex`}], `seq` (the last), `now_ms` |
| `quit` | — | exits the sim |
| **power and robustness** | | |
| `reset` | `reason` (`panic`, `task_wdt`, `int_wdt`, `wdt`, `brownout`, `sw`, `ext`) | the chip resets for that reason: `esp_reset_reason()` says so on the next boot and RTC memory (`RTC_NOINIT_ATTR`: the crash reporter's breadcrumbs, diag's storage trace) survives. Unflushed card writes are lost, as with `reboot` |
| `gps_time` | `unix` | sets the GPS module's clock (UTC s); it keeps counting from there (a UTC midnight, a week-number rollover) |
| `gps_raw` | `data` | puts bytes on the GPS UART as they are (noise, bad checksums, torn or made-up sentences); also while `gps_silent` |
| `gps_paced` | `on` | the module's sentences arrive one by one, each after its bytes have crossed the UART, instead of as one batch a second |

`status` carries `reset_reason`: this boot's `esp_reset_reason()` (1 power-on,
3 software, 4 panic, 5 interrupt watchdog, 6 task watchdog, 7 watchdog,
8 wake from deep sleep, 9 brownout).

Resets in the sim: a reset re-execs it. `<dir>/rtc_mem.bin` carries the reset
reason and the `RTC_NOINIT_ATTR` section across (absent: a power-on).
`esp_restart()` is a software reset; the firmware's `abort()` (the console's
`crashlog panic`, a failed assert) is a panic; `--sd-crash-on-read` an
interrupt watchdog. Deep sleep exits the process, and the next launch of the
same directory is the wake (`ESP_RST_DEEPSLEEP`). The GPS module's clock runs
on across all of these (`<dir>/gps_clock.txt`, its backup supply); its fix and
track do not, so a test sets them again after a reset.

### The LoRa radio and its air

`--lora` fits the SX1262 (the mesh, `src/mesh_service.cpp`); without it the
board has none, as before. `sim/host/lora_sim.cpp` implements `lora_radio.h`
as the chip behaves: a transmission lasts its time on air (Semtech's formula)
and DIO1 fires at its end; the radio is half-duplex (what ends while it
transmits, sleeps or retunes is lost — `deaf`; after a transmission it receives again only when the firmware calls `listen()`/`finishSend()`, as the chip drops to standby at TxDone); a frame is heard only on the
same frequency, bandwidth, spreading factor and sync word (the coding rate is
in the LoRa header); the FIFO holds one packet, so one not read before the
next arrives is overwritten (`overrun`, as continuous receive does on the
chip); a damaged frame fires DIO1 and reads as nothing (`crc`). Channel-activity
detection is busy while a frame is on the air or `busy` is set. Collisions
between two senders are not modelled: frames queue one after another.

The air is the control channel. A test is a virtual node (`lora_rx`) and a
listener (`lora_tx`); `otpsim.mesh.Air` joins several sims by relaying each
one's transmissions to the others' `lora_rx` as they start, with per-link
loss/RSSI. `--mac` gives each sim its own MAC, i.e. its own mesh node number
(bytes 2–5, little-endian, as Meshtastic derives it).

## Command line

`otp-sim --help` lists the options. `--lora` fits the LoRa radio and
`--mac AA:BB:CC:DD:EE:FF` sets the board's MAC (above). Those about the card: `--sd DIR` (its
root, default `<dir>/sd`), `--sd-size-mb N` (capacity, default 32 GB),
`--sd-cluster-kb N` (FAT cluster size), `--sd-latency-ms N` (device time per
card call), `--sd-ready-ms N`, `--sd-crash-on-read PATH`.
