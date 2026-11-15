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
// The map is drawn north-up: a raster cannot be rotated cheaply.

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
// Returns the number of tiles that were missing (drawn white). Through the
// canvas (topo_layer.cpp): a view near the last one is a copy, not a redraw.
int drawBase(const MapScreenData& map, uint8_t* fb);

// With nothing else to do: draws one more block of the canvas round this view
// (the map half a screen beyond each edge), so a drag or a walk lands on map
// already drawn. False when there is nothing left to draw for it.
bool idleStep(const MapScreenData& map);
// While a finger drags the map: the view straight off the canvas as it stands,
// blocks not drawn yet left as paper, nothing read from the card. False when the
// view is off the canvas (the finger has gone beyond the margin).
bool drawQuick(const MapScreenData& map, uint8_t* fb);
// On a zoom tap: the last view shown, enlarged 2x (in) or shrunk 2x (out) about
// its centre, from the canvas, at once. The sharp frame follows.
bool drawZoomPreview(uint8_t* fb, bool zoomIn);
// This view is already drawn on the canvas: showing it is a copy.
bool viewReady(const MapScreenData& map);
// Console `topo canvas off`: every frame straight from the tiles (to compare).
void setCanvas(bool on);

// Forget open packs and cached tiles (the card was removed or rewritten).
void reset();
// Moves on with every reset(): a map layer drawn before it is out of date.
uint32_t generation();

// For the console / sim tests: tiles decoded, cache hits, packs opened.
struct Stats {
    uint32_t decoded, hits, missing, packsOpened;
    uint32_t blocks, blits, anchors;   // canvas: blocks drawn, views copied, re-anchors
};
Stats stats();

}  // namespace topo
