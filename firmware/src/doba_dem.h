#pragma once

// DOBA: ground height from the card — /doba/dem/N52E006.dem, one file per
// 1x1 degree, made by maps/dempack.py from Copernicus GLO-30 (format there).
// Bilinear between ~90 m samples; a few blocks of 20 KB cached in PSRAM.

namespace doba_dem {

// Ground height (m) at a point; false where the card has no file for it.
bool height(double lat, double lon, float& metres);

void rescan();   // the card changed

}  // namespace doba_dem
