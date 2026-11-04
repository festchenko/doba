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
    {tileRect(0, 0), "MAP", TO_MAP},         {tileRect(1, 0), "POSITION", TO_POSITION},
    {tileRect(0, 1), "LIBRARY", TO_LIBRARY}, {tileRect(1, 1), "COMMS", TO_COMMS},
    {tileRect(0, 2), "TRACKS", TO_TRACKS},   {tileRect(1, 2), "ROUTES", TO_ROUTES},
    {tileRect(0, 3), "SETTINGS", TO_SETTINGS}, {tileRect(1, 3), "", TO_RECORD},
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
            case TO_POSITION:
                if (s.gpsFix || s.everHadFix) milgeo::mgrs(s.latitude, s.longitude, sub, sizeof sub);
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
            case TO_SETTINGS: snprintf(sub, sizeof sub, "mils · radio · light"); break;
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

}  // namespace

void render(Page p, const RideState& s, uint8_t* fb) {
    switch (p) {
        case HOME:    renderHome(s, fb); topo_bg::draw({0, TOP, W, H - TOP}, fb, 0); break;
        case COMMS:   renderComms(s, fb); topo_bg::draw({0, TOP, W, H - TOP}, fb, 1); break;
        case COMPOSE: renderCompose(s, fb); break;
        case LIBRARY:
            doba_lib::renderSearch(g_search, fb);
            topo_bg::draw({0, TOP, W, g_search.top - TOP}, fb, 2);   // not among the keys
            break;
        case ARTICLE: doba_lib::renderArticle(fb); break;
    }
}

Go tap(Page p, int x, int y, const RideState& s) {
    (void)s;
    switch (p) {
        case HOME:    return tapHome(x, y);
        case COMMS:   return tapComms(x, y);
        case COMPOSE: return tapCompose(x, y);
        case LIBRARY: return doba_lib::tapSearch(g_search, x, y) ? TO_ARTICLE : STAY;
        case ARTICLE: doba_lib::tapArticle(x, y); return STAY;
    }
    return STAY;
}

Go back(Page p) {
    switch (p) {
        case HOME:    return TO_MAP;
        case COMMS:   return TO_HOME;
        case COMPOSE: return TO_COMMS;
        case LIBRARY: return TO_HOME;
        case ARTICLE: return TO_LIBRARY;
    }
    return TO_HOME;
}

}  // namespace doba_ui
