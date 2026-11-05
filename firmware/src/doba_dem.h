#pragma once

// DOBA: ground height from the card — /doba/dem/N52E006.dem, one file per
// 1x1 degree, made by maps/dempack.py from Copernicus GLO-30 (format there).
// Bilinear between ~90 m samples; a few blocks of 20 KB cached in PSRAM.

namespace doba_dem {

// Ground height (m) at a point; false where the card has no file for it, or
// where the point is on water (then see depth()).
bool height(double lat, double lon, float& metres);

// Water depth (m, positive) where the card's file marks sea (maps/dempack.py
// --bathy, EMODnet; relative to lowest astronomical tide). Not for navigation.
bool depth(double lat, double lon, float& metres);

// Both in one: true if the card knows the point; water says which it is.
bool ground(double lat, double lon, float& value, bool& water);

void rescan();   // the card changed

}  // namespace doba_dem
