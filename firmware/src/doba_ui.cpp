// DOBA: main screen, comms and library pages — see doba_ui.h.
#include "doba_ui.h"
#include "doba_topo_bg.h"

#include <Arduino.h>
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
int g_commsScroll = 0;     // messages skipped from the newest

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
constexpr int TILE_GAP = 10;
constexpr int TILE_W = (W - 3 * TILE_GAP) / 2;
constexpr int TILE_H = (H - TOP - 5 * TILE_GAP) / 4;

EpdRect tileRect(int col, int row) {
    return {TILE_GAP + col * (TILE_W + TILE_GAP), TOP + TILE_GAP + row * (TILE_H + TILE_GAP),
            TILE_W, TILE_H};
}

const Tile kTiles[8] = {
    {tileRect(0, 0), "MAP", TO_MAP},         {tileRect(1, 0), "POSITION", TO_POINTS},
    {tileRect(0, 1), "LIBRARY", TO_LIBRARY}, {tileRect(1, 1), "COMMS", TO_COMMS},
    {tileRect(0, 2), "TRACKS", TO_TRACKS},   {tileRect(1, 2), "ROUTES", TO_ROUTES},
    {tileRect(0, 3), "SETTINGS", TO_SETUP}, {tileRect(1, 3), "", TO_RECORD},
};

void renderHome(const RideState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    char sub[64];
    for (int i = 0; i < 8; ++i) {
        const Tile& t = kTiles[i];
        const bool rec = t.go == TO_RECORD;
        const bool dark = rec && s.recording;
        if (dark) epd_fill_rect(t.r, INK, fb); else frame(t.r, 3, fb);
        const uint8_t fg = dark ? PAPER : INK;
        const char* title = rec ? (s.recording ? "STOP" : "RECORD") : t.title;
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
                if (!mesh_service::enabled()) snprintf(sub, sizeof sub, "radio off");
                else snprintf(sub, sizeof sub, "%d new · %d nodes", mesh_service::unreadCount(),
                              mesh_service::nodeCount());
                break;
            case TO_RECORD:
                if (s.recording)
                    snprintf(sub, sizeof sub, "%lu:%02lu · %.2f km", (unsigned long)(s.elapsedS / 3600),
                             (unsigned long)(s.elapsedS / 60 % 60), s.distanceM / 1000.0);
                else snprintf(sub, sizeof sub, "start a track");
                break;
            case TO_TRACKS: snprintf(sub, sizeof sub, "saved .fit tracks"); break;
            case TO_ROUTES: snprintf(sub, sizeof sub, "follow a .gpx"); break;
            case TO_SETUP: snprintf(sub, sizeof sub, "%s · %s", mesh::region(mesh_service::regionIndex()).name,
                                    milspec::milsSystem() == 6400 ? "6400" : "60-00"); break;
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
}

Go tapHome(int x, int y) {
    for (const Tile& t : kTiles)
        if (hit(t.r, x, y)) return t.go;
    return STAY;
}

// --- COMMS -------------------------------------------------------------------

const EpdRect kWrite = {12, H - 92, 260, 80};
const EpdRect kOlder = {284, H - 92, 118, 80};
const EpdRect kNewer = {410, H - 92, 118, 80};
const EpdRect kRadioOn = {120, 420, 300, 96};

void renderComms(const RideState& s, uint8_t* fb) {
    (void)s;
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    const EpdFont* small = ui::font(ui::F_SMALL);
    const EpdFont* tiny = ui::font(ui::F_TINY);
    const EpdFont* body = ui::font(ui::F_LABEL);
    char hdr[80];
    if (!mesh_service::enabled()) {
        ui::text(ui::font(ui::F_TITLE), W / 2, 360, "RADIO OFF", fb, EPD_DRAW_ALIGN_CENTER, INK);
        button(kRadioOn, "TURN ON", true, fb);
        ui::text(tiny, W / 2, 560, "LoRa 868 MHz · Meshtastic", fb, EPD_DRAW_ALIGN_CENTER, INK);
        return;
    }
    snprintf(hdr, sizeof hdr, "CH %s · %.3f MHz · %d nodes", mesh_service::channelName(),
             mesh_service::frequencyMHz(), mesh_service::nodeCount());
    ui::text(small, 14, TOP + 34, hdr, fb, EPD_DRAW_ALIGN_LEFT, INK);
    epd_fill_rect({0, TOP + 48, W, 2}, INK, fb);

    // Newest at the bottom; draw upward from the buttons.
    const int areaTop = TOP + 56, areaBottom = H - 104;
    int y = areaBottom;
    const int total = mesh_service::messageCount();
    if (g_commsScroll > total - 1) g_commsScroll = total > 0 ? total - 1 : 0;
    for (int i = total - 1 - g_commsScroll; i >= 0; --i) {
        mesh_service::Message m;
        if (!mesh_service::messageAt(i, m)) continue;
        char lines[8][256];
        int nl = 0;
        wrap(body, m.text, W - 40, [&](const char* l) {
            if (nl < 8) snprintf(lines[nl++], sizeof lines[0], "%s", l);
        });
        const int lineH = 30;
        const int blockH = 28 + nl * lineH + 14;
        if (y - blockH < areaTop) break;
        y -= blockH;
        char who[48], when[8], meta[96];
        clockOf(m.utc, when, sizeof when);
        if (m.outgoing) {
            const char* st = m.status == mesh_service::TX_ACKED ? "ACK"
                           : m.status == mesh_service::TX_SENT ? "SENT"
                           : m.status == mesh_service::TX_FAILED ? "FAILED" : "...";
            snprintf(meta, sizeof meta, "%s  ME  [%s]", when, st);
        } else {
            if (!mesh_service::nodeName(m.from, who, sizeof who))
                snprintf(who, sizeof who, "!%08lx", (unsigned long)m.from);
            snprintf(meta, sizeof meta, "%s  %s  %ddB", when, who, m.snr);
        }
        const int x = m.outgoing ? 40 : 14;
        ui::text(tiny, x, y + 20, meta, fb, EPD_DRAW_ALIGN_LEFT, INK);
        for (int k = 0; k < nl; ++k)
            ui::text(body, x, y + 28 + (k + 1) * lineH - 6, lines[k], fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (m.outgoing) epd_fill_rect({20, y + 6, 4, blockH - 18}, INK, fb);   // own-message bar
        epd_fill_rect({14, y + blockH - 4, W - 28, 1}, INK, fb);
    }
    if (total == 0)
        ui::text(small, W / 2, 420, "no messages yet", fb, EPD_DRAW_ALIGN_CENTER, INK);
    mesh_service::markAllRead();
    button(kWrite, "WRITE", true, fb);
    button(kOlder, "OLDER", false, fb);
    button(kNewer, "NEWER", false, fb);
}

Go tapComms(int x, int y) {
    if (!mesh_service::enabled()) {
        if (hit(kRadioOn, x, y)) mesh_service::setEnabled(true);
        return STAY;
    }
    if (hit(kWrite, x, y)) { g_compose.clear(); return TO_COMPOSE; }
    if (hit(kOlder, x, y)) { g_commsScroll += 3; return STAY; }
    if (hit(kNewer, x, y)) { g_commsScroll = g_commsScroll > 3 ? g_commsScroll - 3 : 0; return STAY; }
    return STAY;
}

// --- COMPOSE -----------------------------------------------------------------

void renderCompose(const RideState& s, uint8_t* fb) {
    (void)s;
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    g_compose.top = H - g_compose.height();
    g_compose.enterLabel = "SEND";
    char hdr[64];
    snprintf(hdr, sizeof hdr, "TO: ALL on %s", mesh_service::channelName());
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
        mesh_service::queueText(mesh::BROADCAST_ADDR, g_compose.text, 0);
        g_compose.clear();
        g_commsScroll = 0;
        return TO_COMMS;
    }
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

// --- SETUP: settings without the phone -------------------------------------------

constexpr int SROW_TOP = TOP + 8, SROW_H = 86;
enum SetupRow { R_RADIO, R_REGION, R_PRESET, R_NAME, R_MILS, R_LIGHT, R_TZ, R_MORE, R_COUNT };
// The bands that make sense for a device carried in Europe and Ukraine, then
// the rest of the world for the odd trip. Cycled in this order.
const char* const kRegionCycle[] = {"EU_868", "UA_868", "EU_433", "UA_433", "US", "RU"};
kbd::Keyboard g_nameKbd;

void renderSetup(const RideState& s, uint8_t* fb) {
    (void)s;
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    char v[48];
    for (int r = 0; r < R_COUNT; ++r) {
        const int y = SROW_TOP + r * SROW_H;
        switch (r) {
            case R_RADIO: row(y, SROW_H, "RADIO", onOff(mesh_service::enabled()), fb); break;
            case R_REGION: {
                const mesh::Region& rg = mesh::region(mesh_service::regionIndex());
                snprintf(v, sizeof v, "%s", rg.name);
                row(y, SROW_H, "BAND", v, fb);
                break;
            }
            case R_PRESET: row(y, SROW_H, "MODE", mesh_service::presetName(), fb); break;
            case R_NAME: {
                snprintf(v, sizeof v, "%.18s", mesh_service::longName());
                row(y, SROW_H, "CALLSIGN", v[0] ? v : "--", fb);
                break;
            }
            case R_MILS:
                row(y, SROW_H, "ANGLES", milspec::milsSystem() == 6400 ? "6400 mils" : "60-00 mils", fb);
                break;
            case R_LIGHT: {
                static const char* kL[] = {"OFF", "LOW", "MID", "HIGH"};
                const int b = settings::backlight();
                row(y, SROW_H, "LIGHT", kL[b < 0 ? 0 : b > 3 ? 3 : b], fb);
                break;
            }
            case R_TZ: {
                const int m = settings::tzMinutes();
                if (m % 60) snprintf(v, sizeof v, "-  UTC%+d:%02d  +", m / 60, abs(m % 60));
                else snprintf(v, sizeof v, "-  UTC%+d  +", m / 60);
                row(y, SROW_H, "TIME", v, fb, false);
                break;
            }
            case R_MORE: row(y, SROW_H, "MORE", "GPS, sensors", fb); break;
        }
    }
    char f[64];
    snprintf(f, sizeof f, "%.3f MHz · %d nodes heard", mesh_service::frequencyMHz(), mesh_service::nodeCount());
    ui::text(ui::font(ui::F_TINY), W / 2, H - 24, f, fb, EPD_DRAW_ALIGN_CENTER, INK);
}

Go tapSetup(int x, int y) {
    const int r = (y - SROW_TOP) / SROW_H;
    if (y < SROW_TOP || r < 0 || r >= R_COUNT) return STAY;
    switch (r) {
        case R_RADIO: mesh_service::setEnabled(!mesh_service::enabled()); break;
        case R_REGION: {
            const char* cur = mesh::region(mesh_service::regionIndex()).name;
            const int n = sizeof(kRegionCycle) / sizeof(kRegionCycle[0]);
            int at = -1;
            for (int i = 0; i < n; ++i) if (!strcmp(cur, kRegionCycle[i])) at = i;
            for (int k = 1; k <= n; ++k) {
                const int ri = mesh::regionIndexByName(kRegionCycle[(at + k + n) % n]);
                if (ri >= 0) { mesh_service::setRegion((uint8_t)ri); break; }
            }
            break;
        }
        case R_PRESET: mesh_service::setPreset((uint8_t)((mesh_service::presetIndex() + 1) % mesh::PRESET_COUNT)); break;
        case R_NAME:
            g_nameKbd.clear();
            snprintf(g_nameKbd.text, sizeof g_nameKbd.text, "%s", mesh_service::longName());
            return TO_SETUP_NAME;
        case R_MILS: milspec::setMilsSystem(milspec::milsSystem() == 6400 ? 6000 : 6400); break;
        case R_LIGHT: settings::setBacklight((settings::backlight() + 1) % 4); break;
        case R_TZ: {
            int m = settings::tzMinutes() + (x < W / 2 ? -60 : 60);
            if (m < -12 * 60) m = 14 * 60;
            if (m > 14 * 60) m = -12 * 60;
            settings::setTzMinutes(m);
            break;
        }
        case R_MORE: return TO_SETTINGS;
    }
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
        snprintf(ll, sizeof ll, "%c%.5f %c%.5f  DEPTH %dm  SAT %d", lat >= 0 ? 'N' : 'S', fabs(lat),
                 lon >= 0 ? 'E' : 'W', fabs(lon), (int)lroundf(dep), s.satellites);
    else if (have) snprintf(ll, sizeof ll, "%c%.5f %c%.5f  ALT %dm  SAT %d", lat >= 0 ? 'N' : 'S', fabs(lat),
                       lon >= 0 ? 'E' : 'W', fabs(lon), (int)lroundf(groundAlt(s)), s.satellites);
    else snprintf(ll, sizeof ll, "waiting for satellites · SAT %d", s.satellites);
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
        ui::text(ui::font(ui::F_SMALL), W / 2, PROW_TOP + 120, "no points yet", fb, EPD_DRAW_ALIGN_CENTER, INK);
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
            snprintf(hz, sizeof hz, "HEIGHT %dm  (%+dm)", (int)lroundf(pz), dz);
            ui::text(ui::font(ui::F_SMALL), tx, dialY + 126, hz, fb, EPD_DRAW_ALIGN_LEFT, INK);
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

}  // namespace

void render(Page p, const RideState& s, uint8_t* fb) {
    switch (p) {
        case HOME:    renderHome(s, fb); topo_bg::draw({0, TOP, W, H - TOP}, fb, 0); break;
        case COMMS:   renderComms(s, fb); topo_bg::draw({0, TOP, W, H - TOP}, fb, 1); break;
        case COMPOSE: renderCompose(s, fb); break;
        case LIBRARY:
            doba_lib::renderSearch(g_search, fb);
            topo_bg::draw({0, TOP + 146, W, g_search.top - TOP - 146}, fb, 2);   // not in the boxes or keys
            break;
        case ARTICLE: doba_lib::renderArticle(fb); break;
        case SETUP:   renderSetup(s, fb); break;
        case SETUP_NAME:
            renderField("CALLSIGN  (how the mesh sees you)", "Short name: first four letters", g_nameKbd, "SAVE", fb);
            break;
        case POINTS:  renderPoints(s, fb); break;
        case POINT:   renderPoint(s, fb); break;
        case POINT_NEW: renderPointNew(s, fb); break;
    }
}

const char* title(Page p) {
    switch (p) {
        case HOME: return nullptr;
        case COMMS: return "COMMS";
        case COMPOSE: return "WRITE";
        case LIBRARY: case ARTICLE: return "LIBRARY";
        case SETUP: case SETUP_NAME: return "SETTINGS";
        case POINTS: return "POSITION";
        case POINT: return "POINT";
        case POINT_NEW: return "NEW POINT";
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
    }
    return STAY;
}

Go back(Page p) {
    switch (p) {
        case HOME:    return TO_MAP;
        case COMMS:   return TO_HOME;
        case COMPOSE: return TO_COMMS;
        case LIBRARY: return TO_HOME;
        case ARTICLE:
            if (doba_lib::articleBack()) return STAY;   // back along a followed link
            return TO_LIBRARY;
        case SETUP:   return TO_HOME;
        case SETUP_NAME: return TO_SETUP;
        case POINTS:  return TO_HOME;
        case POINT:   return TO_POINTS;
        case POINT_NEW: return TO_POINTS;
    }
    return TO_HOME;
}

}  // namespace doba_ui
