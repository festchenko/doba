#pragma once

// What the UI shows now (published by ui_dashboard every frame), for the
// diagnostics console and the simulator's status.
struct LiveView {
    unsigned char page = 0xFF;       // dashboard page on screen; 0xFF = none
    unsigned char pageCount = 0;
    unsigned char screen = 2;        // 0 dashboard page, 1 map, 2 anything else
};

namespace live_view {
void publish(const LiveView& v);
void read(LiveView& v);
}  // namespace live_view
