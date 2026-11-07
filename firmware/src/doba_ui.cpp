// DOBA: main screen, comms and library pages — see doba_ui.h.
#include "doba_ui.h"
#include "stealth.h"
#include "settings.h"
#include "usb_storage.h"
#include "doba_topo_bg.h"
#include "tz_auto.h"

#include <Arduino.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "doba_keyboard.h"
#include "doba_library.h"
#include "epd_compat.h"
#include "mesh_proto.h"
#include "mesh_service.h"
#include "milgeo.h"
#include "milspec.h"
#include "doba_points.h"
#include "doba_dem.h"
#include "doba_sun.h"
#include <time.h>
#include "ride_state.h"
#include "settings.h"
#include "ui_render.h"

namespace doba_ui {
namespace {

constexpr int W = 540, H = 960;
constexpr int TOP = ui::STATUS_H;
constexpr uint8_t INK = 0x00, PAPER = 0xFF;

kbd::Keyboard g_compose;   // the message being written
kbd::Keyboard g_search;    // the library query

void frame(const EpdRect& r, int t, uint8_t* fb) {
    for (int i = 0; i < t; ++i)
        epd_draw_rect({r.x + i, r.y + i, r.width - 2 * i, r.height - 2 * i}, INK, fb);
}

bool hit(const EpdRect& r, int x, int y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

void button(const EpdRect& r, const char* label, bool dark, uint8_t* fb) {
    if (dark) epd_fill_rect(r, INK, fb);
    else frame(r, 3, fb);
    ui::text(ui::font(ui::F_LABEL), r.x + r.width / 2, r.y + r.height / 2 + 10, label, fb,
             EPD_DRAW_ALIGN_CENTER, dark ? PAPER : INK);
}

// Word-wraps UTF-8 text to maxW; calls emit(line) for each line. Returns lines.
template <typename F>
int wrap(const EpdFont* f, const char* s, int maxW, F emit) {
    char line[256] = {}, word[128] = {}, trial[384];
    int lines = 0;
    auto flushLine = [&]() { emit(line); ++lines; line[0] = 0; };
    while (true) {
        // next word (up to a space or end)
        size_t n = 0;
        while (s[n] && s[n] != ' ' && s[n] != '\n' && n < sizeof(word) - 1) ++n;
        memcpy(word, s, n); word[n] = 0;
        if (line[0]) snprintf(trial, sizeof trial, "%s %s", line, word);
        else snprintf(trial, sizeof trial, "%s", word);
        if (ui::textWidth(f, trial) <= maxW || !line[0]) {
            snprintf(line, sizeof line, "%s", trial);
        } else {
            flushLine();
            snprintf(line, sizeof line, "%s", word);
        }
        s += n;
        if (*s == '\n') { flushLine(); ++s; continue; }
        if (*s == ' ') { ++s; continue; }
        if (!*s) break;
    }
    if (line[0]) flushLine();
    return lines;
}

void clockOf(uint32_t utc, char* out, size_t n) {
    if (!utc) { snprintf(out, n, "--:--"); return; }
    const time_t t = (time_t)utc + settings::tzMinutes() * 60;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(out, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

// --- HOME --------------------------------------------------------------------

struct Tile { EpdRect r; const char* title; Go go; };
void sosSubtitle(char* out, size_t n);
void renderStealthBar(uint8_t* fb);
void sunSubtitle(const RideState& s, char* out, size_t n);
constexpr int TILE_GAP = 10;
constexpr int TILE_W = (W - 3 * TILE_GAP) / 2;
constexpr int STEALTH_H = 82;
constexpr int TILE_H = (H - TOP - STEALTH_H - 6 * TILE_GAP) / 4;
const EpdRect kStealthBar = {TILE_GAP, H - TILE_GAP - STEALTH_H, W - 2 * TILE_GAP, STEALTH_H};
uint32_t g_stealthOffAsk = 0;   // first tap on a lit stealth bar: ask again

EpdRect tileRect(int col, int row) {
    return {TILE_GAP + col * (TILE_W + TILE_GAP), TOP + TILE_GAP + row * (TILE_H + TILE_GAP),
            TILE_W, TILE_H};
}

// DOBA is for survival, not a navigator: no routes, no recorded tracks.
const Tile kTiles[8] = {
    {tileRect(0, 0), "MAP", TO_MAP},         {tileRect(1, 0), "POSITION", TO_POINTS},
    {tileRect(0, 1), "LIBRARY", TO_LIBRARY}, {tileRect(1, 1), "COMMS", TO_COMMS},
    {tileRect(0, 2), "SOS", TO_SOS},         {tileRect(1, 2), "SUN", TO_SUN},
    {tileRect(0, 3), "SETTINGS", TO_SETUP},  {tileRect(1, 3), "POWER", TO_POWER},
};

void renderHome(const RideState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    char sub[64];
    for (int i = 0; i < 8; ++i) {
        const Tile& t = kTiles[i];
        // SOS is always the one black tile: found without reading. Sending, it
        // gets a white inner ring as well.
        const bool sos = t.go == TO_SOS, dark = sos;
        if (dark) epd_fill_rect(t.r, INK, fb); else frame(t.r, 3, fb);
        if (sos && sosActive()) {
            const EpdRect in = {t.r.x + 8, t.r.y + 8, t.r.width - 16, t.r.height - 16};
            epd_fill_rect(in, PAPER, fb);
            epd_fill_rect({in.x + 5, in.y + 5, in.width - 10, in.height - 10}, INK, fb);
        }
        const uint8_t fg = dark ? PAPER : INK;
        const char* title = sos && sosActive() ? "SOS ON" : t.title;
        ui::text(ui::font(ui::F_TITLE), t.r.x + 16, t.r.y + 52, title, fb, EPD_DRAW_ALIGN_LEFT, fg);
        sub[0] = 0;
        switch (t.go) {
            case TO_MAP: snprintf(sub, sizeof sub, "topo + MGRS grid"); break;
            case TO_POINTS:
                if (doba_points::target() >= 0 && (s.gpsFix || s.everHadFix)) {
                    double b, d;
                    doba_points::fromHere(doba_points::target(), s.latitude, s.longitude, b, d);
                    char az[8], ds[16];
                    milgeo::mils((float)b, milspec::milsSystem(), az, sizeof az);
                    doba_points::formatDistance(d, ds, sizeof ds);
                    snprintf(sub, sizeof sub, "> %.8s %s %s", doba_points::at(doba_points::target())->name, az, ds);
                } else if (s.gpsFix || s.everHadFix) milgeo::mgrs(s.latitude, s.longitude, sub, sizeof sub);
                else snprintf(sub, sizeof sub, "no fix yet");
                break;
            case TO_LIBRARY: snprintf(sub, sizeof sub, "%d book%s on card", doba_lib::count(), doba_lib::count() == 1 ? "" : "s"); break;
            case TO_COMMS:
                if (stealth::active()) snprintf(sub, sizeof sub, stealth::listen() ? "stealth · listening" : "stealth · radio off");
                else if (!mesh_service::enabled()) snprintf(sub, sizeof sub, "radio off");
                else snprintf(sub, sizeof sub, "%d new · %d nodes", mesh_service::unreadCount(),
                              mesh_service::nodeCount());
                break;
            case TO_SOS: sosSubtitle(sub, sizeof sub); break;
            case TO_SUN: sunSubtitle(s, sub, sizeof sub); break;
            case TO_POWER:
                if (s.batteryPercent) snprintf(sub, sizeof sub, "%u%%%s · sleep / off", s.batteryPercent, s.charging ? " charging" : "");
                else snprintf(sub, sizeof sub, "sleep / off");
                break;
            case TO_SETUP: {
                const int m = settings::tzMinutes();
                snprintf(sub, sizeof sub, "UTC%+d · %s", m / 60, milspec::milsSystem() == 6400 ? "6400" : "60-00");
                break;
            }
            default: break;
        }
        // Subtitle may be long (MGRS): shrink to fit the tile.
        const EpdFont* sf = ui::font(ui::F_SMALL);
        if (ui::textWidth(sf, sub) > t.r.width - 32) sf = ui::font(ui::F_TINY);
        ui::text(sf, t.r.x + 16, t.r.y + t.r.height - 24, sub, fb, EPD_DRAW_ALIGN_LEFT, fg);
        // Unread badge on COMMS.
        if (t.go == TO_COMMS && mesh_service::enabled() && mesh_service::unreadCount() > 0) {
            epd_fill_circle(t.r.x + t.r.width - 34, t.r.y + 36, 18, INK, fb);
            char nb[8];
            snprintf(nb, sizeof nb, "%d", mesh_service::unreadCount() > 99 ? 99 : mesh_service::unreadCount());
            ui::text(ui::font(ui::F_SMALL), t.r.x + t.r.width - 34, t.r.y + 44, nb, fb,
                     EPD_DRAW_ALIGN_CENTER, PAPER);
        }
    }
    renderStealthBar(fb);
}

// The stealth bar: one tap hides the device; going loud again takes a second
// tap within a few seconds, so a brush of the screen cannot give you away.
void renderStealthBar(uint8_t* fb) {
    const EpdRect r = kStealthBar;
    const bool on = stealth::active();
    const bool asking = on && g_stealthOffAsk && millis() - g_stealthOffAsk < 5000;
    if (on) epd_fill_rect(r, INK, fb); else frame(r, 3, fb);
    const uint8_t fg = on ? PAPER : INK;
    // an eye, crossed out when hidden
    const int ex = r.x + 52, ey = r.y + r.height / 2;
    for (int t = 0; t < 3; ++t) {
        epd_draw_circle(ex, ey, 14 + t, fg, fb);
    }
    epd_fill_circle(ex, ey, 6, fg, fb);
    if (on) for (int t = -2; t <= 2; ++t) epd_draw_line(ex - 24, ey + 20 + t, ex + 24, ey - 20 + t, fg, fb);
    ui::text(ui::font(ui::F_TITLE), r.x + 96, r.y + 40, on ? "STEALTH ON" : "STEALTH", fb, EPD_DRAW_ALIGN_LEFT, fg);
    const char* sub = asking ? "tap again to go loud" :
                      on ? (stealth::listen() ? "silent, still listening" : "radio, BT, light off")
                         : "tap to go silent";
    ui::text(ui::font(ui::F_SMALL), r.x + 96, r.y + r.height - 14, sub, fb, EPD_DRAW_ALIGN_LEFT, fg);
}

Go tapHome(int x, int y) {
    if (hit(kStealthBar, x, y)) {
        if (!stealth::active()) { stealth::set(true); g_stealthOffAsk = 0; }
        else if (g_stealthOffAsk && millis() - g_stealthOffAsk < 5000) { stealth::set(false); g_stealthOffAsk = 0; }
        else g_stealthOffAsk = millis();
        return STAY;
    }
    for (const Tile& t : kTiles)
        if (hit(t.r, x, y)) return t.go;
    return STAY;
}

// --- shared bits for the pages below -------------------------------------------

const char* onOff(bool b) { return b ? "ON" : "OFF"; }

// A page row: caption on the left, value on the right, ruled underneath.
void row(int y, int h, const char* caption, const char* value, uint8_t* fb, bool hint = true) {
    ui::text(ui::font(ui::F_LABEL), 18, y + h / 2 + 10, caption, fb, EPD_DRAW_ALIGN_LEFT, INK);
    char v[48];
    snprintf(v, sizeof v, hint ? "%s  >" : "%s", value);
    // The value takes what the caption leaves; a long one steps down a size.
    const int room = W - 36 - ui::textWidth(ui::font(ui::F_LABEL), caption) - 24;
    const EpdFont* f = ui::font(ui::F_KEY);
    if (ui::textWidth(f, v) > room) f = ui::font(ui::F_LABEL);
    if (ui::textWidth(f, v) > room) f = ui::font(ui::F_SMALL);
    ui::text(f, W - 18, y + h / 2 + 12, v, fb, EPD_DRAW_ALIGN_RIGHT, INK);
    epd_fill_rect({10, y + h - 2, W - 20, 2}, INK, fb);
}

// The current position, if there is one worth using.
bool here(const RideState& s, double& lat, double& lon) {
    if (!s.gpsFix && !s.everHadFix) return false;
    lat = s.latitude;
    lon = s.longitude;
    return true;
}

// Height: barometer, else the card's DEM, else GPS.
float groundAlt(const RideState& s) {
    return s.baroValid ? s.baroAltM : s.mapElevationValid ? s.mapElevationM : s.altitudeM;
}

// "12-34" / "1234" and degrees.
void azText(double deg, char* out, size_t n) {
    char m[8];
    milgeo::mils((float)deg, milspec::milsSystem(), m, sizeof m);
    snprintf(out, n, "%s", m);
}

// A UTF-8 short name: the first four non-space characters, upper-cased ASCII.
void shortName(const char* lng, char* out, size_t n) {
    size_t o = 0;
    int glyphs = 0;
    for (const unsigned char* p = (const unsigned char*)lng; *p && glyphs < 4;) {
        int len = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : 4;
        if (*p == ' ') { ++p; continue; }
        if (o + len >= n) break;
        for (int i = 0; i < len && p[i]; ++i) out[o++] = (char)(i == 0 && len == 1 ? toupper(p[0]) : p[i]);
        p += len;
        ++glyphs;
    }
    out[o] = 0;
}

// --- controls: a switch, segments, a stepper -----------------------------------
// One visual language for every setting: what you see is what a tap does.

constexpr int CTRL_W = 250, CTRL_H = 56, CTRL_X = W - 14 - CTRL_W;

void section(int y, const char* name, uint8_t* fb) {
    ui::text(ui::font(ui::F_TINY), 16, y + 24, name, fb, EPD_DRAW_ALIGN_LEFT, INK);
    epd_fill_rect({16 + ui::textWidth(ui::font(ui::F_TINY), name) + 10, y + 16, W - 46 - ui::textWidth(ui::font(ui::F_TINY), name), 2}, INK, fb);
}

void caption(int y, int h, const char* name, const char* hint, uint8_t* fb) {
    ui::text(ui::font(ui::F_LABEL), 16, y + (hint ? h / 2 - 2 : h / 2 + 10), name, fb, EPD_DRAW_ALIGN_LEFT, INK);
    if (hint) ui::text(ui::font(ui::F_TINY), 16, y + h / 2 + 26, hint, fb, EPD_DRAW_ALIGN_LEFT, INK);
}

// n labels side by side, the chosen one filled.
void segments(int y, int h, const char* const* labels, int n, int sel, uint8_t* fb, int x0 = CTRL_X, int w = CTRL_W) {
    const int top = y + (h - CTRL_H) / 2;
    frame({x0, top, w, CTRL_H}, 3, fb);
    for (int i = 0; i < n; ++i) {
        const int sx = x0 + w * i / n, sw = w * (i + 1) / n - w * i / n;
        if (i == sel) epd_fill_rect({sx, top, sw, CTRL_H}, INK, fb);
        else if (i) epd_fill_rect({sx, top + 8, 2, CTRL_H - 16}, INK, fb);
        // One size for the whole control: the smaller one if any label needs it.
        const EpdFont* lf = ui::font(ui::F_SMALL);
        for (int j = 0; j < n; ++j)
            if (ui::textWidth(ui::font(ui::F_SMALL), labels[j]) > w / n - 12) lf = ui::font(ui::F_TINY);
        ui::text(lf, sx + sw / 2, top + CTRL_H / 2 + (lf == ui::font(ui::F_TINY) ? 6 : 9), labels[i], fb, EPD_DRAW_ALIGN_CENTER,
                 i == sel ? PAPER : INK);
    }
}
int segmentHit(int x, int n, int x0 = CTRL_X, int w = CTRL_W) {
    if (x < x0 - 10 || x > x0 + w + 10) return -1;
    const int i = (x - x0) * n / w;
    return i < 0 ? 0 : i >= n ? n - 1 : i;
}

// "‹  value  ›": the halves step back and forward.
void stepper(int y, int h, const char* value, uint8_t* fb) {
    const int top = y + (h - CTRL_H) / 2;
    frame({CTRL_X, top, CTRL_W, CTRL_H}, 3, fb);
    epd_fill_rect({CTRL_X + 52, top + 8, 2, CTRL_H - 16}, INK, fb);
    epd_fill_rect({CTRL_X + CTRL_W - 54, top + 8, 2, CTRL_H - 16}, INK, fb);
    ui::text(ui::font(ui::F_KEY), CTRL_X + 26, top + CTRL_H / 2 + 11, "<", fb, EPD_DRAW_ALIGN_CENTER, INK);
    ui::text(ui::font(ui::F_KEY), CTRL_X + CTRL_W - 26, top + CTRL_H / 2 + 11, ">", fb, EPD_DRAW_ALIGN_CENTER, INK);
    ui::text(ui::font(ui::F_SMALL), CTRL_X + CTRL_W / 2, top + CTRL_H / 2 + 9, value, fb, EPD_DRAW_ALIGN_CENTER, INK);
}
int stepperHit(int x) {   // -1, +1, or 0 (the middle: also forward)
    if (x < CTRL_X - 10) return 0;
    return x < CTRL_X + CTRL_W / 2 ? -1 : +1;
}

// ON | OFF as two segments.
void toggle(int y, int h, bool on, uint8_t* fb) {
    static const char* const kOnOff[] = {"OFF", "ON"};
    segments(y, h, kOnOff, 2, on ? 1 : 0, fb, CTRL_X + CTRL_W - 150, 150);
}
bool toggleHit(int x) { return x >= CTRL_X + CTRL_W - 160; }

// --- SETUP: settings without the phone -------------------------------------------

// The bands that make sense for a device carried in Europe and Ukraine, then
// the rest of the world for the odd trip. Stepped through in this order.
const char* const kRegionCycle[] = {"EU_868", "UA_868", "EU_433", "UA_433", "US", "RU"};
constexpr int kRegions = sizeof(kRegionCycle) / sizeof(kRegionCycle[0]);
kbd::Keyboard g_nameKbd;

// The layout, top to bottom: section headers and rows.
enum SetupRow { R_STEALTH, R_RADIO, R_REGION, R_NAME, R_LIGHT, R_TZ, R_MILS, R_MORE, R_COUNT };
constexpr int SROW_H = 78, SEC_H = 34;
int setupRowY(int r) {
    // RADIO: 4 rows (stealth first) · DISPLAY: 2 · NAVIGATION: 2
    const int sec = r < R_LIGHT ? 1 : r < R_MILS ? 2 : 3;
    return TOP + 4 + sec * SEC_H + r * SROW_H;
}
int setupRowAt(int y) {
    for (int r = 0; r < R_COUNT; ++r)
        if (y >= setupRowY(r) && y < setupRowY(r) + SROW_H) return r;
    return -1;
}

void renderSetup(const RideState& s, uint8_t* fb) {
    (void)s;
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    section(setupRowY(R_STEALTH) - SEC_H, "RADIO", fb);
    section(setupRowY(R_LIGHT) - SEC_H, "SCREEN AND CLOCK", fb);
    section(setupRowY(R_MILS) - SEC_H, "NAVIGATION", fb);
    char v[48], hint[48];
    for (int r = 0; r < R_COUNT; ++r) {
        const int y = setupRowY(r);
        switch (r) {
            case R_STEALTH: {
                static const char* const kS[] = {"OFF", "LISTEN", "SILENT"};
                const int st = !stealth::active() ? 0 : stealth::listen() ? 1 : 2;
                caption(y, SROW_H, "STEALTH", st == 0 ? "nothing hidden" : st == 1 ? "no transmit, still hears" :
                        "radio, BT, light all off", fb);
                segments(y, SROW_H, kS, 3, st, fb);
                break;
            }
            case R_RADIO:
                caption(y, SROW_H, "RADIO", stealth::active() ? "held by stealth" :
                        mesh_service::enabled() ? "on: messages + nodes" : "off: saves battery", fb);
                toggle(y, SROW_H, mesh_service::enabled(), fb);
                break;
            case R_REGION: {
                // The country decides the band; nobody should have to know its name.
                const mesh::Region& rg = mesh::region(mesh_service::regionIndex());
                const char* where = !strcmp(rg.name, "EU_868") ? "Europe" : !strcmp(rg.name, "UA_868") ? "Ukraine" : rg.name;
                if (settings::regionAuto()) snprintf(hint, sizeof hint, "by GPS: %s", where);
                else snprintf(hint, sizeof hint, "set by hand: %s", where);
                caption(y, SROW_H, "COUNTRY", hint, fb);
                stepper(y, SROW_H, settings::regionAuto() ? "AUTO" : rg.name, fb);
                break;
            }
            case R_NAME: {
                char sh[20];
                shortName(mesh_service::longName(), sh, sizeof sh);
                snprintf(hint, sizeof hint, "short: %s · tap to edit", sh);
                caption(y, SROW_H, "CALLSIGN", hint, fb);
                snprintf(v, sizeof v, "%.14s", mesh_service::longName());
                const int top = y + (SROW_H - CTRL_H) / 2;
                frame({CTRL_X, top, CTRL_W, CTRL_H}, 1, fb);
                ui::text(ui::font(ui::F_SMALL), CTRL_X + 14, top + CTRL_H / 2 + 9, v[0] ? v : "--", fb,
                         EPD_DRAW_ALIGN_LEFT, INK);
                // a pencil: a slanted bar and a tip
                const int px = CTRL_X + CTRL_W - 34, py = top + CTRL_H / 2;
                for (int t = -2; t <= 2; ++t) epd_draw_line(px - 10 + t, py + 12, px + 10 + t, py - 12, INK, fb);
                epd_fill_triangle(px - 14, py + 16, px - 10, py + 8, px - 6, py + 14, INK, fb);
                break;
            }
            case R_LIGHT: {
                static const char* const kL[] = {"OFF", "LOW", "MID", "HI"};
                const int b = settings::backlight();
                caption(y, SROW_H, "LIGHT", "front light", fb);
                segments(y, SROW_H, kL, 4, b < 0 ? 0 : b > 3 ? 3 : b, fb);
                break;
            }
            case R_TZ: {
                const int m = settings::tzMinutes();
                char off[16];
                if (m % 60) snprintf(off, sizeof off, "UTC%+d:%02d", m / 60, abs(m % 60));
                else snprintf(off, sizeof off, "UTC%+d", m / 60);
                if (settings::tzAuto()) {
                    double la, lo;
                    const bool pos = settings::lastPosition(la, lo);
                    if (!pos) snprintf(hint, sizeof hint, "by GPS: waiting for a fix");
                    else snprintf(hint, sizeof hint, "by GPS: %s%s", off,
                                  tz_auto::summerAt(la, lo, time(nullptr)) ? ", summer" : "");
                    caption(y, SROW_H, "TIME ZONE", hint, fb);
                    stepper(y, SROW_H, "AUTO", fb);
                } else {
                    caption(y, SROW_H, "TIME ZONE", "set by hand", fb);
                    stepper(y, SROW_H, off, fb);
                }
                break;
            }
            case R_MILS: {
                static const char* const kM[] = {"60-00", "6400"};
                caption(y, SROW_H, "ANGLES", milspec::milsSystem() == 6400 ? "NATO mils" : "6000 mils (UA/RU)", fb);
                segments(y, SROW_H, kM, 2, milspec::milsSystem() == 6400 ? 1 : 0, fb);
                break;
            }
            case R_MORE: {
                caption(y, SROW_H, "MORE", "USB drive, GPS, radio mode", fb);
                const int top = y + (SROW_H - CTRL_H) / 2;
                frame({CTRL_X, top, CTRL_W, CTRL_H}, 1, fb);
                ui::text(ui::font(ui::F_SMALL), CTRL_X + CTRL_W / 2, top + CTRL_H / 2 + 9, "OPEN  >", fb,
                         EPD_DRAW_ALIGN_CENTER, INK);
                break;
            }
        }
        epd_fill_rect({16, y + SROW_H - 1, W - 32, 1}, INK, fb);
    }
    char f[64];
    snprintf(f, sizeof f, "%d nodes heard · settings save at once", mesh_service::nodeCount());
    ui::text(ui::font(ui::F_TINY), W / 2, H - 24, f, fb, EPD_DRAW_ALIGN_CENTER, INK);
}

Go tapSetup(int x, int y) {
    const int r = setupRowAt(y);
    if (r < 0) return STAY;
    switch (r) {
        case R_STEALTH: {
            const int i = segmentHit(x, 3);
            if (i == 0) stealth::set(false);
            else if (i > 0) { stealth::setListen(i == 1); stealth::set(true); }
            break;
        }
        case R_RADIO:
            if (stealth::active()) break;   // stealth owns the radio until it ends
            if (toggleHit(x)) mesh_service::setEnabled(x >= CTRL_X + CTRL_W - 75);
            break;
        case R_REGION: {
            // AUTO, then the bands by hand, round again to AUTO.
            const int step = stepperHit(x) < 0 ? -1 : 1;
            const char* cur = mesh::region(mesh_service::regionIndex()).name;
            int at = kRegions;   // AUTO sits after the last band
            if (!settings::regionAuto())
                for (int i = 0; i < kRegions; ++i) if (!strcmp(cur, kRegionCycle[i])) at = i;
            for (int k = 1; k <= kRegions + 1; ++k) {
                const int n = ((at + step * k) % (kRegions + 1) + kRegions + 1) % (kRegions + 1);
                if (n == kRegions) { settings::setRegionAuto(true); break; }
                const int ri = mesh::regionIndexByName(kRegionCycle[n]);
                if (ri >= 0) { settings::setRegionAuto(false); mesh_service::setRegion((uint8_t)ri); break; }
            }
            break;
        }
        case R_NAME:
            g_nameKbd.clear();
            snprintf(g_nameKbd.text, sizeof g_nameKbd.text, "%s", mesh_service::longName());
            return TO_SETUP_NAME;
        case R_LIGHT: {
            const int i = segmentHit(x, 4);
            if (i >= 0) settings::setBacklight(i);
            break;
        }
        case R_TZ: {
            // AUTO, then whole hours by hand from where AUTO had it.
            const int step = stepperHit(x) < 0 ? -60 : 60;
            if (settings::tzAuto()) {
                settings::setTzMinutes(settings::tzMinutes() + step);
                settings::setTzAuto(false);
                break;
            }
            int m = settings::tzManualMinutes() + step;
            if (m < -12 * 60 || m > 14 * 60) { settings::setTzAuto(true); break; }
            settings::setTzMinutes(m);
            break;
        }
        case R_MILS: {
            const int i = segmentHit(x, 2);
            if (i >= 0) milspec::setMilsSystem(i ? 6400 : 6000);
            break;
        }
        case R_MORE: return TO_MORE;
    }
    return STAY;
}

// --- COMMS: chats and nodes, Meshtastic-style ----------------------------------
//
// Three levels, like the Meshtastic app: CHATS (the public channel, private
// channels, one chat per person written to) and NODES (everyone heard: name,
// when, how far, which way); a CHAT is one conversation with QUICK replies and
// WRITE. Delivery shows as in the app: ... queued, one tick on the air, two
// ticks acknowledged, ! failed.

enum CommsView : uint8_t { CV_CHATS, CV_NODES, CV_CHAT, CV_QUICK };
CommsView g_cv = CV_CHATS;
int g_listPage = 0;
// The open conversation: a channel (dm == 0) or a person (dm = node number).
uint8_t g_chatCh = 0;
uint32_t g_chatDm = 0;
double g_myLat = 0, g_myLon = 0;
bool g_myPos = false;
// When each conversation was last looked at (millis), for its unread count.
struct Seen { uint8_t ch; uint32_t dm; uint32_t ms; };
constexpr int MAX_SEEN = 24;
Seen g_seen[MAX_SEEN];
int g_seenCount = 0;

constexpr int TAB_Y = TOP + 8, TAB_H = 56, LIST_Y = TOP + 76, ROW_H = 100;
constexpr int BAR_Y = H - 92, BAR_H = 80;
const EpdRect kRadioOn = {120, 420, 300, 96};
const EpdRect kPrev = {12, BAR_Y, 110, BAR_H};
const EpdRect kNext = {W - 122, BAR_Y, 110, BAR_H};
const EpdRect kQuick = {130, BAR_Y, 120, BAR_H};
const EpdRect kWrite = {258, BAR_Y, 150, BAR_H};
const EpdRect kOlder = {12, BAR_Y, 110, BAR_H};
const EpdRect kNewer = {416, BAR_Y, 112, BAR_H};
// In a chat: ^ older, QUICK, WRITE, v newer.

bool isDm(const mesh_service::Message& m) {
    return m.outgoing ? m.to != mesh::BROADCAST_ADDR : m.to == mesh_service::nodeNum();
}
uint32_t peerOf(const mesh_service::Message& m) { return m.outgoing ? m.to : m.from; }
bool inChat(const mesh_service::Message& m, uint8_t ch, uint32_t dm) {
    if (dm) return isDm(m) && peerOf(m) == dm;
    return !isDm(m) && m.channel == ch;
}
uint32_t& seenMs(uint8_t ch, uint32_t dm) {
    for (int i = 0; i < g_seenCount; ++i)
        if (g_seen[i].ch == ch && g_seen[i].dm == dm) return g_seen[i].ms;
    if (g_seenCount == MAX_SEEN) { memmove(g_seen, g_seen + 1, sizeof(Seen) * (MAX_SEEN - 1)); --g_seenCount; }
    g_seen[g_seenCount] = {ch, dm, 0};
    return g_seen[g_seenCount++].ms;
}

void nameOf(uint32_t num, char* out, size_t n) {
    if (!mesh_service::nodeName(num, out, n) || !out[0]) snprintf(out, n, "!%08lx", (unsigned long)num);
}
void chatTitle(uint8_t ch, uint32_t dm, char* out, size_t n) {
    if (dm) { nameOf(dm, out, n); return; }
    if (ch == 0) { snprintf(out, n, "EVERYONE"); return; }
    mesh_service::ChannelInfo ci;
    if (mesh_service::channelAt(ch, ci) && ci.name[0]) snprintf(out, n, "# %s", ci.name);
    else snprintf(out, n, "# channel %u", (unsigned)ch);
}
void ago(uint32_t ms, char* out, size_t n) {
    const uint32_t s = (millis() - ms) / 1000;
    if (s < 60) snprintf(out, n, "now");
    else if (s < 3600) snprintf(out, n, "%lum", (unsigned long)(s / 60));
    else if (s < 86400) snprintf(out, n, "%luh", (unsigned long)(s / 3600));
    else snprintf(out, n, "%lud", (unsigned long)(s / 86400));
}
// "1.2 km · 34-50" from here to a node, "" without both positions.
void whereIs(const mesh_service::Node& nd, char* out, size_t n) {
    out[0] = 0;
    if (!g_myPos || !nd.hasPosition) return;
    double b, d;
    milgeo::bearingDistance(g_myLat, g_myLon, nd.latitude, nd.longitude, b, d);
    char az[8], ds[16];
    milgeo::mils((float)b, milspec::milsSystem(), az, sizeof az);
    doba_points::formatDistance(d, ds, sizeof ds);
    snprintf(out, n, "%s · %s", ds, az);
}
// Cut a line to fit with "...", at a character.
void fit(const EpdFont* f, char* s, int maxW) {
    if (ui::textWidth(f, s) <= maxW) return;
    size_t n = strlen(s);
    while (n > 0) {
        do { --n; } while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80);
        memcpy(s + n, "...", 4);
        if (ui::textWidth(f, s) <= maxW) return;
    }
}

// The conversations: the channels in use, then everyone written with, newest first.
struct Chat { uint8_t ch; uint32_t dm; uint32_t lastMs; int unread; int lastIdx; };
constexpr int MAX_CHATS = 24;
int collectChats(Chat* out) {
    int n = 0;
    for (int c = 0; c < 8 && n < MAX_CHATS; ++c) {
        mesh_service::ChannelInfo ci;
        if (c == 0 || (mesh_service::channelAt(c, ci) && ci.used)) out[n++] = {(uint8_t)c, 0, 0, 0, -1};
    }
    const int total = mesh_service::messageCount();
    for (int i = 0; i < total; ++i) {
        mesh_service::Message m;
        if (!mesh_service::messageAt(i, m)) continue;
        const uint32_t dm = isDm(m) ? peerOf(m) : 0;
        const uint8_t ch = dm ? 0 : m.channel;
        int k = 0;
        while (k < n && !(out[k].ch == ch && out[k].dm == dm)) ++k;
        if (k == n) { if (n == MAX_CHATS) continue; out[n++] = {ch, dm, 0, 0, -1}; }
        out[k].lastMs = m.ms;
        out[k].lastIdx = i;
        if (!m.outgoing && m.ms > seenMs(ch, dm)) ++out[k].unread;
    }
    // channels first (in slot order), then people by the newest message
    std::stable_sort(out, out + n, [](const Chat& a, const Chat& b) {
        if ((a.dm == 0) != (b.dm == 0)) return a.dm == 0;
        if (a.dm == 0) return a.ch < b.ch;
        return a.lastMs > b.lastMs;
    });
    return n;
}

void tabs(uint8_t* fb) {
    char nodes[24];
    snprintf(nodes, sizeof nodes, "NODES  %d", mesh_service::nodeCount());
    const char* const l[] = {"CHATS", nodes};
    segments(TAB_Y, TAB_H, l, 2, g_cv == CV_NODES ? 1 : 0, fb, 12, W - 24);
}

void pager(int page, int pages, uint8_t* fb) {
    if (pages <= 1) return;
    button(kPrev, "<", false, fb);
    button(kNext, ">", false, fb);
    char p[16];
    snprintf(p, sizeof p, "%d / %d", page + 1, pages);
    ui::text(ui::font(ui::F_LABEL), W / 2, BAR_Y + BAR_H / 2 + 10, p, fb, EPD_DRAW_ALIGN_CENTER, INK);
}
int rowsPerPage() { return (BAR_Y - 8 - LIST_Y) / ROW_H; }

void renderChats(uint8_t* fb) {
    tabs(fb);
    Chat chats[MAX_CHATS];
    const int n = collectChats(chats);
    const int per = rowsPerPage(), pages = (n + per - 1) / per;
    if (g_listPage >= pages) g_listPage = pages ? pages - 1 : 0;
    const EpdFont* big = ui::font(ui::F_LABEL);
    const EpdFont* small = ui::font(ui::F_SMALL);
    for (int r = 0; r < per && g_listPage * per + r < n; ++r) {
        const Chat& c = chats[g_listPage * per + r];
        const int y = LIST_Y + r * ROW_H;
        char t[48], pv[160], when[12] = "";
        chatTitle(c.ch, c.dm, t, sizeof t);
        // a mark for what it is: a person, the open channel, a private one
        if (c.dm) { epd_fill_circle(36, y + 30, 12, INK, fb); epd_fill_rect({22, y + 46, 29, 12}, INK, fb); epd_fill_circle(36, y + 46, 14, INK, fb); epd_fill_rect({18, y + 58, 40, 6}, PAPER, fb); }
        else if (c.ch == 0) { for (int k = 0; k < 3; ++k) epd_draw_circle(36, y + 42, 8 + k * 7, INK, fb), epd_draw_circle(36, y + 42, 9 + k * 7, INK, fb); epd_fill_circle(36, y + 42, 5, INK, fb); }
        else { epd_fill_rect({22, y + 34, 28, 22}, INK, fb); for (int k = 0; k < 3; ++k) epd_draw_circle(36, y + 32, 9 - k, INK, fb); epd_fill_rect({24, y + 36, 24, 18}, INK, fb); }
        pv[0] = 0;
        if (c.lastIdx >= 0) {
            mesh_service::Message m;
            if (mesh_service::messageAt(c.lastIdx, m)) {
                char who[40];
                if (m.outgoing) snprintf(who, sizeof who, "you");
                else nameOf(m.from, who, sizeof who);
                snprintf(pv, sizeof pv, c.dm && !m.outgoing ? "%s" : "%s: %s", c.dm && !m.outgoing ? m.text : who, m.text);
                ago(m.ms, when, sizeof when);
            }
        } else {
            snprintf(pv, sizeof pv, c.ch == 0 ? "everyone in range · %s" : "private channel", mesh_service::presetName());
        }
        const int tx = 70, right = W - 16;
        ui::text(small, right, y + 36, when, fb, EPD_DRAW_ALIGN_RIGHT, INK);
        fit(big, t, right - tx - ui::textWidth(small, when) - 16);
        ui::text(big, tx, y + 38, t, fb, EPD_DRAW_ALIGN_LEFT, INK);
        int pw = right - tx;
        if (c.unread) {
            char u[8];
            snprintf(u, sizeof u, "%d", c.unread > 99 ? 99 : c.unread);
            const int bw = ui::textWidth(small, u) + 22;
            epd_fill_rect({right - bw, y + 54, bw, 32}, INK, fb);
            ui::text(small, right - bw / 2, y + 77, u, fb, EPD_DRAW_ALIGN_CENTER, PAPER);
            pw -= bw + 10;
        }
        fit(small, pv, pw);
        ui::text(small, tx, y + 78, pv, fb, EPD_DRAW_ALIGN_LEFT, INK);
        epd_fill_rect({tx, y + ROW_H - 1, W - tx - 12, 1}, INK, fb);
    }
    pager(g_listPage, pages, fb);
    if (pages <= 1)
        ui::text(ui::font(ui::F_TINY), W / 2, BAR_Y + 44, "tap a chat to read and write", fb, EPD_DRAW_ALIGN_CENTER, INK);
}

void renderNodes(uint8_t* fb) {
    tabs(fb);
    const int n = mesh_service::nodeCount();
    const int per = rowsPerPage(), pages = (n + per - 1) / per;
    if (g_listPage >= pages) g_listPage = pages ? pages - 1 : 0;
    const EpdFont* big = ui::font(ui::F_LABEL);
    const EpdFont* small = ui::font(ui::F_SMALL);
    if (n == 0) {
        ui::text(small, W / 2, 420, "nobody heard yet", fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::text(ui::font(ui::F_TINY), W / 2, 456, "nodes show here as their radios are heard", fb, EPD_DRAW_ALIGN_CENTER, INK);
    }
    for (int r = 0; r < per && g_listPage * per + r < n; ++r) {
        mesh_service::Node nd;
        if (!mesh_service::nodeAt(g_listPage * per + r, nd)) continue;
        const int y = LIST_Y + r * ROW_H;
        // the short name in a box, as Meshtastic shows it
        char sh[8];
        snprintf(sh, sizeof sh, "%.4s", nd.shortName[0] ? nd.shortName : "?");
        epd_fill_rect({12, y + 16, 76, 64}, INK, fb);
        ui::text(small, 50, y + 56, sh, fb, EPD_DRAW_ALIGN_CENTER, PAPER);
        char nm[48], when[12], where[40], meta[96];
        snprintf(nm, sizeof nm, "%s", nd.longName[0] ? nd.longName : sh);
        ago(nd.lastHeardMs, when, sizeof when);
        whereIs(nd, where, sizeof where);
        const int tx = 104, right = W - 16;
        ui::text(small, right, y + 40, when, fb, EPD_DRAW_ALIGN_RIGHT, INK);
        fit(big, nm, right - tx - ui::textWidth(small, when) - 16);
        ui::text(big, tx, y + 42, nm, fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (nd.hops == 0) snprintf(meta, sizeof meta, "%s%sdirect · %d dB", where, where[0] ? " · " : "", nd.snr);
        else if (nd.hops == mesh_service::HOPS_UNKNOWN) snprintf(meta, sizeof meta, "%s", where[0] ? where : "relayed");
        else snprintf(meta, sizeof meta, "%s%s%u hop%s", where, where[0] ? " · " : "", nd.hops, nd.hops == 1 ? "" : "s");
        fit(small, meta, right - tx);
        ui::text(small, tx, y + 78, meta, fb, EPD_DRAW_ALIGN_LEFT, INK);
        epd_fill_rect({tx, y + ROW_H - 1, W - tx - 12, 1}, INK, fb);
    }
    pager(g_listPage, pages, fb);
    if (pages <= 1 && n)
        ui::text(ui::font(ui::F_TINY), W / 2, BAR_Y + 44, "tap a node to write to it", fb, EPD_DRAW_ALIGN_CENTER, INK);
}

int g_chatScroll = 0;   // messages skipped from the newest in the open chat

void renderChat(uint8_t* fb) {
    const EpdFont* small = ui::font(ui::F_SMALL);
    const EpdFont* tiny = ui::font(ui::F_TINY);
    const EpdFont* body = ui::font(ui::F_LABEL);
    // Header: who, and for a person how far and which way.
    char t[48], sub[80] = "";
    chatTitle(g_chatCh, g_chatDm, t, sizeof t);
    if (g_chatDm) {
        for (int i = 0; i < mesh_service::nodeCount(); ++i) {
            mesh_service::Node nd;
            if (mesh_service::nodeAt(i, nd) && nd.num == g_chatDm) {
                char when[12], where[40];
                ago(nd.lastHeardMs, when, sizeof when);
                whereIs(nd, where, sizeof where);
                snprintf(sub, sizeof sub, "%s%sheard %s · private", where, where[0] ? " · " : "", when);
                break;
            }
        }
    } else {
        snprintf(sub, sizeof sub, g_chatCh == 0 ? "everyone in range can read this" : "only those with the key");
    }
    fit(body, t, W - 28);
    ui::text(body, 14, TOP + 34, t, fb, EPD_DRAW_ALIGN_LEFT, INK);
    ui::text(tiny, 14, TOP + 58, sub, fb, EPD_DRAW_ALIGN_LEFT, INK);
    epd_fill_rect({0, TOP + 70, W, 2}, INK, fb);

    // Bubbles, newest at the bottom: theirs on the left in a frame with the
    // sender's name, yours on the right, filled, with the delivery state.
    const int areaTop = TOP + 78, areaBottom = BAR_Y - 8;
    const int total = mesh_service::messageCount();
    int mine = 0;
    for (int i = 0; i < total; ++i) { mesh_service::Message m; if (mesh_service::messageAt(i, m) && inChat(m, g_chatCh, g_chatDm)) ++mine; }
    if (g_chatScroll > mine - 1) g_chatScroll = mine > 0 ? mine - 1 : 0;
    int y = areaBottom, skip = g_chatScroll, shown = 0;
    const int maxW = W - 120;
    for (int i = total - 1; i >= 0; --i) {
        mesh_service::Message m;
        if (!mesh_service::messageAt(i, m) || !inChat(m, g_chatCh, g_chatDm)) continue;
        if (skip > 0) { --skip; continue; }
        char lines[8][256];
        int nl = 0, wmax = 0;
        wrap(body, m.text, maxW - 28, [&](const char* l) {
            if (nl < 8) { snprintf(lines[nl], sizeof lines[0], "%s", l); wmax = std::max(wmax, ui::textWidth(body, lines[nl])); ++nl; }
        });
        char meta[80], when[12], who[40] = "";
        if (m.utc) clockOf(m.utc, when, sizeof when);
        else ago(m.ms, when, sizeof when);   // no wall clock yet: how long ago
        const int ticks = !m.outgoing ? 0 : m.status == mesh_service::TX_ACKED ? 2 : m.status == mesh_service::TX_SENT ? 1 : 0;
        if (m.outgoing) {
            snprintf(meta, sizeof meta, "%s%s", when, m.status == mesh_service::TX_FAILED ? "  NOT SENT"
                                                   : m.status == mesh_service::TX_PENDING ? "  ..." : "");
        } else {
            nameOf(m.from, who, sizeof who);
            snprintf(meta, sizeof meta, g_chatDm ? "%s" : "%s  %s", g_chatDm ? when : who, when);
        }
        const int lineH = 32, bw = std::max(wmax, ui::textWidth(tiny, meta) + ticks * 14 + 8) + 28;
        const int bh = 30 + nl * lineH + 8;
        if (y - bh - 10 < areaTop) break;
        y -= bh + 10;
        const int x = m.outgoing ? W - 14 - bw : 14;
        const uint8_t fg = m.outgoing ? PAPER : INK;
        if (m.outgoing) epd_fill_rect({x, y, bw, bh}, INK, fb); else frame({x, y, bw, bh}, 2, fb);
        ui::text(tiny, x + 14, y + 22, meta, fb, EPD_DRAW_ALIGN_LEFT, fg);
        // delivery ticks, drawn: one on the air, two acknowledged
        for (int k = 0; k < ticks; ++k) {
            const int tx = x + 14 + ui::textWidth(tiny, meta) + 10 + k * 12, ty = y + 16;
            for (int t = 0; t < 2; ++t) {
                epd_draw_line(tx, ty + t, tx + 4, ty + 4 + t, fg, fb);
                epd_draw_line(tx + 4, ty + 4 + t, tx + 12, ty - 5 + t, fg, fb);
            }
        }
        for (int k = 0; k < nl; ++k)
            ui::text(body, x + 14, y + 30 + (k + 1) * lineH - 8, lines[k], fb, EPD_DRAW_ALIGN_LEFT, fg);
        ++shown;
    }
    if (mine == 0) {
        ui::text(small, W / 2, 400, "no messages yet", fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::text(tiny, W / 2, 436, "QUICK sends a ready phrase, WRITE types one", fb, EPD_DRAW_ALIGN_CENTER, INK);
    }
    seenMs(g_chatCh, g_chatDm) = millis();
    mesh_service::markAllRead();
    button(kOlder, "OLDER", false, fb);
    button(kQuick, "QUICK", false, fb);
    button(kWrite, "WRITE", true, fb);
    button(kNewer, "NEWER", false, fb);
    (void)shown;
}

// Ready phrases: one tap, no keyboard, with cold hands.
const char* const kQuickText[] = {
    "OK", "Copy", "On my way", "Wait for me", "Need help", "All good", "Where are you?",
    "Stay there", "Coming back", "Low battery",
};
constexpr int N_QUICK = sizeof(kQuickText) / sizeof(kQuickText[0]);
constexpr int Q_TOP = TOP + 90, Q_H = 76, Q_GAP = 10;
EpdRect quickRect(int i) {
    const int cw = (W - 24 - Q_GAP) / 2;
    return {12 + (i % 2) * (cw + Q_GAP), Q_TOP + (i / 2) * (Q_H + Q_GAP), cw, Q_H};
}
const EpdRect kSendPos = {12, Q_TOP + (N_QUICK / 2) * (Q_H + Q_GAP), W - 24, Q_H};

void renderQuick(uint8_t* fb) {
    char t[48], h[80];
    chatTitle(g_chatCh, g_chatDm, t, sizeof t);
    snprintf(h, sizeof h, "SEND TO %s", t);
    fit(ui::font(ui::F_LABEL), h, W - 28);
    ui::text(ui::font(ui::F_LABEL), 14, TOP + 40, h, fb, EPD_DRAW_ALIGN_LEFT, INK);
    epd_fill_rect({0, TOP + 60, W, 2}, INK, fb);
    for (int i = 0; i < N_QUICK; ++i) button(quickRect(i), kQuickText[i], false, fb);
    button(kSendPos, g_myPos ? "MY POSITION (MGRS)" : "MY POSITION: no fix", true, fb);
    ui::text(ui::font(ui::F_TINY), W / 2, H - 30, "one tap sends · BACK to the chat", fb, EPD_DRAW_ALIGN_CENTER, INK);
}

void sendToChat(const char* text) {
    mesh_service::queueText(g_chatDm ? g_chatDm : mesh::BROADCAST_ADDR, text, g_chatDm ? 0 : g_chatCh);
    g_chatScroll = 0;
    Serial.printf("[comms] send to %s%lx: %s\n", g_chatDm ? "!" : "ch", (unsigned long)(g_chatDm ? g_chatDm : g_chatCh), text);
}

void renderComms(const RideState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    g_myPos = s.gpsFix || s.everHadFix;
    g_myLat = s.latitude;
    g_myLon = s.longitude;
    const EpdFont* small = ui::font(ui::F_SMALL);
    const EpdFont* tiny = ui::font(ui::F_TINY);
    if (!mesh_service::enabled()) {
        // Contours behind, a clear card in front: the words stay readable.
        topo_bg::draw({0, TOP, W, H - TOP}, fb, 1);
        const EpdRect card = {40, 280, W - 80, 330};
        epd_fill_rect(card, PAPER, fb);
        frame(card, 2, fb);
        if (stealth::active()) {
            ui::text(ui::font(ui::F_TITLE), W / 2, 360, "STEALTH", fb, EPD_DRAW_ALIGN_CENTER, INK);
            ui::text(small, W / 2, 410, "Nothing in, nothing out.", fb, EPD_DRAW_ALIGN_CENTER, INK);
            ui::text(tiny, W / 2, 450, stealth::listen() ? "To listen, turn RADIO on in SETTINGS."
                                                         : "STEALTH: LISTEN in SETTINGS receives.", fb, EPD_DRAW_ALIGN_CENTER, INK);
            ui::text(tiny, W / 2, 560, "End stealth on HOME (two taps)", fb, EPD_DRAW_ALIGN_CENTER, INK);
            return;
        }
        ui::text(ui::font(ui::F_TITLE), W / 2, 360, "RADIO OFF", fb, EPD_DRAW_ALIGN_CENTER, INK);
        button(kRadioOn, "TURN ON", true, fb);
        ui::text(tiny, W / 2, 570, "messages and nodes need the radio", fb, EPD_DRAW_ALIGN_CENTER, INK);
        return;
    }
    switch (g_cv) {
        case CV_CHATS: renderChats(fb); break;
        case CV_NODES: renderNodes(fb); break;
        case CV_CHAT: renderChat(fb); break;
        case CV_QUICK: renderQuick(fb); break;
    }
    if (stealth::active() && g_cv != CV_QUICK) {
        // Listening in stealth: a dark band says nothing is going out.
        epd_fill_rect({0, BAR_Y - 46, W, 38}, INK, fb);
        ui::text(small, W / 2, BAR_Y - 20, "STEALTH: listening, nothing is sent", fb, EPD_DRAW_ALIGN_CENTER, PAPER);
    }
}

void openChat(uint8_t ch, uint32_t dm) {
    g_chatCh = ch;
    g_chatDm = dm;
    g_chatScroll = 0;
    g_cv = CV_CHAT;
}

Go tapComms(int x, int y) {
    if (!mesh_service::enabled()) {
        if (!stealth::active() && hit(kRadioOn, x, y)) mesh_service::setEnabled(true);
        return STAY;
    }
    switch (g_cv) {
        case CV_CHATS:
        case CV_NODES: {
            if (y >= TAB_Y && y < TAB_Y + TAB_H) {
                const CommsView v = x < W / 2 ? CV_CHATS : CV_NODES;
                if (v != g_cv) { g_cv = v; g_listPage = 0; }
                return STAY;
            }
            if (y >= BAR_Y) {
                if (hit(kPrev, x, y) && g_listPage > 0) --g_listPage;
                if (hit(kNext, x, y)) ++g_listPage;   // clamped when drawn
                return STAY;
            }
            const int r = (y - LIST_Y) / ROW_H, per = rowsPerPage();
            if (y < LIST_Y || r >= per) return STAY;
            const int i = g_listPage * per + r;
            if (g_cv == CV_CHATS) {
                Chat chats[MAX_CHATS];
                if (i < collectChats(chats)) openChat(chats[i].ch, chats[i].dm);
            } else {
                mesh_service::Node nd;
                if (i < mesh_service::nodeCount() && mesh_service::nodeAt(i, nd)) openChat(0, nd.num);
            }
            return STAY;
        }
        case CV_CHAT:
            if (hit(kWrite, x, y)) { g_compose.clear(); return TO_COMPOSE; }
            if (hit(kQuick, x, y)) { g_cv = CV_QUICK; return STAY; }
            if (hit(kOlder, x, y)) { g_chatScroll += 3; return STAY; }
            if (hit(kNewer, x, y)) { g_chatScroll = g_chatScroll > 3 ? g_chatScroll - 3 : 0; return STAY; }
            return STAY;
        case CV_QUICK:
            for (int i = 0; i < N_QUICK; ++i)
                if (hit(quickRect(i), x, y)) { sendToChat(kQuickText[i]); g_cv = CV_CHAT; return STAY; }
            if (hit(kSendPos, x, y) && g_myPos) {
                char m[64], g[32];
                milgeo::mgrs(g_myLat, g_myLon, g, sizeof g);
                snprintf(m, sizeof m, "I am at %s (%.5f, %.5f)", g, g_myLat, g_myLon);
                sendToChat(m);
                g_cv = CV_CHAT;
            }
            return STAY;
    }
    return STAY;
}

// Back inside COMMS: quick -> chat -> the list it came from. false at the list.
bool commsBack() {
    if (g_cv == CV_QUICK) { g_cv = CV_CHAT; return true; }
    if (g_cv == CV_CHAT) { g_cv = g_chatDm && g_seenCount >= 0 ? CV_CHATS : CV_CHATS; return true; }
    return false;
}

// --- COMPOSE -----------------------------------------------------------------

void renderCompose(const RideState& s, uint8_t* fb) {
    (void)s;
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    g_compose.top = H - g_compose.height();
    g_compose.enterLabel = "SEND";
    char t[48], hdr[64];
    chatTitle(g_chatCh, g_chatDm, t, sizeof t);
    snprintf(hdr, sizeof hdr, "TO: %s", t);
    fit(ui::font(ui::F_SMALL), hdr, W - 120);
    ui::text(ui::font(ui::F_SMALL), 14, TOP + 34, hdr, fb, EPD_DRAW_ALIGN_LEFT, INK);
    char cnt[16];
    snprintf(cnt, sizeof cnt, "%u/%u", (unsigned)strlen(g_compose.text), (unsigned)mesh::MAX_TEXT_LEN);
    ui::text(ui::font(ui::F_TINY), W - 14, TOP + 34, cnt, fb, EPD_DRAW_ALIGN_RIGHT, INK);
    const EpdRect box = {10, TOP + 48, W - 20, g_compose.top - TOP - 64};
    frame(box, 2, fb);
    char shown[300];
    snprintf(shown, sizeof shown, "%s_", g_compose.text);
    int ly = box.y + 40;
    wrap(ui::font(ui::F_KEY), shown, box.width - 28, [&](const char* l) {
        if (ly < box.y + box.height - 8)
            ui::text(ui::font(ui::F_KEY), box.x + 14, ly, l, fb, EPD_DRAW_ALIGN_LEFT, INK);
        ly += 40;
    });
    g_compose.render(fb);
}

Go tapCompose(int x, int y) {
    const kbd::Action a = g_compose.tap(x, y);
    if (strlen(g_compose.text) > mesh::MAX_TEXT_LEN) kbd::popUtf8(g_compose.text);
    if (a == kbd::ENTER && g_compose.text[0]) {
        sendToChat(g_compose.text);
        g_compose.clear();
        g_cv = CV_CHAT;
        return TO_COMMS;
    }
    return STAY;
}

// --- MAPOPTS: what the map shows ----------------------------------------------------

constexpr int MROW_TOP = TOP + 30, MROW_H = 96;
const EpdRect kOpenPoints = {14, TOP + 30 + 3 * 96 + 36, W - 28, 80};

void renderMapOpts(const RideState& s, uint8_t* fb) {
    (void)s;
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    static const char* const kO[] = {"NORTH", "COURSE"};
    int y = MROW_TOP;
    caption(y, MROW_H, "UP IS", mapTrackUp() ? "map turns with your course" : "north always up", fb);
    segments(y, MROW_H, kO, 2, mapTrackUp() ? 1 : 0, fb);
    epd_fill_rect({16, y + MROW_H - 1, W - 32, 1}, INK, fb);
    y += MROW_H;
    caption(y, MROW_H, "MGRS GRID", "1 km lines, 10 km zoomed out", fb);
    toggle(y, MROW_H, milspec::gridOn(), fb);
    epd_fill_rect({16, y + MROW_H - 1, W - 32, 1}, INK, fb);
    y += MROW_H;
    caption(y, MROW_H, "POINTS", "your waypoints on the map", fb);
    toggle(y, MROW_H, milspec::pointsOn(), fb);
    epd_fill_rect({16, y + MROW_H - 1, W - 32, 1}, INK, fb);
    button(kOpenPoints, "POINTS LIST  >", false, fb);
    const char* tips[] = {"Hold a finger on the map 1 s", "to drop a point there.",
                          "Tap the compass to switch", "north-up / course-up."};
    for (int i = 0; i < 4; ++i)
        ui::text(ui::font(ui::F_SMALL), W / 2, kOpenPoints.y + kOpenPoints.height + 60 + i * 34 + (i >= 2 ? 20 : 0),
                 tips[i], fb, EPD_DRAW_ALIGN_CENTER, INK);
}

Go tapMapOpts(int x, int y) {
    if (hit(kOpenPoints, x, y)) return TO_POINTS;
    const int r = (y - MROW_TOP) / MROW_H;
    if (y < MROW_TOP || r > 2) return STAY;
    if (r == 0) { const int i = segmentHit(x, 2); if (i >= 0) setMapTrackUp(i == 1); }
    if (r == 1 && toggleHit(x)) milspec::setGridOn(x >= CTRL_X + CTRL_W - 75);
    if (r == 2 && toggleHit(x)) milspec::setPointsOn(x >= CTRL_X + CTRL_W - 75);
    return STAY;
}

// A single text field over the keyboard (the callsign).
void renderField(const char* caption, const char* note, kbd::Keyboard& k, const char* enter, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    k.top = H - k.height();
    k.enterLabel = enter;
    ui::text(ui::font(ui::F_SMALL), 14, TOP + 40, caption, fb, EPD_DRAW_ALIGN_LEFT, INK);
    const EpdRect box = {10, TOP + 60, W - 20, 90};
    frame(box, 3, fb);
    char shown[300];
    snprintf(shown, sizeof shown, "%s_", k.text);
    ui::text(ui::font(ui::F_KEY), box.x + 16, box.y + 58, shown, fb, EPD_DRAW_ALIGN_LEFT, INK);
    if (note) ui::text(ui::font(ui::F_TINY), 14, box.y + box.height + 34, note, fb, EPD_DRAW_ALIGN_LEFT, INK);
    k.render(fb);
}

Go tapSetupName(int x, int y) {
    const kbd::Action a = g_nameKbd.tap(x, y);
    while (strlen(g_nameKbd.text) > 36) kbd::popUtf8(g_nameKbd.text);
    if (a == kbd::ENTER && g_nameKbd.text[0]) {
        char sh[20];
        shortName(g_nameKbd.text, sh, sizeof sh);
        mesh_service::setNames(g_nameKbd.text, sh);
        return TO_SETUP;
    }
    return STAY;
}

// --- POINTS ----------------------------------------------------------------------

int g_pointsScroll = 0;
int g_pointSel = -1;
bool g_deleteArmed = false;
const char* g_pointsNote = nullptr;     // a one-off line ("no fix yet")
constexpr int PROW_TOP = TOP + 262, PROW_H = 88, PROW_N = 6;
const EpdRect kMark = {12, TOP + 158, 250, 88};
const EpdRect kAdd = {278, TOP + 158, 250, 88};
const EpdRect kUp = {12, H - 92, 250, 80};
const EpdRect kDown = {278, H - 92, 250, 80};

void renderPoints(const RideState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    double lat, lon;
    const bool have = here(s, lat, lon);
    ui::text(ui::font(ui::F_TINY), 14, TOP + 30, s.gpsFix ? "MY POSITION" : have ? "LAST POSITION (NO FIX)" : "MY POSITION",
             fb, EPD_DRAW_ALIGN_LEFT, INK);
    char m[32] = "--";
    if (have) milgeo::mgrs(lat, lon, m, sizeof m);
    ui::text(ui::font(ui::F_BODY_H), 14, TOP + 76, have ? m : "NO FIX YET", fb, EPD_DRAW_ALIGN_LEFT, INK);
    char ll[64];
    float dep;
    if (have && doba_dem::depth(lat, lon, dep))   // on water: depth under you (EMODnet)
        snprintf(ll, sizeof ll, "%c%.5f %c%.5f  DEPTH %dm  GPS %d", lat >= 0 ? 'N' : 'S', fabs(lat),
                 lon >= 0 ? 'E' : 'W', fabs(lon), (int)lroundf(dep), s.satellites);
    else if (have) snprintf(ll, sizeof ll, "%c%.5f %c%.5f  ALT %dm  GPS %d", lat >= 0 ? 'N' : 'S', fabs(lat),
                       lon >= 0 ? 'E' : 'W', fabs(lon), (int)lroundf(groundAlt(s)), s.satellites);
    else snprintf(ll, sizeof ll, "waiting for satellites · GPS %d", s.satellites);
    ui::text(ui::font(ui::F_SMALL), 14, TOP + 116, ll, fb, EPD_DRAW_ALIGN_LEFT, INK);
    if (g_pointsNote) {
        ui::text(ui::font(ui::F_TINY), W - 14, TOP + 30, g_pointsNote, fb, EPD_DRAW_ALIGN_RIGHT, INK);
        g_pointsNote = nullptr;
    }
    button(kMark, "MARK HERE", true, fb);
    button(kAdd, "ADD GRID", false, fb);

    const int n = doba_points::count();
    if (g_pointsScroll > n - 1) g_pointsScroll = n > 0 ? ((n - 1) / PROW_N) * PROW_N : 0;
    if (n == 0)
    {
        ui::text(ui::font(ui::F_KEY), W / 2, PROW_TOP + 100, "No points yet", fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::text(ui::font(ui::F_SMALL), W / 2, PROW_TOP + 150, "MARK HERE saves where you stand,", fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::text(ui::font(ui::F_SMALL), W / 2, PROW_TOP + 182, "ADD GRID takes an MGRS reference,", fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::text(ui::font(ui::F_SMALL), W / 2, PROW_TOP + 214, "or hold a finger on the map.", fb, EPD_DRAW_ALIGN_CENTER, INK);
    }
    for (int k = 0; k < PROW_N && g_pointsScroll + k < n; ++k) {
        const int i = g_pointsScroll + k;
        const doba_points::Point* p = doba_points::at(i);
        const int y = PROW_TOP + k * PROW_H;
        const bool tgt = doba_points::target() == i;
        if (tgt) epd_fill_rect({10, y + 4, W - 20, PROW_H - 8}, INK, fb);
        const uint8_t fg = tgt ? PAPER : INK;
        char nm[40];
        snprintf(nm, sizeof nm, tgt ? "> %s" : "%s", p->name);
        ui::text(ui::font(ui::F_LABEL), 22, y + 38, nm, fb, EPD_DRAW_ALIGN_LEFT, fg);
        char pm[32];
        milgeo::mgrs(p->lat, p->lon, pm, sizeof pm);
        ui::text(ui::font(ui::F_TINY), 22, y + 70, pm, fb, EPD_DRAW_ALIGN_LEFT, fg);
        if (have) {
            double b, d;
            doba_points::fromHere(i, lat, lon, b, d);
            char az[16], ds[24];
            azText(b, az, sizeof az);
            doba_points::formatDistance(d, ds, sizeof ds);
            ui::text(ui::font(ui::F_KEY), W - 22, y + 40, az, fb, EPD_DRAW_ALIGN_RIGHT, fg);
            ui::text(ui::font(ui::F_TINY), W - 30 - ui::textWidth(ui::font(ui::F_KEY), az), y + 38, "AZ", fb,
                     EPD_DRAW_ALIGN_RIGHT, fg);
            ui::text(ui::font(ui::F_SMALL), W - 22, y + 72, ds, fb, EPD_DRAW_ALIGN_RIGHT, fg);
        }
        if (!tgt) epd_fill_rect({10, y + PROW_H - 2, W - 20, 1}, INK, fb);
    }
    if (n > PROW_N) {
        button(kUp, "UP", false, fb);
        button(kDown, "DOWN", false, fb);
    }
}

kbd::Keyboard g_newGrid, g_newName;
bool g_newFocusName = false;
const char* g_newError = nullptr;

Go tapPoints(int x, int y, const RideState& s) {
    double lat, lon;
    if (hit(kMark, x, y)) {
        if (!here(s, lat, lon)) { g_pointsNote = "no fix yet"; return STAY; }
        const int i = doba_points::add(nullptr, lat, lon);
        g_pointsNote = i < 0 ? "list full" : "marked";
        if (i >= 0) g_pointsScroll = (i / PROW_N) * PROW_N;
        return STAY;
    }
    if (hit(kAdd, x, y)) {
        g_newGrid.clear();
        g_newName.clear();
        g_newFocusName = false;
        g_newError = nullptr;
        // Start from this square: most points are typed in near where you are.
        if (here(s, lat, lon)) {
            char m[32];
            milgeo::mgrs(lat, lon, m, sizeof m);
            if (strlen(m) > 7) { memcpy(g_newGrid.text, m, 7); g_newGrid.text[7] = 0; }
        }
        g_newGrid.layout = kbd::SYM;   // digits first
        return TO_POINT_NEW;
    }
    const int n = doba_points::count();
    if (n > PROW_N && hit(kUp, x, y)) { g_pointsScroll = g_pointsScroll >= PROW_N ? g_pointsScroll - PROW_N : 0; return STAY; }
    if (n > PROW_N && hit(kDown, x, y)) { if (g_pointsScroll + PROW_N < n) g_pointsScroll += PROW_N; return STAY; }
    if (y >= PROW_TOP && y < PROW_TOP + PROW_N * PROW_H) {
        const int i = g_pointsScroll + (y - PROW_TOP) / PROW_H;
        if (i < n) { g_pointSel = i; g_deleteArmed = false; return TO_POINT; }
    }
    return STAY;
}

// --- POINT: one waypoint -----------------------------------------------------------

const EpdRect kGoto = {12, H - 196, 516, 92};
const EpdRect kDel = {12, H - 92, 250, 80};
const EpdRect kToMap = {278, H - 92, 250, 80};

void arrow(int cx, int cy, int r, float deg, uint8_t c, uint8_t* fb) {
    const float a = deg * (float)M_PI / 180.0f;
    const float sx = sinf(a), sy = -cosf(a);
    const int tx = cx + (int)(sx * r), ty = cy + (int)(sy * r);
    const int bx = cx - (int)(sx * r * 0.35f), by = cy - (int)(sy * r * 0.35f);
    const int px = (int)(-sy * r * 0.28f), py = (int)(sx * r * 0.28f);
    epd_fill_triangle(tx, ty, bx + px, by + py, cx, cy, c, fb);
    epd_fill_triangle(tx, ty, bx - px, by - py, cx, cy, c, fb);
}

void renderPoint(const RideState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    const doba_points::Point* p = doba_points::at(g_pointSel);
    if (!p) return;
    ui::text(ui::font(ui::F_TITLE), 14, TOP + 60, p->name, fb, EPD_DRAW_ALIGN_LEFT, INK);
    char pm[32];
    milgeo::mgrs(p->lat, p->lon, pm, sizeof pm);
    ui::text(ui::font(ui::F_BODY_H), 14, TOP + 108, pm, fb, EPD_DRAW_ALIGN_LEFT, INK);
    char ll[48];
    snprintf(ll, sizeof ll, "%c%.5f  %c%.5f", p->lat >= 0 ? 'N' : 'S', fabs(p->lat), p->lon >= 0 ? 'E' : 'W', fabs(p->lon));
    ui::text(ui::font(ui::F_SMALL), 14, TOP + 144, ll, fb, EPD_DRAW_ALIGN_LEFT, INK);

    double lat, lon;
    const int dialY = TOP + 340, dialR = 150, dialX = 175;
    if (here(s, lat, lon)) {
        double b, d;
        doba_points::fromHere(g_pointSel, lat, lon, b, d);
        // North-up dial: ticks every 10-00 (60°), N marked, the arrow to the point.
        for (int t = 0; t < 3; ++t) epd_draw_circle(dialX, dialY, dialR - t, INK, fb);
        for (int k = 0; k < 12; ++k) {
            const float a = k * 30.0f * (float)M_PI / 180.0f;
            const int r0 = k % 3 ? dialR - 14 : dialR - 26;
            epd_draw_line(dialX + (int)(sinf(a) * r0), dialY - (int)(cosf(a) * r0),
                          dialX + (int)(sinf(a) * dialR), dialY - (int)(cosf(a) * dialR), INK, fb);
        }
        ui::text(ui::font(ui::F_LABEL), dialX, dialY - dialR + 52, "N", fb, EPD_DRAW_ALIGN_CENTER, INK);
        arrow(dialX, dialY, dialR - 34, (float)b, INK, fb);
        epd_fill_circle(dialX, dialY, 6, PAPER, fb);
        epd_draw_circle(dialX, dialY, 6, INK, fb);
        if (s.gpsFix && s.gpsSpeedKmh >= 2.0f) {   // where you are heading, as a ring tick
            const float a = s.courseDeg * (float)M_PI / 180.0f;
            epd_fill_circle(dialX + (int)(sinf(a) * (dialR + 12)), dialY - (int)(cosf(a) * (dialR + 12)), 8, INK, fb);
        }
        char az[16], deg[16], ds[24];
        azText(b, az, sizeof az);
        snprintf(deg, sizeof deg, "%03d\xC2\xB0", (int)lround(b) % 360);
        doba_points::formatDistance(d, ds, sizeof ds);
        const int tx = 346;
        ui::text(ui::font(ui::F_TINY), tx, dialY - 92, "AZIMUTH", fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(ui::font(ui::F_TITLE), tx, dialY - 46, az, fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(ui::font(ui::F_SMALL), tx, dialY - 12, deg, fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(ui::font(ui::F_TINY), tx, dialY + 42, "DISTANCE", fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(ui::font(ui::F_TITLE), tx, dialY + 88, ds, fb, EPD_DRAW_ALIGN_LEFT, INK);
        // Height of the point (DEM) and the climb to it.
        float pz;
        bool wet = false;
        if (doba_dem::ground(p->lat, p->lon, pz, wet) && wet) {
            char hz[48];
            snprintf(hz, sizeof hz, "DEPTH %dm", (int)lroundf(pz));
            ui::text(ui::font(ui::F_SMALL), tx, dialY + 126, hz, fb, EPD_DRAW_ALIGN_LEFT, INK);
        } else if (doba_dem::height(p->lat, p->lon, pz)) {
            char hz[48];
            const int dz = (int)lroundf(pz - groundAlt(s));
            snprintf(hz, sizeof hz, "HEIGHT %dm", (int)lroundf(pz));
            ui::text(ui::font(ui::F_SMALL), tx, dialY + 126, hz, fb, EPD_DRAW_ALIGN_LEFT, INK);
            snprintf(hz, sizeof hz, dz >= 0 ? "climb %+dm" : "drop %dm", dz);
            ui::text(ui::font(ui::F_SMALL), tx, dialY + 156, hz, fb, EPD_DRAW_ALIGN_LEFT, INK);
        }
        if (!s.gpsFix)
            ui::text(ui::font(ui::F_TINY), W / 2, dialY + dialR + 40, "from the last fix", fb, EPD_DRAW_ALIGN_CENTER, INK);
    } else {
        ui::text(ui::font(ui::F_SMALL), W / 2, dialY, "no fix: azimuth when GPS has one", fb, EPD_DRAW_ALIGN_CENTER, INK);
    }
    const bool tgt = doba_points::target() == g_pointSel;
    button(kGoto, tgt ? "STOP GUIDING" : "GO TO  (show on map)", !tgt, fb);
    button(kDel, g_deleteArmed ? "SURE? TAP" : "DELETE", g_deleteArmed, fb);
    button(kToMap, "MAP", false, fb);
}

Go tapPoint(int x, int y) {
    if (!doba_points::at(g_pointSel)) return TO_POINTS;
    if (hit(kGoto, x, y)) {
        if (doba_points::target() == g_pointSel) { doba_points::setTarget(-1); return STAY; }
        doba_points::setTarget(g_pointSel);
        return TO_MAP;
    }
    if (hit(kDel, x, y)) {
        if (!g_deleteArmed) { g_deleteArmed = true; return STAY; }
        doba_points::remove(g_pointSel);
        g_pointSel = -1;
        return TO_POINTS;
    }
    if (hit(kToMap, x, y)) return TO_MAP;
    g_deleteArmed = false;
    return STAY;
}

// --- POINT_NEW: a waypoint typed in as MGRS ------------------------------------------

// At least 1 km precision (two digit pairs) before a grid counts: the bare
// "32U LC" the field starts with would otherwise be a point 50 km away.
int gridDigits(const char* t) {
    int n = 0;
    for (; *t; ++t) if (*t >= '0' && *t <= '9') ++n;
    return n;
}
bool parseGrid(const char* t, double& la, double& lo) {
    // zone digits (1-2) are not easting/northing digits
    int zone = 0;
    while (t[zone] >= '0' && t[zone] <= '9') ++zone;
    return gridDigits(t) - zone >= 4 && milgeo::parseMgrs(t, la, lo);
}

const EpdRect kGridBox = {10, TOP + 46, W - 20, 84};
const EpdRect kNameBox = {10, TOP + 176, W - 20, 84};

void renderPointNew(const RideState& s, uint8_t* fb) {
    (void)s;
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    kbd::Keyboard& k = g_newFocusName ? g_newName : g_newGrid;
    k.top = H - k.height();
    k.enterLabel = "SAVE";
    auto field = [&](const EpdRect& r, const char* cap, const char* txt, bool focus, const char* empty) {
        ui::text(ui::font(ui::F_TINY), r.x + 4, r.y - 12, cap, fb, EPD_DRAW_ALIGN_LEFT, INK);
        frame(r, focus ? 4 : 1, fb);
        char shown[300];
        snprintf(shown, sizeof shown, focus ? "%s_" : "%s", txt[0] ? txt : (focus ? "" : empty));
        ui::text(ui::font(ui::F_KEY), r.x + 16, r.y + 54, shown, fb, EPD_DRAW_ALIGN_LEFT, INK);
    };
    field(kGridBox, "GRID (MGRS)  e.g. 32U LC 12345 67890", g_newGrid.text, !g_newFocusName, "");
    field(kNameBox, "NAME (optional)", g_newName.text, g_newFocusName, "PT..");
    double la, lo;
    const char* line = g_newError;
    char ok[64];
    if (!line && g_newGrid.text[0] && parseGrid(g_newGrid.text, la, lo)) {
        snprintf(ok, sizeof ok, "= %c%.5f %c%.5f", la >= 0 ? 'N' : 'S', fabs(la), lo >= 0 ? 'E' : 'W', fabs(lo));
        line = ok;
    }
    if (line) ui::text(ui::font(ui::F_SMALL), 14, kNameBox.y + kNameBox.height + 44, line, fb, EPD_DRAW_ALIGN_LEFT, INK);
    k.render(fb);
}

Go tapPointNew(int x, int y) {
    if (hit(kGridBox, x, y)) { g_newFocusName = false; return STAY; }
    if (hit(kNameBox, x, y)) { g_newFocusName = true; return STAY; }
    kbd::Keyboard& k = g_newFocusName ? g_newName : g_newGrid;
    const kbd::Action a = k.tap(x, y);
    while (strlen(g_newName.text) > doba_points::NAME_LEN - 1) kbd::popUtf8(g_newName.text);
    while (strlen(g_newGrid.text) > 24) kbd::popUtf8(g_newGrid.text);
    g_newError = nullptr;
    if (a == kbd::ENTER) {
        double la, lo;
        if (!parseGrid(g_newGrid.text, la, lo)) { g_newError = "need zone, square and 4-10 digits"; g_newFocusName = false; return STAY; }
        const int i = doba_points::add(g_newName.text, la, lo);
        if (i < 0) { g_newError = "list full (64)"; return STAY; }
        g_pointsScroll = (i / PROW_N) * PROW_N;
        g_pointsNote = "added";
        return TO_POINTS;
    }
    return STAY;
}

// --- SOS: a distress call over the mesh -------------------------------------------

constexpr uint32_t SOS_EVERY_MS = 5 * 60 * 1000;
bool g_sosOn = false, g_sosArmed = false;
uint32_t g_sosArmedAt = 0, g_sosLast = 0;
int g_sosSent = 0;
const EpdRect kSosBtn = {30, TOP + 300, W - 60, 220};
const EpdRect kSosCancel = {30, H - 130, W - 60, 96};

void wrapAt(const EpdFont* f, int x, int y, int maxW, int lh, const char* text, uint8_t* fb) {
    int k = 0;
    wrap(f, text, maxW, [&](const char* l) {
        ui::text(f, x, y + k * lh, l, fb, EPD_DRAW_ALIGN_LEFT, INK);
        ++k;
    });
}

void sosMessage(const RideState& s, char* out, size_t n) {
    char where[32] = "position unknown";
    if (s.gpsFix || s.everHadFix) milgeo::mgrs(s.latitude, s.longitude, where, sizeof where);
    char when[16] = "";
    if (s.utc) {
        struct tm g;
        const time_t t = s.utc;
        gmtime_r(&t, &g);
        snprintf(when, sizeof when, " %02d%02dZ", g.tm_hour, g.tm_min);
    }
    snprintf(out, n, "SOS SOS SOS %s %s%s%s", mesh_service::longName(), where, when,
             s.gpsFix ? "" : (s.everHadFix ? " (last fix)" : ""));
}

void sosSend(const RideState& s) {
    // Calling for help is the one thing that ends stealth by itself.
    if (stealth::active()) stealth::set(false);
    if (!mesh_service::enabled()) mesh_service::setEnabled(true);
    char msg[200];
    sosMessage(s, msg, sizeof msg);
    mesh_service::queueText(0xFFFFFFFFu, msg, 0);
    g_sosLast = millis();
    ++g_sosSent;
    Serial.printf("[sos] sent #%d: %s\n", g_sosSent, msg);
}

void sosSubtitle(char* out, size_t n) {
    if (g_sosOn) snprintf(out, n, "ACTIVE · sent %d", g_sosSent);
    else snprintf(out, n, "distress call");
}

void renderSos(const RideState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    char msg[200];
    sosMessage(s, msg, sizeof msg);
    if (!g_sosOn) {
        ui::text(ui::font(ui::F_SMALL), W / 2, TOP + 60, "Broadcasts on the mesh, every 5 min:", fb, EPD_DRAW_ALIGN_CENTER, INK);
        frame({20, TOP + 84, W - 40, 150}, 1, fb);
        wrapAt(ui::font(ui::F_BODY), 36, TOP + 124, W - 72, 34, msg, fb);
        const bool armed = g_sosArmed && millis() - g_sosArmedAt < 6000;
        epd_fill_rect(kSosBtn, INK, fb);
        ui::text(ui::font(ui::F_BIG), W / 2, kSosBtn.y + 120, "SOS", fb, EPD_DRAW_ALIGN_CENTER, PAPER);
        ui::text(ui::font(ui::F_SMALL), W / 2, kSosBtn.y + 180, armed ? "TAP AGAIN TO SEND" : "tap, then tap again", fb,
                 EPD_DRAW_ALIGN_CENTER, PAPER);
        ui::text(ui::font(ui::F_TINY), W / 2, kSosBtn.y + kSosBtn.height + 50,
                 stealth::active() ? "STEALTH IS ON: sending ends it and reveals you" :
                 mesh_service::enabled() ? "radio is on" : "radio is off: it will be turned on", fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::text(ui::font(ui::F_TINY), W / 2, kSosBtn.y + kSosBtn.height + 80,
                 "Anyone on the channel within range sees it.", fb, EPD_DRAW_ALIGN_CENTER, INK);
        return;
    }
    epd_fill_rect({0, TOP, W, 290}, INK, fb);
    ui::text(ui::font(ui::F_BIG), W / 2, TOP + 130, "SOS ACTIVE", fb, EPD_DRAW_ALIGN_CENTER, PAPER);
    char line[64];
    const uint32_t left = g_sosLast + SOS_EVERY_MS > millis() ? (g_sosLast + SOS_EVERY_MS - millis()) / 1000 : 0;
    snprintf(line, sizeof line, "sent %d · next in %lu:%02lu", g_sosSent, (unsigned long)(left / 60),
             (unsigned long)(left % 60));
    ui::text(ui::font(ui::F_LABEL), W / 2, TOP + 210, line, fb, EPD_DRAW_ALIGN_CENTER, PAPER);
    frame({20, TOP + 320, W - 40, 150}, 1, fb);
    wrapAt(ui::font(ui::F_BODY), 36, TOP + 360, W - 72, 34, msg, fb);
    ui::text(ui::font(ui::F_SMALL), W / 2, TOP + 530, "Stay with the device. Replies show in COMMS.", fb,
             EPD_DRAW_ALIGN_CENTER, INK);
    button(kSosCancel, "CANCEL SOS", false, fb);
}

Go tapSos(int x, int y, const RideState& s) {
    if (!g_sosOn) {
        if (!hit(kSosBtn, x, y)) { g_sosArmed = false; return STAY; }
        if (g_sosArmed && millis() - g_sosArmedAt < 6000) {
            g_sosArmed = false;
            g_sosOn = true;
            g_sosSent = 0;
            sosSend(s);
        } else {
            g_sosArmed = true;
            g_sosArmedAt = millis();
        }
        return STAY;
    }
    if (hit(kSosCancel, x, y)) {
        g_sosOn = false;
        Serial.println("[sos] cancelled");
        char msg[96];
        snprintf(msg, sizeof msg, "SOS cancelled by %s", mesh_service::longName());
        mesh_service::queueText(0xFFFFFFFFu, msg, 0);
    }
    return STAY;
}

// --- SUN ---------------------------------------------------------------------------

void hhmm(time_t utc, char* out, size_t n) {
    const time_t t = utc + settings::tzMinutes() * 60;
    struct tm g;
    gmtime_r(&t, &g);
    snprintf(out, n, "%02d:%02d", g.tm_hour, g.tm_min);
}

void sunSubtitle(const RideState& s, char* out, size_t n) {
    if (!s.utc || !(s.gpsFix || s.everHadFix)) { snprintf(out, n, "needs a GPS fix"); return; }
    const doba_sun::Day d = doba_sun::day(s.latitude, s.longitude, s.utc);
    char t[8];
    if (d.setOk && s.utc < d.set && (!d.riseOk || s.utc > d.rise)) { hhmm(d.set, t, sizeof t); snprintf(out, n, "sunset %s", t); }
    else if (d.riseOk) {
        const time_t r = s.utc > d.rise ? doba_sun::day(s.latitude, s.longitude, s.utc + 86400).rise : d.rise;
        hhmm(r, t, sizeof t);
        snprintf(out, n, "sunrise %s", t);
    } else snprintf(out, n, d.setOk ? "" : "polar day/night");
}

void renderSun(const RideState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    if (!s.utc || !(s.gpsFix || s.everHadFix)) {
        ui::text(ui::font(ui::F_KEY), W / 2, TOP + 300, "Needs a GPS fix", fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::text(ui::font(ui::F_SMALL), W / 2, TOP + 340, "for the date, time and place.", fb, EPD_DRAW_ALIGN_CENTER, INK);
        return;
    }
    const doba_sun::Day d = doba_sun::day(s.latitude, s.longitude, s.utc);
    char a[8], b[8], line[64];
    // Daylight left: the headline when the sun is up.
    int y = TOP + 40;
    if (d.riseOk && d.setOk && s.utc >= d.rise && s.utc < d.set) {
        const long left = (long)(d.set - s.utc) / 60;
        ui::text(ui::font(ui::F_TINY), 20, y + 6, "DAYLIGHT LEFT", fb, EPD_DRAW_ALIGN_LEFT, INK);
        snprintf(line, sizeof line, "%ld h %02ld min", left / 60, left % 60);
        ui::text(ui::font(ui::F_BIG), 20, y + 80, line, fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (d.duskOk) {
            const long l2 = (long)(d.dusk - s.utc) / 60;
            snprintf(line, sizeof line, "+ %ld min of twilight after sunset", l2 - left);
            ui::text(ui::font(ui::F_SMALL), 20, y + 120, line, fb, EPD_DRAW_ALIGN_LEFT, INK);
        }
    } else {
        ui::text(ui::font(ui::F_TINY), 20, y + 6, "NIGHT", fb, EPD_DRAW_ALIGN_LEFT, INK);
        const doba_sun::Day nx = s.utc > d.rise ? doba_sun::day(s.latitude, s.longitude, s.utc + 86400) : d;
        if (nx.riseOk) {
            const long to = (long)(nx.rise - s.utc) / 60;
            snprintf(line, sizeof line, "%ld h %02ld min", to / 60, to % 60);
            ui::text(ui::font(ui::F_BIG), 20, y + 80, line, fb, EPD_DRAW_ALIGN_LEFT, INK);
            ui::text(ui::font(ui::F_SMALL), 20, y + 120, "until sunrise", fb, EPD_DRAW_ALIGN_LEFT, INK);
        }
    }
    // The day's table.
    y = TOP + 200;
    epd_fill_rect({20, y, W - 40, 2}, INK, fb);
    struct Row { const char* name; bool ok; time_t t; } rows[] = {
        {"FIRST LIGHT", d.dawnOk, d.dawn}, {"SUNRISE", d.riseOk, d.rise},
        {"SUNSET", d.setOk, d.set},        {"LAST LIGHT", d.duskOk, d.dusk},
    };
    for (int i = 0; i < 4; ++i) {
        const int ry = y + 16 + i * 72;
        ui::text(ui::font(ui::F_LABEL), 24, ry + 44, rows[i].name, fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (rows[i].ok) hhmm(rows[i].t, a, sizeof a); else snprintf(a, sizeof a, "--");
        ui::text(ui::font(ui::F_TITLE), W - 24, ry + 48, a, fb, EPD_DRAW_ALIGN_RIGHT, INK);
        epd_fill_rect({20, ry + 70, W - 40, 1}, INK, fb);
    }
    // Sun's bearing at rise and set? Keep it simple: the moon.
    y += 16 + 4 * 72 + 30;
    const double lit = doba_sun::moonLit(s.utc);
    const int mx = 90, my = y + 80, mr = 52;
    epd_fill_circle(mx, my, mr, INK, fb);
    // lit part: an ellipse-ish terminator, drawn as white columns
    const double age = doba_sun::moonAge(s.utc);
    const bool waxing = age < 14.765;
    for (int dx = -mr; dx <= mr; ++dx) {
        const int h = (int)sqrt((double)(mr * mr - dx * dx));
        const double term = mr * (1 - 2 * lit);                // x of the terminator, from the lit edge
        const double xs = waxing ? dx : -dx;                   // lit edge on the right when waxing
        if (xs > term) epd_fill_rect({mx + dx, my - h, 1, 2 * h}, PAPER, fb);
    }
    epd_draw_circle(mx, my, mr, INK, fb);
    ui::text(ui::font(ui::F_KEY), 170, y + 66, doba_sun::moonName(s.utc), fb, EPD_DRAW_ALIGN_LEFT, INK);
    snprintf(line, sizeof line, "%d%% lit · day %d of 29", (int)lround(lit * 100), (int)age);
    ui::text(ui::font(ui::F_SMALL), 170, y + 104, line, fb, EPD_DRAW_ALIGN_LEFT, INK);
    hhmm(s.utc, b, sizeof b);
    snprintf(line, sizeof line, "now %s  ·  UTC%+d  ·  times local", b, settings::tzMinutes() / 60);
    ui::text(ui::font(ui::F_TINY), W / 2, H - 24, line, fb, EPD_DRAW_ALIGN_CENTER, INK);
}


// --- MORE ----------------------------------------------------------------------

enum MoreRow { M_USB, M_GPS, M_MODE, M_CARD, M_ABOUT, M_COUNT };
int moreRowY(int r) { return TOP + 4 + SEC_H + r * SROW_H; }

void renderMore(const RideState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    section(TOP + 4, "DEVICE", fb);
    for (int r = 0; r < M_COUNT; ++r) {
        const int y = moreRowY(r);
        char sub[64];
        switch (r) {
            case M_USB:
                caption(y, SROW_H, "USB DRIVE", settings::usbDrive() ? "the card shows on a computer" : "off: the card is Doba's", fb);
                toggle(y, SROW_H, settings::usbDrive(), fb);
                break;
            case M_GPS:
                snprintf(sub, sizeof sub, s.gpsFix ? "fix · %u satellites" : "no fix yet", (unsigned)s.satellites);
                caption(y, SROW_H, "GPS", sub, fb);
                button({CTRL_X, y + (SROW_H - CTRL_H) / 2, CTRL_W, CTRL_H}, "DETAILS  >", false, fb);
                break;
            case M_MODE:
                // Meshtastic's modem preset. Every node on the mesh has to match;
                // LongFast is what they all use unless told otherwise.
                snprintf(sub, sizeof sub, "all nodes must match");
                caption(y, SROW_H, "RADIO MODE", sub, fb);
                stepper(y, SROW_H, mesh_service::presetName(), fb);
                break;
            case M_CARD:
                snprintf(sub, sizeof sub, s.sdCard == SD_CARD_OK ? "%d books" : "no card", doba_lib::count());
                caption(y, SROW_H, "CARD", sub, fb);
                break;
            case M_ABOUT:
                caption(y, SROW_H, "DOBA", "offline backcountry assistant · " __DATE__, fb);
                break;
        }
        epd_fill_rect({16, y + SROW_H - 1, W - 32, 1}, INK, fb);
    }
    ui::text(ui::font(ui::F_TINY), W / 2, H - 24, "USB DRIVE: copy books and maps from a computer", fb,
             EPD_DRAW_ALIGN_CENTER, INK);
}

Go tapMore(int x, int y) {
    const int r = (y - moreRowY(0)) / SROW_H;
    if (y < moreRowY(0) || r >= M_COUNT) return STAY;
    if (r == M_USB && toggleHit(x)) {
        const bool on = x >= CTRL_X + CTRL_W - 75;
        settings::setUsbDrive(on);
        usb_storage::setDriveEnabled(on);
    }
    if (r == M_GPS && x >= CTRL_X) return TO_GPSDEBUG;
    if (r == M_MODE && x >= CTRL_X) {
        const int n = mesh::PRESET_COUNT, step = stepperHit(x) < 0 ? -1 : 1;
        mesh_service::setPreset((uint8_t)(((mesh_service::presetIndex() + step) % n + n) % n));
    }
    return STAY;
}

}  // namespace

void render(Page p, const RideState& s, uint8_t* fb) {
    switch (p) {
        case HOME:    renderHome(s, fb); topo_bg::draw({0, TOP, W, H - TOP}, fb, 0); break;
        case COMMS:   renderComms(s, fb); break;
        case COMPOSE: renderCompose(s, fb); break;
        case LIBRARY:
            doba_lib::renderSearch(g_search, fb);   // icon grid: no contours behind it
            break;
        case ARTICLE: doba_lib::renderArticle(fb); break;
        case SETUP:   renderSetup(s, fb); break;
        case SETUP_NAME:
            renderField("CALLSIGN  (how the mesh sees you)", "Short name: first four letters", g_nameKbd, "SAVE", fb);
            break;
        case POINTS:  renderPoints(s, fb); break;
        case POINT:   renderPoint(s, fb); break;
        case POINT_NEW: renderPointNew(s, fb); break;
        case MAPOPTS: renderMapOpts(s, fb); break;
        case SOS:     renderSos(s, fb); break;
        case SUN:     renderSun(s, fb); break;
        case MORE:    renderMore(s, fb); break;
    }
}

bool sosActive() { return g_sosOn; }

bool tick(const RideState& s) {
    if (g_sosOn && millis() - g_sosLast >= SOS_EVERY_MS) {
        sosSend(s);
        return true;
    }
    return false;
}

void openPoint(int i) {
    g_pointSel = i;
    g_deleteArmed = false;
}

const char* title(Page p) {
    switch (p) {
        case HOME: return nullptr;
        case COMMS: return "COMMS";
        case COMPOSE: return "WRITE";
        case LIBRARY: return doba_lib::libraryTitle();
        case ARTICLE: return "LIBRARY";
        case SETUP: case SETUP_NAME: return "SETTINGS";
        case POINTS: return "POSITION";
        case POINT: return "POINT";
        case POINT_NEW: return "NEW POINT";
        case MAPOPTS: return "MAP";
        case SOS: return "SOS";
        case SUN: return "SUN & MOON";
        case MORE: return "MORE";
    }
    return nullptr;
}

Go tap(Page p, int x, int y, const RideState& s) {
    switch (p) {
        case HOME:    return tapHome(x, y);
        case COMMS:   return tapComms(x, y);
        case COMPOSE: return tapCompose(x, y);
        case LIBRARY: return doba_lib::tapSearch(g_search, x, y) ? TO_ARTICLE : STAY;
        case ARTICLE: doba_lib::tapArticle(x, y); return STAY;
        case SETUP:   return tapSetup(x, y);
        case SETUP_NAME: return tapSetupName(x, y);
        case POINTS:  return tapPoints(x, y, s);
        case POINT:   return tapPoint(x, y);
        case POINT_NEW: return tapPointNew(x, y);
        case MAPOPTS: return tapMapOpts(x, y);
        case SOS:     return tapSos(x, y, s);
        case SUN:     return STAY;
        case MORE:    return tapMore(x, y);
    }
    return STAY;
}

Go back(Page p) {
    switch (p) {
        case HOME:    return TO_MAP;
        case COMMS:   return commsBack() ? STAY : TO_HOME;
        case COMPOSE: return TO_COMMS;
        case LIBRARY: return doba_lib::libraryBack() ? STAY : TO_HOME;
        case ARTICLE:
            if (doba_lib::articleBack()) return STAY;   // back along a followed link
            return TO_LIBRARY;
        case SETUP:   return TO_HOME;
        case SETUP_NAME: return TO_SETUP;
        case POINTS:  return TO_HOME;
        case POINT:   return TO_POINTS;
        case POINT_NEW: return TO_POINTS;
        case MAPOPTS: return TO_MAP;
        case SOS:     return TO_HOME;
        case SUN:     return TO_HOME;
        case MORE:    return TO_SETUP;
    }
    return TO_HOME;
}

}  // namespace doba_ui
