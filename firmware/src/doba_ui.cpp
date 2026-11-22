// DOBA: main screen, comms and library pages — see doba_ui.h. The pages live in
// doba_ui_*.cpp; what they share is in doba_ui_shared.h.
#include "doba_ui_shared.h"
#include <esp_heap_caps.h>
#include "ui_shared.h"

namespace doba_ui {
namespace pg {

// --- HOME --------------------------------------------------------------------

struct Tile { EpdRect r; const char* title; Go go; };
void sosSubtitle(char* out, size_t n);
void renderStealthBar(uint8_t* fb);
void renderAidBar(uint8_t* fb);
void sunSubtitle(const DeviceState& s, char* out, size_t n);
// The design canvas's home: 24 px margins, 14 px gaps, rounded tiles.
constexpr int MARGIN = 24, TILE_GAP = 14;
constexpr int TILE_W = (W - 2 * MARGIN - TILE_GAP) / 2;
constexpr int STEALTH_H = 72;
constexpr int TILE_TOP = TOP + 6;
// Top to bottom (user, 19 Nov): first aid; the map and the library full width;
// position, comms, SOS and sun smaller, two by two; settings and power low;
// the stealth bar.
constexpr int AID_H = 64, LOW_H = 64;
constexpr int SPACE = H - 24 - STEALTH_H - TILE_GAP - TILE_TOP;   // from the top to the stealth bar
constexpr int ROWS_H = SPACE - AID_H - LOW_H - 5 * TILE_GAP;      // the two wide and the two small rows
constexpr int WIDE_H = ROWS_H * 55 / 100 / 2, SMALL_H = (ROWS_H - 2 * WIDE_H) / 2;
constexpr int FULL_W = W - 2 * MARGIN;
constexpr int Y_AID = TILE_TOP, Y_MAP = Y_AID + AID_H + TILE_GAP, Y_LIB = Y_MAP + WIDE_H + TILE_GAP;
constexpr int Y_S1 = Y_LIB + WIDE_H + TILE_GAP, Y_S2 = Y_S1 + SMALL_H + TILE_GAP, Y_LOW = Y_S2 + SMALL_H + TILE_GAP;
const EpdRect kStealthBar = {MARGIN, H - 24 - STEALTH_H, W - 2 * MARGIN, STEALTH_H};
const EpdRect kAidBar = {MARGIN, Y_AID, FULL_W, AID_H};
uint32_t g_stealthOffAsk = 0;   // a short tap on a lit stealth bar: say to hold
uint32_t g_pressMs = 0;         // how long the finger was down for this tap

EpdRect half(int col, int y, int h) { return {MARGIN + col * (TILE_W + TILE_GAP), y, TILE_W, h}; }

// DOBA is for survival, not a navigator: no routes, no recorded tracks.
const Tile kTiles[8] = {
    {{MARGIN, Y_MAP, FULL_W, WIDE_H}, "Map", TO_MAP},
    {{MARGIN, Y_LIB, FULL_W, WIDE_H}, "Library", TO_LIBRARY},
    {half(0, Y_S1, SMALL_H), "Position", TO_POINTS}, {half(1, Y_S1, SMALL_H), "Comms", TO_COMMS},
    {half(0, Y_S2, SMALL_H), "SOS", TO_SOS},         {half(1, Y_S2, SMALL_H), "Sun", TO_SUN},
    {half(0, Y_LOW, LOW_H), "Settings", TO_SETUP},   {half(1, Y_LOW, LOW_H), "Power", TO_POWER},
};

// The contour lines take nearly all of this page's 360 ms (18 Nov), and the
// page is live (1 Hz), so a tap waited behind them most of every second. The
// lines keep clear of whatever is drawn, so they depend on the rest of the
// page: drawn once for a page, and copied in while the page under them is the
// same (the hash of the page without them).
void tileSubtitle(const Tile& t, const DeviceState& s, char* sub, size_t n) {
    sub[0] = 0;
    switch (t.go) {
        case TO_MAP: snprintf(sub, n, TR("topo + MGRS grid")); break;
        case TO_POINTS:
            if (doba_points::target() >= 0 && (s.gpsFix || s.everHadFix)) {
                double b, d;
                doba_points::fromHere(doba_points::target(), s.latitude, s.longitude, b, d);
                char az[8], ds[16];
                milgeo::mils((float)b, milspec::milsSystem(), az, sizeof az);
                doba_points::formatDistance(d, ds, sizeof ds);
                snprintf(sub, n, "> %.8s %s %s", doba_points::at(doba_points::target())->name, az, ds);
            } else if (s.gpsFix || s.everHadFix) milgeo::mgrs(s.latitude, s.longitude, sub, n);
            else snprintf(sub, n, TR("no fix yet"));
            break;
        case TO_LIBRARY: snprintf(sub, n, TRN(doba_lib::shelfCount(), "%d book on card", "%d books on card"), doba_lib::shelfCount()); break;
        case TO_COMMS:
            if (stealth::active()) snprintf(sub, n, stealth::listen() ? TR("stealth · listening") : TR("stealth · radio off"));
            else if (!mesh_service::enabled()) snprintf(sub, n, TR("radio off"));
            else snprintf(sub, n, TR("%d new · %d nodes"), mesh_service::unreadCount(),
                          mesh_service::nodeCount());
            break;
        case TO_SOS: sosSubtitle(sub, n); break;
        case TO_SUN: sunSubtitle(s, sub, n); break;
        case TO_POWER:
            if (s.batteryPercent) snprintf(sub, n, TR("%u%%%s · sleep / off"), s.batteryPercent, s.charging ? TR(" charging") : "");
            else snprintf(sub, n, TR("sleep / off"));
            break;
        case TO_SETUP: {
            const int m = settings::tzMinutes();
            snprintf(sub, n, TR("UTC%+d · %s"), m / 60, milspec::milsSystem() == 6400 ? "6400" : "60-00");
            break;
        }
        default: break;
    }
}

// What the home page shows, as a number: the page is redrawn when this
// changes (or every 30 s), not every second. Every tile's words, the stealth
// bar's state, the SOS state and what the status bar carries.
uint32_t homeKeyOf(const DeviceState& s) {
    uint32_t h = 2166136261u;
    auto mix = [&](const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
    };
    char sub[64];
    for (const Tile& t : kTiles) { tileSubtitle(t, s, sub, sizeof sub); mix(sub, strlen(sub)); }
    const bool on = stealth::active();
    const uint8_t flags[] = {(uint8_t)on, (uint8_t)stealth::listen(),
                             (uint8_t)(on && g_stealthOffAsk && millis() - g_stealthOffAsk < 5000),
                             (uint8_t)sosActive(), (uint8_t)s.gpsFix, (uint8_t)s.charging, (uint8_t)s.batteryPercent};
    mix(flags, sizeof flags);
    const int unread = mesh_service::unreadCount();
    mix(&unread, sizeof unread);
    const uint32_t minute = (uint32_t)(time(nullptr) / 60);
    mix(&minute, sizeof minute);
    return h;
}

uint8_t* g_homeDone = nullptr;   // the last page with its lines
uint32_t g_homeHash = 0;

uint32_t pageHash(const uint8_t* fb) {
    // the whole buffer (the panel is rotated: no rows to skip). The status bar
    // in it is the last frame's, so the lines are drawn again once a minute.
    const uint32_t* w = (const uint32_t*)fb;
    uint32_t h = 2166136261u;
    for (size_t i = 0, n = uid::fbSize / 4; i < n; ++i) h = (h ^ w[i]) * 16777619u;
    return h ? h : 1;
}

void dropHomeCache() {
    if (g_homeDone) heap_caps_free(g_homeDone);
    g_homeDone = nullptr;
    g_homeHash = 0;
}

void renderHomePage(const DeviceState& s, uint8_t* fb);

void renderHome(const DeviceState& s, uint8_t* fb) {
    renderHomePage(s, fb);
    const uint32_t h = pageHash(fb);
    if (g_homeDone && h == g_homeHash) { memcpy(fb, g_homeDone, uid::fbSize); return; }
    // the contour lines inside the light tiles only: one map, seen through them
    for (const Tile& t : kTiles)
        if (t.go != TO_SOS) topo_bg::draw({t.r.x + 3, t.r.y + 3, t.r.width - 6, t.r.height - 6}, fb, 0);
        else if (!sosActive()) topo_bg::drawInverted({t.r.x + 6, t.r.y + 6, t.r.width - 12, t.r.height - 12}, fb, 0);
    // and the bars: first aid on top, stealth below (black when on)
    topo_bg::draw({kAidBar.x + 4, kAidBar.y + 4, kAidBar.width - 8, kAidBar.height - 8}, fb, 0);
    {
        const EpdRect r = kStealthBar;
        if (stealth::active()) topo_bg::drawInverted({r.x + 6, r.y + 6, r.width - 12, r.height - 12}, fb, 0);
        else topo_bg::draw({r.x + 3, r.y + 3, r.width - 6, r.height - 6}, fb, 0);
    }
    if (!g_homeDone) g_homeDone = (uint8_t*)heap_caps_malloc(uid::fbSize, MALLOC_CAP_SPIRAM);   // a cache: it gives way
    if (g_homeDone) { memcpy(g_homeDone, fb, uid::fbSize); g_homeHash = h; }
}

void renderHomePage(const DeviceState& s, uint8_t* fb) {
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
        const char* title = sos && sosActive() ? TR("SOS on") : TR(t.title);
        const bool low = t.r.height <= LOW_H;   // settings, power: a name and one short line
        const EpdFont* tf = ui::font(low ? ui::F_KEY : ui::F_TITLE);
        if (low && ui::textWidth(tf, title) > t.r.width - 18) tf = ui::font(ui::F_SMALL);   // a long name, another language
        ui::text(tf, t.r.x + (low ? 10 : 18), t.r.y + (low ? 28 : 46), title, fb,
                 EPD_DRAW_ALIGN_LEFT, fg);
        tileSubtitle(t, s, sub, sizeof sub);
        // Subtitle may be long (MGRS): shrink to fit the tile.
        const bool mono = t.go == TO_POINTS && sub[0] >= '0' && sub[0] <= '9';   // a grid reference
        const EpdFont* sf = mono ? ui::font(ui::F_MONO) : ui::font(ui::F_SMALL);
        if (low || ui::textWidth(sf, sub) > t.r.width - 36) sf = ui::font(ui::F_TINY);
        ui::text(sf, t.r.x + (low ? 10 : 18), t.r.y + t.r.height - (low ? 12 : 20), sub, fb, EPD_DRAW_ALIGN_LEFT, dark ? PAPER : 0x30);
        // Unread badge on COMMS.
        if (t.go == TO_COMMS && mesh_service::enabled() && mesh_service::unreadCount() > 0) {
            epd_fill_circle(t.r.x + t.r.width - 30, t.r.y + 34, 16, INK, fb);
            char nb[8];
            snprintf(nb, sizeof nb, "%d", mesh_service::unreadCount() > 99 ? 99 : mesh_service::unreadCount());
            ui::text(ui::font(ui::F_SMALL), t.r.x + t.r.width - 30, t.r.y + 41, nb, fb,
                     EPD_DRAW_ALIGN_CENTER, PAPER);
        }
    }
    renderAidBar(fb);
    renderStealthBar(fb);
}

// First aid, one tap from home (QA 18 Nov: in trouble, the library's
// encyclopedia articles kept what to do on page 15). Doba's own short cards,
// in the language of the interface.
void renderAidBar(uint8_t* fb) {
    const EpdRect r = kAidBar;
    ui::frameRound(r, 8, 3, fb);
    // a cross, drawn: the bar is found without reading
    const int cx = r.x + 36, cy = r.y + r.height / 2;
    epd_fill_rect({cx - 7, cy - 20, 14, 40}, INK, fb);
    epd_fill_rect({cx - 20, cy - 7, 40, 14}, INK, fb);
    ui::text(ui::font(ui::F_KEY), r.x + 70, r.y + 30, TR("First aid"), fb, EPD_DRAW_ALIGN_LEFT, INK);
    ui::text(ui::font(ui::F_SMALL), r.x + 70, r.y + r.height - 12, TR("bleeding, CPR, burns, cold, water"), fb,
             EPD_DRAW_ALIGN_LEFT, ui::GREY);
}

// The stealth bar: one tap hides the device; going loud again takes a second
// tap within a few seconds, so a brush of the screen cannot give you away.
void renderStealthBar(uint8_t* fb) {
    const EpdRect r = kStealthBar;
    const bool on = stealth::active();
    const bool asking = on && g_stealthOffAsk && millis() - g_stealthOffAsk < 5000;
    if (on) ui::fillRound(r, 8, INK, fb); else ui::frameRound(r, 8, 2, fb);
    const uint8_t fg = on ? PAPER : INK;
    ui::text(ui::font(ui::F_KEY), r.x + 20, r.y + 34, on ? TR("Stealth on") : TR("Stealth"), fb, EPD_DRAW_ALIGN_LEFT, fg);
    const char* sub = asking ? "Hold for 5 seconds to go loud" :
                      on ? (stealth::listen() ? "Silent, still listening \xC2\xB7 hold 5 s to end" : "Radio and light off \xC2\xB7 hold 5 s to end")
                         : "Radio and light off, screen dim";
    ui::text(ui::font(ui::F_SMALL), r.x + 20, r.y + r.height - 14, TR(sub), fb, EPD_DRAW_ALIGN_LEFT, on ? PAPER : ui::GREY);
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
    if (hit(kAidBar, x, y)) {
        const bool uk = i18n::lang() == i18n::UK;
        if (!doba_lib::showBook(uk ? "aid_uk" : "aid_en")) doba_lib::showBook(uk ? "aid_en" : "aid_uk");
        return TO_LIBRARY;
    }
    for (const Tile& t : kTiles)
        if (hit(t.r, x, y)) {
            // Comms from home lands on a chat or a list, never on the ready-phrase
            // sheet left open when the page was last left (QA, 18 Nov).
            if (t.go == TO_COMMS && g_cv == CV_QUICK) g_cv = CV_CHAT;
            return t.go;
        }
    return STAY;
}

// --- SETUP: settings without the phone -------------------------------------------

// The bands that make sense for a device carried in Europe and Ukraine, then
// the rest of the world for the odd trip. Stepped through in this order.
const char* const kRegionCycle[] = {"EU_868", "UA_868", "EU_433", "UA_433", "US", "RU"};
constexpr int kRegions = sizeof(kRegionCycle) / sizeof(kRegionCycle[0]);
kbd::Keyboard g_nameKbd;

// The layout, top to bottom: section headers and rows.
enum SetupRow { R_STEALTH, R_RADIO, R_REGION, R_NAME, R_LIGHT, R_TZ, R_LANG, R_MILS, R_MORE, R_COUNT };
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

void renderSetup(const DeviceState& s, uint8_t* fb) {
    (void)s;
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    section(setupRowY(R_STEALTH) - SEC_H, "RADIO", fb);
    section(setupRowY(R_LIGHT) - SEC_H, "SCREEN, CLOCK, LANGUAGE", fb);
    section(setupRowY(R_MILS) - SEC_H, "NAVIGATION", fb);
    char v[96], hint[96];
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
                const char* where = !strcmp(rg.name, "EU_868") ? TR("Europe") : !strcmp(rg.name, "UA_868") ? TR("Ukraine") : rg.name;
                if (settings::regionAuto()) snprintf(hint, sizeof hint, TR("by GPS: %s"), where);
                else snprintf(hint, sizeof hint, TR("set by hand: %s"), where);
                caption(y, SROW_H, "COUNTRY", hint, fb);
                stepper(y, SROW_H, settings::regionAuto() ? "AUTO" : rg.name, fb);
                break;
            }
            case R_NAME: {
                char sh[20];
                shortName(mesh_service::longName(), sh, sizeof sh);
                snprintf(hint, sizeof hint, TR("short: %s · tap to edit"), sh);
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
                if (m % 60) snprintf(off, sizeof off, TR("UTC%+d:%02d"), m / 60, abs(m % 60));
                else snprintf(off, sizeof off, TR("UTC%+d"), m / 60);
                if (settings::tzAuto()) {
                    double la, lo;
                    const bool pos = settings::lastPosition(la, lo);
                    if (!pos) snprintf(hint, sizeof hint, TR("by GPS: waiting for a fix"));
                    else snprintf(hint, sizeof hint, TR("by GPS: %s%s"), off,
                                  tz_auto::summerAt(la, lo, time(nullptr)) ? ", summer" : "");
                    caption(y, SROW_H, "TIME ZONE", hint, fb);
                    stepper(y, SROW_H, "AUTO", fb);
                } else {
                    caption(y, SROW_H, "TIME ZONE", "set by hand", fb);
                    stepper(y, SROW_H, off, fb);
                }
                break;
            }
            case R_LANG: {
                // each language named in itself: whoever cannot read the other finds theirs
                static const char* const kLg[] = {"English", "\xD0\xA3\xD0\xBA\xD1\x80\xD0\xB0\xD1\x97\xD0\xBD\xD1\x81\xD1\x8C\xD0\xBA\xD0\xB0"};
                caption(y, SROW_H, "LANGUAGE", "the words on every screen", fb);
                segments(y, SROW_H, kLg, 2, i18n::lang() == i18n::UK ? 1 : 0, fb);
                break;
            }
            case R_MILS: {
                static const char* const kM[] = {"60-00", "6400"};
                caption(y, SROW_H, "ANGLES", milspec::milsSystem() == 6400 ? "NATO mils" : "6000 mils (UA)", fb);
                segments(y, SROW_H, kM, 2, milspec::milsSystem() == 6400 ? 1 : 0, fb);
                break;
            }
            case R_MORE: {
                caption(y, SROW_H, "MORE", "battery, GPS, card", fb);
                const int top = y + (SROW_H - CTRL_H) / 2;
                ui::frameRound({CTRL_X, top, CTRL_W, CTRL_H}, 8, 2, fb);
                ui::text(ui::font(ui::F_SMALL), CTRL_X + CTRL_W / 2, top + CTRL_H / 2 + 8, TR("Open  \xE2\x80\xBA"), fb,
                         EPD_DRAW_ALIGN_CENTER, INK);
                break;
            }
        }
        epd_fill_rect({24, y + SROW_H - 1, W - 48, 1}, ui::HAIR, fb);
    }
    char f[128];
    snprintf(f, sizeof f, TR("%d nodes heard · settings save at once"), mesh_service::nodeCount());
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
        case R_LANG: {
            const int i = segmentHit(x, 2);
            if (i >= 0) i18n::setLang(i == 1 ? i18n::UK : i18n::EN);
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

}  // namespace pg
using namespace pg;


// --- the power-off card -------------------------------------------------------------
//
// E-paper keeps its picture with no power at all, so a switched-off Doba is a
// paper card: what someone (you, later, or whoever finds it) most needs to read.

// Cut to a width with "..." (whole UTF-8 characters).
void fitTextTo(const EpdFont* f, char* t, int w) {
    if (ui::textWidth(f, t) <= w) return;
    size_t n = strlen(t);
    while (n > 1) {
        --n;
        while (n > 0 && ((uint8_t)t[n] & 0xC0) == 0x80) --n;
        memcpy(t + n, "...", 4);
        if (ui::textWidth(f, t) <= w) return;
        t[n] = 0;
    }
}

void renderOffCard(const DeviceState& s, bool powerKey, uint8_t* fb) {
    // The map is already drawn, centred on the position at y 430 (riderY).
    constexpr int MAP_TOP = 262, MAP_BOT = 560;
    // Paper above and below the map window, then contour lines on it: the
    // same landscape texture as Doba's own screens. Cards go on top, white.
    epd_fill_rect({0, 0, W, MAP_TOP}, PAPER, fb);
    epd_fill_rect({0, MAP_BOT, W, H - MAP_BOT}, PAPER, fb);
    topo_bg::draw({0, 0, W, MAP_TOP}, fb, 3);
    topo_bg::draw({0, MAP_BOT, W, H - MAP_BOT}, fb, 3);

    auto card = [&](int y, int h) {
        const EpdRect r = {16, y, W - 32, h};
        ui::fillRound(r, 12, PAPER, fb);
        ui::frameRound(r, 12, 2, fb, INK, PAPER);
        return r;
    };
    auto label = [&](int x, int y, const char* en) {
        char up[64];
        ui::upper(TR(en), up, sizeof up);
        ui::text(ui::font(ui::F_TINY), x, y, up, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    };
    char line[160], a[24];

    // --- head: the mark, and how to wake it
    {
        const EpdRect m = {24, 22, 150, 54};
        ui::fillRound(m, 6, PAPER, fb);
        for (int t = 0; t < 3; ++t) epd_draw_rect({m.x + t, m.y + t, m.width - 2 * t, m.height - 2 * t}, INK, fb);
        ui::text(ui::font(ui::F_TITLE), m.x + m.width / 2, m.y + 40, "D.O.B.A", fb, EPD_DRAW_ALIGN_CENTER, INK);
        const EpdRect st = {190, 22, W - 24 - 190, 54};
        ui::fillRound(st, 8, INK, fb);
        char up[64];
        ui::upper(TR("POWERED OFF"), up, sizeof up);
        ui::text(ui::font(ui::F_LABEL), st.x + st.width / 2, st.y + 24, up, fb, EPD_DRAW_ALIGN_CENTER, PAPER);
        ui::text(ui::font(ui::F_TINY), st.x + st.width / 2, st.y + 46,
                 powerKey ? TR("press POWER to switch on") : TR("press BOOT to wake"), fb, EPD_DRAW_ALIGN_CENTER, PAPER);
    }

    // --- where it was switched off
    double lat = s.latitude, lon = s.longitude;
    bool pos = s.everHadFix;
    if (!pos) pos = settings::lastPosition(lat, lon);
    doba_batt::Report br;
    doba_batt::report(br);
    const int pct = br.have ? br.soc : s.batteryPercent;
    {
        const EpdRect r = card(92, 156);
        label(r.x + 18, r.y + 28, "WHERE I WAS");
        if (pos) {
            milgeo::mgrs(lat, lon, line, sizeof line);
            ui::text(ui::font(ui::F_MONO), r.x + 18, r.y + 72, line, fb, EPD_DRAW_ALIGN_LEFT, INK);
            snprintf(line, sizeof line, "%.5f, %.5f", lat, lon);
            ui::text(ui::font(ui::F_SMALL), r.x + 18, r.y + 104, line, fb, EPD_DRAW_ALIGN_LEFT, INK);
        } else {
            ui::text(ui::font(ui::F_SMALL), r.x + 18, r.y + 72, TR("no position yet"), fb, EPD_DRAW_ALIGN_LEFT, INK);
        }
        if (s.timeValid) {
            const time_t t = s.utc + settings::tzMinutes() * 60;
            struct tm tm;
            gmtime_r(&t, &tm);
            snprintf(a, sizeof a, "%02d.%02d %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
        } else snprintf(a, sizeof a, "--");
        if (br.hoursLeft > 0) snprintf(line, sizeof line, TR("off %s · charge %d%% (about %s of use)"), a, pct, hoursText(br.hoursLeft));
        else snprintf(line, sizeof line, TR("off %s · charge %d%%"), a, pct);
        ui::text(ui::font(ui::F_TINY), r.x + 18, r.y + 138, line, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        // the charge as a small cell, right
        const EpdRect cell = {r.x + r.width - 18 - 64, r.y + 14, 58, 24};
        ui::frameRound(cell, 4, 2, fb, INK, PAPER);
        epd_fill_rect({cell.x + cell.width, cell.y + 7, 4, 10}, INK, fb);
        epd_fill_rect({cell.x + 4, cell.y + 4, (cell.width - 8) * std::max(0, std::min(100, pct)) / 100, cell.height - 8}, INK, fb);
    }

    // --- the map window
    epd_fill_rect({0, MAP_TOP, W, 2}, INK, fb);
    epd_fill_rect({0, MAP_BOT - 2, W, 2}, INK, fb);

    // --- tomorrow's light, four times in a row, and the moon
    int y = MAP_BOT + 12;
    {
        const EpdRect r = card(y, 104);
        label(r.x + 18, r.y + 26, "TOMORROW");
        if (pos && s.timeValid) {
            const doba_sun::Day dd = doba_sun::day(lat, lon, s.utc + 86400);
            const struct { const char* name; bool ok; time_t t; } col[4] = {
                {"light", dd.dawnOk, dd.dawn}, {"sunrise", dd.riseOk, dd.rise},
                {"sunset", dd.setOk, dd.set},  {"dark", dd.duskOk, dd.dusk}};
            const int cw = (r.width - 36) / 4;
            for (int k = 0; k < 4; ++k) {
                const int cx = r.x + 18 + k * cw;
                if (col[k].ok) hhmm(col[k].t, a, sizeof a); else snprintf(a, sizeof a, "--");
                ui::text(ui::font(ui::F_KEY), cx, r.y + 62, a, fb, EPD_DRAW_ALIGN_LEFT, INK);
                ui::text(ui::font(ui::F_TINY), cx, r.y + 86, TR(col[k].name), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
            }
            snprintf(line, sizeof line, TR("moon: %s, %d%% lit"), TR(doba_sun::moonName(s.utc)),
                     (int)(doba_sun::moonLit(s.utc) * 100 + 0.5));
            ui::text(ui::font(ui::F_TINY), r.x + r.width - 18, r.y + 26, line, fb, EPD_DRAW_ALIGN_RIGHT, INK);
        } else {
            ui::text(ui::font(ui::F_SMALL), r.x + 18, r.y + 70, TR("needs a GPS fix once"), fb, EPD_DRAW_ALIGN_LEFT, INK);
        }
        y += 104 + 10;
    }

    // --- the owner card: a line per kind of fact, empty ones left out
    if (settings::ownerSet()) {
        const EpdFont* f = ui::font(ui::F_SMALL);
        const int tw = W - 32 - 36;
        char ln[3][160];
        int n = 0;
        const char* nm = settings::ownerField(0); const char* born = settings::ownerField(1);
        const char* blood = settings::ownerField(2); const char* med = settings::ownerField(3);
        const char* ice = settings::ownerField(4);
        if (nm[0] || born[0]) snprintf(ln[n++], sizeof ln[0], "%s%s%s", nm, nm[0] && born[0] ? " \xC2\xB7 " : "", born);
        if (blood[0] || med[0]) {
            char b[80] = "";
            if (blood[0]) snprintf(b, sizeof b, TR("blood %s"), blood);
            snprintf(ln[n++], sizeof ln[0], "%s%s%s", b, b[0] && med[0] ? " \xC2\xB7 " : "", med);
        }
        if (ice[0]) snprintf(ln[n++], sizeof ln[0], TR("contact: %s"), ice);
        const int h = 44 + n * 28;
        const EpdRect r = card(y, h);
        label(r.x + 18, r.y + 26, "IF FOUND");
        for (int k = 0; k < n; ++k) {
            fitTextTo(f, ln[k], tw);
            ui::text(f, r.x + 18, r.y + 56 + k * 28, ln[k], fb, EPD_DRAW_ALIGN_LEFT, INK);
        }
        y += h + 10;
    }

    // --- the crib, black: the part to find at a glance
    {
        const int h = H - 10 - y;
        const EpdRect r = {16, y, W - 32, h};
        ui::fillRound(r, 12, INK, fb);
        char up[64];
        ui::upper(TR("IN AN EMERGENCY"), up, sizeof up);
        ui::text(ui::font(ui::F_TINY), r.x + 18, r.y + 26, up, fb, EPD_DRAW_ALIGN_LEFT, 0xB0);
        static const char* const kCrib[] = {
            "112 · SOS: ... --- ... (3 short, 3 long, 3 short)",
            "Ground to air: V help · X medical",
            "N no · Y yes · arrow = going this way",
            "Three of anything (fires, whistles, flashes) = distress",
            "Rule of three: 3 min air · 3 h cold",
            "3 days water · 3 weeks food",
        };
        int ly = r.y + 54;
        for (const char* k : kCrib) {
            if (ly > r.y + h - 8) break;
            char t[160];
            snprintf(t, sizeof t, "%s", TR(k));
            fitTextTo(ui::font(ui::F_TINY), t, r.width - 36);
            ui::text(ui::font(ui::F_TINY), r.x + 18, ly, t, fb, EPD_DRAW_ALIGN_LEFT, PAPER);
            ly += 25;
        }
    }
}

void render(Page p, const DeviceState& s, uint8_t* fb) {
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
            if (g_fieldOwner)
                renderField(kOwnerCap[g_ownerField], "Shown when Doba is off. SAVE goes to the next field.", g_nameKbd,
                            g_ownerField + 1 < settings::OWNER_FIELDS ? "NEXT" : "SAVE", fb);
            else
                renderField("CALLSIGN  (how the mesh sees you)", "Short name: first four letters", g_nameKbd, "SAVE", fb);
            break;
        case POINTS:  renderPoints(s, fb); break;
        case POINT:   renderPoint(s, fb); break;
        case POINT_NEW: renderPointNew(s, fb); break;
        case MAPOPTS: renderMapOpts(s, fb); break;
        case SOS:     renderSos(s, fb); break;
        case SUN:     renderSun(s, fb); break;
        case MORE:    renderMore(s, fb); break;
        case BATTERY: renderBattery(s, fb); break;
    }
}

bool sosActive() { return g_sosOn; }

bool tick(const DeviceState& s) {
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
        case COMMS: return (g_cv == CV_CHATS || g_cv == CV_NODES || !mesh_service::enabled()) ? "" : TR("Comms");   // the page says TR("Comms") big
        case COMPOSE: return TR("Write");
        case LIBRARY: return doba_lib::libraryTitle();
        case ARTICLE: return "";
        case SETUP: case SETUP_NAME: return TR("Settings");
        case POINTS: return g_nearby ? "" : TR("Position");
        case POINT: return TR("Point");
        case POINT_NEW: return TR("New point");
        case MAPOPTS: return g_legend ? TR("Legend") : TR("Map options");
        case SOS: return TR("SOS");
        case SUN: return TR("Sun & moon");
        case MORE: return TR("More");
        case BATTERY: return TR("Battery");
    }
    return nullptr;
}

void setPressMs(uint32_t ms) { g_pressMs = ms; }

Go tap(Page p, int x, int y, const DeviceState& s) {
    switch (p) {
        case HOME:    return tapHome(x, y);
        case COMMS:   return tapComms(x, y);
        case COMPOSE: return tapCompose(x, y);
        case LIBRARY: return doba_lib::tapSearch(g_search, x, y) ? TO_ARTICLE : STAY;
        case ARTICLE: {
            if (doba_lib::tapArticle(x, y)) return back(ARTICLE);   // CLOSE: as BACK
            double la, lo;
            char nm[24];
            if (doba_lib::takePlace(la, lo, nm, sizeof nm)) {   // a geo: link: the place on the map
                char pt[doba_points::NAME_LEN];
                snprintf(pt, sizeof pt, "%s", nm[0] ? nm : "PLACE");
                const int idx = doba_points::add(pt, la, lo);
                if (idx >= 0) { doba_points::setTarget(idx); g_mapToTarget = true; return TO_MAP; }
            }
            return STAY;
        }
        case SETUP:   return tapSetup(x, y);
        case SETUP_NAME: return tapSetupName(x, y);
        case POINTS:  return tapPoints(x, y, s);
        case POINT:   return tapPoint(x, y);
        case POINT_NEW: return tapPointNew(x, y);
        case MAPOPTS: return tapMapOpts(x, y);
        case SOS:     return tapSos(x, y, s);
        case SUN:     return STAY;
        case MORE:    return tapMore(x, y);
        case BATTERY: return STAY;
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
        case SETUP_NAME: if (g_fieldOwner) { g_fieldOwner = false; return TO_MORE; } return TO_SETUP;
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
        case BATTERY: return TO_MORE;
    }
    return TO_HOME;
}

bool g_mapToTarget = false;
void dropHomeCache() { pg::dropHomeCache(); }
uint32_t homeKey(const DeviceState& s) { return pg::homeKeyOf(s); }

}  // namespace doba_ui
