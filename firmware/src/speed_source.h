#pragma once

// Which speed the user sees. Doba has one source, its own GPS receiver; the
// receiver writes gpsSpeedKmh and calls publish() under the g_state lock, so
// speedKmh is 0 the moment the fix goes stale.

#include <cstdint>

#include "ride_state.h"

namespace speed_source {

enum Source : uint8_t { NONE = 0, GPS = 2 };

inline const char* name(uint8_t s) { return s == GPS ? "gps" : "none"; }

inline Source pick(const RideState& s, uint32_t) { return s.gpsFix ? GPS : NONE; }

// Recompute speedKmh/speedSource. Call with the g_state lock held.
inline void publish(RideState& s, uint32_t nowMs) {
    const Source src = pick(s, nowMs);
    s.speedKmh = src == GPS ? s.gpsSpeedKmh : 0.0f;
    s.speedSource = src;
}

}  // namespace speed_source
