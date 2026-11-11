#include "doba_i18n.h"
#include "doba_pois.h"

#include <Arduino.h>
#include <SD.h>
#include <cmath>
#include <cstring>
#include <esp_heap_caps.h>

#include "milgeo.h"
#include "sd_bus.h"

namespace doba_pois {
namespace {

const char* kPath = "/doba/map/pois.dgp";
constexpr float CELL = 0.25f;
constexpr int REC = 44;
constexpr int RINGS = 1;   // the 3x3 cells round you: about 40 km each way
struct Cell { int16_t lat, lon; uint32_t first, n; };
Cell* g_cells = nullptr;
uint32_t g_cellCount = 0, g_count = 0;
uint32_t g_dataOff = 0;
int g_state = 0;   // 0 not tried, 1 loaded, -1 none

const char* const kNames[KINDS] = {
    "", "Hospital", "Doctor", "Pharmacy", "Water", "Food shop", "Farm", "Shelter", "Bunker",
    "Fuel", "Police", "Fire station", "Hunting & arms", "Hardware", "Outdoor shop", "Campsite",
    "Assembly point", "Underground parking"};
const char* const kHints[KINDS] = {
    "", "Emergency care, surgery", "Clinics and GPs", "Medicines, bandages",
    "Wells, springs, taps", "Supermarkets, bakers, butchers", "Farm shops and farmyards",
    "Huts and shelters", "Bunkers and shelters underground", "Petrol and diesel", "Police stations",
    "Fire stations", "Gun and hunting shops", "Tools, timber, building", "Camping and outdoor gear",
    "Camp sites", "Where people gather in an emergency",
    "Car parks below ground: cover overhead"};

bool load() {
    if (g_state) return g_state > 0;
    g_state = -1;
    sdLock();
    File f = SD.open(kPath, FILE_READ);
    uint8_t h[12];
    if (f && f.read(h, 12) == 12 && !memcmp(h, "DGP1", 4)) {
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
    if (g_state > 0) Serial.printf("[pois] %lu places in %lu cells\n", (unsigned long)g_count, (unsigned long)g_cellCount);
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

// Every record in the cells round lat/lon, through fn(Poi&).
template <typename F> void scan(double lat, double lon, F fn) {
    if (!load()) return;
    const int la = (int)floor(lat / CELL), lo = (int)floor(lon / CELL);
    sdLock();
    File f = SD.open(kPath, FILE_READ);
    static uint8_t buf[REC * 64];
    for (int dy = -RINGS; f && dy <= RINGS; ++dy)
        for (int dx = -RINGS; dx <= RINGS; ++dx) {
            const Cell* c = findCell(la + dy, lo + dx);
            if (!c) continue;
            f.seek(g_dataOff + (size_t)c->first * REC);
            uint32_t left = c->n;
            while (left) {
                const uint32_t k = left < 64 ? left : 64;
                if (f.read(buf, k * REC) != (int)(k * REC)) { left = 0; break; }
                for (uint32_t i = 0; i < k; ++i) {
                    const uint8_t* r = buf + i * REC;
                    float plat, plon;
                    memcpy(&plat, r, 4);
                    memcpy(&plon, r + 4, 4);
                    Poi p;
                    p.lat = plat;
                    p.lon = plon;
                    p.kind = r[8];
                    const size_t len = strnlen((const char*)r + 10, 34);
                    memcpy(p.name, r + 10, len);
                    p.name[len] = 0;
                    p.dist = -1;
                    fn(p);
                }
                left -= k;
            }
        }
    if (f) f.close();
    sdUnlock();
}

}  // namespace

const char* kindName(int k) { return k > 0 && k < KINDS ? TR(kNames[k]) : ""; }
const char* kindHint(int k) { return k > 0 && k < KINDS ? TR(kHints[k]) : ""; }
bool available() { return load(); }

int nearest(double lat, double lon, int kind, Poi* out, int n) {
    int have = 0;
    scan(lat, lon, [&](Poi& p) {
        if (kind && p.kind != kind) return;
        // a cheap filter before the real distance
        const double dlat = (p.lat - lat) * 111.32, dlon = (p.lon - lon) * 111.32 * cos(lat * M_PI / 180.0);
        const double approx = dlat * dlat + dlon * dlon;
        if (have == n && approx > (out[n - 1].dist / 1000.0 + 1) * (out[n - 1].dist / 1000.0 + 1)) return;
        milgeo::bearingDistance(lat, lon, p.lat, p.lon, p.bearing, p.dist);
        const bool full = have == n;
        if (full && p.dist >= out[n - 1].dist) return;
        int i = full ? n - 1 : have++;
        while (i > 0 && out[i - 1].dist > p.dist) { out[i] = out[i - 1]; --i; }
        out[i] = p;
    });
    return have;
}

void nearestOfEach(double lat, double lon, Poi* out) {
    for (int k = 0; k < KINDS; ++k) { out[k].dist = -1; out[k].kind = (uint8_t)k; out[k].name[0] = 0; }
    scan(lat, lon, [&](Poi& p) {
        if (p.kind <= 0 || p.kind >= KINDS) return;
        milgeo::bearingDistance(lat, lon, p.lat, p.lon, p.bearing, p.dist);
        if (out[p.kind].dist < 0 || p.dist < out[p.kind].dist) out[p.kind] = p;
    });
}

}  // namespace doba_pois
