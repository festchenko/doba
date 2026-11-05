#include "doba_dem.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "sd_bus.h"

#ifdef OTP_SIM
#include <zlib.h>
#else
#include "rom/miniz.h"
#endif

namespace doba_dem {
namespace {

constexpr int HDR = 32;
constexpr int MAX_BLOCK = 120;               // samples per block side we accept
constexpr int CACHED = 6;

struct FileInfo {
    int lat = 999, lon = 999;                // which degree (999 = empty slot)
    bool present = false;
    int n = 0, block = 0, nb = 0;
    uint32_t* index = nullptr;               // nb*nb*(offset, length)
};
FileInfo g_files[2];
int g_fileNext = 0;

struct Block { int lat = 999, lon = 999, bi = -1, bj = -1; uint32_t used = 0; int16_t* h = nullptr; };
Block g_blocks[CACHED];
uint32_t g_tick = 0;

bool inflateTo(const uint8_t* src, size_t len, uint8_t* dst, size_t cap) {
#ifdef OTP_SIM
    uLongf out = cap;
    return uncompress(dst, &out, src, len) == Z_OK && out == cap;
#else
    static tinfl_decompressor* d = nullptr;
    if (!d) d = (tinfl_decompressor*)heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_8BIT);
    if (!d) return false;
    tinfl_init(d);
    size_t inLen = len, out = cap;
    const tinfl_status st = tinfl_decompress(d, src, &inLen, dst, dst, &out,
        TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    return st == TINFL_STATUS_DONE && out == cap;
#endif
}

void pathFor(int lat, int lon, char* out, size_t n) {
    snprintf(out, n, "/doba/dem/%c%02d%c%03d.dem", lat >= 0 ? 'N' : 'S', abs(lat), lon >= 0 ? 'E' : 'W', abs(lon));
}

uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
int16_t rd16(const uint8_t* p) { return (int16_t)(p[0] | p[1] << 8); }

FileInfo* fileFor(int lat, int lon) {
    for (FileInfo& f : g_files)
        if (f.lat == lat && f.lon == lon) return &f;
    FileInfo& f = g_files[g_fileNext];
    g_fileNext = (g_fileNext + 1) % 2;
    if (f.index) { heap_caps_free(f.index); f.index = nullptr; }
    f.lat = lat; f.lon = lon; f.present = false;
    char path[40];
    pathFor(lat, lon, path, sizeof path);
    sdLock();
    File fh = SD.open(path, FILE_READ);
    if (fh) {
        uint8_t h[HDR];
        if (fh.read(h, HDR) == HDR && memcmp(h, "DOBADEM1", 8) == 0) {
            f.n = (uint16_t)rd16(h + 12);
            f.block = (uint16_t)rd16(h + 14);
            f.nb = (uint16_t)rd16(h + 16);
            const size_t isz = (size_t)f.nb * f.nb * 8;
            if (f.n > 0 && f.block > 0 && f.block <= MAX_BLOCK && f.nb > 0 && f.nb <= 64) {
                f.index = (uint32_t*)heap_caps_malloc(isz, MALLOC_CAP_SPIRAM);
                if (f.index && fh.read((uint8_t*)f.index, isz) == isz) f.present = true;
            }
        }
        fh.close();
    }
    sdUnlock();
    return &f;
}

const int16_t* blockFor(FileInfo& f, int bi, int bj) {
    ++g_tick;
    Block* slot = nullptr;
    for (Block& b : g_blocks) {
        if (b.h && b.lat == f.lat && b.lon == f.lon && b.bi == bi && b.bj == bj) { b.used = g_tick; return b.h; }
        if (!slot || b.used < slot->used) slot = &b;
    }
    const size_t raw = (size_t)f.block * f.block * 2;
    if (!slot->h) slot->h = (int16_t*)heap_caps_malloc(MAX_BLOCK * MAX_BLOCK * 2, MALLOC_CAP_SPIRAM);
    if (!slot->h) return nullptr;
    const uint8_t* e = (const uint8_t*)&f.index[(bi * f.nb + bj) * 2];
    const uint32_t off = rd32(e), len = rd32(e + 4);
    if (len == 0 || len > 2 * raw) return nullptr;
    uint8_t* z = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!z) return nullptr;
    char path[40];
    pathFor(f.lat, f.lon, path, sizeof path);
    sdLock();
    File fh = SD.open(path, FILE_READ);
    bool ok = fh && fh.seek(off) && fh.read(z, len) == len;
    if (fh) fh.close();
    sdUnlock();
    ok = ok && inflateTo(z, len, (uint8_t*)slot->h, raw);
    heap_caps_free(z);
    slot->lat = 999;
    if (!ok) return nullptr;
    // Undo the row deltas.
    for (int r = 0; r < f.block; ++r) {
        int16_t* row = slot->h + r * f.block;
        int32_t acc = 0;
        for (int c = 0; c < f.block; ++c) { acc += row[c]; row[c] = (int16_t)acc; }
    }
    slot->lat = f.lat; slot->lon = f.lon; slot->bi = bi; slot->bj = bj; slot->used = g_tick;
    return slot->h;
}

bool sample(FileInfo& f, int i, int j, float& out) {
    if (i < 0) i = 0;
    if (j < 0) j = 0;
    if (i >= f.n) i = f.n - 1;
    if (j >= f.n) j = f.n - 1;
    const int16_t* b = blockFor(f, i / f.block, j / f.block);
    if (!b) return false;
    out = b[(i % f.block) * f.block + (j % f.block)];
    return true;
}

double g_lastLat = 999, g_lastLon = 999;
float g_lastM = 0;
bool g_lastOk = false;

}  // namespace

bool height(double lat, double lon, float& m) {
    if (fabs(lat - g_lastLat) < 2e-6 && fabs(lon - g_lastLon) < 2e-6) { m = g_lastM; return g_lastOk; }
    g_lastLat = lat; g_lastLon = lon; g_lastOk = false;
    const int la = (int)floor(lat), lo = (int)floor(lon);
    FileInfo* f = fileFor(la, lo);
    if (!f->present) return false;
    // Sample centres sit half a step in from the edges.
    const double fi = (la + 1 - lat) * f->n - 0.5, fj = (lon - lo) * f->n - 0.5;
    const int i0 = (int)floor(fi), j0 = (int)floor(fj);
    const float di = (float)(fi - i0), dj = (float)(fj - j0);
    float a, b, c, d;
    if (!sample(*f, i0, j0, a) || !sample(*f, i0, j0 + 1, b) || !sample(*f, i0 + 1, j0, c) ||
        !sample(*f, i0 + 1, j0 + 1, d))
        return false;
    g_lastM = m = (a * (1 - dj) + b * dj) * (1 - di) + (c * (1 - dj) + d * dj) * di;
    g_lastOk = true;
    return true;
}

void rescan() {
    for (FileInfo& f : g_files) { f.lat = 999; f.lon = 999; f.present = false; }
    for (Block& b : g_blocks) b.lat = 999;
    g_lastLat = 999;
}

}  // namespace doba_dem
