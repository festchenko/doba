#include "doba_places.h"

#include <Arduino.h>
#include <SD.h>
#include <cmath>
#include <cstring>
#include <esp_heap_caps.h>

#include "milgeo.h"
#include "sd_bus.h"

namespace doba_places {
namespace {

const char* kPath = "/doba/map/places.dgz";
constexpr float CELL = 0.25f;
constexpr int REC = 36;
struct Cell { int16_t lat, lon; uint32_t first, n; };
Cell* g_cells = nullptr;
uint32_t g_cellCount = 0, g_count = 0;
uint32_t g_dataOff = 0;
int g_state = 0;   // 0 not tried, 1 loaded, -1 none

bool load() {
    if (g_state) return g_state > 0;
    g_state = -1;
    sdLock();
    File f = SD.open(kPath, FILE_READ);
    uint8_t h[12];
    if (f && f.read(h, 12) == 12 && !memcmp(h, "DGZ1", 4)) {
        memcpy(&g_count, h + 4, 4);
        memcpy(&g_cellCount, h + 8, 4);
        g_cells = (Cell*)heap_caps_malloc(sizeof(Cell) * g_cellCount, MALLOC_CAP_SPIRAM);
        if (g_cells) {
            bool ok = true;
            for (uint32_t i = 0; ok && i < g_cellCount; ++i) {
                uint8_t c[12];
                ok = f.read(c, 12) == 12;
                memcpy(&g_cells[i].lat, c, 2);
                memcpy(&g_cells[i].lon, c + 2, 2);
                memcpy(&g_cells[i].first, c + 4, 4);
                memcpy(&g_cells[i].n, c + 8, 4);
            }
            g_dataOff = 12 + 12 * g_cellCount;
            if (ok) g_state = 1;
        }
    }
    if (f) f.close();
    sdUnlock();
    if (g_state > 0) Serial.printf("[places] %lu places in %lu cells\n", (unsigned long)g_count, (unsigned long)g_cellCount);
    return g_state > 0;
}

const Cell* findCell(int la, int lo) {
    uint32_t a = 0, b = g_cellCount;
    while (a < b) {
        const uint32_t m = (a + b) / 2;
        const Cell& c = g_cells[m];
        if (c.lat < la || (c.lat == la && c.lon < lo)) a = m + 1; else b = m;
    }
    return a < g_cellCount && g_cells[a].lat == la && g_cells[a].lon == lo ? &g_cells[a] : nullptr;
}

}  // namespace

bool nearest(double lat, double lon, char* name, size_t n, double& distM, double& bearingDeg) {
    if (!load()) return false;
    const int la = (int)floor(lat / CELL), lo = (int)floor(lon / CELL);
    double best = 1e18;
    bool found = false;
    sdLock();
    File f = SD.open(kPath, FILE_READ);
    for (int dy = -1; f && dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const Cell* c = findCell(la + dy, lo + dx);
            if (!c) continue;
            f.seek(g_dataOff + (size_t)c->first * REC);
            for (uint32_t i = 0; i < c->n; ++i) {
                uint8_t r[REC];
                if (f.read(r, REC) != REC) break;
                float plat, plon;
                memcpy(&plat, r, 4);
                memcpy(&plon, r + 4, 4);
                const int rank = r[8];
                double b, d;
                milgeo::bearingDistance(lat, lon, plat, plon, b, d);
                if (d > 25000) continue;
                // a city 6 km off names the spot before a hamlet 1 km off
                const double score = d / (1.0 + 0.9 * rank * rank);
                if (score < best) {
                    best = score;
                    found = true;
                    distM = d;
                    bearingDeg = b;
                    const size_t len = strnlen((const char*)r + 9, 27);
                    const size_t k = len < n - 1 ? len : n - 1;
                    memcpy(name, r + 9, k);
                    name[k] = 0;
                }
            }
        }
    if (f) f.close();
    sdUnlock();
    return found;
}

}  // namespace doba_places
