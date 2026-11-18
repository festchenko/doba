#pragma once

// The ESP32-S3's native USB: a serial port (console, logs, flashing). Nothing
// else. Doba used to offer its card to a computer as a USB drive as well; that
// was removed on 18 Nov 2026 — a Mac mounting the 1 TB card hung Doba and
// dropped its serial port, and it never worked well enough to be the way books
// get onto the card. Books and maps go on with a card reader (doba_card.py).

namespace usb_storage {

// Starts the USB serial port. Called once from the UI task after boot.
void begin();

}
