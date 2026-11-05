#include <cctype>
#include <cstring>
// DOBA: see milgeo.h. Transverse Mercator after Snyder, "Map Projections — A
// Working Manual" (USGS PP 1395), eqs. 8-9..8-25; sub-millimetre within a zone.
#include "milgeo.h"

#include <cmath>
#include <cstdio>

namespace milgeo {
namespace {

constexpr double A = 6378137.0;
constexpr double F = 1.0 / 298.257223563;
constexpr double K0 = 0.9996;
constexpr double E2 = F * (2 - F);           // e^2
constexpr double EP2 = E2 / (1 - E2);        // e'^2
constexpr double DEG = M_PI / 180.0;

double meridianArc(double phi) {
    const double e4 = E2 * E2, e6 = e4 * E2;
    return A * ((1 - E2 / 4 - 3 * e4 / 64 - 5 * e6 / 256) * phi -
                (3 * E2 / 8 + 3 * e4 / 32 + 45 * e6 / 1024) * sin(2 * phi) +
                (15 * e4 / 256 + 45 * e6 / 1024) * sin(4 * phi) -
                (35 * e6 / 3072) * sin(6 * phi));
}

char bandLetter(double lat) {
    static const char bands[] = "CDEFGHJKLMNPQRSTUVWXX";
    if (lat < -80 || lat > 84) return 0;
    int i = (int)floor((lat + 80) / 8);
    if (i > 20) i = 20;
    return bands[i];
}

int zoneFor(double lat, double lon) {
    if (lon >= 180) lon -= 360;
    int zone = (int)floor((lon + 180) / 6) + 1;
    if (lat >= 56 && lat < 64 && lon >= 3 && lon < 12) zone = 32;   // Norway
    if (lat >= 72 && lat < 84) {                                     // Svalbard
        if (lon >= 0 && lon < 9) zone = 31;
        else if (lon >= 9 && lon < 21) zone = 33;
        else if (lon >= 21 && lon < 33) zone = 35;
        else if (lon >= 33 && lon < 42) zone = 37;
    }
    return zone;
}

}  // namespace

Utm toUtmInZone(double lat, double lon, int zone) {
    Utm u;
    u.zone = zone;
    u.band = bandLetter(lat);
    u.north = lat >= 0;
    const double phi = lat * DEG;
    const double lam0 = ((zone - 1) * 6 - 180 + 3) * DEG;
    const double s = sin(phi), c = cos(phi), t = tan(phi);
    const double N = A / sqrt(1 - E2 * s * s);
    const double T = t * t;
    const double C = EP2 * c * c;
    const double Aa = c * (lon * DEG - lam0);
    const double M = meridianArc(phi);
    u.easting = K0 * N * (Aa + (1 - T + C) * pow(Aa, 3) / 6 +
                          (5 - 18 * T + T * T + 72 * C - 58 * EP2) * pow(Aa, 5) / 120) +
                500000.0;
    u.northing = K0 * (M + N * t * (Aa * Aa / 2 +
                                    (5 - T + 9 * C + 4 * C * C) * pow(Aa, 4) / 24 +
                                    (61 - 58 * T + T * T + 600 * C - 330 * EP2) *
                                        pow(Aa, 6) / 720));
    if (!u.north) u.northing += 10000000.0;
    return u;
}

Utm toUtm(double lat, double lon) { return toUtmInZone(lat, lon, zoneFor(lat, lon)); }

void fromUtm(int zone, bool north, double easting, double northing,
             double& lat, double& lon) {
    const double x = easting - 500000.0;
    const double y = north ? northing : northing - 10000000.0;
    const double M = y / K0;
    const double e4 = E2 * E2, e6 = e4 * E2;
    const double mu = M / (A * (1 - E2 / 4 - 3 * e4 / 64 - 5 * e6 / 256));
    const double e1 = (1 - sqrt(1 - E2)) / (1 + sqrt(1 - E2));
    const double phi1 = mu + (3 * e1 / 2 - 27 * pow(e1, 3) / 32) * sin(2 * mu) +
                        (21 * e1 * e1 / 16 - 55 * pow(e1, 4) / 32) * sin(4 * mu) +
                        (151 * pow(e1, 3) / 96) * sin(6 * mu) +
                        (1097 * pow(e1, 4) / 512) * sin(8 * mu);
    const double s = sin(phi1), c = cos(phi1), t = tan(phi1);
    const double C1 = EP2 * c * c;
    const double T1 = t * t;
    const double N1 = A / sqrt(1 - E2 * s * s);
    const double R1 = A * (1 - E2) / pow(1 - E2 * s * s, 1.5);
    const double D = x / (N1 * K0);
    const double phi = phi1 - (N1 * t / R1) *
        (D * D / 2 - (5 + 3 * T1 + 10 * C1 - 4 * C1 * C1 - 9 * EP2) * pow(D, 4) / 24 +
         (61 + 90 * T1 + 298 * C1 + 45 * T1 * T1 - 252 * EP2 - 3 * C1 * C1) * pow(D, 6) / 720);
    const double lam = (D - (1 + 2 * T1 + C1) * pow(D, 3) / 6 +
                        (5 - 2 * C1 + 28 * T1 - 3 * C1 * C1 + 8 * EP2 + 24 * T1 * T1) *
                            pow(D, 5) / 120) / c;
    lat = phi / DEG;
    lon = ((zone - 1) * 6 - 180 + 3) + lam / DEG;
}

void mgrs(double lat, double lon, char* out, size_t n) {
    if (n) out[0] = 0;
    if (lat < -80 || lat > 84) return;
    const Utm u = toUtm(lat, lon);
    static const char* colSets[3] = {"ABCDEFGH", "JKLMNPQR", "STUVWXYZ"};
    static const char rows[] = "ABCDEFGHJKLMNPQRSTUV";
    const int set = (u.zone - 1) % 3;
    int col = (int)floor(u.easting / 100000.0) - 1;
    if (col < 0) col = 0;
    if (col > 7) col = 7;
    int row = (int)floor(fmod(u.northing, 2000000.0) / 100000.0);
    if (u.zone % 2 == 0) row += 5;   // even zones start the row letters at F
    row %= 20;
    const long e = (long)floor(fmod(u.easting, 100000.0));
    const long nn = (long)floor(fmod(u.northing, 100000.0));
    snprintf(out, n, "%d%c %c%c %05ld %05ld", u.zone, u.band, colSets[set][col],
             rows[row], e, nn);
}

bool parseMgrs(const char* text, double& lat, double& lon) {
    char c[40];
    size_t k = 0;
    for (const char* p = text; *p && k < sizeof(c) - 1; ++p)
        if (*p != ' ' && *p != '-') c[k++] = (char)toupper((unsigned char)*p);
    c[k] = 0;
    size_t i = 0;
    int zone = 0;
    while (i < k && isdigit((unsigned char)c[i]) && i < 2) zone = zone * 10 + (c[i++] - '0');
    if (zone < 1 || zone > 60 || i + 3 > k) return false;
    const char band = c[i++];
    static const char bands[] = "CDEFGHJKLMNPQRSTUVWX";
    const char* bp = strchr(bands, band);
    if (!bp) return false;
    const char colL = c[i++], rowL = c[i++];
    static const char* colSets[3] = {"ABCDEFGH", "JKLMNPQR", "STUVWXYZ"};
    static const char rows[] = "ABCDEFGHJKLMNPQRSTUV";
    const char* cp = strchr(colSets[(zone - 1) % 3], colL);
    const char* rp = strchr(rows, rowL);
    if (!cp || !rp || !colL || !rowL) return false;
    const size_t nd = k - i;
    if (nd % 2 || nd > 10) return false;
    for (size_t j = i; j < k; ++j) if (!isdigit((unsigned char)c[j])) return false;
    const int half = (int)nd / 2;
    double e = 0, n = 0, unit = 100000;
    for (int j = 0; j < half; ++j) {
        unit /= 10;
        e += (c[i + j] - '0') * unit;
        n += (c[i + half + j] - '0') * unit;
    }
    e += unit / 2;   // centre of the named square
    n += unit / 2;
    const double easting = ((cp - colSets[(zone - 1) % 3]) + 1) * 100000.0 + e;
    int row = (int)(rp - rows);
    if (zone % 2 == 0) row = (row + 15) % 20;    // even zones start at F
    double northing = row * 100000.0 + n;
    // The band fixes which 2000 km cycle: the latitude band's southern edge.
    const int bi = (int)(bp - bands);
    const double bandLat = -80.0 + bi * 8.0;
    const bool north = bandLat >= 0;
    const Utm edge = toUtmInZone(bandLat, zone * 6.0 - 183.0, zone);
    double edgeN = edge.northing;
    while (northing < edgeN - 100000.0) northing += 2000000.0;
    fromUtm(zone, north, easting, northing, lat, lon);
    return true;
}

void bearingDistance(double lat1, double lon1, double lat2, double lon2,
                     double& bearingDeg, double& distM) {
    const double d2r = M_PI / 180.0;
    const double p1 = lat1 * d2r, p2 = lat2 * d2r, dl = (lon2 - lon1) * d2r;
    const double y = sin(dl) * cos(p2);
    const double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    bearingDeg = fmod(atan2(y, x) / d2r + 360.0, 360.0);
    const double a = sin((p2 - p1) / 2) * sin((p2 - p1) / 2) +
                     cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    distM = 2 * 6371008.8 * asin(sqrt(a < 1 ? a : 1));
}

void mils(float deg, int system, char* out, size_t n) {
    float d = fmodf(deg, 360.0f);
    if (d < 0) d += 360.0f;
    if (system == 6400) {
        int m = (int)lroundf(d / 360.0f * 6400.0f) % 6400;
        snprintf(out, n, "%04d", m);
    } else {
        int m = (int)lroundf(d / 360.0f * 6000.0f) % 6000;
        snprintf(out, n, "%02d-%02d", m / 100, m % 100);
    }
}

}  // namespace milgeo
