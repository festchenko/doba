// DOBA: main screen, comms and library pages — see doba_ui.h.
#include "doba_ui.h"
#include "stealth.h"
#include "settings.h"
#include "usb_storage.h"
#include "doba_topo_bg.h"
#include "doba_pois.h"
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
    if (dark) ui::fillRound(r, 8, INK, fb);
    else ui::frameRound(r, 8, 2, fb);
    char l[48];
    ui::text(ui::font(ui::F_LABEL), r.x + r.width / 2, r.y + r.height / 2 + 10, ui::sentence(label, l, sizeof l), fb,
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
// The design canvas's home: 24 px margins, 14 px gaps, rounded tiles.
constexpr int MARGIN = 24, TILE_GAP = 14;
constexpr int TILE_W = (W - 2 * MARGIN - TILE_GAP) / 2;
constexpr int STEALTH_H = 72;
constexpr int TILE_TOP = TOP + 6;
constexpr int TILE_H = (H - 24 - STEALTH_H - TILE_GAP - TILE_TOP - 3 * TILE_GAP) / 4;
const EpdRect kStealthBar = {MARGIN, H - 24 - STEALTH_H, W - 2 * MARGIN, STEALTH_H};
uint32_t g_stealthOffAsk = 0;   // a short tap on a lit stealth bar: say to hold
uint32_t g_pressMs = 0;         // how long the finger was down for this tap
constexpr uint32_t kHoldMs = 4500;   // "hold 5 s": going loud again, and sending SOS (touch is read a beat late)

EpdRect tileRect(int col, int row) {
    return {MARGIN + col * (TILE_W + TILE_GAP), TILE_TOP + row * (TILE_H + TILE_GAP), TILE_W, TILE_H};
}

// DOBA is for survival, not a navigator: no routes, no recorded tracks.
const Tile kTiles[8] = {
    {tileRect(0, 0), "Map", TO_MAP},         {tileRect(1, 0), "Position", TO_POINTS},
    {tileRect(0, 1), "Library", TO_LIBRARY}, {tileRect(1, 1), "Comms", TO_COMMS},
    {tileRect(0, 2), "SOS", TO_SOS},         {tileRect(1, 2), "Sun", TO_SUN},
    {tileRect(0, 3), "Settings", TO_SETUP},  {tileRect(1, 3), "Power", TO_POWER},
};

void renderHome(const RideState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    char sub[64];
    for (int i = 0; i < 8; ++i) {
        const Tile& t = kTiles[i];
        // SOS is always the one black tile: found without reading. Sending, it
        // gets a white inner ring as well.
        const bool sos = t.go == TO_SOS, dark = sos;
        if (dark) ui::fillRound(t.r, 8, INK, fb); else ui::frameRound(t.r, 8, 2, fb);
        if (sos && sosActive()) {
            const EpdRect in = {t.r.x + 8, t.r.y + 8, t.r.width - 16, t.r.height - 16};
            ui::frameRound(in, 5, 4, fb, PAPER, INK);
        }
        const uint8_t fg = dark ? PAPER : INK;
        const char* title = sos && sosActive() ? "SOS on" : t.title;
        ui::text(ui::font(ui::F_TITLE), t.r.x + 18, t.r.y + 46, title, fb, EPD_DRAW_ALIGN_LEFT, fg);
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
        const bool mono = t.go == TO_POINTS && sub[0] >= '0' && sub[0] <= '9';   // a grid reference
        const EpdFont* sf = mono ? ui::font(ui::F_MONO) : ui::font(ui::F_SMALL);
        if (ui::textWidth(sf, sub) > t.r.width - 36) sf = ui::font(ui::F_TINY);
        ui::text(sf, t.r.x + 18, t.r.y + t.r.height - 20, sub, fb, EPD_DRAW_ALIGN_LEFT, dark ? PAPER : 0x30);
        // Unread badge on COMMS.
        if (t.go == TO_COMMS && mesh_service::enabled() && mesh_service::unreadCount() > 0) {
            epd_fill_circle(t.r.x + t.r.width - 30, t.r.y + 34, 16, INK, fb);
            char nb[8];
            snprintf(nb, sizeof nb, "%d", mesh_service::unreadCount() > 99 ? 99 : mesh_service::unreadCount());
            ui::text(ui::font(ui::F_SMALL), t.r.x + t.r.width - 30, t.r.y + 41, nb, fb,
                     EPD_DRAW_ALIGN_CENTER, PAPER);
        }
    }
    // the contour lines inside the light tiles only: one map, seen through them
    for (const Tile& t : kTiles)
        if (t.go != TO_SOS) topo_bg::draw({t.r.x + 3, t.r.y + 3, t.r.width - 6, t.r.height - 6}, fb, 0);
        else if (!sosActive()) topo_bg::drawInverted({t.r.x + 6, t.r.y + 6, t.r.width - 12, t.r.height - 12}, fb, 0);
    renderStealthBar(fb);
}

// The stealth bar: one tap hides the device; going loud again takes a second
// tap within a few seconds, so a brush of the screen cannot give you away.
void renderStealthBar(uint8_t* fb) {
    const EpdRect r = kStealthBar;
    const bool on = stealth::active();
    const bool asking = on && g_stealthOffAsk && millis() - g_stealthOffAsk < 5000;
    if (on) ui::fillRound(r, 8, INK, fb); else ui::frameRound(r, 8, 2, fb);
    const uint8_t fg = on ? PAPER : INK;
    ui::text(ui::font(ui::F_KEY), r.x + 20, r.y + 34, on ? "Stealth on" : "Stealth", fb, EPD_DRAW_ALIGN_LEFT, fg);
    const char* sub = asking ? "Hold for 5 seconds to go loud" :
                      on ? (stealth::listen() ? "Silent, still listening \xC2\xB7 hold 5 s to end" : "Radio and light off \xC2\xB7 hold 5 s to end")
                         : "Radio and light off, screen dim";
    ui::text(ui::font(ui::F_SMALL), r.x + 20, r.y + r.height - 14, sub, fb, EPD_DRAW_ALIGN_LEFT, on ? PAPER : ui::GREY);
    // the switch, its knob where the state is
    // a switch: off = outlined track, dark knob left; on = filled track, white knob right
    const EpdRect sw = {r.x + r.width - 88, r.y + r.height / 2 - 18, 64, 36};
    if (on) {
        ui::frameRound(sw, 18, 2, fb, PAPER, INK);   // white rim on the black bar
        epd_fill_circle(sw.x + sw.width - 18, sw.y + 18, 12, PAPER, fb);
    } else {
        ui::frameRound(sw, 18, 2, fb, INK, PAPER);
        epd_fill_circle(sw.x + 18, sw.y + 18, 12, INK, fb);
    }
}

Go tapHome(int x, int y) {
    if (hit(kStealthBar, x, y)) {
        // On with a tap; off only with a finger held 5 s, so a brush of the
        // screen (or a pocket) cannot give you away.
        if (!stealth::active()) { stealth::set(true); g_stealthOffAsk = 0; }
        else if (g_pressMs >= kHoldMs) { stealth::set(false); g_stealthOffAsk = 0; }
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
    char cap[48];
    ui::text(ui::font(ui::F_LABEL), 24, y + h / 2 + 10, ui::sentence(caption, cap, sizeof cap), fb, EPD_DRAW_ALIGN_LEFT, INK);
    char v[48];
    snprintf(v, sizeof v, hint ? "%s  >" : "%s", value);
    // The value takes what the caption leaves; a long one steps down a size.
    const int room = W - 36 - ui::textWidth(ui::font(ui::F_LABEL), caption) - 24;
    const EpdFont* f = ui::font(ui::F_KEY);
    if (ui::textWidth(f, v) > room) f = ui::font(ui::F_LABEL);
    if (ui::textWidth(f, v) > room) f = ui::font(ui::F_SMALL);
    ui::text(f, W - 24, y + h / 2 + 12, v, fb, EPD_DRAW_ALIGN_RIGHT, INK);
    epd_fill_rect({24, y + h - 1, W - 48, 1}, ui::HAIR, fb);
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

constexpr int CTRL_W = 250, CTRL_H = 52, CTRL_X = W - 24 - CTRL_W;

void section(int y, const char* name, uint8_t* fb) {
    ui::text(ui::font(ui::F_TINY), 24, y + 26, name, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
}

void caption(int y, int h, const char* name, const char* hint, uint8_t* fb) {
    char n[48];
    ui::text(ui::font(ui::F_LABEL), 24, y + (hint ? h / 2 - 2 : h / 2 + 10), ui::sentence(name, n, sizeof n), fb, EPD_DRAW_ALIGN_LEFT, INK);
    if (hint) ui::text(ui::font(ui::F_TINY), 24, y + h / 2 + 24, hint, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
}

// n labels side by side, the chosen one filled.
void segments(int y, int h, const char* const* labels, int n, int sel, uint8_t* fb, int x0 = CTRL_X, int w = CTRL_W) {
    const int top = y + (h - CTRL_H) / 2;
    ui::frameRound({x0, top, w, CTRL_H}, 8, 2, fb);
    for (int i = 0; i < n; ++i) {
        const int sx = x0 + w * i / n, sw = w * (i + 1) / n - w * i / n;
        if (i == sel) ui::fillRound({sx + 4, top + 4, sw - 8, CTRL_H - 8}, 6, INK, fb);
        else if (i && i != sel + 1) epd_fill_rect({sx, top + 12, 1, CTRL_H - 24}, ui::HAIR, fb);
        // One size for the whole control: the smaller one if any label needs it.
        const EpdFont* lf = ui::font(ui::F_SMALL);
        for (int j = 0; j < n; ++j)
            if (ui::textWidth(ui::font(ui::F_SMALL), labels[j]) > w / n - 12) lf = ui::font(ui::F_TINY);
        char lb[24];
        ui::text(lf, sx + sw / 2, top + CTRL_H / 2 + (lf == ui::font(ui::F_TINY) ? 6 : 9), ui::sentence(labels[i], lb, sizeof lb), fb, EPD_DRAW_ALIGN_CENTER,
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
    ui::frameRound({CTRL_X, top, CTRL_W, CTRL_H}, 8, 2, fb);
    epd_fill_rect({CTRL_X + 52, top + 12, 1, CTRL_H - 24}, ui::HAIR, fb);
    epd_fill_rect({CTRL_X + CTRL_W - 53, top + 12, 1, CTRL_H - 24}, ui::HAIR, fb);
    const int cy = top + CTRL_H / 2;
    for (int t = -1; t <= 1; ++t) {
        const int lx = CTRL_X + 26, rx = CTRL_X + CTRL_W - 26;
        epd_draw_line(lx + 4 + t, cy - 9, lx - 4 + t, cy, INK, fb);
        epd_draw_line(lx - 4 + t, cy, lx + 4 + t, cy + 9, INK, fb);
        epd_draw_line(rx - 4 + t, cy - 9, rx + 4 + t, cy, INK, fb);
        epd_draw_line(rx + 4 + t, cy, rx - 4 + t, cy + 9, INK, fb);
    }
    char v[32];
    ui::text(ui::font(ui::F_SMALL), CTRL_X + CTRL_W / 2, top + CTRL_H / 2 + 8, ui::sentence(value, v, sizeof v), fb, EPD_DRAW_ALIGN_CENTER, INK);
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
                caption(y, SROW_H, "ANGLES", milspec::milsSystem() == 6400 ? "NATO mils" : "6000 mils (UA)", fb);
                segments(y, SROW_H, kM, 2, milspec::milsSystem() == 6400 ? 1 : 0, fb);
                break;
            }
            case R_MORE: {
                caption(y, SROW_H, "MORE", "USB, GPS, radio", fb);
                const int top = y + (SROW_H - CTRL_H) / 2;
                ui::frameRound({CTRL_X, top, CTRL_W, CTRL_H}, 8, 2, fb);
                ui::text(ui::font(ui::F_SMALL), CTRL_X + CTRL_W / 2, top + CTRL_H / 2 + 8, "Open  \xE2\x80\xBA", fb,
                         EPD_DRAW_ALIGN_CENTER, INK);
                break;
            }
        }
        epd_fill_rect({24, y + SROW_H - 1, W - 48, 1}, ui::HAIR, fb);
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

// The design canvas's comms: a big "Comms", the radio in a line, the tabs as
// words, the chats as rows.
constexpr int TAB_Y = TOP + 96, TAB_H = 46, LIST_Y = TOP + 150, ROW_H = 96;
constexpr int BAR_Y = H - 80, BAR_H = 56;
const EpdRect kRadioOn = {24, TOP + 260, W - 48, 64};
const EpdRect kPrev = {W - 24 - 56 - 12 - 56, H - 78, 56, 52};
const EpdRect kNext = {W - 24 - 56, H - 78, 56, 52};
// a chat's bar: older ^, Quick, Write (the one black), newer v
const EpdRect kOlder = {24, BAR_Y, 56, BAR_H};
const EpdRect kQuick = {92, BAR_Y, 170, BAR_H};
const EpdRect kWrite = {274, BAR_Y, 174, BAR_H};
const EpdRect kNewer = {W - 24 - 56, BAR_Y, 56, BAR_H};
const EpdRect kOnMap = {W - 24 - 124, TOP + 14, 124, 48};
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
    if (ch == 0) { snprintf(out, n, "Everyone"); return; }
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

void chevButton(const EpdRect& r, bool live, bool vertical, int d, uint8_t* fb);
void commsHead(uint8_t* fb) {
    ui::text(ui::font(ui::F_H1), 24, TOP + 44, "Comms", fb, EPD_DRAW_ALIGN_LEFT, INK);
    char sub[96];
    snprintf(sub, sizeof sub, "Mesh radio \xC2\xB7 %s \xC2\xB7 %d heard", mesh_service::presetName(), mesh_service::nodeCount());
    ui::text(ui::font(ui::F_SMALL), 24, TOP + 78, sub, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
}
void tabs(uint8_t* fb) {
    commsHead(fb);
    char nodes[24];
    snprintf(nodes, sizeof nodes, "Nodes %d", mesh_service::nodeCount());
    const char* const l[] = {"Chats", nodes};
    int x = 28;
    for (int t = 0; t < 2; ++t) {
        const bool on = (g_cv == CV_NODES) == (t == 1);
        const int w = ui::textWidth(ui::font(ui::F_LABEL), l[t]);
        ui::text(ui::font(ui::F_LABEL), x, TAB_Y + 26, l[t], fb, EPD_DRAW_ALIGN_LEFT, on ? INK : ui::GREY);
        if (on) epd_fill_rect({x, TAB_Y + 36, w, 3}, INK, fb);
        x += w + 32;
    }
}

void pager(int page, int pages, uint8_t* fb) {
    if (pages <= 1) return;
    char p[16];
    snprintf(p, sizeof p, "%d of %d", page + 1, pages);
    ui::text(ui::font(ui::F_SMALL), 28, kPrev.y + 34, p, fb, EPD_DRAW_ALIGN_LEFT, 0x40);
    chevButton(kPrev, page > 0, false, -1, fb);
    chevButton(kNext, page + 1 < pages, false, 1, fb);
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
        const int tx = 24, right = W - 24;
        ui::text(small, right, y + 38, when, fb, EPD_DRAW_ALIGN_RIGHT, ui::GREY);
        fit(big, t, right - tx - ui::textWidth(small, when) - 16);
        ui::text(big, tx, y + 38, t, fb, EPD_DRAW_ALIGN_LEFT, INK);
        int pw = right - tx;
        if (c.unread) {
            char u[8];
            snprintf(u, sizeof u, "%d", c.unread > 99 ? 99 : c.unread);
            const int bw = std::max(32, ui::textWidth(small, u) + 20);
            ui::fillRound({right - bw, y + 52, bw, 32}, 16, INK, fb);
            ui::text(small, right - bw / 2, y + 75, u, fb, EPD_DRAW_ALIGN_CENTER, PAPER);
            pw -= bw + 10;
        }
        fit(small, pv, pw);
        ui::text(small, tx, y + 76, pv, fb, EPD_DRAW_ALIGN_LEFT, c.unread ? 0x20 : ui::GREY);
        epd_fill_rect({24, y + ROW_H - 1, W - 48, 1}, ui::HAIR, fb);
    }
    pager(g_listPage, pages, fb);
    if (pages <= 1)
        ui::text(ui::font(ui::F_SMALL), 24, BAR_Y + 34, "Tap a chat to read and write", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
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
        ui::fillRound({24, y + 18, 68, 56}, 8, INK, fb);
        ui::text(small, 58, y + 54, sh, fb, EPD_DRAW_ALIGN_CENTER, PAPER);
        char nm[48], when[12], where[40], meta[96];
        snprintf(nm, sizeof nm, "%s", nd.longName[0] ? nd.longName : sh);
        ago(nd.lastHeardMs, when, sizeof when);
        whereIs(nd, where, sizeof where);
        const int tx = 110, right = W - 24;
        ui::text(small, right, y + 40, when, fb, EPD_DRAW_ALIGN_RIGHT, ui::GREY);
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
    bool onMap = false;
    if (g_chatDm) {
        for (int i = 0; i < mesh_service::nodeCount(); ++i) {
            mesh_service::Node nd;
            if (mesh_service::nodeAt(i, nd) && nd.num == g_chatDm) {
                onMap = nd.hasPosition;
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
    // A person with a known position: ON MAP marks them and points the way.
    if (onMap) button(kOnMap, "On map", false, fb);
    fit(ui::font(ui::F_TITLE), t, onMap ? kOnMap.x - 40 : W - 48);
    fit(small, sub, onMap ? kOnMap.x - 40 : W - 48);
    ui::text(ui::font(ui::F_TITLE), 24, TOP + 38, t, fb, EPD_DRAW_ALIGN_LEFT, INK);
    if (sub[0] >= 'a' && sub[0] <= 'z') sub[0] -= 32;
    ui::text(small, 24, TOP + 68, sub, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    epd_fill_rect({24, TOP + 84, W - 48, 1}, ui::HAIR, fb);

    // Bubbles, newest at the bottom: theirs on the left in a frame with the
    // sender's name, yours on the right, filled, with the delivery state.
    const int areaTop = TOP + 94, areaBottom = BAR_Y - 14;
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
        const int x = m.outgoing ? W - 24 - bw : 24;
        const uint8_t fg = m.outgoing ? PAPER : INK;
        if (m.outgoing) ui::fillRound({x, y, bw, bh}, 12, INK, fb); else ui::fillRound({x, y, bw, bh}, 12, 0xE6, fb);
        ui::text(tiny, x + 14, y + 22, meta, fb, EPD_DRAW_ALIGN_LEFT, m.outgoing ? 0xC0 : ui::GREY);
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
        ui::text(ui::font(ui::F_LABEL), W / 2, 420, "No messages yet", fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::text(small, W / 2, 456, "Quick sends a ready phrase, Write types one", fb, EPD_DRAW_ALIGN_CENTER, ui::GREY);
    }
    seenMs(g_chatCh, g_chatDm) = millis();
    mesh_service::markAllRead();
    chevButton(kOlder, true, true, -1, fb);
    button(kQuick, "Quick", false, fb);
    button(kWrite, "Write", true, fb);
    chevButton(kNewer, g_chatScroll > 0, true, 1, fb);
    (void)shown;
}

// Ready phrases: one tap, no keyboard, with cold hands.
const char* const kQuickText[] = {
    "OK", "Copy", "On my way", "Wait for me", "Need help", "All good", "Where are you?",
    "Stay there", "Coming back", "Low battery",
};
constexpr int N_QUICK = sizeof(kQuickText) / sizeof(kQuickText[0]);
constexpr int Q_TOP = TOP + 124, Q_H = 68, Q_GAP = 12;
EpdRect quickRect(int i) {
    const int cw = (W - 48 - Q_GAP) / 2;
    return {24 + (i % 2) * (cw + Q_GAP), Q_TOP + (i / 2) * (Q_H + Q_GAP), cw, Q_H};
}
const EpdRect kSendPos = {24, Q_TOP + (N_QUICK / 2) * (Q_H + Q_GAP) + 10, W - 48, Q_H};

void renderQuick(uint8_t* fb) {
    char t[48], h[80];
    chatTitle(g_chatCh, g_chatDm, t, sizeof t);
    ui::text(ui::font(ui::F_SMALL), 24, TOP + 36, "Send a ready phrase", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    snprintf(h, sizeof h, "To %s", t);
    fit(ui::font(ui::F_H1), h, W - 48);
    ui::text(ui::font(ui::F_H1), 24, TOP + 84, h, fb, EPD_DRAW_ALIGN_LEFT, INK);
    // the phrases as soft grey keys: one tap sends
    for (int i = 0; i < N_QUICK; ++i) {
        const EpdRect r = quickRect(i);
        ui::fillRound(r, 10, 0xE6, fb);
        ui::text(ui::font(ui::F_LABEL), r.x + r.width / 2, r.y + r.height / 2 + 9, kQuickText[i], fb, EPD_DRAW_ALIGN_CENTER, INK);
    }
    button(kSendPos, g_myPos ? "Send my position (MGRS)" : "My position: no fix yet", true, fb);
    ui::text(ui::font(ui::F_SMALL), 24, H - 34, "One tap sends it. Back returns to the chat.", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
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
        ui::text(ui::font(ui::F_H1), 24, TOP + 44, "Comms", fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (stealth::active()) {
            ui::text(small, 24, TOP + 78, "Stealth: nothing in, nothing out", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
            ui::text(small, 24, TOP + 150, stealth::listen() ? "To listen, turn the radio on in Settings."
                                                             : "Stealth: listen (in Settings) receives.", fb, EPD_DRAW_ALIGN_LEFT, 0x30);
            ui::text(small, 24, TOP + 182, "End stealth on Home: hold the switch 5 s.", fb, EPD_DRAW_ALIGN_LEFT, 0x30);
            return;
        }
        ui::text(small, 24, TOP + 78, "The radio is off", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        ui::text(small, 24, TOP + 150, "Messages and nodes need the radio.", fb, EPD_DRAW_ALIGN_LEFT, 0x30);
        ui::text(small, 24, TOP + 182, "It uses more battery while on.", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        button(kRadioOn, "Turn the radio on", true, fb);
        (void)tiny;
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
        ui::fillRound({24, BAR_Y - 50, W - 48, 40}, 8, INK, fb);
        ui::text(small, W / 2, BAR_Y - 23, "Stealth: listening, nothing is sent", fb, EPD_DRAW_ALIGN_CENTER, PAPER);
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
                const CommsView v = x < 130 ? CV_CHATS : CV_NODES;
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
            if (g_chatDm && hit(kOnMap, x, y)) {
                // the node as a waypoint (one per node, moved to where it is now), targeted
                for (int i = 0; i < mesh_service::nodeCount(); ++i) {
                    mesh_service::Node nd;
                    if (!mesh_service::nodeAt(i, nd) || nd.num != g_chatDm || !nd.hasPosition) continue;
                    char nm[doba_points::NAME_LEN];
                    snprintf(nm, sizeof nm, "@%.20s", nd.shortName[0] ? nd.shortName : nd.longName);
                    for (int k = doba_points::count() - 1; k >= 0; --k)
                        if (!strcmp(doba_points::at(k)->name, nm)) doba_points::remove(k);
                    const int idx = doba_points::add(nm, nd.latitude, nd.longitude);
                    if (idx >= 0) {
                        doba_points::setTarget(idx);
                        Serial.printf("[comms] %s on the map\n", nm);
                        return TO_MAP;
                    }
                }
                return STAY;
            }
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

// --- MAPOPTS: what the map shows, and its legend --------------------------------------

constexpr int MROW_TOP = TOP + 20, MROW_H = 90, MROWS = 4;
const EpdRect kOpenLegend = {24, TOP + 20 + 4 * 90 + 30, W - 48, 64};
const EpdRect kOpenPoints = {24, TOP + 20 + 4 * 90 + 30 + 64 + 16, W - 48, 64};
bool g_legend = false;

// A row that opens a page: the words left, a chevron right, in a hairline box.
void linkRow(const EpdRect& r, const char* label, const char* hint, uint8_t* fb) {
    ui::frameRound(r, 10, 2, fb);
    ui::text(ui::font(ui::F_LABEL), r.x + 20, r.y + (hint ? 28 : r.height / 2 + 9), label, fb, EPD_DRAW_ALIGN_LEFT, INK);
    if (hint) ui::text(ui::font(ui::F_TINY), r.x + 20, r.y + 52, hint, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    const int cx = r.x + r.width - 26, cy = r.y + r.height / 2;
    for (int t = -1; t <= 1; ++t) {
        epd_draw_line(cx - 4 + t, cy - 9, cx + 4 + t, cy, INK, fb);
        epd_draw_line(cx + 4 + t, cy, cx - 4 + t, cy + 9, INK, fb);
    }
}

// One line of the legend: a sample drawn at x 24..96, the words after it.
void legendLine(int y, const char* what, const char* hint, uint8_t* fb) {
    ui::text(ui::font(ui::F_SMALL), 116, y + (hint ? 4 : 14), what, fb, EPD_DRAW_ALIGN_LEFT, INK);
    if (hint) ui::text(ui::font(ui::F_TINY), 116, y + 28, hint, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
}

void poiTag(int cx, int cy, int kind, uint8_t* fb) {
    const EpdFont* f = ui::monoFont(13);
    const char* c = milspec::poiCode(kind);
    const int w = ui::textWidth(f, c) + 12;
    ui::fillRound({cx - w / 2, cy - 12, w, 24}, 6, INK, fb);
    ui::text(f, cx, cy + 6, c, fb, EPD_DRAW_ALIGN_CENTER, PAPER);
}

void renderLegend(const RideState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    double lat = 50, lon = 30;
    float mpp = 8;
    milspec::lastView(lat, lon, mpp);
    if (s.gpsFix || s.everHadFix) { lat = s.latitude; lon = s.longitude; }
    float ev, ix, fev, fix;
    milspec::contourInterval(lat, mpp, false, ev, ix);
    milspec::contourInterval(lat, mpp, true, fev, fix);
    char t1[64], t2[64];
    int y = TOP + 6;

    section(y, "RELIEF AT THIS ZOOM", fb);
    y += 52;
    epd_fill_rect({24, y, 72, 1}, 0x44, fb);
    snprintf(t1, sizeof t1, "Contour every %g m", ev);
    snprintf(t2, sizeof t2, "flat country: every %g m", fev);
    legendLine(y - 6, t1, t2, fb);
    y += 56;
    epd_fill_rect({24, y - 1, 72, 3}, 0x44, fb);
    ui::text(ui::font(ui::F_TINY), 60, y - 6, "120", fb, EPD_DRAW_ALIGN_CENTER, INK);
    snprintf(t1, sizeof t1, "Bold every %g m, height on it", ix);
    snprintf(t2, sizeof t2, "flat country: every %g m \xC2\xB7 zoom in for finer", fix);
    legendLine(y - 6, t1, t2, fb);

    y += 48;
    section(y, "GRID AND COMPASS", fb);
    y += 52;
    epd_fill_rect({24, y, 72, 2}, 0x22, fb);
    epd_fill_rect({58, y - 18, 2, 38}, 0x22, fb);
    char sq[4];
    milspec::square(lat, lon, sq, sizeof sq);
    snprintf(t2, sizeof t2, "square %s: the 100 km letters, said before the numbers", sq[0] ? sq : "LC");
    legendLine(y - 6, "MGRS lines, 1 km apart", t2, fb);
    y += 56;
    {
        const float d = milgeo::declination(lat, lon);
        snprintf(t1, sizeof t1, "Magnetic north %.1f\xC2\xB0 %s of true", fabsf(d), d >= 0 ? "east" : "west");
        // a needle: N up, turned by the declination
        const float a = d * (float)M_PI / 180.0f;
        const int cx = 60, cy = y + 4;
        for (int k = -1; k <= 1; ++k)
            epd_draw_line(cx + k, cy + 18, cx + (int)(sinf(a) * 22) + k, cy - (int)(cosf(a) * 22), INK, fb);
        ui::text(ui::font(ui::F_TINY), cx + (int)(sinf(a) * 22) + 10, cy - 12, "N", fb, EPD_DRAW_ALIGN_LEFT, INK);
        legendLine(y - 6, t1, "compass az on the map already allows for it", fb);
    }

    y += 48;
    section(y, "MARKS", fb);
    y += 50;
    {   // saved point · going to · the cross · measure from A
        int x = 40;
        for (int k = 0; k < 3; ++k) {
            epd_draw_line(x, y - 12 + k, x - 10 + k, y + 7 - k, INK, fb);
            epd_draw_line(x - 10 + k, y + 7 - k, x + 10 - k, y + 7 - k, INK, fb);
            epd_draw_line(x + 10 - k, y + 7 - k, x, y - 12 + k, INK, fb);
        }
        ui::text(ui::font(ui::F_SMALL), 64, y + 8, "Saved point", fb, EPD_DRAW_ALIGN_LEFT, INK);
        x = 290;
        for (int r = 0; r < 3; ++r) epd_draw_circle(x, y, 14 - r, INK, fb);
        epd_fill_rect({x - 20, y - 1, 40, 3}, INK, fb);
        epd_fill_rect({x - 1, y - 20, 3, 40}, INK, fb);
        ui::text(ui::font(ui::F_SMALL), 318, y + 8, "Going to", fb, EPD_DRAW_ALIGN_LEFT, INK);
        y += 54;
        x = 40;
        epd_fill_rect({x - 16, y - 2, 11, 5}, INK, fb);
        epd_fill_rect({x + 6, y - 2, 11, 5}, INK, fb);
        epd_fill_rect({x - 2, y - 16, 5, 11}, INK, fb);
        epd_fill_rect({x - 2, y + 6, 5, 11}, INK, fb);
        ui::text(ui::font(ui::F_SMALL), 64, y - 2, "Cross", fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(ui::font(ui::F_TINY), 64, y + 20, "drag the map: read here", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        x = 290;
        epd_fill_circle(x, y, 13, INK, fb);
        ui::text(ui::font(ui::F_STRONG), x, y + 6, "A", fb, EPD_DRAW_ALIGN_CENTER, PAPER);
        ui::text(ui::font(ui::F_SMALL), 318, y - 2, "Measure from A", fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(ui::font(ui::F_TINY), 318, y + 20, "tap the cross to set", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    }

    y += 40;
    section(y, "PLACES", fb);
    y += 48;
    const int per = (doba_pois::KINDS - 1 + 1) / 2;
    for (int k = 1; k < doba_pois::KINDS; ++k) {
        const int col = (k - 1) / per, row = (k - 1) % per;
        const int x = 24 + col * 258, yy = y + row * 36;
        poiTag(x + 20, yy, k, fb);
        ui::text(ui::font(ui::F_SMALL), x + 48, yy + 7, doba_pois::kindName(k), fb, EPD_DRAW_ALIGN_LEFT, INK);
    }
}

void renderMapOpts(const RideState& s, uint8_t* fb) {
    if (g_legend) { renderLegend(s, fb); return; }
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    static const char* const kO[] = {"NORTH", "COURSE"};
    int y = MROW_TOP;
    caption(y, MROW_H, "UP IS", mapTrackUp() ? "the map turns with you" : "north always up", fb);
    segments(y, MROW_H, kO, 2, mapTrackUp() ? 1 : 0, fb);
    epd_fill_rect({24, y + MROW_H - 1, W - 48, 1}, ui::HAIR, fb);
    y += MROW_H;
    caption(y, MROW_H, "MGRS GRID", "1 km lines, 10 km zoomed out", fb);
    toggle(y, MROW_H, milspec::gridOn(), fb);
    epd_fill_rect({24, y + MROW_H - 1, W - 48, 1}, ui::HAIR, fb);
    y += MROW_H;
    caption(y, MROW_H, "POINTS", "your saved points", fb);
    toggle(y, MROW_H, milspec::pointsOn(), fb);
    epd_fill_rect({24, y + MROW_H - 1, W - 48, 1}, ui::HAIR, fb);
    y += MROW_H;
    caption(y, MROW_H, "PLACES", doba_pois::available() ? "hospitals, water, food... zoomed in" : "no places file on the card", fb);
    toggle(y, MROW_H, milspec::poisOn(), fb);
    linkRow(kOpenLegend, "Legend", "contours, grid, marks, place letters", fb);
    linkRow(kOpenPoints, "Saved points", nullptr, fb);
    const char* tips[] = {"Hold a finger on the map 1 s to drop a point.",
                          "Drag the map: the strip reads the cross.",
                          "Tap the cross to measure from there.",
                          "Tap the compass: north-up or course-up."};
    for (int i = 0; i < 4; ++i)
        ui::text(ui::font(ui::F_SMALL), 24, kOpenPoints.y + kOpenPoints.height + 56 + i * 36,
                 tips[i], fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
}

Go tapMapOpts(int x, int y) {
    if (g_legend) return STAY;
    if (hit(kOpenLegend, x, y)) { g_legend = true; return STAY; }
    if (hit(kOpenPoints, x, y)) return TO_POINTS;
    const int r = (y - MROW_TOP) / MROW_H;
    if (y < MROW_TOP || r >= MROWS) return STAY;
    if (r == 0) { const int i = segmentHit(x, 2); if (i >= 0) setMapTrackUp(i == 1); }
    if (r == 1 && toggleHit(x)) milspec::setGridOn(x >= CTRL_X + CTRL_W - 75);
    if (r == 2 && toggleHit(x)) milspec::setPointsOn(x >= CTRL_X + CTRL_W - 75);
    if (r == 3 && toggleHit(x)) milspec::setPoisOn(x >= CTRL_X + CTRL_W - 75);
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
constexpr int PROW_TOP = TOP + 436, PROW_H = 80, PROW_N = 4;
const EpdRect kMark = {24, TOP + 306, (W - 48 - 14) / 2, 64};
const EpdRect kAdd = {24 + (W - 48 - 14) / 2 + 14, TOP + 306, (W - 48 - 14) / 2, 64};
const EpdRect kUp = {W - 24 - 56 - 12 - 56, H - 78, 56, 52};
const EpdRect kDown = {W - 24 - 56, H - 78, 56, 52};

// A small square button with a chevron: d -1 left/up, +1 right/down.
void chevButton(const EpdRect& r, bool live, bool vertical, int d, uint8_t* fb) {
    const uint8_t c = live ? INK : 0xA0;
    ui::frameRound(r, 8, 2, fb, c, PAPER);
    const int cx = r.x + r.width / 2, cy = r.y + r.height / 2;
    for (int t = -1; t <= 1; ++t) {
        if (vertical) {
            epd_draw_line(cx - 10, cy - d * 4 + t, cx, cy + d * 5 + t, c, fb);
            epd_draw_line(cx, cy + d * 5 + t, cx + 10, cy - d * 4 + t, c, fb);
        } else {
            epd_draw_line(cx - d * 4 + t, cy - 10, cx + d * 5 + t, cy, c, fb);
            epd_draw_line(cx + d * 5 + t, cy, cx - d * 4 + t, cy + 10, c, fb);
        }
    }
}

// --- NEARBY: useful places round you (doba_pois) ---------------------------------
int g_nearby = 0;       // 0 off, 1 the kinds, 2 the places of one kind
int g_nearKind = 0, g_nearPage = 0;
constexpr int NEAR_TOP = TOP + 112, NEAR_ROW = 84;
constexpr int NEAR_MAX = 30;
doba_pois::Poi g_near[NEAR_MAX];
int g_nearCount = 0;
doba_pois::Poi g_each[doba_pois::KINDS];
int g_kindOrder[doba_pois::KINDS], g_kindCount = 0;   // the kinds found, nearest first

void compass8(double deg, char* out, size_t n) {
    static const char* const k[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    snprintf(out, n, "%s", k[(int)((fmod(deg + 360.0, 360.0) + 22.5) / 45.0) % 8]);
}
void nearLine(const doba_pois::Poi& p, char* out, size_t n) {
    char ds[16], az[16], c8[4];
    doba_points::formatDistance(p.dist, ds, sizeof ds);
    azText(p.bearing, az, sizeof az);
    compass8(p.bearing, c8, sizeof c8);
    snprintf(out, n, "%s %s \xC2\xB7 az %s", ds, c8, az);
}
int nearPerPage() { return (H - 90 - NEAR_TOP) / NEAR_ROW; }

void renderNearby(const RideState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    double lat, lon;
    if (!here(s, lat, lon)) {
        ui::text(ui::font(ui::F_H1), 24, TOP + 44, "Nearby", fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(ui::font(ui::F_SMALL), 24, TOP + 120, "Needs a position: wait for a GPS fix.", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        return;
    }
    if (!doba_pois::available()) {
        ui::text(ui::font(ui::F_H1), 24, TOP + 44, "Nearby", fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(ui::font(ui::F_SMALL), 24, TOP + 120, "No list of places on this card.", fb, EPD_DRAW_ALIGN_LEFT, 0x30);
        ui::text(ui::font(ui::F_SMALL), 24, TOP + 150, "It is /doba/map/pois.dgp (maps/pois.py).", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        return;
    }
    const int per = nearPerPage();
    char line[96];
    if (g_nearby == 1) {
        ui::text(ui::font(ui::F_H1), 24, TOP + 44, "Nearby", fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(ui::font(ui::F_SMALL), 24, TOP + 78, "Useful places within about 40 km, offline", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        doba_pois::nearestOfEach(lat, lon, g_each);
        g_kindCount = 0;
        // the fixed order, most urgent first; underground parking with the shelters
        static const uint8_t kOrder[] = {1, 2, 3, 4, 5, 6, 7, 8, 17, 9, 10, 11, 12, 13, 14, 15, 16};
        static_assert(sizeof kOrder == doba_pois::KINDS - 1, "every kind once");
        for (uint8_t k : kOrder) g_kindOrder[g_kindCount++] = k;
        const int pages = (g_kindCount + per - 1) / per;
        if (g_nearPage >= pages) g_nearPage = pages - 1;
        for (int r = 0; r < per && g_nearPage * per + r < g_kindCount; ++r) {
            const int k = g_kindOrder[g_nearPage * per + r];
            const int y = NEAR_TOP + r * NEAR_ROW;
            const doba_pois::Poi& p = g_each[k];
            const bool none = p.dist < 0;
            ui::text(ui::font(ui::F_LABEL), 24, y + 36, doba_pois::kindName(k), fb, EPD_DRAW_ALIGN_LEFT, none ? ui::GREY : INK);
            if (none) snprintf(line, sizeof line, "none within 40 km");
            else {
                char d[48];
                nearLine(p, d, sizeof d);
                snprintf(line, sizeof line, "%s%s%s", d, p.name[0] ? " \xC2\xB7 " : "", p.name);
            }
            fit(ui::font(ui::F_SMALL), line, W - 90);
            ui::text(ui::font(ui::F_SMALL), 24, y + 66, line, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
            if (!none) ui::text(ui::font(ui::F_KEY), W - 26, y + NEAR_ROW / 2 + 12, "\xE2\x80\xBA", fb, EPD_DRAW_ALIGN_RIGHT, 0x80);
            epd_fill_rect({24, y + NEAR_ROW - 1, W - 48, 1}, ui::HAIR, fb);
        }
        if (pages > 1) {
            char pg[16];
            snprintf(pg, sizeof pg, "%d of %d", g_nearPage + 1, pages);
            ui::text(ui::font(ui::F_SMALL), 28, kUp.y + 34, pg, fb, EPD_DRAW_ALIGN_LEFT, 0x40);
            chevButton(kUp, g_nearPage > 0, false, -1, fb);
            chevButton(kDown, g_nearPage + 1 < pages, false, 1, fb);
        }
        return;
    }
    // one kind: the nearest, a tap makes it a point to go to
    ui::text(ui::font(ui::F_H1), 24, TOP + 44, doba_pois::kindName(g_nearKind), fb, EPD_DRAW_ALIGN_LEFT, INK);
    ui::text(ui::font(ui::F_SMALL), 24, TOP + 78, doba_pois::kindHint(g_nearKind), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    g_nearCount = doba_pois::nearest(lat, lon, g_nearKind, g_near, NEAR_MAX);
    const int pages = (g_nearCount + per - 1) / per;
    if (g_nearPage >= pages) g_nearPage = pages ? pages - 1 : 0;
    if (!g_nearCount)
        ui::text(ui::font(ui::F_SMALL), 24, NEAR_TOP + 40, "None within about 40 km.", fb, EPD_DRAW_ALIGN_LEFT, 0x30);
    for (int r = 0; r < per && g_nearPage * per + r < g_nearCount; ++r) {
        const doba_pois::Poi& p = g_near[g_nearPage * per + r];
        const int y = NEAR_TOP + r * NEAR_ROW;
        char nm[48];
        snprintf(nm, sizeof nm, "%s", p.name[0] ? p.name : doba_pois::kindName(p.kind));
        fit(ui::font(ui::F_LABEL), nm, W - 90);
        ui::text(ui::font(ui::F_LABEL), 24, y + 36, nm, fb, EPD_DRAW_ALIGN_LEFT, INK);
        nearLine(p, line, sizeof line);
        ui::text(ui::font(ui::F_SMALL), 24, y + 66, line, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        ui::text(ui::font(ui::F_KEY), W - 26, y + NEAR_ROW / 2 + 12, "\xE2\x80\xBA", fb, EPD_DRAW_ALIGN_RIGHT, 0x80);
        epd_fill_rect({24, y + NEAR_ROW - 1, W - 48, 1}, ui::HAIR, fb);
    }
    char pg[32];
    snprintf(pg, sizeof pg, pages > 1 ? "%d of %d \xC2\xB7 tap one to go there" : "Tap one to go there", g_nearPage + 1, pages);
    ui::text(ui::font(ui::F_SMALL), 28, kUp.y + 34, pg, fb, EPD_DRAW_ALIGN_LEFT, 0x40);
    if (pages > 1) {
        chevButton(kUp, g_nearPage > 0, false, -1, fb);
        chevButton(kDown, g_nearPage + 1 < pages, false, 1, fb);
    }
}

Go tapNearby(int x, int y, const RideState& s) {
    const int per = nearPerPage();
    if (hit(kUp, x, y)) { if (g_nearPage > 0) --g_nearPage; return STAY; }
    if (hit(kDown, x, y)) { ++g_nearPage; return STAY; }   // clamped when drawn
    if (y < NEAR_TOP) return STAY;
    const int r = (y - NEAR_TOP) / NEAR_ROW, i = g_nearPage * per + r;
    if (r >= per) return STAY;
    if (g_nearby == 1) {
        if (i >= g_kindCount) return STAY;
        const int k = g_kindOrder[i];
        if (g_each[k].dist < 0) return STAY;
        g_nearKind = k;
        g_nearPage = 0;
        g_nearby = 2;
        Serial.printf("[pois] kind %s\n", doba_pois::kindName(k));
        return STAY;
    }
    if (i >= g_nearCount) return STAY;
    const doba_pois::Poi& p = g_near[i];
    char nm[24];
    snprintf(nm, sizeof nm, "%.23s", p.name[0] ? p.name : doba_pois::kindName(p.kind));
    const int at = doba_points::add(nm, p.lat, p.lon);
    if (at < 0) { g_pointsNote = "list full"; return STAY; }
    Serial.printf("[pois] point %d '%s'\n", at, nm);
    g_nearby = 0;
    g_pointSel = at;
    g_deleteArmed = false;
    return TO_POINT;
}

void renderPoints(const RideState& s, uint8_t* fb) {
    if (g_nearby) { renderNearby(s, fb); return; }
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    double lat, lon;
    const bool have = here(s, lat, lon);
    ui::text(ui::font(ui::F_SMALL), 24, TOP + 34,
             s.gpsFix ? "Your position \xC2\xB7 MGRS" : have ? "Last position \xC2\xB7 no fix now" : "Your position",
             fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    if (g_pointsNote) {
        ui::text(ui::font(ui::F_SMALL), W - 24, TOP + 34, g_pointsNote, fb, EPD_DRAW_ALIGN_RIGHT, INK);
        g_pointsNote = nullptr;
    }
    char m[32] = "";
    if (have) milgeo::mgrs(lat, lon, m, sizeof m);
    // the grid reference as big as it reads: zone small, the two numbers large
    char zone[16] = "", e[12] = "", nn[12] = "";
    {
        char t[32];
        snprintf(t, sizeof t, "%s", m);
        char* tok[4] = {};
        int k = 0;
        for (char* p = strtok(t, " "); p && k < 4; p = strtok(nullptr, " ")) tok[k++] = p;
        if (k == 4) {
            snprintf(zone, sizeof zone, "%s %s", tok[0], tok[1]);
            snprintf(e, sizeof e, "%s", tok[2]);
            snprintf(nn, sizeof nn, "%s", tok[3]);
        }
    }
    if (zone[0]) {
        ui::text(ui::monoFont(22), 24, TOP + 84, zone, fb, EPD_DRAW_ALIGN_LEFT, 0x40);
        ui::text(ui::valueFont(46), 20, TOP + 152, e, fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(ui::valueFont(46), 20, TOP + 216, nn, fb, EPD_DRAW_ALIGN_LEFT, INK);
    } else {
        ui::text(ui::font(ui::F_H1), 24, TOP + 120, have ? m : "No fix yet", fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (!have) ui::text(ui::font(ui::F_SMALL), 24, TOP + 160, "Waiting for satellites", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    }
    char ll[64], info[64];
    float dep;
    if (have) {
        snprintf(ll, sizeof ll, "%.4f\xC2\xB0 %c, %.4f\xC2\xB0 %c \xC2\xB7 decl %+.1f\xC2\xB0", fabs(lat), lat >= 0 ? 'N' : 'S',
                 fabs(lon), lon >= 0 ? 'E' : 'W', milgeo::declination(lat, lon));
        if (doba_dem::depth(lat, lon, dep))   // on water: depth under you (EMODnet)
            snprintf(info, sizeof info, "%d satellites \xC2\xB7 %d m deep", s.satellites, (int)lroundf(dep));
        else
            snprintf(info, sizeof info, "%d satellites \xC2\xB7 %d m high", s.satellites, (int)lroundf(groundAlt(s)));
        ui::text(ui::font(ui::F_SMALL), 24, TOP + 252, ll, fb, EPD_DRAW_ALIGN_LEFT, 0x30);
        ui::text(ui::font(ui::F_SMALL), 24, TOP + 280, info, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    }
    button(kMark, "Mark here", true, fb);
    button(kAdd, "Enter coords", false, fb);

    const int n = doba_points::count();
    if (g_pointsScroll > n - 1) g_pointsScroll = n > 0 ? ((n - 1) / PROW_N) * PROW_N : 0;
    ui::text(ui::font(ui::F_TINY), 24, PROW_TOP - 14, "SAVED POINTS", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    // Nearby: hospitals, water, food... (maps/pois.py) — a link, top right of the list
    {
        const EpdFont* lf = ui::font(ui::F_LABEL);
        const char* nb = "Nearby places \xE2\x80\xBA";
        ui::text(lf, W - 24, PROW_TOP - 12, nb, fb, EPD_DRAW_ALIGN_RIGHT, INK);
    }
    if (n == 0) {
        ui::text(ui::font(ui::F_SMALL), 24, PROW_TOP + 34, "Mark here saves where you stand,", fb, EPD_DRAW_ALIGN_LEFT, 0x30);
        ui::text(ui::font(ui::F_SMALL), 24, PROW_TOP + 62, "Enter coords takes a grid reference,", fb, EPD_DRAW_ALIGN_LEFT, 0x30);
        ui::text(ui::font(ui::F_SMALL), 24, PROW_TOP + 90, "or hold a finger on the map.", fb, EPD_DRAW_ALIGN_LEFT, 0x30);
    }
    for (int k = 0; k < PROW_N && g_pointsScroll + k < n; ++k) {
        const int i = g_pointsScroll + k;
        const doba_points::Point* p = doba_points::at(i);
        const int y = PROW_TOP + k * PROW_H;
        const bool tgt = doba_points::target() == i;
        if (tgt) ui::fillRound({16, y + 4, W - 32, PROW_H - 8}, 8, INK, fb);
        const uint8_t fg = tgt ? PAPER : INK, dim = tgt ? PAPER : ui::GREY;
        char nm[40];
        snprintf(nm, sizeof nm, tgt ? "\xE2\x80\xBA %s" : "%s", p->name);
        ui::text(ui::font(ui::F_LABEL), 28, y + 36, nm, fb, EPD_DRAW_ALIGN_LEFT, fg);
        char pm[32];
        milgeo::mgrs(p->lat, p->lon, pm, sizeof pm);
        ui::text(ui::font(ui::F_TINY), 28, y + 64, pm, fb, EPD_DRAW_ALIGN_LEFT, dim);
        if (have) {
            double b, d;
            doba_points::fromHere(i, lat, lon, b, d);
            char az[16], ds[24];
            azText(b, az, sizeof az);
            doba_points::formatDistance(d, ds, sizeof ds);
            ui::text(ui::font(ui::F_LABEL), W - 28, y + 36, ds, fb, EPD_DRAW_ALIGN_RIGHT, fg);
            char azl[24];
            snprintf(azl, sizeof azl, "az %s", az);
            ui::text(ui::font(ui::F_SMALL), W - 28, y + 64, azl, fb, EPD_DRAW_ALIGN_RIGHT, dim);
        }
        if (!tgt) epd_fill_rect({24, y + PROW_H - 1, W - 48, 1}, ui::HAIR, fb);
    }
    if (n > PROW_N) {
        char pg[24];
        snprintf(pg, sizeof pg, "%d of %d", g_pointsScroll / PROW_N + 1, (n + PROW_N - 1) / PROW_N);
        ui::text(ui::font(ui::F_SMALL), 28, kUp.y + 34, pg, fb, EPD_DRAW_ALIGN_LEFT, 0x40);
        chevButton(kUp, g_pointsScroll > 0, false, -1, fb);
        chevButton(kDown, g_pointsScroll + PROW_N < n, false, 1, fb);
    }
}

kbd::Keyboard g_newGrid, g_newName;
bool g_newFocusName = false;
const char* g_newError = nullptr;

Go tapPoints(int x, int y, const RideState& s) {
    if (g_nearby) return tapNearby(x, y, s);
    if (y >= PROW_TOP - 46 && y < PROW_TOP + 4 && x > W / 2) { g_nearby = 1; g_nearPage = 0; return STAY; }
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
        {   // what to set on the compass here: true less the declination
            float mb = (float)b - milgeo::declination(lat, lon);
            if (mb < 0) mb += 360.0f;
            char cm[8], ct[32];
            milgeo::mils(mb, milspec::milsSystem(), cm, sizeof cm);
            snprintf(ct, sizeof ct, "compass %s", cm);
            ui::text(ui::font(ui::F_TINY), tx, dialY + 14, ct, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        }
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
    if (g_sosOn) snprintf(out, n, "Active \xC2\xB7 sent %d", g_sosSent);
    else snprintf(out, n, "Hold 5 s to send");
}

void renderSos(const RideState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    char msg[200];
    sosMessage(s, msg, sizeof msg);
    if (!g_sosOn) {
        ui::text(ui::font(ui::F_SMALL), W / 2, TOP + 60, "Broadcasts on the mesh, every 5 min:", fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::frameRound({24, TOP + 84, W - 48, 150}, 8, 2, fb, 0x90, PAPER);
        wrapAt(ui::font(ui::F_BODY), 36, TOP + 124, W - 72, 34, msg, fb);
        const bool armed = g_sosArmed && millis() - g_sosArmedAt < 6000;
        ui::fillRound(kSosBtn, 10, INK, fb);
        ui::text(ui::font(ui::F_BIG), W / 2, kSosBtn.y + 120, "SOS", fb, EPD_DRAW_ALIGN_CENTER, PAPER);
        ui::text(ui::font(ui::F_SMALL), W / 2, kSosBtn.y + 180, armed ? "Hold it for 5 seconds to send" : "Hold for 5 seconds to send", fb,
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
    ui::frameRound({24, TOP + 320, W - 48, 150}, 8, 2, fb, 0x90, PAPER);
    wrapAt(ui::font(ui::F_BODY), 36, TOP + 360, W - 72, 34, msg, fb);
    ui::text(ui::font(ui::F_SMALL), W / 2, TOP + 530, "Stay with the device. Replies show in COMMS.", fb,
             EPD_DRAW_ALIGN_CENTER, INK);
    button(kSosCancel, "CANCEL SOS", false, fb);
}

Go tapSos(int x, int y, const RideState& s) {
    if (!g_sosOn) {
        if (!hit(kSosBtn, x, y)) { g_sosArmed = false; return STAY; }
        // Sent only with a finger held 5 s: never by a brush or a pocket.
        if (g_pressMs >= kHoldMs) {
            g_sosArmed = false;
            g_sosOn = true;
            g_sosSent = 0;
            sosSend(s);
        } else {
            g_sosArmed = true;   // a short tap: say how
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
        ui::text(ui::font(ui::F_SMALL), 24, y + 6, "Daylight left", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        snprintf(line, sizeof line, "%ld h %02ld min", left / 60, left % 60);
        ui::text(ui::font(ui::F_BIG), 20, y + 80, line, fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (d.duskOk) {
            const long l2 = (long)(d.dusk - s.utc) / 60;
            snprintf(line, sizeof line, "+ %ld min of twilight after sunset", l2 - left);
            ui::text(ui::font(ui::F_SMALL), 20, y + 120, line, fb, EPD_DRAW_ALIGN_LEFT, INK);
        }
    } else {
        ui::text(ui::font(ui::F_SMALL), 24, y + 6, "Night", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
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
    epd_fill_rect({24, y, W - 48, 1}, ui::HAIR, fb);
    struct Row { const char* name; bool ok; time_t t; } rows[] = {
        {"FIRST LIGHT", d.dawnOk, d.dawn}, {"SUNRISE", d.riseOk, d.rise},
        {"SUNSET", d.setOk, d.set},        {"LAST LIGHT", d.duskOk, d.dusk},
    };
    for (int i = 0; i < 4; ++i) {
        const int ry = y + 16 + i * 72;
        char rn[32];
        ui::text(ui::font(ui::F_LABEL), 24, ry + 44, ui::sentence(rows[i].name, rn, sizeof rn), fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (rows[i].ok) hhmm(rows[i].t, a, sizeof a); else snprintf(a, sizeof a, "--");
        ui::text(ui::font(ui::F_TITLE), W - 24, ry + 48, a, fb, EPD_DRAW_ALIGN_RIGHT, INK);
        epd_fill_rect({24, ry + 70, W - 48, 1}, ui::HAIR, fb);
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
    char mn[32];
    snprintf(mn, sizeof mn, "%s", doba_sun::moonName(s.utc));
    if (mn[0] >= 'a' && mn[0] <= 'z') mn[0] -= 32;
    ui::text(ui::font(ui::F_KEY), 170, y + 66, mn, fb, EPD_DRAW_ALIGN_LEFT, INK);
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
                button({CTRL_X, y + (SROW_H - CTRL_H) / 2, CTRL_W, CTRL_H}, "Details  \xE2\x80\xBA", false, fb);
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
        epd_fill_rect({24, y + SROW_H - 1, W - 48, 1}, ui::HAIR, fb);
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
        case HOME:    renderHome(s, fb); break;
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
        case COMMS: return (g_cv == CV_CHATS || g_cv == CV_NODES || !mesh_service::enabled()) ? "" : "Comms";   // the page says "Comms" big
        case COMPOSE: return "Write";
        case LIBRARY: return doba_lib::libraryTitle();
        case ARTICLE: return "";
        case SETUP: case SETUP_NAME: return "Settings";
        case POINTS: return g_nearby ? "" : "Position";
        case POINT: return "Point";
        case POINT_NEW: return "New point";
        case MAPOPTS: return g_legend ? "Legend" : "Map options";
        case SOS: return "SOS";
        case SUN: return "Sun & moon";
        case MORE: return "More";
    }
    return nullptr;
}

void setPressMs(uint32_t ms) { g_pressMs = ms; }

Go tap(Page p, int x, int y, const RideState& s) {
    switch (p) {
        case HOME:    return tapHome(x, y);
        case COMMS:   return tapComms(x, y);
        case COMPOSE: return tapCompose(x, y);
        case LIBRARY: return doba_lib::tapSearch(g_search, x, y) ? TO_ARTICLE : STAY;
        case ARTICLE: return doba_lib::tapArticle(x, y) ? back(ARTICLE) : STAY;   // CLOSE: as BACK
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
        case POINTS:
            if (g_nearby) { g_nearby = g_nearby == 2 ? 1 : 0; g_nearPage = 0; return STAY; }
            return TO_HOME;
        case POINT:   return TO_POINTS;
        case POINT_NEW: return TO_POINTS;
        case MAPOPTS:
            if (g_legend) { g_legend = false; return STAY; }
            return TO_MAP;
        case SOS:     return TO_HOME;
        case SUN:     return TO_HOME;
        case MORE:    return TO_SETUP;
    }
    return TO_HOME;
}

}  // namespace doba_ui
