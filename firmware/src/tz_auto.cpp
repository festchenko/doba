#include "tz_auto.h"

#include "tz_grid.h"

namespace tz_auto {
namespace {

const tz_grid::Zone* zoneAt(double lat, double lon) {
    const int j = (int)((lat - tz_grid::LAT0) / tz_grid::STEP);
    if (j < 0 || j >= tz_grid::NLAT) return nullptr;
    while (lon < -180) lon += 360;
    while (lon >= 180) lon -= 360;
    const int i = (int)((lon + 180) / tz_grid::STEP);
    uint32_t p = tz_grid::kRowOff[j];
    int c = 0;
    while (true) {
        const int n = tz_grid::kRuns[p], z = tz_grid::kRuns[p + 1];
        if (i < c + n) return &tz_grid::kZones[z];
        c += n;
        p += 2;
        if (c >= tz_grid::NLON) return nullptr;
    }
}

// Seconds since 1970 for a UTC date (no timegm() in the ESP32 toolchain).
time_t utcOf(int y, int m, int d, int hh) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = (long)era * 146097 + (long)doe - 719468;
    return (time_t)days * 86400 + hh * 3600;
}

// Day of month of the n-th (1..) or last (n = 0) Sunday of month m (1..12) in year y.
int sunday(int y, int m, int n) {
    const time_t first = utcOf(y, m, 1, 12);
    const int wday = (int)((first / 86400 + 4) % 7);   // 1970-01-01 was a Thursday
    const int firstSun = 1 + (7 - wday) % 7;
    if (n > 0) return firstSun + 7 * (n - 1);
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int last = kDays[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 || y % 400 == 0));
    int d = firstSun;
    while (d + 7 <= last) d += 7;
    return d;
}

time_t at(int y, int m, int d, int hh) { return utcOf(y, m, d, hh); }

bool summer(const tz_grid::Zone& z, time_t utc) {
    if (z.rule == 0 || utc < 1000000000) return false;
    struct tm t;
    gmtime_r(&utc, &t);
    const int y = t.tm_year + 1900;
    if (z.rule == 1)   // EU: 01:00 UTC, last Sunday of March .. last Sunday of October
        return utc >= at(y, 3, sunday(y, 3, 0), 1) && utc < at(y, 10, sunday(y, 10, 0), 1);
    // US: 02:00 local, second Sunday of March .. first Sunday of November
    const int std = z.stdMin;
    return utc >= at(y, 3, sunday(y, 3, 2), 2) - std * 60 &&
           utc < at(y, 11, sunday(y, 11, 1), 2) - (std + 60) * 60;
}

}  // namespace

bool offsetAt(double lat, double lon, time_t utc, int& minutes) {
    const tz_grid::Zone* z = zoneAt(lat, lon);
    if (!z) return false;
    minutes = z->stdMin + (summer(*z, utc) ? 60 : 0);
    return true;
}

bool standardAt(double lat, double lon, int& minutes) {
    const tz_grid::Zone* z = zoneAt(lat, lon);
    if (!z) return false;
    minutes = z->stdMin;
    return true;
}

bool summerAt(double lat, double lon, time_t utc) {
    const tz_grid::Zone* z = zoneAt(lat, lon);
    return z && summer(*z, utc);
}

bool inUkraine(double lat, double lon) {
    const tz_grid::Zone* z = zoneAt(lat, lon);
    return z && z->ukraine;
}

}  // namespace tz_auto
