#include "doba_i18n.h"
#include "map_view.h"
#include "topo_layer.h"   // DOBA
#include "milspec.h"      // DOBA

#include <cmath>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>

#include "epdiy.h"
#include "epd_compat.h"
#include "device_state.h"
#include "ui_render.h"
#include "vfont_faces.h"

namespace {

// Layout bands (portrait 540x960, design 1f)
constexpr int MAP_TOP = ui::STATUS_H;
constexpr int STRIP_TOP = ui::MAP_STRIP_TOP;   // 3-cell footer below
constexpr int MAP_BOTTOM = STRIP_TOP;

// Other riders from a private mesh channel: a black dot ringed in white so it
// reads over roads and fills alike, hollow once their fix is getting old, with
// their short name beside it on a white chip. One who is off the edge of the
// map is a small arrow at the edge pointing their way, so the rider can tell
// which way the group went without zooming out to find them.
void drawPeers(const MapScreenData& map, uint8_t* fb) {
    constexpr int R = 26;      // ring outside radius
    constexpr int RING = 4;    // and its weight
    for (int i = 0; i < map.peerCount; ++i) {
        const MapPeer& p = map.peers[i];
        float ux = 0, uy = 0;
        if (p.offscreen) {
            const float dx = p.x - map.riderX, dy = p.y - map.riderY;
            const float len = sqrtf(dx * dx + dy * dy);
            if (len < 1) continue;
            ux = dx / len;
            uy = dy / len;
        }
        // Off the screen the ring becomes a pin: a broad black point out of
        // it toward them, twice as long as the ring is wide, so which way reads at a
        // glance and never as a road or the compass needle.
        auto pin = [&](int tipR, int halfW, uint8_t c) {
            const int tx = p.x + lroundf(ux * tipR), ty = p.y + lroundf(uy * tipR);
            const int lx = p.x - lroundf(uy * halfW), ly = p.y + lroundf(ux * halfW);
            const int rx = p.x + lroundf(uy * halfW), ry = p.y - lroundf(ux * halfW);
            epd_fill_triangle(tx, ty, lx, ly, rx, ry, c, fb);
        };
        // White under it first, so roads don't run through the marker.
        epd_fill_circle(p.x, p.y, R + 3, 0xFF, fb);
        if (p.offscreen) pin(3 * R + 6, R + 3, 0xFF);
        if (p.offscreen) pin(3 * R, R, 0x00);
        if (p.stale) {
            // Not heard from for a while: the ring dashed, where they WERE.
            for (int a = 0; a < 360; a += 2) {
                if ((a / 20) % 2) continue;
                const float r = a * (float)M_PI / 180.0f;
                for (int d = R - RING + 1; d <= R; ++d)
                    epd_draw_pixel(p.x + lroundf(cosf(r) * d),
                                   p.y + lroundf(sinf(r) * d), 0x00, fb);
            }
        } else {
            epd_fill_circle(p.x, p.y, R, 0x00, fb);
        }
        epd_fill_circle(p.x, p.y, R - RING, 0xFF, fb);
        // Two letters at the size one used to be, a step down for a wide
        // pair ("Mw") so they stay inside the ring.
        const EpdFont* f = &ArialBold_14;
        if (ui::textWidth(f, p.initials) > 2 * (R - RING) - 4) f = &Arial_B;
        ui::text(f, p.x, p.y + epdc_digit_height(f) / 2, p.initials, fb,
                 EPD_DRAW_ALIGN_CENTER, 0x00);
        if (!p.label[0]) continue;
        // The distance on a chip beside it, on whichever side has room and
        // is not where the pointer goes.
        const int tw = ui::textWidth(&Arial_B, p.label);
        const int gap = R + 8;
        bool left = p.x + gap + tw + 12 > 540 || ux > 0.3f;
        if (left && p.x - gap - tw - 12 < 0) left = false;
        const int cx = left ? p.x - gap - tw - 12 : p.x + gap;
        epd_fill_rect({cx, p.y - 16, tw + 12, 32}, 0xFF, fb);
        for (int k = 0; k < 2; ++k)
            epd_draw_rect({cx + k, p.y - 16 + k, tw + 12 - 2 * k, 32 - 2 * k}, 0x00, fb);
        ui::text(&Arial_B, cx + 6, p.y + epdc_digit_height(&Arial_B) / 2,
                 p.label, fb, EPD_DRAW_ALIGN_LEFT, 0x00);
    }
}

void drawRider(int x, int y, float headingDeg, uint8_t* fb) {
    float rad = (headingDeg - 90.0f) * (float)M_PI / 180.0f;
    float c = cosf(rad), s = sinf(rad);
    auto rot = [&](float lx, float ly, int& ox, int& oy) {
        ox = x + lroundf(lx * c - ly * s);
        oy = y + lroundf(lx * s + ly * c);
    };
    int nx, ny, lx, ly, rx, ry, bx, by;
    rot(17, 0, nx, ny);
    rot(-11, -10, lx, ly);
    rot(-11, 10, rx, ry);
    rot(-5, 0, bx, by);

    // DOBA: a small marker that hides little of the map — a white disc just
    // big enough to carry the arrow, a ring round it.
    epd_fill_circle(x, y, 20, 0xFF, fb);
    for (int r = 19; r <= 21; ++r) epd_draw_circle(x, y, r, 0x00, fb);
    epd_fill_triangle(nx, ny, lx, ly, bx, by, 0x00, fb);
    epd_fill_triangle(nx, ny, rx, ry, bx, by, 0x00, fb);
}

void drawScaleBar(float metersPerPixel, bool miles, uint8_t* fb) {
    int x = 20, y = MAP_BOTTOM - 26;
    char buf[16];
    float barM;
    if (miles) {
        // Round imperial rungs (metres, label): feet then miles.
        struct Rung { float m; const char* lbl; };
        static const Rung rungs[] = {
            {30.48f, "100 FT"}, {76.2f, "250 FT"}, {152.4f, "500 FT"},
            {304.8f, "1000 FT"}, {804.67f, "0.5 MI"}, {1609.34f, "1 MI"},
            {3218.69f, "2 MI"}};
        const Rung* pick = &rungs[0];
        for (const Rung& r : rungs) if (r.m / metersPerPixel <= 160) pick = &r;
        barM = pick->m;
        snprintf(buf, sizeof(buf), "%s", pick->lbl);
    } else {
        // DOBA: topo zooms out to ~200 m/px, so the rungs go on to 20 km.
        const int targets[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
        int meters = targets[0];
        for (int t : targets) if (t / metersPerPixel <= 160) meters = t;
        barM = meters;
        if (meters >= 1000) snprintf(buf, sizeof(buf), TR("%d km"), meters / 1000);
        else snprintf(buf, sizeof(buf), "%d m", meters);
    }
    int px = (int)(barM / metersPerPixel);
    // DOBA: on a white chip, so the map under it cannot eat it
    const int tw = ui::textWidth(ui::font(ui::F_SMALL), buf);
    ui::fillRound({x - 8, y - 34, std::max(px, tw) + 16, 46}, 6, 0xFF, fb);
    epd_fill_rect({x, y, px, 4}, 0x00, fb);
    epd_fill_rect({x, y - 6, 2, 10}, 0x00, fb);
    epd_fill_rect({x + px - 2, y - 6, 2, 10}, 0x00, fb);
    ui::text(ui::font(ui::F_SMALL), x, y - 10, buf, fb, EPD_DRAW_ALIGN_LEFT, 0x00);
}

void drawCompass(int cx, int cy, float northDeg, bool trackUp, uint8_t* fb) {
    epd_fill_circle(cx, cy, 28, 0xFF, fb);
    for (int r = 26; r <= 28; ++r) epd_draw_circle(cx, cy, r, 0x00, fb);

    // Needle points to true north.
    float a = northDeg * (float)M_PI / 180.0f;
    float nxd = sinf(a), nyd = -cosf(a);   // unit vector toward north
    float pxd = -nyd, pyd = nxd;           // perpendicular
    int tipX = cx + lroundf(nxd * 19), tipY = cy + lroundf(nyd * 19);
    int b1X = cx + lroundf(pxd * 7), b1Y = cy + lroundf(pyd * 7);
    int b2X = cx - lroundf(pxd * 7), b2Y = cy - lroundf(pyd * 7);
    epd_fill_triangle(tipX, tipY, b1X, b1Y, b2X, b2Y, 0x00, fb);

    if (trackUp) {
        // underline marks track-up mode
        epd_fill_rect({cx - 12, cy + 34, 24, 3}, 0x00, fb);
    }
}

}  // namespace

const MapTouchZones kMapZoom = {540 - 78, 560, 640, 76, 560 - 76 - 8};
int mapCentreY() { return kMapZoom.layersY - kMapZoom.size - 8; }
const MapCompassZone kMapCompass = {540 - 46, 64 + 48, 34};
// Bottom edge of the turn-by-turn banner (ui_dashboard's kNavBanner is
// {0, STATUS_H, 540, 138}); the compass drops below this while navigating.
constexpr int kNavBannerBottom = 64 + 138;

int mapCompassCy(bool navBannerVisible) {
    return navBannerVisible ? kNavBannerBottom + kMapCompass.r + 14
                            : kMapCompass.cy;
}

void ui_map_draw_zoom_button(bool zoomIn, bool pressed, uint8_t* fb) {
    EpdRect r = {kMapZoom.zoomX, zoomIn ? kMapZoom.zoomInY : kMapZoom.zoomOutY,
                 kMapZoom.size, kMapZoom.size};
    const uint8_t face = pressed ? 0x00 : 0xFF;   // pressed = filled ink
    const uint8_t mark = pressed ? 0xFF : 0x00;
    ui::frameRound(r, 10, 2, fb, 0x00, face);   // DOBA: rounded, like every button
    int cx = r.x + r.width / 2, cy = r.y + r.height / 2;
    epd_fill_rect({cx - 13, cy - 2, 26, 4}, mark, fb);
    if (zoomIn) epd_fill_rect({cx - 2, cy - 13, 4, 26}, mark, fb);
}

// MAP LAYERS button: same box as the zoom buttons, stacked above them, with a
// "stack of sheets" glyph — two offset outlined squares, the front one filled.
static void drawLayersButton(uint8_t* fb) {
    // DOBA: rounded like the zoom buttons; opens the map options and legend.
    EpdRect r = {kMapZoom.zoomX, kMapZoom.layersY, kMapZoom.size, kMapZoom.size};
    ui::frameRound(r, 10, 2, fb, 0x00, 0xFF);
    const int cx = r.x + r.width / 2, cy = r.y + r.height / 2;
    // two stacked sheets: the back one outlined, the front one solid
    ui::frameRound({cx - 10, cy - 18, 28, 24}, 4, 2, fb, 0x00, 0xFF);
    ui::fillRound({cx - 19, cy - 6, 30, 26}, 5, 0xFF, fb);
    ui::fillRound({cx - 17, cy - 4, 26, 22}, 4, 0x00, fb);
}



void ui_render_map_base(const MapScreenData& map, uint8_t* fb) {
    // The topo tiles are the whole base layer; without a pack the paper stays blank.
    if (map.topo) topo::drawBase(map, fb);
}

namespace {
// What goes over the base layer, redrawn on every frame.
void renderMapTop(const MapScreenData& map, uint8_t* fb) {
    if (map.riderX > -30 && map.riderX < 570 && map.riderY > ui::STATUS_H - 30 && map.riderY < 990)
        drawRider(map.riderX, map.riderY, map.headingDeg, fb);   // DOBA: dragged far off, it isn't on screen
}
}  // namespace

void ui_render_map_features(const MapScreenData& map, const DeviceState& s, uint8_t* fb) {
    (void)s;
    ui_render_map_base(map, fb);
    renderMapTop(map, fb);
}

void ui_render_map(const MapScreenData& map, const DeviceState& s, uint8_t* fb,
                   bool baseDrawn, void (*onBase)(const uint8_t* fb)) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();
    char buf[32];

    if (!baseDrawn) {
        ui_render_map_base(map, fb);
        if (onBase) onBase(fb);
    }
    if (milspec::gridOn()) milspec::drawGrid(map, fb);   // DOBA: km grid under route + rider
    renderMapTop(map, fb);
    milspec::drawPois(map, fb);   // DOBA: hospitals, water, food...
    milspec::drawTarget(map, s, fb);   // DOBA: the active waypoint
    if (map.panned) milspec::drawMeasure(map, fb);   // DOBA: A -> the cross
    drawScaleBar(map.metersPerPixel, false, fb);
    // The turn banner occupies the top of the map, exactly where the compass
    // lives (compass spans y 78..146; the banner runs to y 202), so the north
    // indicator was drawn underneath it and invisible while navigating. Drop it
    // clear of the banner instead of hiding it.
    drawCompass(kMapCompass.cx, mapCompassCy(map.navBannerVisible) + 56,   // DOBA: below the header band
                map.northDeg, map.trackUp, fb);

    // No map covers this position — tell the rider how to get one instead of
    // showing a blank screen.
    if (!map.topo) {
        // Left of the zoom column, never under it.
        const int bx = 16, bw = W - 16 - 96, bh = 96, by = (H - bh) / 2 - 30;
        epd_fill_rect({bx, by, bw, bh}, 0xFF, fb);
        for (int i = 0; i < 2; ++i)
            epd_draw_rect({bx - i, by - i, bw + 2 * i, bh + 2 * i}, 0x00, fb);
        // Downloaded maps live on the card, so without one the app's advice
        // would not help: say what is actually missing.
        const char* head = "NO MAP HERE";
        const char* sub = "This area is not on the card";
        if (s.sdCard == SD_CARD_NONE) {
            head = "NO SD CARD";
            sub = "Insert an exFAT card for maps";
        } else if (s.sdCard == SD_CARD_UNREADABLE) {
            head = "SD CARD UNREADABLE";
            sub = "Reformat it as exFAT";
        }
        ui::text(&Arial_B, bx + bw / 2, by + 38, TR(head), fb, EPD_DRAW_ALIGN_CENTER,
                 0x00);
        ui::text(&Arial_L, bx + bw / 2, by + 70, TR(sub), fb, EPD_DRAW_ALIGN_CENTER,
                 0x00);
    }

    // Over the NO MAP box: where the group is matters most when there is no
    // map to read, and the edge arrows sit in the box's rows.
    drawPeers(map, fb);

    // Zoom buttons (design 1f, right edge). At the end of its range a button
    // goes grey: a tap that cannot do anything should look like it.
    ui_map_draw_zoom_button(true, false, fb);
    drawLayersButton(fb);   // DOBA: map options (grid, points, places, legend)
    ui_map_draw_zoom_button(false, false, fb);
    for (int k = 0; k < 2; ++k) {
        if (!(k ? map.zoomOutEnd : map.zoomInEnd)) continue;
        const EpdRect r = {kMapZoom.zoomX + 3, (k ? kMapZoom.zoomOutY : kMapZoom.zoomInY) + 3,
                           kMapZoom.size - 6, kMapZoom.size - 6};
        for (int y = r.y; y < r.y + r.height; ++y)
            for (int x = r.x + ((y & 1)); x < r.x + r.width; x += 2) epd_draw_pixel(x, y, 0xFF, fb);
    }
    // DOBA: dragged away from where you are: CENTRE brings the map back.
    if (map.panned) {
        const EpdRect r = {kMapZoom.zoomX, mapCentreY(), kMapZoom.size, kMapZoom.size};
        ui::fillRound(r, 10, 0x00, fb);
        const int cx = r.x + r.width / 2, cy = r.y + r.height / 2;
        for (int t = 0; t < 3; ++t) epd_draw_circle(cx, cy, 15 - t, 0xFF, fb);
        epd_fill_circle(cx, cy, 5, 0xFF, fb);
        epd_fill_rect({cx - 2, cy - 26, 4, 10}, 0xFF, fb);
        epd_fill_rect({cx - 2, cy + 16, 4, 10}, 0xFF, fb);
        epd_fill_rect({cx - 26, cy - 2, 10, 4}, 0xFF, fb);
        epd_fill_rect({cx + 16, cy - 2, 10, 4}, 0xFF, fb);
    }

    // Status bar drawn after the map (epdiy has no clipping)
    epd_fill_rect({0, 0, W, ui::STATUS_H - 3}, 0xFF, fb);
    ui::statusBarBack(s, fb, TR("Map"));   // DOBA: BACK to home like every page

    // DOBA: a sheet-style header over the map: the place, how far from
    // it, and the scale as on a paper map (1:N at this screen's pixel pitch).
    {
        const int y0 = ui::STATUS_H, hb = 52;
        epd_fill_rect({0, y0, W, hb}, ui::PAPER, fb);
        epd_fill_rect({0, y0 + hb - 1, W, 1}, ui::HAIR, fb);
        char scale[24];
        const long ratio = lroundf(map.metersPerPixel / 0.000108f / 500.0f) * 500;   // 235 dpi panel
        if (ratio >= 1000) snprintf(scale, sizeof scale, "1:%ld %03ld", ratio / 1000, ratio % 1000);
        else snprintf(scale, sizeof scale, "1:%ld", ratio);
        ui::text(ui::font(ui::F_SMALL), W - 24, y0 + 22, scale, fb, EPD_DRAW_ALIGN_RIGHT, ui::INK);
        {   // the grid spacing and the 100 km square letters, as on a sheet's margin
            char sq[4], gl[40];
            milspec::square(map.centerLat, map.centerLon, sq, sizeof sq);
            snprintf(gl, sizeof gl, "%s%s%s", TR(map.metersPerPixel < 20.0f ? "grid 1 km" : map.metersPerPixel < 200.0f ? "grid 10 km" : "grid 100 km"),
                     sq[0] ? TR(" \xC2\xB7 square ") : "", sq);
            ui::text(ui::font(ui::F_TINY), W - 24, y0 + 44, gl, fb, EPD_DRAW_ALIGN_RIGHT, ui::GREY);
        }
        const int room = W - 24 - ui::textWidth(ui::font(ui::F_SMALL), scale) - 16;
        if (map.place[0]) {
            char up[40];
            snprintf(up, sizeof up, "%s", map.place);
            const EpdFont* pf = ui::font(ui::F_LABEL);
            if (ui::textWidth(pf, up) > room - 10) pf = ui::font(ui::F_SMALL);
            ui::text(pf, 24, y0 + 24, up, fb, EPD_DRAW_ALIGN_LEFT, ui::INK);
            if (map.placeSub[0]) {
                char sub[40];
                snprintf(sub, sizeof sub, "%s", map.placeSub);
                ui::capFirst(sub);
                ui::text(ui::font(ui::F_TINY), 24, y0 + 44, sub, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
            }
        } else {
            ui::text(ui::font(ui::F_SMALL), 24, y0 + 34, TR("Topo map"), fb, EPD_DRAW_ALIGN_LEFT, ui::INK);
        }
    }

    if (map.panned) milspec::drawCross(fb);
    // DOBA: the footer is the position readout, on every kind of map (the
    // vector map drew the bike strip: speed, distance, ride time).
    milspec::drawStrip(map, s, fb);
}

