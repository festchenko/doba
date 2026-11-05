#pragma once

// DOBA: the military-style map overlay for the topo layer.
//  - UTM/MGRS kilometre grid over the map, labelled at the map edges with the
//    two-digit kilometre values, as on a military topographic sheet.
//  - The bottom strip becomes a position readout: MGRS (1 m), latitude and
//    longitude, GPS altitude, satellites, and direction of travel in mils.

#include <cstdint>

struct MapScreenData;
struct RideState;

namespace milspec {

// Grid lines and their edge labels, over the base layer, under the chrome.
void drawGrid(const MapScreenData& map, uint8_t* fb);

// The readout strip, from ui::MAP_STRIP_TOP to the bottom of the panel.
void drawStrip(const MapScreenData& map, const RideState& s, uint8_t* fb);

// The active waypoint (doba_points): a dashed line from the rider to it and
// its mark, over the map, under the chrome.
void drawTarget(const MapScreenData& map, const RideState& s, uint8_t* fb);

// Mils system: 6000 ("05-33", the 60-00 dial) or 6400 (NATO). Kept in NVS.
int milsSystem();
void setMilsSystem(int system);

}  // namespace milspec
