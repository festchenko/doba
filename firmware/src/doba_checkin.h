#pragma once
// DOBA: "I am OK" on the mesh every few hours, and the 72-hour mode.
//
// Check-in: every N hours (off, 2, 4, 6, 12, 24) Doba sends "OK" with its
// name, position (MGRS) and time to the channel, so the group sees who is
// fine and where. It also runs while Doba is off: going to sleep, Doba sets
// the chip's timer for the next check-in; woken by it, Doba waits up to 3
// minutes for a GPS fix (else sends the last position, saying how old), sends,
// and goes back to sleep.
//
// 72-hour mode: the settings that make a battery last through a long outage,
// in one switch: light off, sleep after 1 minute idle, check-in every 6 hours
// (unless one is already set).
#include <cstdint>
#include "device_state.h"

namespace checkin {

void begin(bool wokeByTimer);       // at boot: was this a check-in wake?
int hours();                        // 0: off
void stepHours(int dir);            // the More page's stepper
bool longMode();                    // the 72-hour mode
void setLongMode(bool on);
uint32_t idleSleepMs();             // auto-sleep after this much idle
// The UI loop, every pass. true: a check-in wake is done, go back to sleep.
bool tick(const DeviceState& s);
// Seconds until the next check-in is due (0: none), for the sleep timer.
uint32_t secondsToNext();
bool sentLast(char* out, unsigned n);
void dueNow();                      // console "checkin now": the next tick sends   // "22:10" of the last one sent, if any

}  // namespace checkin
