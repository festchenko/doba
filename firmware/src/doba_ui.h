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
};

// Where a tap leads. STAY = redraw this page.
enum Go : uint8_t {
    STAY, TO_HOME, TO_MAP, TO_POSITION, TO_LIBRARY, TO_ARTICLE, TO_COMMS,
    TO_COMPOSE, TO_TRACKS, TO_ROUTES, TO_SETTINGS, TO_RECORD,
    TO_SETUP, TO_SETUP_NAME, TO_POINTS, TO_POINT, TO_POINT_NEW,
};

// The status band's title for a page (nullptr: the clock line only).
const char* title(Page p);

void render(Page p, const RideState& s, uint8_t* fb);
Go tap(Page p, int x, int y, const RideState& s);
Go back(Page p);

}  // namespace doba_ui
