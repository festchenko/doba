"""Bound the USB serial's wait for room, so a host that holds the port open
without reading cannot hang the device.

The Arduino core's USBCDC::write() loops `while (to_send)` and, when the TX
FIFO is full, flushes and tries again with no timeout and without yielding.
A host that has the port open (DTR set) but is not reading -- a terminal
being reopened, two programs fighting over the port, a Mac busy mounting the
card over USB -- leaves the FIFO full forever: the task that printed (the GPS
task's once-a-second line, on 16 Nov 2026) spins on core 0 and the task
watchdog resets Doba, or everything that logs stops (the "frozen" Doba).

Patched here: a full FIFO is waited on for at most tx_timeout_ms, yielding a
tick at a time; then the rest of the line is dropped. Runs before the build;
the edit is to the framework package and repeatable.
"""
from pathlib import Path

Import("env")  # noqa: F821  (PlatformIO)

MARKER = "// Doba: bounded wait for the host (tools/usbcdc_patch.py)"
OLD = """        size_t space = tud_cdc_n_write_available(itf);
        if(!space){
            tud_cdc_n_write_flush(itf);
            continue;
        }"""
NEW = """        size_t space = tud_cdc_n_write_available(itf);
        if(!space){
            """ + MARKER + """
            tud_cdc_n_write_flush(itf);
            if((uint32_t)(millis() - doba_t0) >= tx_timeout_ms){
                size = so_far;
                break;
            }
            if(!xPortInIsrContext()) vTaskDelay(1);
            continue;
        }"""
START_OLD = "    size_t to_send = size, so_far = 0;\n    while(to_send){"
START_NEW = "    size_t to_send = size, so_far = 0;\n    const uint32_t doba_t0 = millis();\n    while(to_send){"

pkg = env.PioPlatform().get_package_dir("framework-arduinoespressif32")  # noqa: F821
src = Path(pkg) / "cores" / "esp32" / "USBCDC.cpp"
text = src.read_text()
if MARKER not in text:
    if text.count(OLD) != 1 or text.count(START_OLD) != 1:
        raise SystemExit("usbcdc_patch: USBCDC.cpp changed upstream; review the patch")
    text = text.replace(START_OLD, START_NEW).replace(OLD, NEW)
    src.write_text(text)
    print("usbcdc_patch: USBCDC::write now gives up on a host that does not read")
