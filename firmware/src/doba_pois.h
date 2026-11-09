#pragma once
// DOBA: useful places nearby — hospitals, water, food, shelter, fuel... from
// /doba/map/pois.dgp (maps/pois.py, built from the OSM extracts).
#include <cstddef>
#include <cstdint>

namespace doba_pois {

constexpr int KINDS = 18;   // 1..17; 0 is unused
const char* kindName(int kind);   // "Hospital"
const char* kindHint(int kind);   // what it is good for, one short line

struct Poi { double lat, lon, dist, bearing; uint8_t kind; char name[36]; };

// Is there a file at all.
bool available();
// The places of one kind (0: every kind) nearest to lat/lon, nearest first,
// out to about 40 km. Returns how many were written to out (at most n).
int nearest(double lat, double lon, int kind, Poi* out, int n);
// For the overview: per kind, the nearest one (dist < 0: none in range).
void nearestOfEach(double lat, double lon, Poi* out /* KINDS entries */);

}  // namespace doba_pois
