#pragma once

#include <stdint.h>

// DOBA: the microSD card — mounting, retries, lending it to a USB host. Split
// out of the old ride recorder when track recording went away.

namespace sd_card {

bool begin();                 // mounts the card at boot
// Bumped on every successful mount: open File handles from before are dead.
uint32_t mountGeneration();
// The mounted card's FatFs drive number, for raw sector access (USB storage);
// 0xFF before the first mount.
uint8_t sdDrive();
// The USB host has the card: unmount the firmware's volume (driver kept).
// remount() undoes it.
bool lendToHost();
// SD.end() then SD.begin(), under the SD lock. `why` is logged.
bool remount(const char* why);
// ~1 Hz from loop(): while unmounted, retries on a slow cadence.
void retryMountIfNeeded();
// True once after a card mounted later than boot did (the UI then re-runs the
// card bring-up boot skipped).
bool consumeLateMount();
bool sdMounted();             // mounted and not lent to a USB host
// SD_CARD_OK / SD_CARD_NONE / SD_CARD_UNREADABLE (ride_state.h). Lock-free.
uint8_t sdCardState();
uint32_t sdFreeMB();

}  // namespace sd_card
