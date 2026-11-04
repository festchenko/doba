#pragma once

// DOBA: military grid maths — UTM (WGS84), MGRS strings and mils.
// Plain C++, no Arduino: tested on the host by tools/milgeo_test.

#include <cstddef>

namespace milgeo {

struct Utm {
    int zone = 0;        // 1..60
    char band = 0;       // MGRS latitude band C..X
    bool north = true;
    double easting = 0;  // m
    double northing = 0; // m (southern hemisphere: + 10 000 km false northing)
};

// Latitude/longitude (degrees, WGS84) to UTM, with the Norway (32V) and
// Svalbard (31X..37X) zone exceptions, as MGRS uses them.
Utm toUtm(double lat, double lon);

// The same, but forced into `zone` (grid lines across a zone border are drawn
// in the rider's zone, as a map sheet is).
Utm toUtmInZone(double lat, double lon, int zone);

// UTM back to latitude/longitude.
void fromUtm(int zone, bool north, double easting, double northing,
             double& lat, double& lon);

// MGRS, 1 m precision: "32U LC 12345 67890". `out` needs 24 bytes.
// Empty string outside -80..84 latitude (UPS is not supported).
void mgrs(double lat, double lon, char* out, size_t n);

// Degrees to mils. system = 6000 ("05-33", the Soviet/Ukrainian 60-00 dial)
// or 6400 ("0533", NATO). `out` needs 8 bytes.
void mils(float deg, int system, char* out, size_t n);

}  // namespace milgeo
