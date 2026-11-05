#include "doba_icons.h"

#include <cmath>
#include <cstring>

#include "epd_compat.h"

namespace doba_icons {
namespace {

uint8_t* g_fb = nullptr;
uint8_t g_ink = 0, g_paper = 0xFF;

void rect(int x, int y, int w, int h, bool ink = true) {
    if (w > 0 && h > 0) epd_fill_rect({x, y, w, h}, ink ? g_ink : g_paper, g_fb);
}
void disc(int x, int y, int r, bool ink = true) { epd_fill_circle(x, y, r, ink ? g_ink : g_paper, g_fb); }
void tri(int x0, int y0, int x1, int y1, int x2, int y2, bool ink = true) {
    epd_fill_triangle(x0, y0, x1, y1, x2, y2, ink ? g_ink : g_paper, g_fb);
}
// A line t px thick: a fat stroke from discs along it (round caps, any angle).
void line(float x0, float y0, float x1, float y1, int t, bool ink = true) {
    const float len = hypotf(x1 - x0, y1 - y0);
    const int n = (int)(len / 1.5f) + 1;
    for (int i = 0; i <= n; ++i) {
        const float k = (float)i / n;
        disc((int)lroundf(x0 + (x1 - x0) * k), (int)lroundf(y0 + (y1 - y0) * k), t / 2, ink);
    }
}
// A ring of thickness t (outer radius r).
void ring(int x, int y, int r, int t, bool ink = true) {
    disc(x, y, r, ink);
    disc(x, y, r - t, !ink);
}
// An arc of a ring, angles in degrees (0 = right, counter-clockwise, y up).
void arc(float x, float y, float r, float a0, float a1, int t, bool ink = true) {
    const int n = (int)(fabsf(a1 - a0) * r / 90.0f) + 4;
    for (int i = 0; i <= n; ++i) {
        const float a = (a0 + (a1 - a0) * i / n) * (float)M_PI / 180.0f;
        disc((int)lroundf(x + r * cosf(a)), (int)lroundf(y - r * sinf(a)), t / 2, ink);
    }
}
// A filled drop, point up, bulb of radius r centred at (x, y).
void drop(int x, int y, int r, bool ink = true) {
    disc(x, y, r, ink);
    tri(x, y - (int)(r * 2.1f), x - (int)(r * 0.87f), y - r / 2, x + (int)(r * 0.87f), y - r / 2, ink);
}

}  // namespace

void draw(Icon icon, int cx, int cy, int s, uint8_t ink, uint8_t paper, uint8_t* fb) {
    g_fb = fb;
    g_ink = ink;
    g_paper = paper;
    const int t = s / 11 > 3 ? s / 11 : 3;   // stroke
    const int h = s / 2;
    switch (icon) {
        case I_NONE:
            break;
        case I_FIRSTAID: {
            // rounded square frame, bold cross
            const int r = t * 2;
            rect(cx - h + r, cy - h, s - 2 * r, s);
            rect(cx - h, cy - h + r, s, s - 2 * r);
            disc(cx - h + r, cy - h + r, r); disc(cx + h - r, cy - h + r, r);
            disc(cx - h + r, cy + h - r, r); disc(cx + h - r, cy + h - r, r);
            const int a = s * 3 / 10, w = s / 7;
            rect(cx - w, cy - a, 2 * w, 2 * a, false);
            rect(cx - a, cy - w, 2 * a, 2 * w, false);
            break;
        }
        case I_WATER:
            drop(cx, cy + s / 6, s * 3 / 10);
            arc(cx, cy + s / 6, s * 3 / 10 - t * 2, 200, 250, t, false);   // a shine
            break;
        case I_BLEED:
            drop(cx - s / 8, cy + s / 7, s / 4);
            rect(cx + s / 8, cy - h + t, t * 2, s - 2 * t);               // a stop bar
            rect(cx + s / 8 - t, cy - h + t, t * 4, t * 2);
            break;
        case I_FIRE: {
            // a flame of three tongues over crossed logs
            const int r = s * 9 / 40, by = cy + s / 14;
            drop(cx, by, r);
            drop(cx - r * 9 / 10, by + r / 3, r * 3 / 5);
            drop(cx + r * 9 / 10, by + r / 3, r * 3 / 5);
            drop(cx, by + r / 2, r * 9 / 20, false);                    // the hot core, hollow
            line(cx - h + t, cy + h - t, cx + h - t, cy + h - 3 * t, t + 2);
            line(cx - h + t, cy + h - 3 * t, cx + h - t, cy + h - t, t + 2);
            break;
        }
        case I_SHELTER: {
            const int base = cy + h - t;
            line(cx - h + t, base, cx, cy - h + t, t + 1);
            line(cx + h - t, base, cx, cy - h + t, t + 1);
            line(cx - h, base, cx + h, base, t + 1);
            tri(cx, cy - s / 10, cx - s / 6, base, cx + s / 6, base);      // the door
            line(cx, cy - h + t, cx, cy - h - t, t);                       // pole tip
            break;
        }
        case I_FOOD: {
            // fish: an oval body, a tail, an eye
            const int bw = s * 3 / 8, bh = s / 4;
            for (int dy = -bh; dy <= bh; ++dy) {
                const int w = (int)(bw * sqrtf(1.0f - (float)(dy * dy) / (bh * bh)));
                rect(cx - s / 10 - w, cy + dy, 2 * w, 1);
            }
            tri(cx + s / 4, cy, cx + h, cy - bh, cx + h, cy + bh);
            disc(cx - s / 4, cy - bh / 3, t / 2 + 1, false);
            line(cx - s / 10, cy - bh + t, cx - s / 10, cy + bh - t, t / 2 + 1, false);   // gill
            break;
        }
        case I_PLANT: {
            // a leaf: two arcs meeting, a midrib
            for (int dy = -h + t; dy <= h - t; ++dy) {
                const float k = (float)(dy + h) / s;
                const int w = (int)(s * 0.32f * sinf(k * (float)M_PI));
                rect(cx - w + dy / 4, cy + dy, 2 * w, 1);
            }
            line(cx - s / 8, cy + h - t, cx + s / 8, cy - h + t * 2, t / 2 + 1, false);
            line(cx - s / 8, cy + h - t, cx - s / 5, cy + h + t, t);       // stem
            break;
        }
        case I_NAV: {
            ring(cx, cy, h, t);
            const int n = h - t * 2;
            tri(cx, cy - n, cx - n / 3, cy, cx + n / 3, cy);               // north half, solid
            tri(cx, cy + n, cx - n / 3, cy, cx + n / 3, cy);
            tri(cx, cy + n - t * 2, cx - n / 3 + t, cy + 1, cx + n / 3 - t, cy + 1, false);   // south half, open
            disc(cx, cy, t / 2 + 1, false);
            break;
        }
        case I_HAZARD: {
            const int top = cy - h + t, bot = cy + h - t;
            tri(cx, top - t, cx - h - t / 2, bot + t / 2, cx + h + t / 2, bot + t / 2);
            tri(cx, top + t * 3, cx - h + t * 3, bot - t, cx + h - t * 3, bot - t, false);
            rect(cx - t / 2 - 1, cy - s / 8, t + 2, s / 4);                 // !
            disc(cx, bot - t * 3, t / 2 + 1);
            break;
        }
        case I_WEATHER: {
            const int cyc = cy - s / 8;
            disc(cx - s / 6, cyc + s / 12, s / 6);
            disc(cx + s / 12, cyc - s / 12, s / 4 - 2);
            disc(cx + s / 4, cyc + s / 12, s / 6);
            rect(cx - s / 6, cyc + s / 12, s * 5 / 12, s / 6);
            // bolt: two triangles, cut out of the cloud's shadow
            tri(cx + s / 24, cyc + s / 8, cx - s / 8, cy + s / 4, cx + s / 24, cy + s / 4 - s / 16);
            tri(cx - s / 24, cy + s / 6, cx + s / 8, cy + s / 6, cx - s / 12, cy + h);
            break;
        }
        case I_COLD: {
            for (int k = 0; k < 3; ++k) {
                const float a = k * (float)M_PI / 3;
                const float dx = cosf(a) * (h - t), dy = sinf(a) * (h - t);
                line(cx - dx, cy - dy, cx + dx, cy + dy, t);
                for (int e = -1; e <= 1; e += 2) {   // the barbs at each end
                    const float ex = cx + e * dx * 0.62f, ey = cy + e * dy * 0.62f;
                    const float b = s / 7.0f;
                    line(ex, ey, ex + e * cosf(a + 0.8f) * b, ey + e * sinf(a + 0.8f) * b, t - 1);
                    line(ex, ey, ex + e * cosf(a - 0.8f) * b, ey + e * sinf(a - 0.8f) * b, t - 1);
                }
            }
            break;
        }
        case I_TOOLS: {
            // wrench: a shaft, a head ring with a jaw cut
            const float hx = cx + s / 5.0f, hy = cy - s / 5.0f;
            line(cx - h + t, cy + h - t, hx, hy, t * 2);
            disc((int)hx, (int)hy, s / 5);
            disc((int)hx + s / 10, (int)hy - s / 10, s / 9, false);
            tri((int)hx + s / 10, (int)hy - s / 10, (int)hx + s / 4, (int)hy - s / 30, (int)hx + s / 30, (int)hy - s / 4, false);
            break;
        }
        case I_KNOTS: {
            // a figure-eight knot: two loops, one over the other, and two ends
            const int r = s * 9 / 40;
            ring(cx, cy - r + t, r, t);
            ring(cx, cy + r - t, r, t);
            line(cx - r * 7 / 10, cy + r * 3 / 2, cx - h + t, cy + h - t, t);   // standing end
            line(cx + r, cy - r, cx + h - t, cy - h + t, t);                     // working end
            // the crossing in the middle: a gap in one strand
            line(cx - r * 7 / 10, cy - t, cx + r * 7 / 10, cy + t, t + 4, false);
            line(cx - r * 7 / 10, cy - t, cx + r * 7 / 10, cy + t, t);
            break;
        }
        case I_COMMS: {
            const int top = cy - s / 8;
            line(cx, top, cx, cy + h - t, t);
            line(cx, top + s / 6, cx - s / 6, cy + h - t, t);
            line(cx, top + s / 6, cx + s / 6, cy + h - t, t);
            disc(cx, top, t);
            arc(cx, top, s / 5, 120, 240, t);
            arc(cx, top, s / 5, -60, 60, t);
            arc(cx, top, s / 3 + 2, 125, 235, t);
            arc(cx, top, s / 3 + 2, -55, 55, t);
            break;
        }
        case I_BOOKS: {
            // an open book: two pages and a spine
            const int top = cy - s * 3 / 10, bot = cy + s * 3 / 10;
            tri(cx, top + t, cx - h, top - t, cx - h, bot);
            tri(cx, top + t, cx, bot + t, cx - h, bot);
            tri(cx, top + t, cx + h, top - t, cx + h, bot);
            tri(cx, top + t, cx, bot + t, cx + h, bot);
            for (int k = 0; k < 3; ++k) {   // lines of text
                const int y = top + s / 8 + k * s / 9;
                line(cx - h + t * 2, y, cx - t * 2, y + 2, t / 2 + 1, false);
                line(cx + t * 2, y + 2, cx + h - t * 2, y, t / 2 + 1, false);
            }
            break;
        }
        case I_SHELF: {
            const int base = cy + h - t;
            rect(cx - h, base, s, t);
            const int w[5] = {s / 8, s / 7, s / 9, s / 7, s / 8};
            const int hh[5] = {s * 7 / 10, s * 6 / 10, s * 8 / 10, s * 5 / 10, s * 7 / 10};
            int x = cx - h + t;
            for (int k = 0; k < 4; ++k) {
                rect(x, base - hh[k], w[k], hh[k]);
                rect(x + 2, base - hh[k] + s / 8, w[k] - 4, 2, false);
                x += w[k] + 3;
            }
            // one leaning book
            tri(x + 2, base, x + w[4] + 2, base, x + s / 4, base - hh[4]);
            break;
        }
        case I_FILES: {
            const int top = cy - s / 4;
            rect(cx - h, top, s * 2 / 5, s / 8);
            rect(cx - h, top + s / 10, s, s * 6 / 10);
            rect(cx - h + t, top + s / 10 + s / 6, s - 2 * t, t / 2 + 1, false);
            break;
        }
        case I_SEARCH:
            ring(cx - s / 10, cy - s / 10, s * 3 / 10, t);
            line(cx + s / 8, cy + s / 8, cx + h - t, cy + h - t, t + 2);
            break;
        case I_BREATH: {
            // lungs: two lobes and a trachea
            line(cx, cy - h + t, cx, cy - s / 10, t);
            for (int e = -1; e <= 1; e += 2) {
                const int lx = cx + e * s / 4;
                disc(lx, cy + s / 10, s / 5);
                disc(lx, cy - s / 20, s / 7);
                rect(lx - s / 7, cy - s / 20, 2 * s / 7, s / 6);
                line(cx, cy - s / 10, cx + e * s / 8, cy + s / 20, t);
            }
            rect(cx - t / 2, cy - s / 10, t, s / 2, false);   // the gap between lobes
            line(cx, cy - h + t, cx, cy - s / 10, t);
            break;
        }
    }
}

Icon byName(const char* n) {
    struct { const char* name; Icon icon; } kMap[] = {
        {"firstaid", I_FIRSTAID}, {"medicine", I_FIRSTAID}, {"water", I_WATER}, {"fire", I_FIRE},
        {"shelter", I_SHELTER}, {"food", I_FOOD}, {"plant", I_PLANT}, {"nav", I_NAV},
        {"navigation", I_NAV}, {"hazard", I_HAZARD}, {"weather", I_WEATHER}, {"cold", I_COLD},
        {"tools", I_TOOLS}, {"knots", I_KNOTS}, {"comms", I_COMMS}, {"books", I_BOOKS},
        {"shelf", I_SHELF}, {"files", I_FILES}, {"search", I_SEARCH}, {"bleed", I_BLEED},
        {"breath", I_BREATH},
    };
    for (const auto& m : kMap)
        if (!strcmp(n, m.name)) return m.icon;
    return I_BOOKS;
}

}  // namespace doba_icons
