#pragma once
#include <cstddef>

// DOBA: the military-style map overlay for the topo layer.
//  - UTM/MGRS kilometre grid over the map, labelled at the map edges with the
//    two-digit kilometre values, as on a military topographic sheet.
//  - The bottom strip becomes a position readout: MGRS (1 m), latitude and
//    longitude, GPS altitude, satellites, and direction of travel in mils.

#include <cstdint>

struct MapScreenData;
struct RideState;

namespace milspec {

// DOBA: where the cross sits on a dragged map (the view's middle, ui_dashboard).
constexpr int kCrossY = 430;
// A cross at the view's middle while the map is dragged off your position.
void drawCross(uint8_t* fb);

// Grid lines and their edge labels, over the base layer, under the chrome.
void drawGrid(const MapScreenData& map, uint8_t* fb);

// The readout strip, from ui::MAP_STRIP_TOP to the bottom of the panel.
void drawStrip(const MapScreenData& map, const RideState& s, uint8_t* fb);

// The active waypoint (doba_points): a dashed line from the rider to it and
// its mark, over the map, under the chrome.
void drawTarget(const MapScreenData& map, const RideState& s, uint8_t* fb);

// Map options kept in NVS: the MGRS grid, every waypoint on the map.
bool gridOn();
void setGridOn(bool on);
bool pointsOn();
void setPointsOn(bool on);

// The map as last drawn: screen point -> lat/lon (false before a first frame).
bool screenToLatLon(int x, int y, double& lat, double& lon);
// The middle and scale of the map as last drawn (false before a first frame).
bool lastView(double& lat, double& lon, float& mpp);

// Mils system: 6000 ("05-33", the 60-00 dial) or 6400 (NATO). Kept in NVS.
int milsSystem();
void setMilsSystem(int system);

// Places (doba_pois) on the map as small lettered tags, zoomed in. Kept in NVS.
bool poisOn();
void setPoisOn(bool on);
void drawPois(const MapScreenData& map, uint8_t* fb);
// The one- or two-letter tag a kind wears on the map and in the legend.
const char* poiCode(int kind);

// Measure: mark A at the cross; the strip then reads from A to the cross.
bool measureOn();
void measureToggleAtCross();   // set A at the cross, or clear it
void measureClear();
void drawMeasure(const MapScreenData& map, uint8_t* fb);

// The 100 km square (MGRS letters, "LC") of a point, "" if none.
void square(double lat, double lon, char* out, size_t n);
// Contour interval of the sheet at this zoom: normal and index (bold) line, m.
void contourInterval(double lat, float mpp, bool flat, float& every, float& index);

}  // namespace milspec
