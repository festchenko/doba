#pragma once
// DOBA: "where am I" — the nearest town or village from /doba/map/places.dgz
// (maps/gazetteer.py, built from the OSM extracts the maps came from).
#include <cstddef>

namespace doba_places {

// The place to name for this spot: a town nearby beats a hamlet next door.
// false: no file, or nothing within 25 km.
bool nearest(double lat, double lon, char* name, size_t n, double& distM, double& bearingDeg);

}  // namespace doba_places
