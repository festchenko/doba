// DOBA: military-style overlay for the topo map — see milspec.h.
#include "milspec.h"

#include <Preferences.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "epd_compat.h"
#include "map_view.h"
#include "milgeo.h"
#include "ride_state.h"
#include "topo_layer.h"
#include "ui_render.h"

#include "fonts/doba_mono_l.h"
#include "fonts/doba_mono_m.h"
#include "fonts/doba_mono_s.h"

namespace milspec {
namespace {

constexpr int SCREEN_W = 540;
constexpr uint8_t GRID = 0x22;        // dark grey: reads on every topo fill
constexpr uint8_t GRID_INDEX = 0x00;  // every 10th line, black
constexpr int SAMPLES = 24;           // points per grid line (UTM lines curve)
int g_mils = 0;                       // 0 = not loaded yet

// Web Mercator for the topo view, the projection the tiles are drawn in.
struct View {
    double world, rcx, rcy, scale;
    int rx, ry;
    void toScreen(double lat, double lon, double& x, double& y) const {
        const double wx = (lon + 180.0) / 360.0 * world;
        const double wy = (1.0 - asinh(tan(lat * M_PI / 180.0)) / M_PI) / 2.0 * world;
        x = rx + (wx - rcx) / scale;
        y = ry + (wy - rcy) / scale;
    }
    void toGeo(double x, double y, double& lat, double& lon) const {
        const double wx = rcx + (x - rx) * scale;
        const double wy = rcy + (y - ry) * scale;
        lon = wx / world * 360.0 - 180.0;
        lat = atan(sinh(M_PI * (1.0 - 2.0 * wy / world))) * 180.0 / M_PI;
    }
};

View viewFor(const MapScreenData& map) {
    View v;
    const int z = topo::zoomFor(map.centerLat, map.metersPerPixel);
    v.world = 256.0 * (double)(1u << z);
    v.rcx = (map.centerLon + 180.0) / 360.0 * v.world;
    v.rcy = (1.0 - asinh(tan(map.centerLat * M_PI / 180.0)) / M_PI) / 2.0 * v.world;
    v.scale = map.metersPerPixel / topo::nativeMpp(z, map.centerLat);
    v.rx = map.riderX;
    v.ry = map.riderY;
    return v;
}

void labelBox(int cx, int cy, const char* s, uint8_t* fb) {
    const int w = ui::textWidth(&Mono_S, s) + 8;
    const int h = Mono_S.ascender + 6;
    const EpdRect r = {cx - w / 2, cy - h / 2, w, h};
    epd_fill_rect(r, 0xFF, fb);
    epd_draw_rect(r, 0x00, fb);
    ui::text(&Mono_S, cx, cy + Mono_S.ascender / 2 - 1, s, fb, EPD_DRAW_ALIGN_CENTER, 0x00);
}

}  // namespace

int milsSystem() {
    if (!g_mils) {
        Preferences p;
        p.begin("doba", true);
        g_mils = p.getInt("mils", 6000);
        p.end();
        if (g_mils != 6000 && g_mils != 6400) g_mils = 6000;
    }
    return g_mils;
}

void setMilsSystem(int system) {
    if (system != 6000 && system != 6400) return;
    g_mils = system;
    Preferences p;
    p.begin("doba", false);
    p.putInt("mils", system);
    p.end();
}

void drawGrid(const MapScreenData& map, uint8_t* fb) {
    const View v = viewFor(map);
    const int top = ui::STATUS_H, bottom = ui::MAP_STRIP_TOP;
    const double spacing = map.metersPerPixel < 20.0f ? 1000.0
                         : map.metersPerPixel < 200.0f ? 10000.0 : 100000.0;
    const int zone = milgeo::toUtm(map.centerLat, map.centerLon).zone;

    // The grid's extent: UTM of the map area's corners and edge midpoints.
    double minE = 1e12, maxE = -1e12, minN = 1e12, maxN = -1e12;
    const int xs[3] = {0, SCREEN_W / 2, SCREEN_W - 1};
    const int ys[3] = {top, (top + bottom) / 2, bottom - 1};
    bool north = true;
    for (int xi : xs) for (int yi : ys) {
        double lat, lon;
        v.toGeo(xi, yi, lat, lon);
        const milgeo::Utm u = milgeo::toUtmInZone(lat, lon, zone);
        north = u.north;
        if (u.easting < minE) minE = u.easting;
        if (u.easting > maxE) maxE = u.easting;
        if (u.northing < minN) minN = u.northing;
        if (u.northing > maxN) maxN = u.northing;
    }
    minE = floor(minE / spacing) * spacing; maxE = ceil(maxE / spacing) * spacing;
    minN = floor(minN / spacing) * spacing; maxN = ceil(maxN / spacing) * spacing;
    if ((maxE - minE) / spacing > 60 || (maxN - minN) / spacing > 60) return;

    struct Label { int x, y; char t[6]; };
    Label labels[64];
    int nl = 0;

    // Easting lines (run north-south), then northing lines (east-west).
    for (int pass = 0; pass < 2; ++pass) {
        const double from = pass == 0 ? minE : minN, to = pass == 0 ? maxE : maxN;
        const double along0 = pass == 0 ? minN : minE, along1 = pass == 0 ? maxN : maxE;
        for (double g = from; g <= to + 0.5; g += spacing) {
            const bool index = spacing == 1000.0 && fmod(g, 10000.0) < 0.5;
            const uint8_t col = index ? GRID_INDEX : GRID;
            double px = 0, py = 0;
            bool have = false, labelled = false;
            for (int i = 0; i <= SAMPLES; ++i) {
                const double a = along0 + (along1 - along0) * i / SAMPLES;
                double lat, lon, x, y;
                if (pass == 0) milgeo::fromUtm(zone, north, g, a, lat, lon);
                else           milgeo::fromUtm(zone, north, a, g, lat, lon);
                v.toScreen(lat, lon, x, y);
                if (have) {
                    epd_draw_line((int)px, (int)py, (int)x, (int)y, col, fb);
                    if (index) epd_draw_line((int)px + 1, (int)py, (int)x + 1, (int)y, col, fb);
                    // Label where the line crosses the map's top edge (eastings)
                    // or left edge (northings), like the margin of a map sheet.
                    if (!labelled && nl < 64) {
                        const double edge = pass == 0 ? top + 14 : 18;
                        const double p0 = pass == 0 ? py : px, p1 = pass == 0 ? y : x;
                        if ((p0 - edge) * (p1 - edge) <= 0 && p0 != p1) {
                            const double t = (edge - p0) / (p1 - p0);
                            Label& L = labels[nl];
                            L.x = (int)(pass == 0 ? px + (x - px) * t : edge);
                            L.y = (int)(pass == 0 ? edge : py + (y - py) * t);
                            const long km = (long)llround(g / 1000.0) % 100;
                            snprintf(L.t, sizeof L.t, "%02ld", km);
                            const bool clear = pass == 0
                                ? (L.x > 16 && L.x < 430)                 // compass, zoom keys
                                : (L.y > top + 30 && L.y < bottom - 60);  // scale bar
                            if (clear) ++nl;
                            labelled = true;
                        }
                    }
                }
                px = x; py = y; have = true;
            }
        }
    }
    for (int i = 0; i < nl; ++i) labelBox(labels[i].x, labels[i].y, labels[i].t, fb);
}

void drawStrip(const MapScreenData& map, const RideState& s, uint8_t* fb) {
    const int W = SCREEN_W, H = 960, top = ui::MAP_STRIP_TOP;
    epd_fill_rect({0, top, W, H - top}, 0xFF, fb);
    epd_fill_rect({0, top, W, 3}, 0x00, fb);

    const bool fix = s.gpsFix;
    const double lat = fix || s.everHadFix ? s.latitude : map.centerLat;
    const double lon = fix || s.everHadFix ? s.longitude : map.centerLon;
    const uint8_t ink = fix ? 0x00 : 0x77;   // stale position reads grey
    const int x0 = 14;

    char m[32];
    milgeo::mgrs(lat, lon, m, sizeof m);
    ui::text(&Mono_L, x0, top + 46, m[0] ? m : "MGRS --", fb, EPD_DRAW_ALIGN_LEFT, ink);

    char ll[48];
    snprintf(ll, sizeof ll, "%c%08.5f  %c%09.5f", lat >= 0 ? 'N' : 'S', fabs(lat),
             lon >= 0 ? 'E' : 'W', fabs(lon));
    ui::text(&Mono_M, x0, top + 84, ll, fb, EPD_DRAW_ALIGN_LEFT, ink);

    char az[24];
    // Course over ground means nothing standing still: show it from walking pace.
    if (fix && s.gpsSpeedKmh >= 2.0f) {
        char mil[8];
        milgeo::mils(s.courseDeg, milsSystem(), mil, sizeof mil);
        snprintf(az, sizeof az, "AZ %s %03d\xC2\xB0", mil, (int)lroundf(s.courseDeg) % 360);
    } else {
        snprintf(az, sizeof az, "AZ --");
    }
    char row[64];
    if (fix) snprintf(row, sizeof row, "ALT %4dm  %s  SAT %d", (int)lroundf(s.altitudeM), az, s.satellites);
    else     snprintf(row, sizeof row, "NO FIX  SAT %d", s.satellites);
    ui::text(&Mono_M, x0, top + 122, row, fb, EPD_DRAW_ALIGN_LEFT, 0x00);
    (void)map;
}

}  // namespace milspec
