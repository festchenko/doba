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
