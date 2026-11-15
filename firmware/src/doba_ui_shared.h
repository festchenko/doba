#pragma once

// What the pages of doba_ui share: the page geometry, the drawing helpers and the
// controls. Internal to doba_ui*.cpp.

#include "doba_i18n.h"
#include "doba_psram.h"
#include "doba_battery.h"
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
#include "device_state.h"
#include "settings.h"
#include "ui_render.h"

namespace doba_ui {
void fitTextTo(const EpdFont* f, char* t, int w);
namespace pg {


constexpr int W = 540, H = 960;
constexpr int TOP = ui::STATUS_H;
constexpr uint8_t INK = 0x00, PAPER = 0xFF;

inline kbd::Keyboard g_compose;   // the message being written
inline kbd::Keyboard g_search;    // the library query

inline void frame(const EpdRect& r, int t, uint8_t* fb) {
    for (int i = 0; i < t; ++i)
        epd_draw_rect({r.x + i, r.y + i, r.width - 2 * i, r.height - 2 * i}, INK, fb);
}

inline bool hit(const EpdRect& r, int x, int y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

inline void button(const EpdRect& r, const char* label, bool dark, uint8_t* fb) {
    if (dark) ui::fillRound(r, 8, INK, fb);
    else ui::frameRound(r, 8, 2, fb);
    char l[96];
    ui::text(ui::font(ui::F_LABEL), r.x + r.width / 2, r.y + r.height / 2 + 10, ui::sentence(TR(label), l, sizeof l), fb,
             EPD_DRAW_ALIGN_CENTER, dark ? PAPER : INK);
}

// Word-wraps UTF-8 text to maxW; calls emit(line) for each line. Returns lines.
template <typename F>
inline int wrap(const EpdFont* f, const char* s, int maxW, F emit) {
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

inline void clockOf(uint32_t utc, char* out, size_t n) {
    if (!utc) { snprintf(out, n, "--:--"); return; }
    const time_t t = (time_t)utc + settings::tzMinutes() * 60;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(out, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

// --- shared bits for the pages below -------------------------------------------

// The current position, if there is one worth using.
inline bool here(const DeviceState& s, double& lat, double& lon) {
    if (!s.gpsFix && !s.everHadFix) return false;
    lat = s.latitude;
    lon = s.longitude;
    return true;
}

// Height: barometer, else the card's DEM, else GPS.
inline float groundAlt(const DeviceState& s) {
    return s.baroValid ? s.baroAltM : s.mapElevationValid ? s.mapElevationM : s.altitudeM;
}

// "12-34" / "1234" and degrees.
inline void azText(double deg, char* out, size_t n) {
    char m[8];
    milgeo::mils((float)deg, milspec::milsSystem(), m, sizeof m);
    snprintf(out, n, "%s", m);
}

// A UTF-8 short name: the first four non-space characters, upper-cased ASCII.
inline void shortName(const char* lng, char* out, size_t n) {
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

inline void section(int y, const char* name, uint8_t* fb) {
    char up[96];
    ui::upper(TR(name), up, sizeof up);
    ui::text(ui::font(ui::F_TINY), 24, y + 26, up, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
}

// The hint stops short of the control on its right ("full" rows have none):
// a long one ran under the button ("порожньо: торкніться, щоб напис|Змінити").
inline void caption(int y, int h, const char* name, const char* hint, uint8_t* fb, bool full = false) {
    char n[96];
    ui::text(ui::font(ui::F_LABEL), 24, y + (hint ? h / 2 - 2 : h / 2 + 10), ui::sentence(TR(name), n, sizeof n), fb, EPD_DRAW_ALIGN_LEFT, INK);
    if (hint) {
        char t[128];
        snprintf(t, sizeof t, "%s", TR(hint));
        fitTextTo(ui::font(ui::F_TINY), t, (full ? W - 24 : CTRL_X - 12) - 24);
        ui::text(ui::font(ui::F_TINY), 24, y + h / 2 + 24, t, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    }
}

// n labels side by side, the chosen one filled.
inline void segments(int y, int h, const char* const* labels, int n, int sel, uint8_t* fb, int x0 = CTRL_X, int w = CTRL_W) {
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
        char lb[64];
        ui::text(lf, sx + sw / 2, top + CTRL_H / 2 + (lf == ui::font(ui::F_TINY) ? 6 : 9), ui::sentence(TR(labels[i]), lb, sizeof lb), fb, EPD_DRAW_ALIGN_CENTER,
                 i == sel ? PAPER : INK);
    }
}
inline int segmentHit(int x, int n, int x0 = CTRL_X, int w = CTRL_W) {
    if (x < x0 - 10 || x > x0 + w + 10) return -1;
    const int i = (x - x0) * n / w;
    return i < 0 ? 0 : i >= n ? n - 1 : i;
}

// "‹  value  ›": the halves step back and forward.
inline void stepper(int y, int h, const char* value, uint8_t* fb) {
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
    char v[64];
    ui::text(ui::font(ui::F_SMALL), CTRL_X + CTRL_W / 2, top + CTRL_H / 2 + 8, ui::sentence(TR(value), v, sizeof v), fb, EPD_DRAW_ALIGN_CENTER, INK);
}
inline int stepperHit(int x) {   // -1, +1, or 0 (the middle: also forward)
    if (x < CTRL_X - 10) return 0;
    return x < CTRL_X + CTRL_W / 2 ? -1 : +1;
}

// ON | OFF as two segments.
inline void toggle(int y, int h, bool on, uint8_t* fb) {
    static const char* const kOnOff[] = {"OFF", "ON"};
    segments(y, h, kOnOff, 2, on ? 1 : 0, fb, CTRL_X + CTRL_W - 150, 150);
}
inline bool toggleHit(int x) { return x >= CTRL_X + CTRL_W - 160; }


// ---- across the pages -------------------------------------------------------------
// Page geometry and timings
constexpr int SROW_H = 78, SEC_H = 34;
constexpr uint32_t kHoldMs = 4500;   // "hold 5 s": going loud again, and sending SOS (touch is read a beat late)
constexpr uint32_t SOS_EVERY_MS = 5 * 60 * 1000;
enum CommsView : uint8_t { CV_CHATS, CV_NODES, CV_CHAT, CV_QUICK };

// State one page sets and another reads
extern kbd::Keyboard g_nameKbd;
extern uint32_t g_pressMs;
extern bool g_fieldOwner;
extern int g_ownerField;
extern bool g_sosOn;
extern int g_pointSel;
extern CommsView g_cv;
extern bool g_deleteArmed;
extern const char* const kOwnerCap[settings::OWNER_FIELDS];
extern uint32_t g_sosArmedAt, g_sosLast;
extern bool g_legend;
extern int g_nearby;
extern int g_nearKind, g_nearPage;

// Each page's drawing and taps (doba_ui_*.cpp), and the helpers they lend each other
void hhmm(time_t utc, char* out, size_t n);
const char* hoursText(float h);
void renderComms(const DeviceState& s, uint8_t* fb);
void renderCompose(const DeviceState& s, uint8_t* fb);
void fit(const EpdFont* f, char* s, int maxW);
void renderField(const char* caption, const char* note, kbd::Keyboard& k, const char* enter, uint8_t* fb);
void renderPoint(const DeviceState& s, uint8_t* fb);
void renderPoints(const DeviceState& s, uint8_t* fb);
void renderMapOpts(const DeviceState& s, uint8_t* fb);
void renderPointNew(const DeviceState& s, uint8_t* fb);
void renderSos(const DeviceState& s, uint8_t* fb);
void renderSun(const DeviceState& s, uint8_t* fb);
void renderBattery(const DeviceState& s, uint8_t* fb);
void renderMore(const DeviceState& s, uint8_t* fb);
void sosSend(const DeviceState& s);
Go tapComms(int x, int y);
Go tapCompose(int x, int y);
Go tapSetupName(int x, int y);
Go tapMapOpts(int x, int y);
Go tapPoint(int x, int y);
Go tapPointNew(int x, int y);
Go tapPoints(int x, int y, const DeviceState& s);
Go tapSos(int x, int y, const DeviceState& s);
bool commsBack();
Go tapMore(int x, int y);

}  // namespace pg
}  // namespace doba_ui
