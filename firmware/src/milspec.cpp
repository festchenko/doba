// DOBA: military-style overlay for the topo map — see milspec.h.
#include "milspec.h"
#include "doba_points.h"
#include "doba_dem.h"
#include "doba_pois.h"

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
    const EpdFont* f = ui::monoFont(12);
    const int w = ui::textWidth(f, s) + 12;
    const int h = 24;
    const EpdRect r = {cx - w / 2, cy - h / 2, w, h};
    ui::frameRound(r, 5, 1, fb, 0x00, 0xFF);
    ui::text(f, cx, cy + 6, s, fb, EPD_DRAW_ALIGN_CENTER, 0x00);
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

namespace {
bool flag(const char* key, bool def, int8_t& cache) {
    if (cache < 0) {
        Preferences p;
        p.begin("doba", true);
        cache = p.getBool(key, def) ? 1 : 0;
        p.end();
    }
    return cache == 1;
}
void setFlag(const char* key, bool on, int8_t& cache) {
    cache = on ? 1 : 0;
    Preferences p;
    p.begin("doba", false);
    p.putBool(key, on);
    p.end();
}
int8_t g_grid = -1, g_points = -1, g_pois = -1;
// Measure: point A (lat 99 = none).
double g_aLat = 99, g_aLon = 0;
double g_vLat = 50, g_vLon = 30;   // the view's middle, last frame
// The last frame's projection, for long-press -> point.
struct Frame { bool ok; int rx, ry; float mpp, north; double lat, lon; } g_frame = {};
}  // namespace

bool gridOn() { return flag("grid", true, g_grid); }
void setGridOn(bool on) { setFlag("grid", on, g_grid); }
bool pointsOn() { return flag("pts", true, g_points); }
void setPointsOn(bool on) { setFlag("pts", on, g_points); }
bool poisOn() { return flag("pois", true, g_pois); }
void setPoisOn(bool on) { setFlag("pois", on, g_pois); }

bool measureOn() { return g_aLat < 90; }
void measureClear() { g_aLat = 99; }
void measureToggleAtCross() {
    if (measureOn()) { measureClear(); return; }
    double lat, lon;
    if (screenToLatLon(SCREEN_W / 2, kCrossY, lat, lon)) { g_aLat = lat; g_aLon = lon; }
}

void square(double lat, double lon, char* out, size_t n) {
    char m[32];
    milgeo::mgrs(lat, lon, m, sizeof m);
    const char* sp = strchr(m, ' ');
    if (!sp || n < 3) { if (n) out[0] = 0; return; }
    snprintf(out, n, "%.2s", sp + 1);
}

void contourInterval(double lat, float mpp, bool flat, float& every, float& index) {
    // maps/style.py CONTOUR_INTERVALS(_FLAT), by the tile zoom drawn
    int z = topo::zoomFor(lat, mpp);
    if (z < 11) z = 11;
    if (z > 16) z = 16;
    static const float kN[6][2] = {{100, 500}, {50, 250}, {20, 100}, {10, 50}, {10, 50}, {10, 50}};
    static const float kF[6][2] = {{20, 100}, {10, 50}, {5, 25}, {2.5f, 10}, {2.5f, 10}, {2.5f, 10}};
    const float(*t)[2] = flat ? kF : kN;
    every = t[z - 11][0];
    index = t[z - 11][1];
}

const char* poiCode(int kind) {
    static const char* const k[doba_pois::KINDS] = {
        "", "H", "Dr", "Rx", "W", "F", "Fm", "Sh", "B", "P", "Po", "Fi", "A", "T", "O", "C", "As", "UP"};
    return kind > 0 && kind < doba_pois::KINDS ? k[kind] : "?";
}

void drawPois(const MapScreenData& map, uint8_t* fb) {
    if (!poisOn() || map.metersPerPixel <= 0 || map.metersPerPixel > 25.0f) return;
    // The SD scan is not per frame: the nearest few hundred round the view's
    // middle, again only when the view has moved a good part of a screen.
    static doba_pois::Poi cache[60];
    static int cn = -1;
    static double qLat = 99, qLon = 0;
    static float qMpp = 0;
    double b, moved;
    milgeo::bearingDistance(qLat, qLon, map.centerLat, map.centerLon, b, moved);
    if (cn < 0 || qLat > 90 || moved > 150.0 * map.metersPerPixel || qMpp != map.metersPerPixel) {
        cn = doba_pois::nearest(map.centerLat, map.centerLon, 0, cache, 60);
        qLat = map.centerLat; qLon = map.centerLon; qMpp = map.metersPerPixel;
    }
    if (cn <= 0) return;
    const View v = viewFor(map);
    const int top = ui::STATUS_H + (map.topo ? 56 : 0), bottom = ui::MAP_STRIP_TOP;
    const EpdFont* f = ui::monoFont(13);
    for (int i = cn - 1; i >= 0; --i) {   // nearest drawn last, on top
        double x, y;
        v.toScreen(cache[i].lat, cache[i].lon, x, y);
        if (map.panned == false && map.northDeg != 0.0f) {
            // course-up: turn about the rider
            const double a = map.northDeg * M_PI / 180.0, dx = x - map.riderX, dy = y - map.riderY;
            x = map.riderX + dx * cos(a) - dy * sin(a);
            y = map.riderY + dx * sin(a) + dy * cos(a);
        }
        const char* c = poiCode(cache[i].kind);
        const int w = ui::textWidth(f, c) + 12, h = 24;
        const int bx = (int)x - w / 2, by = (int)y - h / 2;
        if (bx < 2 || bx + w > SCREEN_W - 80 || by < top + 2 || by + h > bottom - 2) continue;
        ui::fillRound({bx - 2, by - 2, w + 4, h + 4}, 7, 0xFF, fb);
        ui::fillRound({bx, by, w, h}, 6, 0x00, fb);
        ui::text(f, (int)x, by + 18, c, fb, EPD_DRAW_ALIGN_CENTER, 0xFF);
        if (map.metersPerPixel < 4.0f && cache[i].name[0]) {
            char nm[24];
            snprintf(nm, sizeof nm, "%.20s", cache[i].name);
            const EpdFont* nf = ui::font(ui::F_TINY);
            const int tw = ui::textWidth(nf, nm) + 8;
            if (bx + w + 4 + tw < SCREEN_W - 80) {
                epd_fill_rect({bx + w + 2, by + 2, tw, 20}, 0xFF, fb);
                ui::text(nf, bx + w + 6, by + 17, nm, fb, EPD_DRAW_ALIGN_LEFT, 0x00);
            }
        }
    }
}

void drawMeasure(const MapScreenData& map, uint8_t* fb) {
    if (!measureOn() || !g_frame.ok) return;
    const View v = viewFor(map);
    double ax, ay;
    v.toScreen(g_aLat, g_aLon, ax, ay);
    const int top = ui::STATUS_H + (map.topo ? 52 : 0), bottom = ui::MAP_STRIP_TOP;
    // A dashed line from A to the cross
    const double cx = SCREEN_W / 2, cy = kCrossY, len = hypot(cx - ax, cy - ay);
    for (double k = 0; k < len - 30; k += 1.0) {
        if (fmod(k, 18.0) > 10.0) continue;
        const int x = (int)lround(ax + (cx - ax) * k / len), y = (int)lround(ay + (cy - ay) * k / len);
        if (y < top + 2 || y > bottom - 3 || x < 1 || x > SCREEN_W - 2) continue;
        epd_fill_rect({x - 1, y - 1, 3, 3}, 0x00, fb);
    }
    const int x = (int)ax, y = (int)ay;
    if (x < 16 || x > SCREEN_W - 16 || y < top + 16 || y > bottom - 16) return;
    epd_fill_circle(x, y, 15, 0xFF, fb);
    epd_fill_circle(x, y, 12, 0x00, fb);
    ui::text(ui::font(ui::F_STRONG), x, y + 6, "A", fb, EPD_DRAW_ALIGN_CENTER, 0xFF);
}

bool lastView(double& lat, double& lon, float& mpp) {
    if (!g_frame.ok) return false;
    lat = g_vLat; lon = g_vLon; mpp = g_frame.mpp;
    return true;
}

bool screenToLatLon(int x, int y, double& lat, double& lon) {
    if (!g_frame.ok) return false;
    const double dx = x - g_frame.rx, dy = y - g_frame.ry;
    const double d = hypot(dx, dy) * g_frame.mpp;
    const double brg = (atan2(dx, -dy) * 180.0 / M_PI - g_frame.north) * M_PI / 180.0;
    const double R = 6371008.8, la1 = g_frame.lat * M_PI / 180.0, lo1 = g_frame.lon * M_PI / 180.0;
    const double la2 = asin(sin(la1) * cos(d / R) + cos(la1) * sin(d / R) * cos(brg));
    const double lo2 = lo1 + atan2(sin(brg) * sin(d / R) * cos(la1), cos(d / R) - sin(la1) * sin(la2));
    lat = la2 * 180.0 / M_PI;
    lon = lo2 * 180.0 / M_PI;
    return true;
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
                        const double edge = pass == 0 ? top + (map.topo ? 52 : 0) + 16 : 18;
                        const double p0 = pass == 0 ? py : px, p1 = pass == 0 ? y : x;
                        if ((p0 - edge) * (p1 - edge) <= 0 && p0 != p1) {
                            const double t = (edge - p0) / (p1 - p0);
                            Label& L = labels[nl];
                            L.x = (int)(pass == 0 ? px + (x - px) * t : edge);
                            L.y = (int)(pass == 0 ? edge : py + (y - py) * t);
                            const long km = (long)llround(g / 1000.0) % 100;
                            snprintf(L.t, sizeof L.t, "%02ld", km);
                            const bool clear = pass == 0
                                ? (L.x > 34 && L.x < 440)                 // northing labels, compass
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

void drawTarget(const MapScreenData& map, const RideState& s, uint8_t* fb) {
    const bool have = s.gpsFix || s.everHadFix;
    const double lat = have ? s.latitude : map.centerLat;
    const double lon = have ? s.longitude : map.centerLon;
    if (map.metersPerPixel <= 0) return;
    g_frame = {true, map.riderX, map.riderY, map.metersPerPixel, map.northDeg, lat, lon};
    g_vLat = map.centerLat; g_vLon = map.centerLon;
    const int top0 = ui::STATUS_H, bottom0 = ui::MAP_STRIP_TOP;
    const int t = doba_points::target();
    // Every waypoint: a small open triangle, its name when zoomed in.
    if (pointsOn()) {
        for (int i = 0; i < doba_points::count(); ++i) {
            if (i == t) continue;
            const doba_points::Point* q = doba_points::at(i);
            double b, d;
            milgeo::bearingDistance(lat, lon, q->lat, q->lon, b, d);
            const double a = (b + map.northDeg) * M_PI / 180.0, px = d / map.metersPerPixel;
            const int x = map.riderX + (int)lround(sin(a) * px), y = map.riderY - (int)lround(cos(a) * px);
            if (x < 12 || x > SCREEN_W - 12 || y < top0 + 14 || y > bottom0 - 12) continue;
            for (int k = 0; k < 3; ++k) {
                epd_draw_line(x, y - 12 + k, x - 10 + k, y + 7 - k, 0x00, fb);
                epd_draw_line(x - 10 + k, y + 7 - k, x + 10 - k, y + 7 - k, 0x00, fb);
                epd_draw_line(x + 10 - k, y + 7 - k, x, y - 12 + k, 0x00, fb);
            }
            if (map.metersPerPixel < 40) {
                const int w = ui::textWidth(&Mono_M, q->name) + 8;
                epd_fill_rect({x + 13, y - 12, w, 24}, 0xFF, fb);
                ui::text(&Mono_M, x + 17, y + 6, q->name, fb, EPD_DRAW_ALIGN_LEFT, 0x00);
            }
        }
    }
    const doba_points::Point* p = doba_points::at(t);
    if (!p) return;
    double b, d;
    milgeo::bearingDistance(lat, lon, p->lat, p->lon, b, d);
    const double a = (b + map.northDeg) * M_PI / 180.0;
    const double px = d / map.metersPerPixel;
    const double tx = map.riderX + sin(a) * px, ty = map.riderY - cos(a) * px;
    const int top = ui::STATUS_H, bottom = ui::MAP_STRIP_TOP, W = SCREEN_W;
    // Dashed, 3 px, stopping short of the rider and of the mark; clipped to the map.
    const double len = px, ux = sin(a), uy = -cos(a);
    for (double k = 18; k < len - 20; k += 1.0) {
        if (fmod(k, 22.0) > 13.0) continue;
        const int x = map.riderX + (int)lround(ux * k), y = map.riderY + (int)lround(uy * k);
        if (y < top + 2 || y > bottom - 3 || x < 1 || x > W - 2) {
            if (k > 40) break;
            continue;
        }
        epd_fill_rect({x - 1, y - 1, 3, 3}, 0x00, fb);
    }
    const bool on = tx > 20 && tx < W - 20 && ty > top + 20 && ty < bottom - 20;
    if (!on) {
        // Off the map: a tag where the line leaves it — name and distance.
        double k = 0;
        for (double kk = 18; kk < len; kk += 2) {
            const double x = map.riderX + ux * kk, y = map.riderY + uy * kk;
            if (x < 30 || x > W - 30 || y < top + 30 || y > bottom - 30) break;
            k = kk;
        }
        char tag[40], ds[16];
        doba_points::formatDistance(d, ds, sizeof ds);
        snprintf(tag, sizeof tag, "%.10s %s", p->name, ds);
        const int tw = ui::textWidth(&Mono_M, tag) + 16;
        int x = map.riderX + (int)lround(ux * k) - tw / 2, y = map.riderY + (int)lround(uy * k) - 16;
        if (x < 4) x = 4;
        if (x + tw > W - 80) x = W - 80 - tw;   // clear of the zoom buttons
        if (y < top + 4) y = top + 4;
        if (y > bottom - 36) y = bottom - 36;
        epd_fill_rect({x, y, tw, 32}, 0x00, fb);
        ui::text(&Mono_M, x + 8, y + 24, tag, fb, EPD_DRAW_ALIGN_LEFT, 0xFF);
    }
    if (on) {
        const int cx = (int)tx, cy = (int)ty;
        epd_fill_circle(cx, cy, 15, 0xFF, fb);
        for (int r = 0; r < 3; ++r) epd_draw_circle(cx, cy, 14 - r, 0x00, fb);
        epd_fill_rect({cx - 22, cy - 1, 44, 3}, 0x00, fb);
        epd_fill_rect({cx - 1, cy - 22, 3, 44}, 0x00, fb);
        const int w = ui::textWidth(&Mono_M, p->name) + 12;
        epd_fill_rect({cx + 20, cy - 44, w, 28}, 0xFF, fb);
        ui::text(&Mono_M, cx + 26, cy - 22, p->name, fb, EPD_DRAW_ALIGN_LEFT, 0x00);
    }
}

void drawCross(uint8_t* fb) {
    const int cx = SCREEN_W / 2, cy = kCrossY, g = 6, L = 30;   // gap in the middle, arm length
    // white under black, so it reads on any map
    epd_fill_rect({cx - L - 2, cy - 4, L - g + 4, 9}, 0xFF, fb);
    epd_fill_rect({cx + g - 2, cy - 4, L - g + 4, 9}, 0xFF, fb);
    epd_fill_rect({cx - 4, cy - L - 2, 9, L - g + 4}, 0xFF, fb);
    epd_fill_rect({cx - 4, cy + g - 2, 9, L - g + 4}, 0xFF, fb);
    epd_fill_rect({cx - L, cy - 2, L - g, 5}, 0x00, fb);
    epd_fill_rect({cx + g, cy - 2, L - g, 5}, 0x00, fb);
    epd_fill_rect({cx - 2, cy - L, 5, L - g}, 0x00, fb);
    epd_fill_rect({cx - 2, cy + g, 5, L - g}, 0x00, fb);
}

// DOBA: the strip under the map, in the calm style: the grid reference big,
// lat/lon in grey under it, then three small facts each with its caption.
void drawStrip(const MapScreenData& map, const RideState& s, uint8_t* fb) {
    const int W = SCREEN_W, H = 960, top = ui::MAP_STRIP_TOP;
    epd_fill_rect({0, top, W, H - top}, 0xFF, fb);
    epd_fill_rect({0, top, W, 1}, ui::HAIR, fb);

    const bool fix = s.gpsFix;
    // Dragged: the readout is for the cross in the middle of the map, so any
    // point on the sheet can be read off; else it is where you stand.
    double clat = map.centerLat, clon = map.centerLon;
    const bool cross = map.panned && screenToLatLon(SCREEN_W / 2, kCrossY, clat, clon);
    const double lat = cross ? clat : fix || s.everHadFix ? s.latitude : map.centerLat;
    const double lon = cross ? clon : fix || s.everHadFix ? s.longitude : map.centerLon;
    const uint8_t ink = fix || cross ? 0x00 : 0x77;   // stale position reads grey
    const int x0 = 24;

    char m[32];
    milgeo::mgrs(lat, lon, m, sizeof m);
    ui::text(ui::monoFont(19), x0, top + 38, m[0] ? m : "MGRS --", fb, EPD_DRAW_ALIGN_LEFT, ink);

    char ll[64];
    const doba_points::Point* tp = doba_points::at(doba_points::target());
    if (tp && (fix || s.everHadFix)) {
        // guiding to a waypoint: its azimuth and distance replace lat/lon
        double b, d;
        milgeo::bearingDistance(lat, lon, tp->lat, tp->lon, b, d);
        char mil[8], ds[16];
        milgeo::mils((float)b, milsSystem(), mil, sizeof mil);
        doba_points::formatDistance(d, ds, sizeof ds);
        snprintf(ll, sizeof ll, "\xE2\x80\xBA %.12s  \xC2\xB7  az %s  \xC2\xB7  %s", tp->name, mil, ds);
        ui::fillRound({x0 - 8, top + 48, W - 2 * x0 + 16, 30}, 6, 0x00, fb);
        ui::text(ui::font(ui::F_SMALL), x0, top + 70, ll, fb, EPD_DRAW_ALIGN_LEFT, 0xFF);
    } else {
        // the short form, as said on the radio (100 m), then lat/lon
        char sh[24] = "";
        {
            char t[32];
            snprintf(t, sizeof t, "%s", m);
            char* tok[4] = {};
            int k = 0;
            for (char* p = strtok(t, " "); p && k < 4; p = strtok(nullptr, " ")) tok[k++] = p;
            if (k == 4) snprintf(sh, sizeof sh, "%s %.3s %.3s", tok[1], tok[2], tok[3]);
        }
        snprintf(ll, sizeof ll, "%s%s%.5f\xC2\xB0 %c  %.5f\xC2\xB0 %c", cross ? "Cross \xC2\xB7 " : "",
                 sh[0] ? "" : "", fabs(lat), lat >= 0 ? 'N' : 'S', fabs(lon), lon >= 0 ? 'E' : 'W');
        if (sh[0]) {
            char say[40];
            snprintf(say, sizeof say, "Say %s", sh);
            ui::text(ui::font(ui::F_SMALL), W - x0, top + 68, say, fb, EPD_DRAW_ALIGN_RIGHT, 0x30);
        }
        ui::text(ui::font(ui::F_SMALL), x0, top + 68, ll, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    }

    // three facts, each under its caption
    char v1[24], v2[24], v3[24];
    const char* c1 = "Height";
    const float alt = s.baroValid ? s.baroAltM : s.mapElevationValid ? s.mapElevationM : s.altitudeM;
    float dep;
    if (fix && doba_dem::depth(s.latitude, s.longitude, dep)) { c1 = "Depth"; snprintf(v1, sizeof v1, "%d m", (int)lroundf(dep)); }
    else if (fix) snprintf(v1, sizeof v1, "%d m", (int)lroundf(alt));
    else snprintf(v1, sizeof v1, "--");
    // course over ground means nothing standing still: shown from walking pace
    if (fix && s.gpsSpeedKmh >= 2.0f) milgeo::mils(s.courseDeg, milsSystem(), v2, sizeof v2);
    else snprintf(v2, sizeof v2, fix ? "standing" : "--");
    if (fix) snprintf(v3, sizeof v3, "%d", s.satellites);
    else snprintf(v3, sizeof v3, "no fix \xC2\xB7 %d", s.satellites);
    const char* caps[3] = {c1, "Course", "Satellites"};
    if (cross) {   // the cross: how far and which way from you (or from A), its height
        const bool fromA = measureOn();
        caps[0] = fromA ? "From A" : "From you";
        caps[1] = "Compass az";
        caps[2] = "Height";
        const bool src = fromA || fix || s.everHadFix;
        if (src) {
            double b, d;
            milgeo::bearingDistance(fromA ? g_aLat : s.latitude, fromA ? g_aLon : s.longitude, lat, lon, b, d);
            doba_points::formatDistance(d, v1, sizeof v1);
            // what to set on a compass: true azimuth less the declination here
            float mb = (float)b - milgeo::declination(lat, lon);
            if (mb < 0) mb += 360.0f;
            milgeo::mils(mb, milsSystem(), v2, sizeof v2);
        } else {
            snprintf(v1, sizeof v1, "--");
            snprintf(v2, sizeof v2, "--");
        }
        float hz, dz;
        if (doba_dem::depth(lat, lon, dz)) { caps[2] = "Depth"; snprintf(v3, sizeof v3, "%d m", (int)lroundf(dz)); }
        else if (doba_dem::height(lat, lon, hz)) snprintf(v3, sizeof v3, "%d m", (int)lroundf(hz));
        else snprintf(v3, sizeof v3, "--");
    }
    const char* vals[3] = {v1, v2, v3};
    const int cols[3] = {x0, x0 + 170, x0 + 330};
    for (int i = 0; i < 3; ++i) {
        ui::text(ui::font(ui::F_TINY), cols[i], top + 104, caps[i], fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        ui::text(ui::font(ui::F_LABEL), cols[i], top + 134, vals[i], fb, EPD_DRAW_ALIGN_LEFT, i == 2 && !fix && !cross ? 0x77 : 0x00);
    }
}

}  // namespace milspec
