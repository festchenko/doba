#pragma once
// DOBA: sun and moon without a network — NOAA's sunrise equation (good to a
// minute or two below the polar circles) and the moon's age from the synodic
// month.
#include <time.h>

namespace doba_sun {

struct Day {
    bool riseOk, setOk;         // false: sun never rises / never sets that day
    time_t rise, set;           // UTC
    time_t dawn, dusk;          // civil twilight (sun 6 deg below)
    bool dawnOk, duskOk;
};

// The day (UTC date of `when`) at lat/lon.
Day day(double lat, double lon, time_t when);

// Moon: age in days (0 = new, ~14.8 = full) and lit fraction 0..1.
double moonAge(time_t when);
double moonLit(time_t when);
const char* moonName(time_t when);   // "new", "waxing crescent", ...

}  // namespace doba_sun
