#pragma once
// DOBA: time zone and radio region from where the device is. No network, no
// phone: the GPS position is looked up in a built-in world grid
// (tools/tzgrid, src/tz_grid.h), summer time follows the zone's rule.
#include <cstdint>
#include <ctime>

namespace tz_auto {

// UTC offset in minutes at (lat, lon) at UTC time `utc` (summer time included).
// false: no zone known there.
bool offsetAt(double lat, double lon, time_t utc, int& minutes);
// The zone's standard offset (winter), for a label.
bool standardAt(double lat, double lon, int& minutes);
bool summerAt(double lat, double lon, time_t utc);
// Inside Ukraine's zone: the radio uses UA_868 there.
bool inUkraine(double lat, double lon);

}  // namespace tz_auto
