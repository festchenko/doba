// DOBA: topographic base layer — see topo_layer.h.
#include "topo_layer.h"
#include "doba_psram.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "epd_compat.h"
#include "map_view.h"
#include "sd_bus.h"

#ifdef OTP_SIM
#include <zlib.h>
#else
#include "rom/miniz.h"   // ROM tinfl (ESP32-S3 has it in mask ROM)
#endif

namespace topo {
namespace {

constexpr int TILE = 256;
constexpr int TILE_BYTES = TILE * TILE / 2;   // 4bpp, high nibble = left px
constexpr int CELL_Z = 8;
constexpr int MAX_Z = 17;                     // 17 only where a detail pack exists
constexpr int DETAIL_Z = 17;
constexpr int CACHE_TILES = 32;               // 32 x 32 KB = 1 MB of PSRAM
constexpr int HEADER = 32;
constexpr int ENTRY = 16;
constexpr uint8_t FLAG_SOLID = 1;
constexpr uint16_t PACK_RELATIVE = 1;   // header flags: x, y relative to the cell
constexpr int SCREEN_W = 540, SCREEN_H = 960;  // portrait
constexpr int NATIVE_W = 960, NATIVE_ROW = NATIVE_W / 2;
const char kMagic[8] = {'D', 'O', 'B', 'A', 'T', 'P', '1', 0};
const char* kRoot = "/doba/map";

struct Slot {
    uint8_t z = 0;
    uint32_t x = 0, y = 0;
    bool valid = false;
    bool present = false;
    bool solid = false;
    uint8_t fill = 15;
    uint32_t lastUse = 0;
    uint8_t* px = nullptr;       // TILE_BYTES, PSRAM
};

Slot g_slots[CACHE_TILES];
uint32_t g_tick = 0;
uint8_t* g_zbuf = nullptr;       // compressed blob scratch
uint32_t g_zbufLen = 0;
Stats g_stats = {0, 0, 0, 0};

// covers(): one entry per recently asked zoom-8 cell.
struct CellSeen { int32_t x = -1, y = -1; bool have = false; int8_t detail = -1; };
CellSeen g_cells[8];
int g_cellNext = 0;

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void packPath(int z, int x, int y, char* out, size_t n) {
    if (z <= CELL_Z) {
        snprintf(out, n, "%s/base.dtp", kRoot);
    } else {
        const int s = z - CELL_Z;
        // detail tiles (z17) live in a pack of their own beside the cell's
        snprintf(out, n, z >= DETAIL_Z ? "%s/8/%d/%d.17.dtp" : "%s/8/%d/%d.dtp", kRoot, x >> s, y >> s);
    }
}

bool inflate(const uint8_t* src, uint32_t len, uint8_t* dst) {
#ifdef OTP_SIM
    uLongf out = TILE_BYTES;
    return uncompress(dst, &out, src, len) == Z_OK && out == TILE_BYTES;
#else
    static tinfl_decompressor* d = nullptr;   // ~11 KB: keep it off the task stack
    if (!d) d = (tinfl_decompressor*)heap_caps_malloc(sizeof(tinfl_decompressor),
                                                      MALLOC_CAP_8BIT);
    if (!d) return false;
    tinfl_init(d);
    size_t inLen = len, outLen = TILE_BYTES;
    tinfl_status st = tinfl_decompress(
        d, src, &inLen, dst, dst, &outLen,
        TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    return st == TINFL_STATUS_DONE && outLen == TILE_BYTES;
#endif
}

// Finds (z,x,y) in a pack and fills the slot. false = no such tile.
bool readTile(int z, int x, int y, Slot& s) {
    char path[64];
    packPath(z, x, y, path, sizeof path);
    sdLock();
    File f = SD.open(path, FILE_READ);
    if (!f) { sdUnlock(); return false; }
    uint8_t hdr[HEADER];
    bool ok = f.read(hdr, HEADER) == HEADER && memcmp(hdr, kMagic, 8) == 0;
    const uint32_t count = ok ? rd32(hdr + 24) : 0;
    // Detail packs keep x, y relative to the cell (absolute z17 x, y overflow u16).
    if (ok && (rd16(hdr + 14) & PACK_RELATIVE)) {
        const int sh = z - CELL_Z;
        x -= (int)(rd32(hdr + 16) << sh);
        y -= (int)(rd32(hdr + 20) << sh);
    }
    // Binary search over the on-card index, sorted by (z, x, y).
    const uint64_t want = (uint64_t)z << 32 | (uint64_t)(uint16_t)x << 16 | (uint64_t)(uint16_t)y;
    uint32_t lo = 0, hi = count;
    uint8_t e[ENTRY];
    bool found = false;
    while (ok && lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (!f.seek(HEADER + mid * ENTRY) || f.read(e, ENTRY) != ENTRY) { ok = false; break; }
        const uint64_t key = (uint64_t)e[0] << 32 | (uint64_t)rd16(e + 2) << 16 | rd16(e + 4);
        if (key == want) { found = true; break; }
        if (key < want) lo = mid + 1; else hi = mid;
    }
    if (!found) { f.close(); sdUnlock(); return false; }
    const uint8_t flags = e[1];
    const uint16_t fill = rd16(e + 6);
    const uint32_t off = rd32(e + 8), len = rd32(e + 12);
    if (flags & FLAG_SOLID) {
        f.close();
        sdUnlock();
        s.solid = true;
        s.fill = (uint8_t)(fill & 0x0F);
        return true;
    }
    if (len > g_zbufLen) {
        if (g_zbuf) heap_caps_free(g_zbuf);
        g_zbufLen = len < 48 * 1024 ? 48 * 1024 : len;
        g_zbuf = (uint8_t*)heap_caps_malloc(g_zbufLen, MALLOC_CAP_SPIRAM);
        if (!g_zbuf) { g_zbufLen = 0; f.close(); sdUnlock(); return false; }
    }
    ok = f.seek(off) && f.read(g_zbuf, len) == len;
    f.close();
    sdUnlock();
    if (!ok) return false;
    if (!s.px) s.px = (uint8_t*)heap_caps_malloc(TILE_BYTES, MALLOC_CAP_SPIRAM);
    if (!s.px || !inflate(g_zbuf, len, s.px)) return false;
    s.solid = false;
    ++g_stats.decoded;
    return true;
}

Slot* tile(int z, int x, int y) {
    ++g_tick;
    Slot* victim = nullptr;
    for (Slot& s : g_slots) {
        if (s.valid && s.z == z && s.x == x && s.y == y) {
            s.lastUse = g_tick;
            ++g_stats.hits;
            return &s;
        }
        if (!s.valid) {
            if (!victim || victim->valid) victim = &s;      // a free slot wins
        } else if (!victim || (victim->valid && s.lastUse < victim->lastUse)) {
            victim = &s;                                    // else the oldest
        }
    }
    Slot& s = *victim;
    s.valid = true;
    s.z = (uint8_t)z; s.x = (uint32_t)x; s.y = (uint32_t)y;
    s.lastUse = g_tick;
    s.present = readTile(z, x, y, s);
    if (!s.present) ++g_stats.missing;
    return &s;
}

// Portrait UI coordinates -> native 960x540 4bpp fb, for the rotation the UI
// runs in (the same mapping as epd_compat.cpp rotateToNative).
bool g_inverted = false;
inline void putPortrait(uint8_t* fb, int x, int y, uint8_t level) {
    int nx, ny;
    if (g_inverted) { nx = y; ny = 539 - x; }               // EPD_ROT_INVERTED_PORTRAIT
    else            { nx = NATIVE_W - 1 - y; ny = x; }      // EPD_ROT_PORTRAIT
    uint8_t* p = &fb[(size_t)ny * NATIVE_ROW + (nx >> 1)];
    if (nx & 1) *p = (uint8_t)((*p & 0x0F) | (level << 4));
    else        *p = (uint8_t)((*p & 0xF0) | level);
}

}  // namespace

float nativeMpp(int z, double lat) {
    return (float)(156543.03392 * cos(lat * M_PI / 180.0) / (double)(1u << z));
}

int zoomFor(double lat, float mpp) {
    if (mpp <= 0) return MAX_Z;
    const double z = log2(156543.03392 * cos(lat * M_PI / 180.0) / mpp);
    int zi = (int)lround(z);
    if (zi < 0) zi = 0;
    if (zi > MAX_Z) zi = MAX_Z;
    return zi;
}

namespace {
void cellOf(double lat, double lon, int& cx, int& cy) {
    const double n = 1 << CELL_Z;
    cx = (int)floor((lon + 180.0) / 360.0 * n);
    const double lr = lat * M_PI / 180.0;
    cy = (int)floor((1.0 - asinh(tan(lr)) / M_PI) / 2.0 * n);
}
}  // namespace

int maxZoomAt(double lat, double lon) {
    int cx, cy;
    cellOf(lat, lon, cx, cy);
    if (!covers(lat, lon)) return 16;
    for (CellSeen& c : g_cells) {
        if (c.x != cx || c.y != cy) continue;
        if (c.detail < 0) {
            char path[64];
            snprintf(path, sizeof path, "%s/8/%d/%d.17.dtp", kRoot, cx, cy);
            sdLock();
            c.detail = SD.exists(path) ? 1 : 0;
            sdUnlock();
        }
        return c.detail ? DETAIL_Z : 16;
    }
    return 16;
}

bool covers(double lat, double lon) {
    int cx, cy;
    cellOf(lat, lon, cx, cy);
    for (const CellSeen& c : g_cells)
        if (c.x == cx && c.y == cy) return c.have;
    char path[64];
    snprintf(path, sizeof path, "%s/8/%d/%d.dtp", kRoot, cx, cy);
    sdLock();
    const bool have = SD.exists(path);
    sdUnlock();
    if (have) ++g_stats.packsOpened;
    g_cells[g_cellNext] = {cx, cy, have, -1};
    g_cellNext = (g_cellNext + 1) % 8;
    return have;
}

int drawBase(const MapScreenData& map, uint8_t* fb) {
    g_inverted = epd_get_rotation() == EPD_ROT_INVERTED_PORTRAIT;
    const double lat = map.centerLat, lon = map.centerLon;
    int z = zoomFor(lat, map.metersPerPixel);
    if (z > 16 && maxZoomAt(lat, lon) < z) z = 16;   // no detail pack here: enlarge z16
    const double world = (double)TILE * (double)(1u << z);
    const double lr = lat * M_PI / 180.0;
    const double rcx = (lon + 180.0) / 360.0 * world;
    const double rcy = (1.0 - asinh(tan(lr)) / M_PI) / 2.0 * world;
    // Screen px -> world px at zoom z (1.0 when mpp was snapped to the zoom).
    const double scale = map.metersPerPixel / nativeMpp(z, lat);
    const int maxT = (1 << z) - 1;

    static PsArr<int32_t, SCREEN_W> colW;
    for (int x = 0; x < SCREEN_W; ++x)
        colW[x] = (int32_t)floor(rcx + (x - map.riderX) * scale);

    const uint32_t missingBefore = g_stats.missing;
    for (int y = 0; y < SCREEN_H; ++y) {
        const int32_t wy = (int32_t)floor(rcy + (y - map.riderY) * scale);
        const int ty = wy >> 8, py = wy & 0xFF;
        int x = 0;
        while (x < SCREEN_W) {
            const int tx = colW[x] >> 8;
            int run = x;
            while (run < SCREEN_W && (colW[run] >> 8) == tx) ++run;
            if (ty < 0 || ty > maxT || tx < 0 || tx > maxT) {
                for (; x < run; ++x) putPortrait(fb, x, y, 15);
                continue;
            }
            Slot* s = tile(z, tx, ty);
            if (!s->present) {
                for (; x < run; ++x) putPortrait(fb, x, y, 15);
            } else if (s->solid) {
                for (; x < run; ++x) putPortrait(fb, x, y, s->fill);
            } else {
                const uint8_t* row = s->px + py * (TILE / 2);
                for (; x < run; ++x) {
                    const int px = colW[x] & 0xFF;
                    const uint8_t b = row[px >> 1];
                    putPortrait(fb, x, y, (px & 1) ? (b & 0x0F) : (b >> 4));
                }
            }
        }
    }
    return (int)(g_stats.missing - missingBefore);
}

void reset() {
    for (Slot& s : g_slots) s.valid = false;
    for (CellSeen& c : g_cells) c = CellSeen();
}

Stats stats() { return g_stats; }

}  // namespace topo
