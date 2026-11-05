#pragma once

// DOBA: waypoints. Kept on the card in /doba/points.txt, one per line:
//   name;latitude;longitude
// (plain text, so points can be typed in on a computer too). One point at a
// time can be the TARGET: the map draws a line to it and the strip shows its
// azimuth in mils and the distance.

#include <cstddef>
#include <cstdint>

namespace doba_points {

constexpr int MAX_POINTS = 64;
constexpr int NAME_LEN = 24;

struct Point {
    char name[NAME_LEN];
    double lat, lon;
};

void load();                         // from the card (lazily, on first use)
void rescan();                       // the card changed: read again on next use
int count();
const Point* at(int i);
int add(const char* name, double lat, double lon);   // index, or -1 when full
void remove(int i);

int target();                        // index, or -1
void setTarget(int i);               // -1 clears

// Bearing (deg, true) and distance (m) from (lat, lon) to point i.
bool fromHere(int i, double lat, double lon, double& bearingDeg, double& distM);

// "1.24 km" / "380 m"
void formatDistance(double m, char* out, size_t n);

}  // namespace doba_points
