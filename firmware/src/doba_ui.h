#pragma once

// DOBA: the screens Doba adds to the firmware — the main screen (launcher),
// comms (mesh messages and compose) and the library (search and article).
// ui_dashboard owns navigation; it asks these for pixels and for where a tap
// or the Home key leads.

#include <cstdint>

struct RideState;

namespace doba_ui {

enum Page : uint8_t {
    HOME, COMMS, COMPOSE, LIBRARY, ARTICLE,
    SETUP,       // settings without the phone
    SETUP_NAME,  // the node's name, typed
    POINTS,      // own position + waypoint list
    POINT,       // one waypoint: azimuth, distance, GO TO / DELETE
    POINT_NEW,   // a waypoint typed in as MGRS
    MAPOPTS,     // map options: orientation, grid, points
    SOS,         // a distress call over the mesh, repeated
    SUN,         // sunrise, sunset, twilight, moon
    MORE,        // the rest of the settings: USB drive, GPS, card, about
    BATTERY,     // what drains it, and how long it will last
};

// Where a tap leads. STAY = redraw this page.
enum Go : uint8_t {
    STAY, TO_HOME, TO_MAP, TO_POSITION, TO_LIBRARY, TO_ARTICLE, TO_COMMS,
    TO_COMPOSE,
    TO_SETUP, TO_SETUP_NAME, TO_POINTS, TO_POINT, TO_POINT_NEW, TO_MAPOPTS,
    TO_SOS, TO_SUN, TO_POWER, TO_MORE, TO_GPSDEBUG, TO_BATTERY,
};

// Called every pass of the UI loop: repeats an active SOS. True when the SOS
// page (or HOME's tile) needs a repaint.
bool tick(const RideState& s);
// The power-off screen around the farewell map: where Doba was switched off,
// tomorrow's sun, the charge, the owner card and an emergency crib. The map is
// already in fb; this keeps a band of it around the position (y 320..640).
void renderOffCard(const RideState& s, bool powerKey, uint8_t* fb);
bool sosActive();

// Show waypoint i on the POINT page next (after a long-press on the map).
void openPoint(int i);

// The map's orientation lives in ui_dashboard; MAPOPTS reads and sets it here.
bool mapTrackUp();
void setMapTrackUp(bool on);

// The status band's title for a page (nullptr: the clock line only).
const char* title(Page p);

void render(Page p, const RideState& s, uint8_t* fb);
Go tap(Page p, int x, int y, const RideState& s);
// How long the finger was down for the tap about to be handled (ms).
void setPressMs(uint32_t ms);
Go back(Page p);

}  // namespace doba_ui
