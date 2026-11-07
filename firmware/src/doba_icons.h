#pragma once
// DOBA: schematic pictograms for the library and HOME, drawn from lines,
// circles and triangles — thick strokes that hold up on e-ink, one style.

#include <cstdint>

namespace doba_icons {

enum Icon : uint8_t {
    I_NONE,
    I_FIRSTAID,   // cross in a rounded square
    I_WATER,      // drop
    I_FIRE,       // flame
    I_SHELTER,    // tent
    I_FOOD,       // fish
    I_NAV,        // compass rose
    I_HAZARD,     // warning triangle
    I_WEATHER,    // cloud and bolt
    I_TOOLS,      // wrench
    I_KNOTS,      // rope loop
    I_COMMS,      // mast and waves
    I_BOOKS,      // open book
    I_SEARCH,     // magnifier
    I_BLEED,      // drop with a bar (stop bleeding)
    I_BREATH,     // lungs
    I_COLD,       // snowflake
    I_PLANT,      // leaf
    I_SHELF,      // books on a shelf
    I_FILES,      // folder
    I_GROW,       // sprout over the soil line
    I_POWER,      // battery with a bolt
    I_MAP,        // folded map with a pin
    I_POS,        // crosshair
    I_SOS,        // beacon light with rays
    I_SUN,        // sun on the horizon
    I_GEAR,       // cog
};

// Draw `icon` centred on (cx, cy), fitting a size x size box, in `ink` on `paper`.
void draw(Icon icon, int cx, int cy, int size, uint8_t ink, uint8_t paper, uint8_t* fb);

// "firstaid", "water"... (as written in sections.txt) -> Icon; I_BOOKS if unknown.
Icon byName(const char* name);

}  // namespace doba_icons
