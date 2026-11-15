#pragma once

// Map screen renderer: the topo tiles (topo_layer.h) as the base layer, then
// the grid, places, points, other riders, the position and the chrome.
//
// Same host/device split as ui_render.h: pure pixels, no hardware.

#include <cstdint>

struct RideState;

// Another rider on one of our private mesh channels, where they last said they
// were. Off-screen ones are kept (offscreen = true, x/y pinned to the map's
// edge on the line from the rider) so the map can point the way to them.
struct MapPeer {
    int16_t x, y;            // screen px
    bool    stale;           // older than MESH_MAP_FRESH_MS: dashed ring
    bool    offscreen;       // pinned to the edge, pointing their way
    char    initials[3];     // first two letters of the name, in the ring
    char    label[16];       // the distance, when off screen
};

struct MapScreenData {
    // Where the frame is centred, for the frame log (the sim tests wait for
    // the frame drawn at a position, not just the next one).
    double centerLat = 0, centerLon = 0;

    // Riders sharing their position on a private mesh channel.
    const MapPeer* peers = nullptr;
    int peerCount = 0;

    int riderX, riderY;    // the position marker, screen px
    float headingDeg;      // 0 = up/north, clockwise

    float metersPerPixel;  // for the scale bar

    // Screen direction of true north (0 = up; track-up sets -heading).
    float northDeg;
    bool trackUp;

    // A message popup is drawn over the top of the map. The compass sits in
    // the same place, so it moves below the popup rather than under it.
    bool navBannerVisible = false;
    // DOBA: the map was dragged off the position (a CENTRE button brings it back),
    // and whether the zoom buttons are at their ends (drawn grey there).
    bool panned = false, zoomInEnd = false, zoomOutEnd = false;
    // DOBA: the header band over the map: where this is, and the scale.
    char place[32] = "";
    char placeSub[32] = "";

    // A topo pack on the card covers the view's centre. When false the base
    // layer is blank paper and the screen says why there is no map.
    bool topo = false;
};

// Compass touch target (tap toggles north-up / track-up)
struct MapCompassZone {
    int cx, cy, r;
};
extern const MapCompassZone kMapCompass;

// Screen centre-Y of the compass. It moves below the turn-by-turn banner while
// navigating, so the hit test in ui_dashboard MUST ask rather than assume — the
// drawn position and the tap target drifting apart is what made the north-up
// button unreachable during navigation.
int mapCompassCy(bool navBannerVisible);

// Touch targets (zoom buttons on the right edge of the map area)
struct MapTouchZones {
    int zoomX, zoomInY, zoomOutY, size;
    int layersY;   // the MAP LAYERS button, stacked above zoom-in
};
extern const MapTouchZones kMapZoom;
// DOBA: CENTRE, shown while the map is dragged away: above the zoom column.
int mapCentreY();

// One zoom button, on its own. Re-projecting the map after a zoom tap takes the
// best part of a second (tiles come off the SD card), and until this existed
// nothing on the panel moved in that time — so a tap that HAD registered was
// indistinguishable from one that had missed, and riders tapped again. Drawing
// the pressed state and painting just this rectangle answers the touch straight
// away; the full redraw then restores it.
void ui_map_draw_zoom_button(bool zoomIn, bool pressed, uint8_t* fb);

// `baseDrawn`: fb already holds this view's base layer (see ui_render_map_base).
// `onBase`: if set, called with fb as soon as the base layer is on it, to keep a copy.
void ui_render_map(const MapScreenData& map, const RideState& s, uint8_t* fb,
                   bool baseDrawn = false, void (*onBase)(const uint8_t* fb) = nullptr);

// The slow part of a map frame on its own: the topo tiles, onto a white fb, with
// no grid, places, rider or chrome, none of which depend on it.
void ui_render_map_base(const MapScreenData& map, uint8_t* fb);

// Just the map (tiles + rider), full-screen, no chrome. Used as the
// backdrop behind the powered-off screen.
void ui_render_map_features(const MapScreenData& map, const RideState& s,
                            uint8_t* fb);
