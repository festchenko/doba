#pragma once

// DOBA: the screens Doba adds to the firmware — the main screen (launcher),
// comms (mesh messages and compose) and the library (search and article).
// ui_dashboard owns navigation; it asks these for pixels and for where a tap
// or the Home key leads.

#include <cstdint>

struct RideState;

namespace doba_ui {

enum Page : uint8_t { HOME, COMMS, COMPOSE, LIBRARY, ARTICLE };

// Where a tap leads. STAY = redraw this page.
enum Go : uint8_t {
    STAY, TO_HOME, TO_MAP, TO_POSITION, TO_LIBRARY, TO_ARTICLE, TO_COMMS,
    TO_COMPOSE, TO_TRACKS, TO_ROUTES, TO_SETTINGS, TO_RECORD,
};

void render(Page p, const RideState& s, uint8_t* fb);
Go tap(Page p, int x, int y, const RideState& s);
Go back(Page p);

}  // namespace doba_ui
