#include "doba_sun.h"

#include <math.h>

namespace doba_sun {
namespace {

constexpr double D2R = M_PI / 180.0, R2D = 180.0 / M_PI;

// Minutes after UTC midnight of the event, NOAA's algorithm; NAN if none.
double event(double lat, double lon, int doy, bool rising, double zenith) {
    const double lngHour = lon / 15.0;
    const double t = doy + ((rising ? 6.0 : 18.0) - lngHour) / 24.0;
    const double M = 0.9856 * t - 3.289;
    double L = M + 1.916 * sin(M * D2R) + 0.020 * sin(2 * M * D2R) + 282.634;
    L = fmod(L + 360.0, 360.0);
    double RA = R2D * atan(0.91764 * tan(L * D2R));
    RA = fmod(RA + 360.0, 360.0);
    RA += floor(L / 90.0) * 90.0 - floor(RA / 90.0) * 90.0;
    RA /= 15.0;
    const double sinDec = 0.39782 * sin(L * D2R);
    const double cosDec = cos(asin(sinDec));
    const double cosH = (cos(zenith * D2R) - sinDec * sin(lat * D2R)) / (cosDec * cos(lat * D2R));
    if (cosH > 1 || cosH < -1) return NAN;
    double H = rising ? 360.0 - R2D * acos(cosH) : R2D * acos(cosH);
    H /= 15.0;
    const double T = H + RA - 0.06571 * t - 6.622;
    double UT = fmod(T - lngHour + 48.0, 24.0);
    return UT * 60.0;
}

}  // namespace

Day day(double lat, double lon, time_t when) {
    struct tm g;
    gmtime_r(&when, &g);
    const int doy = g.tm_yday + 1;
    const time_t midnight = when - (g.tm_hour * 3600 + g.tm_min * 60 + g.tm_sec);
    Day d = {};
    auto at = [&](double minutes, bool& ok, time_t& out) {
        ok = !isnan(minutes);
        if (ok) out = midnight + (time_t)lround(minutes * 60.0);
    };
    at(event(lat, lon, doy, true, 90.833), d.riseOk, d.rise);
    at(event(lat, lon, doy, false, 90.833), d.setOk, d.set);
    at(event(lat, lon, doy, true, 96.0), d.dawnOk, d.dawn);
    at(event(lat, lon, doy, false, 96.0), d.duskOk, d.dusk);
    // East of Greenwich a UTC "evening" can fall on the previous UTC date and
    // vice versa: keep each event on the same side of the day's midday.
    const time_t noon = midnight + (time_t)lround((12.0 - lon / 15.0) * 3600.0);
    auto fix = [&](bool ok, time_t& t, bool morning) {
        if (!ok) return;
        if (morning && t > noon) t -= 86400;
        if (!morning && t < noon) t += 86400;
    };
    fix(d.riseOk, d.rise, true);
    fix(d.dawnOk, d.dawn, true);
    fix(d.setOk, d.set, false);
    fix(d.duskOk, d.dusk, false);
    return d;
}

double moonAge(time_t when) {
    const double jd = when / 86400.0 + 2440587.5;
    const double syn = 29.530588853;
    double a = fmod(jd - 2451550.1, syn);
    if (a < 0) a += syn;
    return a;
}

// Lit fraction from the true phase angle (Meeus, Astronomical Algorithms
// 48.4: elongation and anomalies with their main periodic terms). The mean
// lunar age alone is off by up to ~2 % near new moon, which shows as "6 %"
// where a Garmin says 4 %.
double moonLit(time_t when) {
    const double T = (when / 86400.0 + 2440587.5 - 2451545.0) / 36525.0;
    const double r = M_PI / 180.0;
    const double D = 297.8501921 + 445267.1114034 * T;    // mean elongation
    const double M = 357.5291092 + 35999.0502909 * T;     // sun's mean anomaly
    const double Mp = 134.9633964 + 477198.8675055 * T;   // moon's mean anomaly
    const double i = 180.0 - fmod(D, 360.0) - 6.289 * sin(Mp * r) + 2.100 * sin(M * r)
                     - 1.274 * sin((2 * D - Mp) * r) - 0.658 * sin(2 * D * r)
                     - 0.214 * sin(2 * Mp * r) - 0.110 * sin(D * r);
    return (1 + cos(i * r)) / 2;
}

const char* moonName(time_t when) {
    const double a = moonAge(when);
    if (a < 1.85) return "new moon";
    if (a < 5.54) return "waxing crescent";
    if (a < 9.23) return "first quarter";
    if (a < 12.92) return "waxing gibbous";
    if (a < 16.61) return "full moon";
    if (a < 20.30) return "waning gibbous";
    if (a < 23.99) return "last quarter";
    if (a < 27.68) return "waning crescent";
    return "new moon";
}

}  // namespace doba_sun
