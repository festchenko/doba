#include "doba_topo_bg.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP32)
#include <esp_heap_caps.h>
static void* bigAlloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
#else
static void* bigAlloc(size_t n) { return malloc(n); }
#endif

namespace topo_bg {
namespace {

constexpr int W = 540, H = 960;        // portrait screen
constexpr int S = 4;                   // the field is sampled every 4 px, then interpolated
constexpr int GW = W / S + 2, GH = H / S + 2;
constexpr float kStep = 0.07f;         // contour interval in field units
constexpr int kHills = 9;

struct Hill { float x, y, sx, sy, h; };

uint8_t* g_line = nullptr;             // 1 bit/px: a contour passes here
uint8_t* g_index = nullptr;            // 1 bit/px: an index (every 5th) contour
int g_variant = -1;

inline void setBit(uint8_t* m, int x, int y) { const int i = y * W + x; m[i >> 3] |= (uint8_t)(1 << (i & 7)); }
inline bool getBit(const uint8_t* m, int x, int y) { const int i = y * W + x; return m[i >> 3] >> (i & 7) & 1; }

// The same tiny LCG as the prototype (tools: none; tuned by eye), so a variant
// is the same landscape on every boot and in the simulator.
struct Lcg {
    uint32_t s;
    float next() { s = (s * 1103515245u + 12345u) & 0x7fffffffu; return s / 2147483647.0f; }
};

void hills(int variant, Hill* out) {
    static const uint32_t kSeeds[] = {7, 1234, 99, 4242};
    Lcg r{kSeeds[(unsigned)variant % 4]};
    for (int i = 0; i < kHills; ++i) {
        Hill& h = out[i];
        h.x = r.next() * W * 1.2f - W * 0.1f;
        h.y = r.next() * H * 1.2f - H * 0.1f;
        h.sx = 60 + r.next() * 170;
        h.sy = 60 + r.next() * 170;
        h.h = (0.5f + r.next()) * ((i % 3) ? 1.0f : -0.6f);   // every third a hollow
    }
}

float field(float x, float y, const Hill* hs) {
    float v = 0;
    for (int i = 0; i < kHills; ++i) {
        const float dx = (x - hs[i].x) / hs[i].sx, dy = (y - hs[i].y) / hs[i].sy;
        v += hs[i].h * expf(-(dx * dx + dy * dy));
    }
    v += 0.12f * sinf(x * 0.011f + y * 0.004f) + 0.08f * sinf(y * 0.017f - x * 0.006f);
    return v;
}

bool build(int variant) {
    const size_t bytes = (size_t)W * H / 8;
    if (!g_line) g_line = (uint8_t*)bigAlloc(bytes);
    if (!g_index) g_index = (uint8_t*)bigAlloc(bytes);
    float* grid = (float*)bigAlloc(sizeof(float) * GW * GH);
    int16_t* row0 = (int16_t*)malloc(sizeof(int16_t) * W);
    int16_t* row1 = (int16_t*)malloc(sizeof(int16_t) * W);
    if (!g_line || !g_index || !grid || !row0 || !row1) {
        free(row0); free(row1); free(grid);
        return false;
    }
    Hill hs[kHills];
    hills(variant, hs);
    for (int j = 0; j < GH; ++j)
        for (int i = 0; i < GW; ++i) grid[j * GW + i] = field((float)(i * S), (float)(j * S), hs);
    memset(g_line, 0, bytes);
    memset(g_index, 0, bytes);

    auto levelRow = [&](int y, int16_t* out) {
        const int j = y / S;
        const float fy = (float)(y % S) / S;
        const float* g0 = grid + j * GW;
        const float* g1 = g0 + GW;
        for (int x = 0; x < W; ++x) {
            const int i = x / S;
            const float fx = (float)(x % S) / S;
            const float v = (g0[i] * (1 - fx) + g0[i + 1] * fx) * (1 - fy) + (g1[i] * (1 - fx) + g1[i + 1] * fx) * fy;
            out[x] = (int16_t)floorf(v / kStep);
        }
    };
    levelRow(0, row0);
    for (int y = 0; y < H - 1; ++y) {
        levelRow(y + 1, row1);
        for (int x = 0; x < W - 1; ++x) {
            const int a = row0[x], b = row0[x + 1], c = row1[x];
            if (a == b && a == c) continue;
            int m = a > b ? a : b;
            if (c > m) m = c;
            setBit(g_line, x, y);
            if (((m % 5) + 5) % 5 == 0) {          // index contour: 2 px
                setBit(g_index, x, y);
                setBit(g_index, x + 1, y);
                setBit(g_index, x, y + 1);
            }
        }
        int16_t* t = row0; row0 = row1; row1 = t;
    }
    free(row0); free(row1); free(grid);
    g_variant = variant;
    return true;
}

bool g_inverted = false;   // drawing on black: the paper is black, the content white

}  // namespace

void draw(const EpdRect& r0, uint8_t* fb, int variant) {
    if (g_variant != variant && !build(variant)) return;
    EpdRect r = r0;
    if (r.x < 0) { r.width += r.x; r.x = 0; }
    if (r.y < 0) { r.height += r.y; r.y = 0; }
    if (r.x + r.width > W) r.width = W - r.x;
    if (r.y + r.height > H) r.height = H - r.y;
    // Decide every pixel against the content alone, then draw, so the lines
    // don't push each other away. "Clear" is: no content within kHalo px (a
    // square, inside the rect). The panel is read once into a row-dilated
    // mask, so a line pixel costs 2*kHalo+1 byte reads, not (2*kHalo+1)^2
    // rotated pixel reads — the difference on the ESP32 is most of a frame.
    const int n = r.width * r.height;
    if (n <= 0) return;
    uint8_t* near = (uint8_t*)bigAlloc((size_t)n);   // content within kHalo along the row
    uint8_t* put = (uint8_t*)bigAlloc((size_t)(n + 7) / 8);
    uint8_t* ink = (uint8_t*)malloc((size_t)r.width);
    if (!near || !put || !ink) { free(near); free(put); free(ink); return; }
    memset(put, 0, (size_t)(n + 7) / 8);
    for (int y = 0; y < r.height; ++y) {
        for (int x = 0; x < r.width; ++x) {
            const uint8_t p = doba_get_pixel(r.x + x, r.y + y, fb);
            ink[x] = g_inverted ? p > 0x0F : p < 0xF0;
        }
        // running count of ink in the window [x-kHalo, x+kHalo]
        uint8_t* row = near + (size_t)y * r.width;
        int c = 0;
        for (int x = 0; x < kHalo && x < r.width; ++x) c += ink[x];
        for (int x = 0; x < r.width; ++x) {
            if (x + kHalo < r.width) c += ink[x + kHalo];
            if (x - kHalo - 1 >= 0) c -= ink[x - kHalo - 1];
            row[x] = c > 0;
        }
    }
    for (int y = 0; y < r.height; ++y)
        for (int x = 0; x < r.width; ++x) {
            const int gx = r.x + x, gy = r.y + y;
            if (!getBit(g_index, gx, gy) && !getBit(g_line, gx, gy)) continue;
            bool clear = true;
            const int y0 = y - kHalo < 0 ? 0 : y - kHalo, y1 = y + kHalo >= r.height ? r.height - 1 : y + kHalo;
            for (int yy = y0; yy <= y1 && clear; ++yy) clear = !near[(size_t)yy * r.width + x];
            if (!clear) continue;
            const int k = y * r.width + x;
            put[k >> 3] |= (uint8_t)(1 << (k & 7));
        }
    free(near);
    free(ink);
    for (int k = 0; k < n; ++k)
        if (put[k >> 3] >> (k & 7) & 1) {
            const int x = r.x + k % r.width, y = r.y + k / r.width;
            epd_draw_pixel(x, y, g_inverted ? kLineDark : getBit(g_index, x, y) ? kIndex : kLine, fb);
        }
    free(put);
}

void drawInverted(const EpdRect& r, uint8_t* fb, int variant) {
    g_inverted = true;
    draw(r, fb, variant);
    g_inverted = false;
}

}  // namespace topo_bg
