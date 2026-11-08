#pragma once

// DOBA: the topographic base layer.
//
// Pre-rendered 4-bit grey map tiles (Web Mercator, 256 px) in Doba tile packs
// (.dtp) on the SD card, drawn 1:1 under the route, the rider and the chrome.
// The packs are built on a computer by maps/render.py; the format is described
// at the top of maps/tilepack.py and mirrored in topo_layer.cpp.
//
// Card layout:
//   /doba/map/base.dtp            zoom 0-8 for the whole region
//   /doba/map/8/<x>/<y>.dtp       zoom 9-16 for one zoom-8 cell
//   /doba/map/8/<x>/<y>.17.dtp    zoom 17 (detail, street names) where rendered
//
// When a pack covers the rider, the topo layer replaces the OpenTrailPaper
// vector roads entirely (they would only repeat what the topo tile shows),
// and the map is drawn north-up: a raster cannot be rotated cheaply.

#include <cstdint>

struct MapScreenData;

namespace topo {

// The zoom-8 cell pack for this position exists (cached per cell).
bool covers(double lat, double lon);

// Ground metres per screen pixel of zoom `z` at this latitude.
float nativeMpp(int z, double lat);

// The tile zoom closest to `mpp` at this latitude, clamped to 0..17.
int zoomFor(double lat, float mpp);

// The closest zoom drawn from real tiles here: 17 where a detail pack
// (8/<x>/<y>.17.dtp, street names) is on the card, else 16.
int maxZoomAt(double lat, double lon);

// Draws the topo tiles for map.centerLat/centerLon at map.metersPerPixel, with
// the centre at (map.riderX, map.riderY), over the whole 540x960 portrait fb.
// Returns the number of tiles that were missing (drawn white).
int drawBase(const MapScreenData& map, uint8_t* fb);

// Forget open packs and cached tiles (the card was removed or rewritten).
void reset();

// For the console / sim tests: tiles decoded, cache hits, packs opened.
struct Stats { uint32_t decoded, hits, missing, packsOpened; };
Stats stats();

}  // namespace topo
