// DOBA: decorative contour lines for the background of Doba's own screens.
//
// A made-up landscape (a few hills, hollows and a long swell), contoured like
// a topographic sheet: thin lines every step, every fifth one heavier. Drawn
// AFTER the screen's content, only on white pixels and with a clear margin
// round anything already drawn, so the lines read as paper texture and never
// touch text — the halo a map gives its labels.
#pragma once
#include <stdint.h>
#include "epd_compat.h"

namespace topo_bg {

// Light grey on the panel's 4-level LUT (8-12 = light). Check with `greytest`
// on the real glass; if light greys vanish there, drop to 0x77.
constexpr uint8_t kLine  = 0xBB;
constexpr uint8_t kIndex = 0xAA;   // index contours: same tone, 2 px wide
constexpr int     kHalo  = 3;      // px kept clear round drawn content
constexpr uint8_t kLineDark = 0x44;   // the same lines on black (the SOS tile)

// Contour the area `r` of the screen. `variant` picks another landscape
// (screens get different ones so they don't look stamped).
void draw(const EpdRect& r, uint8_t* fb, int variant = 0);
// The same on a black area: dark grey lines, kept clear of white content.
void drawInverted(const EpdRect& r, uint8_t* fb, int variant = 0);

}  // namespace topo_bg
