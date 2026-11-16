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
#include "sd_card.h"
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
Stats g_stats = {};

// covers(): one entry per recently asked zoom-8 cell.
struct CellSeen { int32_t x = -1, y = -1; bool have = false; int8_t detail = -1; };
CellSeen g_cells[8];
int g_cellNext = 0;

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// Black and white only: a grey is an ordered dither of ink and paper, laid on
// the world's pixels (so it moves with the map and never crawls). The panel
// lands a grey by darkening and lightening a pixel in turn, so every grey that
// moved (the buildings, the woods of a dragged map) blinked; ink and paper do
// not. Console `topo dither off` brings the greys back to compare.
bool g_dither = true;
inline uint8_t dith(uint8_t lvl, int32_t wx, int32_t wy) {
    // The panel's own four tones (maps/style.py): 0-7 dark and black, 8-12
    // light, 13-15 paper. Dark goes to ink whole: it is the fine work (contours,
    // the dots of a wood, the dashes of a marsh, hatching), one pixel wide, and a
    // dither would drop some of it for good. Light is the broad fills (water,
    // buildings): those are the ones dotted.
    if (!g_dither) return lvl;
    if (lvl <= 7) return 0;
    if (lvl >= 13) return 15;
    // light: one pixel in four, on the even world grid. A finer dither (Bayer)
    // was tried: its ink fell beside the one-pixel diagonals of a town's hatch
    // and the towns vanished; every line of this grid's slope meets it.
    return ((wx | wy) & 1) ? 15 : 0;
}
// A decoded tile in place; tiles start on multiples of 256, so tile pixel
// (x, y) dithers as world pixel (x, y) does.
void ditherTile(uint8_t* px) {
    for (int y = 0; y < TILE; ++y)
        for (int x = 0; x < TILE; x += 2) {
            uint8_t& b = px[y * (TILE / 2) + (x >> 1)];
            b = (uint8_t)(dith(b >> 4, x, y) << 4 | dith(b & 0x0F, x + 1, y));
        }
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

// DOBA: open packs stay open, and their index is read in 4 KB pages kept in
// PSRAM. Opening a file on a 1 TB FAT32 card walks its folders, and every seek
// in a 300 MB pack walks the cluster chain from the start: a binary search of
// 17 seeks per tile made a new zoom level take seconds. Now the upper steps of
// every search hit cached pages and a tile costs about one card read.
struct OpenPack {
    char path[48] = "";
    File f;
    uint8_t hdr[HEADER];
    bool ok = false;
    uint32_t lastUse = 0;
};
constexpr int OPEN_PACKS = 3;
OpenPack g_packs[OPEN_PACKS];
uint32_t g_packGen = 0xFFFFFFFF;
constexpr int IDX_PAGE = 4096, IDX_PAGES = 96;          // 384 KB of PSRAM
struct IdxPage { int8_t pack = -1; uint32_t page = 0, lastUse = 0; uint8_t* data = nullptr; };
IdxPage g_idx[IDX_PAGES];

OpenPack* packFor(const char* path) {
    if (g_packGen != sd_card::mountGeneration()) {   // the card was remounted
        for (OpenPack& p : g_packs) { if (p.path[0] && p.f) p.f.close(); p.path[0] = 0; p.ok = false; }
        for (IdxPage& q : g_idx) q.pack = -1;
        g_packGen = sd_card::mountGeneration();
    }
    OpenPack* victim = &g_packs[0];
    for (OpenPack& p : g_packs) {
        if (p.path[0] && !strcmp(p.path, path)) { p.lastUse = g_tick; return p.ok ? &p : nullptr; }
        if (!p.path[0] || p.lastUse < victim->lastUse) victim = &p;
    }
    if (victim->path[0] && victim->f) victim->f.close();
    const int vi = (int)(victim - g_packs);
    for (IdxPage& q : g_idx) if (q.pack == vi) q.pack = -1;
    snprintf(victim->path, sizeof victim->path, "%s", path);
    victim->lastUse = g_tick;
    victim->f = SD.open(path, FILE_READ);
    victim->ok = victim->f && victim->f.read(victim->hdr, HEADER) == HEADER && memcmp(victim->hdr, kMagic, 8) == 0;
    return victim->ok ? victim : nullptr;
}

// One 16-byte index entry, through the page cache.
bool indexEntry(OpenPack& p, uint32_t i, uint8_t* e) {
    const int pi = (int)(&p - g_packs);
    const uint32_t byte = HEADER + i * ENTRY, page = byte / IDX_PAGE, at = byte % IDX_PAGE;
    if (at + ENTRY > IDX_PAGE) {   // straddles two pages (not with 16-byte entries and a 32-byte header)
        return p.f.seek(byte) && p.f.read(e, ENTRY) == ENTRY;
    }
    IdxPage* hit = nullptr; IdxPage* victim = &g_idx[0];
    for (IdxPage& q : g_idx) {
        if (q.pack == pi && q.page == page) { hit = &q; break; }
        if (q.pack < 0 || q.lastUse < victim->lastUse) victim = &q;
    }
    if (!hit) {
        if (!victim->data) victim->data = (uint8_t*)heap_caps_malloc(IDX_PAGE, MALLOC_CAP_SPIRAM);
        if (!victim->data) return p.f.seek(byte) && p.f.read(e, ENTRY) == ENTRY;
        victim->pack = -1;
        if (!p.f.seek((uint32_t)page * IDX_PAGE)) return false;
        const int n = p.f.read(victim->data, IDX_PAGE);
        if (n < (int)(at + ENTRY)) return false;
        victim->pack = (int8_t)pi; victim->page = page;
        hit = victim;
    }
    hit->lastUse = g_tick;
    memcpy(e, hit->data + at, ENTRY);
    return true;
}

// Finds (z,x,y) in a pack and fills the slot. false = no such tile.
bool readTile(int z, int x, int y, Slot& s) {
    char path[64];
    packPath(z, x, y, path, sizeof path);
    sdLock();
    OpenPack* pk = packFor(path);
    if (!pk) { sdUnlock(); return false; }
    File& f = pk->f;
    const uint8_t* hdr = pk->hdr;
    bool ok = true;
    const uint32_t count = rd32(hdr + 24);
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
        if (!indexEntry(*pk, mid, e)) { ok = false; break; }
        const uint64_t key = (uint64_t)e[0] << 32 | (uint64_t)rd16(e + 2) << 16 | rd16(e + 4);
        if (key == want) { found = true; break; }
        if (key < want) lo = mid + 1; else hi = mid;
    }
    if (!found) { sdUnlock(); return false; }
    const uint8_t flags = e[1];
    const uint16_t fill = rd16(e + 6);
    const uint32_t off = rd32(e + 8), len = rd32(e + 12);
    if (flags & FLAG_SOLID) {
        sdUnlock();
        s.solid = true;
        s.fill = (uint8_t)(fill & 0x0F);
        return true;
    }
    if (len > g_zbufLen) {
        if (g_zbuf) heap_caps_free(g_zbuf);
        g_zbufLen = len < 48 * 1024 ? 48 * 1024 : len;
        g_zbuf = (uint8_t*)heap_caps_malloc(g_zbufLen, MALLOC_CAP_SPIRAM);
        if (!g_zbuf) { g_zbufLen = 0; sdUnlock(); return false; }
    }
    ok = f.seek(off) && f.read(g_zbuf, len) == len;
    sdUnlock();
    if (!ok) return false;
    if (!s.px) s.px = (uint8_t*)heap_caps_malloc(TILE_BYTES, MALLOC_CAP_SPIRAM);
    if (!s.px || !inflate(g_zbuf, len, s.px)) return false;
    if (g_dither) ditherTile(s.px);
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

// Straight from the tiles onto the screen: for a view the canvas cannot hold.
static int drawDirect(const MapScreenData& map, uint8_t* fb) {
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
                for (; x < run; ++x) putPortrait(fb, x, y, dith(s->fill, colW[x], wy));
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


// ---- the canvas: the view with half a screen of map on every side ----------------
//
// Dragging the map, or walking with it, moves the view a few hundred pixels, and
// each of those frames used to be drawn again tile by tile (the frame cache below
// it in ui_dashboard keys on the exact view, so a moved view always missed). The
// canvas keeps the map drawn around the view at this zoom: a view that falls on
// it is a row-by-row copy, and only blocks nobody has drawn yet come off the
// tiles. The rest of the canvas is filled in while nothing else happens
// (idleStep), nearest the view first.
//
// It is laid out like the panel's framebuffer (native, 4 bpp, two pixels a byte),
// so the copy is a memcpy per row: CW portrait columns are its rows, CH portrait
// rows its pixels per row.
namespace {
constexpr int CMX = 270, CMY = 480;                       // margin, portrait px
constexpr int CW = SCREEN_W + 2 * CMX, CH = SCREEN_H + 2 * CMY;   // 1080 x 1920
constexpr int CROW = CH / 2;                              // bytes a canvas row
constexpr int BW = 135, BH = 240;                         // blocks: drawn whole
constexpr int BCOLS = CW / BW, BROWS = CH / BH;           // 8 x 8
static_assert(CW % BW == 0 && CH % BH == 0, "blocks tile the canvas");

struct Canvas {
    uint8_t* px = nullptr;            // CW rows of CROW bytes, PSRAM
    bool anchored = false;
    bool inverted = false;
    int z = -1;
    uint32_t gen = 0;
    int32_t wx0 = 0, wy0 = 0;         // world px (at z) of canvas portrait (0, 0)
    bool valid[BROWS][BCOLS] = {};
    int validCount = 0;
    // The view last put on the screen from it: where, and where its centre was.
    bool shown = false;
    int shownOx = 0, shownOy = 0;
} g_cv;

uint32_t g_generationForCanvas();

inline void putCanvas(int cx, int cy, uint8_t level) {
    int nx, ny;
    if (g_cv.inverted) { nx = cy; ny = CW - 1 - cx; }
    else               { nx = CH - 1 - cy; ny = cx; }
    uint8_t* p = &g_cv.px[(size_t)ny * CROW + (nx >> 1)];
    if (nx & 1) *p = (uint8_t)((*p & 0x0F) | (level << 4));
    else        *p = (uint8_t)((*p & 0xF0) | level);
}

// One block off the tiles. Column by column: a column of the portrait canvas is
// a run along one of its native rows, so the writes go out in order.
void renderBlock(int br, int bc) {
    const int z = g_cv.z, maxT = (1 << z) - 1;
    for (int cx = bc * BW; cx < (bc + 1) * BW; ++cx) {
        const int32_t wx = g_cv.wx0 + cx;
        const int tx = wx >> 8, px = wx & 0xFF;
        int cy = br * BH;
        const int end = (br + 1) * BH;
        while (cy < end) {
            const int32_t wy = g_cv.wy0 + cy;
            const int ty = wy >> 8;
            int run = cy + (256 - (wy & 0xFF));
            if (run > end) run = end;
            if (ty < 0 || ty > maxT || tx < 0 || tx > maxT) {
                for (; cy < run; ++cy) putCanvas(cx, cy, 15);
                continue;
            }
            Slot* sl = tile(z, tx, ty);
            if (!sl->present) {
                for (; cy < run; ++cy) putCanvas(cx, cy, 15);
            } else if (sl->solid) {
                for (; cy < run; ++cy) putCanvas(cx, cy, dith(sl->fill, wx, g_cv.wy0 + cy));
            } else {
                for (; cy < run; ++cy) {
                    const int py = (g_cv.wy0 + cy) & 0xFF;
                    const uint8_t b = sl->px[py * (TILE / 2) + (px >> 1)];
                    putCanvas(cx, cy, (px & 1) ? (b & 0x0F) : (b >> 4));
                }
            }
        }
    }
    g_cv.valid[br][bc] = true;
    ++g_cv.validCount;
    ++g_stats.blocks;
}

void invalidateCanvas() {
    memset(g_cv.valid, 0, sizeof g_cv.valid);
    g_cv.validCount = 0;
    g_cv.shown = false;
}

inline uint8_t getCanvas(int cx, int cy) {
    int nx, ny;
    if (g_cv.inverted) { nx = cy; ny = CW - 1 - cx; }
    else               { nx = CH - 1 - cy; ny = cx; }
    const uint8_t b = g_cv.px[(size_t)ny * CROW + (nx >> 1)];
    return (nx & 1) ? (b >> 4) : (b & 0x0F);
}

inline bool canvasPixelReady(int cx, int cy) {
    return cx >= 0 && cy >= 0 && cx < CW && cy < CH && g_cv.valid[cy / BH][cx / BW];
}

// Move the canvas over the world by whole blocks: wx0 += dcols * BW, wy0 +=
// drows * BH. The blocks it still covers keep what they hold (a copy of rows
// and of bytes within rows: blocks are whole bytes either way); the blocks it
// uncovers are left to be drawn. This is what keeps a margin on every side
// after a drag without throwing the drawn map away.
void shiftCanvas(int dcols, int drows) {
    if (!dcols && !drows) return;
    if (abs(dcols) >= BCOLS || abs(drows) >= BROWS) {
        g_cv.wx0 += dcols * BW;
        g_cv.wy0 += drows * BH;
        invalidateCanvas();
        return;
    }
    // native layout: row = cx (inverted: CW-1-cx), pixel = CH-1-cy (inverted: cy)
    const int rs = g_cv.inverted ? -dcols * BW : dcols * BW;          // source row offset
    const int bs = (g_cv.inverted ? drows * BH : -drows * BH) / 2;    // source byte offset
    const int n = CROW - abs(bs);
    // rows that read further down go first going down, and the other way up, so
    // no row is overwritten before it is read
    for (int i = 0; i < CW; ++i) {
        const int r = rs >= 0 ? i : CW - 1 - i;
        const int src = r + rs;
        if (src < 0 || src >= CW) continue;          // an uncovered block column
        uint8_t* d = g_cv.px + (size_t)r * CROW;
        const uint8_t* q = g_cv.px + (size_t)src * CROW;
        if (bs >= 0) memmove(d, q + bs, n);
        else memmove(d - bs, q, n);
    }
    bool v[BROWS][BCOLS] = {};
    int count = 0;
    for (int br = 0; br < BROWS; ++br)
        for (int bc = 0; bc < BCOLS; ++bc) {
            const int sr = br + drows, sc = bc + dcols;
            v[br][bc] = sr >= 0 && sr < BROWS && sc >= 0 && sc < BCOLS && g_cv.valid[sr][sc];
            count += v[br][bc];
        }
    memcpy(g_cv.valid, v, sizeof v);
    g_cv.validCount = count;
    g_cv.wx0 += dcols * BW;
    g_cv.wy0 += drows * BH;
    g_cv.shownOx -= dcols * BW;
    g_cv.shownOy -= drows * BH;
    if (g_cv.shownOx < 0 || g_cv.shownOy < 0 || g_cv.shownOx > CW - SCREEN_W || g_cv.shownOy > CH - SCREEN_H)
        g_cv.shown = false;
    ++g_stats.shifts;
}

// Whole blocks from where the view's top-left is (ox, oy) to where it would sit
// with an even margin round it.
inline int blocksOff(int o, int margin, int b) {
    const int d = o - margin;
    return d >= 0 ? (d + b / 2) / b : -((-d + b / 2) / b);
}

// Where the view would sit on the canvas as it stands, without moving it.
bool viewOnCanvas(const MapScreenData& map, int& ox, int& oy) {
    if (!g_cv.anchored || !g_cv.px) return false;
    const double lat = map.centerLat, lon = map.centerLon;
    const int z = zoomFor(lat, map.metersPerPixel);
    if (z != g_cv.z || g_cv.gen != g_generationForCanvas()) return false;
    if (fabs(map.metersPerPixel / nativeMpp(z, lat) - 1.0) > 1e-3) return false;
    const double world = (double)TILE * (double)(1u << z);
    const double lr = lat * M_PI / 180.0;
    ox = (int32_t)floor((lon + 180.0) / 360.0 * world) - map.riderX - g_cv.wx0;
    oy = (int32_t)floor((1.0 - asinh(tan(lr)) / M_PI) / 2.0 * world) - map.riderY - g_cv.wy0;
    return ox >= 0 && ox <= CW - SCREEN_W && oy >= 0 && oy <= CH - SCREEN_H;
}

// Where the view's top-left falls on the canvas, re-anchoring it round the view
// when the view is off it (or the zoom, the card or the rotation changed).
// False when the canvas cannot take this view.
bool placeView(const MapScreenData& map, int& ox, int& oy) {
    const double lat = map.centerLat, lon = map.centerLon;
    int z = zoomFor(lat, map.metersPerPixel);
    if (z > 16 && maxZoomAt(lat, lon) < z) return false;          // enlarged: direct
    if (fabs(map.metersPerPixel / nativeMpp(z, lat) - 1.0) > 1e-3) return false;
    if (!g_cv.px) {
        g_cv.px = (uint8_t*)heap_caps_malloc((size_t)CW * CROW, MALLOC_CAP_SPIRAM);
        if (!g_cv.px) return false;
    }
    const double world = (double)TILE * (double)(1u << z);
    const double lr = lat * M_PI / 180.0;
    const int32_t vx = (int32_t)floor((lon + 180.0) / 360.0 * world) - map.riderX;
    const int32_t vy = (int32_t)floor((1.0 - asinh(tan(lr)) / M_PI) / 2.0 * world) - map.riderY;
    const bool inv = epd_get_rotation() == EPD_ROT_INVERTED_PORTRAIT;
    const bool same = g_cv.anchored && g_cv.z == z && g_cv.gen == g_generationForCanvas() && g_cv.inverted == inv;
    if (same && (vx - g_cv.wx0 < 0 || vx - g_cv.wx0 > CW - SCREEN_W ||
                 vy - g_cv.wy0 < 0 || vy - g_cv.wy0 > CH - SCREEN_H)) {
        // off the canvas, same map: slide it over, keeping what still overlaps
        shiftCanvas(blocksOff(vx - g_cv.wx0, CMX, BW), blocksOff(vy - g_cv.wy0, CMY, BH));
    }
    if (!same || vx - g_cv.wx0 < 0 || vx - g_cv.wx0 > CW - SCREEN_W ||
        vy - g_cv.wy0 < 0 || vy - g_cv.wy0 > CH - SCREEN_H) {
        g_cv.anchored = true;
        g_cv.z = z;
        g_cv.gen = g_generationForCanvas();
        g_cv.inverted = inv;
        g_cv.wx0 = vx - CMX;
        g_cv.wy0 = vy - CMY;
        invalidateCanvas();
        ++g_stats.anchors;
    }
    ox = vx - g_cv.wx0;
    oy = vy - g_cv.wy0;
    return true;
}

// The window at (ox, oy) of the canvas onto the screen: a row at a time.
void blit(uint8_t* fb, int ox, int oy) {
    for (int r = 0; r < 540; ++r) {
        int crow, c0;
        if (g_cv.inverted) { crow = r + (CW - SCREEN_W - ox); c0 = oy; }
        else               { crow = r + ox;                   c0 = CH - SCREEN_H - oy; }
        const uint8_t* src = g_cv.px + (size_t)crow * CROW;
        uint8_t* dst = fb + (size_t)r * NATIVE_ROW;
        if (!(c0 & 1)) {
            memcpy(dst, src + c0 / 2, NATIVE_ROW);
        } else {
            // odd: each screen byte takes the high half of one canvas byte and the
            // low half of the next (low nibble = the even pixel)
            const uint8_t* q = src + (c0 >> 1);
            for (int i = 0; i < NATIVE_ROW; ++i)
                dst[i] = (uint8_t)((q[i] >> 4) | (q[i + 1] << 4));
        }
    }
    ++g_stats.blits;
}
}  // namespace

static bool g_canvasOn = true;
void setCanvas(bool on) { g_canvasOn = on; g_cv.anchored = false; }

int drawBase(const MapScreenData& map, uint8_t* fb) {
    g_inverted = epd_get_rotation() == EPD_ROT_INVERTED_PORTRAIT;
    int ox, oy;
    if (!g_canvasOn || !placeView(map, ox, oy)) return drawDirect(map, fb);
    const uint32_t missingBefore = g_stats.missing;
    for (int br = oy / BH; br <= (oy + SCREEN_H - 1) / BH; ++br)
        for (int bc = ox / BW; bc <= (ox + SCREEN_W - 1) / BW; ++bc)
            if (!g_cv.valid[br][bc]) renderBlock(br, bc);
    blit(fb, ox, oy);
    g_cv.shown = true;
    g_cv.shownOx = ox; g_cv.shownOy = oy;
    return (int)(g_stats.missing - missingBefore);
}

bool drawQuick(const MapScreenData& map, uint8_t* fb) {
    int ox, oy;
    if (!g_canvasOn || !viewOnCanvas(map, ox, oy)) return false;
    blit(fb, ox, oy);
    // blocks not drawn yet hold whatever was there before: paper instead
    for (int br = oy / BH; br <= (oy + SCREEN_H - 1) / BH; ++br)
        for (int bc = ox / BW; bc <= (ox + SCREEN_W - 1) / BW; ++bc)
            if (!g_cv.valid[br][bc])
                epd_fill_rect({bc * BW - ox, br * BH - oy, BW, BH}, 0xFF, fb);
    return true;
}

bool drawZoomPreview(uint8_t* fb, bool zoomIn, int px, int py) {
    if (!g_canvasOn || !g_cv.shown || !g_cv.px) return false;
    g_inverted = epd_get_rotation() == EPD_ROT_INVERTED_PORTRAIT;
    // About the point the zoom keeps still: the view's centre. It is the rider
    // only on an undragged map; a dragged one zoomed about the rider showed
    // another stretch of map for a moment.
    const int rx = px, ry = py;
    const int ox = g_cv.shownOx, oy = g_cv.shownOy;
    for (int y = 0; y < SCREEN_H; ++y) {
        // the new screen row in the old view: halved (in) or doubled (out) about the centre
        const int sy = zoomIn ? ry + ((y - ry) >> 1) : ry + 2 * (y - ry);
        const int cy = oy + sy;
        for (int x = 0; x < SCREEN_W; ++x) {
            const int sx = zoomIn ? rx + ((x - rx) >> 1) : rx + 2 * (x - rx);
            const int cx = ox + sx;
            putPortrait(fb, x, y, canvasPixelReady(cx, cy) ? getCanvas(cx, cy) : 15);
        }
    }
    return true;
}

bool viewReady(const MapScreenData& map) {
    int ox, oy;
    if (!g_canvasOn || !viewOnCanvas(map, ox, oy)) return false;
    for (int br = oy / BH; br <= (oy + SCREEN_H - 1) / BH; ++br)
        for (int bc = ox / BW; bc <= (ox + SCREEN_W - 1) / BW; ++bc)
            if (!g_cv.valid[br][bc]) return false;
    return true;
}

bool idleStep(const MapScreenData& map) {
    int ox, oy;
    if (!g_canvasOn || !placeView(map, ox, oy)) return false;
    // After a drag the view sits off the middle: slide the canvas back round it
    // first, so the next drag finds a margin on every side.
    const int dc = blocksOff(ox, CMX, BW), dr = blocksOff(oy, CMY, BH);
    if (dc || dr) { shiftCanvas(dc, dr); return true; }
    if (g_cv.validCount >= BROWS * BCOLS) return false;
    // the undrawn block nearest the middle of the view
    const int mx = ox + SCREEN_W / 2, my = oy + SCREEN_H / 2;
    int best = -1, bestD = 0;
    for (int br = 0; br < BROWS; ++br)
        for (int bc = 0; bc < BCOLS; ++bc) {
            if (g_cv.valid[br][bc]) continue;
            const int dx = bc * BW + BW / 2 - mx, dy = br * BH + BH / 2 - my;
            const int d = dx * dx + dy * dy;
            if (best < 0 || d < bestD) { best = br * BCOLS + bc; bestD = d; }
        }
    if (best < 0) return false;
    renderBlock(best / BCOLS, best % BCOLS);
    return true;
}

static uint32_t g_generation = 1;
namespace { uint32_t g_generationForCanvas() { return g_generation; } }
void setDither(bool on) { g_dither = on; reset(); }

void reset() {
    for (Slot& s : g_slots) s.valid = false;
    for (CellSeen& c : g_cells) c = CellSeen();
    ++g_generation;
}
uint32_t generation() { return g_generation; }

Stats stats() { return g_stats; }

}  // namespace topo
