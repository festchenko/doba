#include "usb_storage.h"

#include <Arduino.h>
#include "USB.h"

#include "diag.h"

namespace usb_storage {

void begin() {
    static bool started = false;
    if (started) return;
    USB.begin();
    started = true;
    diag::log("usb: serial port up (no USB drive)");
}

}  // namespace usb_storage
