// DOBA: host preview stand-ins for the card-backed topo layer.
#include "topo_layer.h"
#include <cmath>
namespace topo {
bool covers(double, double) { return false; }
float nativeMpp(int z, double lat) { return (float)(156543.03392 * cos(lat * M_PI / 180.0) / (double)(1u << z)); }
int zoomFor(double, float) { return 16; }
int drawBase(const MapScreenData&, uint8_t*) { return 0; }
void reset() {}
Stats stats() { return {0, 0, 0, 0}; }
}

// DOBA: no waypoints in the preview.
#include "doba_points.h"
namespace doba_points {
int count() { return 0; }
const Point* at(int) { return nullptr; }
int target() { return -1; }
void formatDistance(double, char* out, size_t n) { if (n) out[0] = 0; }
}
