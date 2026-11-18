// DOBA: the COMMS pages (chats, nodes, a chat, quick replies) and COMPOSE.
#include "doba_ui_shared.h"

namespace doba_ui {
namespace pg {

// --- COMMS: chats and nodes, Meshtastic-style ----------------------------------
//
// Three levels, like the Meshtastic app: CHATS (the public channel, private
// channels, one chat per person written to) and NODES (everyone heard: name,
// when, how far, which way); a CHAT is one conversation with QUICK replies and
// WRITE. Delivery shows as in the app: ... queued, one tick on the air, two
// ticks acknowledged, ! failed.

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
    if (ch == 0) { snprintf(out, n, TR("Everyone")); return; }
    mesh_service::ChannelInfo ci;
    if (mesh_service::channelAt(ch, ci) && ci.name[0]) snprintf(out, n, "# %s", ci.name);
    else snprintf(out, n, TR("# channel %u"), (unsigned)ch);
}
void ago(uint32_t ms, char* out, size_t n) {
    const uint32_t s = (millis() - ms) / 1000;
    if (s < 60) snprintf(out, n, TR("now"));
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
    ui::text(ui::font(ui::F_H1), 24, TOP + 44, TR("Comms"), fb, EPD_DRAW_ALIGN_LEFT, INK);
    char sub[96];
    snprintf(sub, sizeof sub, TR("Mesh radio \xC2\xB7 %s \xC2\xB7 %d heard"), mesh_service::presetName(), mesh_service::nodeCount());
    ui::text(ui::font(ui::F_SMALL), 24, TOP + 78, sub, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
}
void tabs(uint8_t* fb) {
    commsHead(fb);
    char nodes[24];
    snprintf(nodes, sizeof nodes, TR("Nodes %d"), mesh_service::nodeCount());
    const char* const l[] = {TR("Chats"), nodes};
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
    snprintf(p, sizeof p, TR("%d of %d"), page + 1, pages);
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
                if (m.outgoing) snprintf(who, sizeof who, TR("you"));
                else nameOf(m.from, who, sizeof who);
                snprintf(pv, sizeof pv, c.dm && !m.outgoing ? "%s" : "%s: %s", c.dm && !m.outgoing ? m.text : who, m.text);
                ago(m.ms, when, sizeof when);
            }
        } else {
            snprintf(pv, sizeof pv, c.ch == 0 ? TR("everyone in range · %s") : TR("private channel"), mesh_service::presetName());
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
        ui::text(ui::font(ui::F_SMALL), 24, BAR_Y + 34, TR("Tap a chat to read and write"), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
}

void renderNodes(uint8_t* fb) {
    tabs(fb);
    const int n = mesh_service::nodeCount();
    const int per = rowsPerPage(), pages = (n + per - 1) / per;
    if (g_listPage >= pages) g_listPage = pages ? pages - 1 : 0;
    const EpdFont* big = ui::font(ui::F_LABEL);
    const EpdFont* small = ui::font(ui::F_SMALL);
    if (n == 0) {
        ui::text(small, W / 2, 420, TR("nobody heard yet"), fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::text(ui::font(ui::F_TINY), W / 2, 456, TR("nodes show here as their radios are heard"), fb, EPD_DRAW_ALIGN_CENTER, INK);
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
        if (nd.hops == 0) snprintf(meta, sizeof meta, TR("%s%sdirect · %d dB"), where, where[0] ? " · " : "", nd.snr);
        else if (nd.hops == mesh_service::HOPS_UNKNOWN) snprintf(meta, sizeof meta, "%s", where[0] ? where : "relayed");
        else snprintf(meta, sizeof meta, TRN(nd.hops, "%s%s%u hop", "%s%s%u hops"), where, where[0] ? " · " : "", nd.hops);
        fit(small, meta, right - tx);
        ui::text(small, tx, y + 78, meta, fb, EPD_DRAW_ALIGN_LEFT, INK);
        epd_fill_rect({tx, y + ROW_H - 1, W - tx - 12, 1}, INK, fb);
    }
    pager(g_listPage, pages, fb);
    if (pages <= 1 && n)
        ui::text(ui::font(ui::F_TINY), W / 2, BAR_Y + 44, TR("tap a node to write to it"), fb, EPD_DRAW_ALIGN_CENTER, INK);
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
                snprintf(sub, sizeof sub, TR("%s%sheard %s · private"), where, where[0] ? " · " : "", when);
                break;
            }
        }
    } else {
        snprintf(sub, sizeof sub, g_chatCh == 0 ? TR("everyone in range can read this") : TR("only those with the key"));
    }
    // A person with a known position: ON MAP marks them and points the way.
    if (onMap) button(kOnMap, "On map", false, fb);
    fit(ui::font(ui::F_TITLE), t, onMap ? kOnMap.x - 40 : W - 48);
    fit(small, sub, onMap ? kOnMap.x - 40 : W - 48);
    ui::text(ui::font(ui::F_TITLE), 24, TOP + 38, t, fb, EPD_DRAW_ALIGN_LEFT, INK);
    ui::capFirst(sub);
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
        ui::text(ui::font(ui::F_LABEL), W / 2, 420, TR("No messages yet"), fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::text(small, W / 2, 456, TR("Quick sends a ready phrase, Write types one"), fb, EPD_DRAW_ALIGN_CENTER, ui::GREY);
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
    ui::text(ui::font(ui::F_SMALL), 24, TOP + 36, TR("Send a ready phrase"), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    snprintf(h, sizeof h, TR("To %s"), t);
    fit(ui::font(ui::F_H1), h, W - 48);
    ui::text(ui::font(ui::F_H1), 24, TOP + 84, h, fb, EPD_DRAW_ALIGN_LEFT, INK);
    // the phrases as soft grey keys: one tap sends
    for (int i = 0; i < N_QUICK; ++i) {
        const EpdRect r = quickRect(i);
        ui::fillRound(r, 10, 0xE6, fb);
        ui::text(ui::font(ui::F_LABEL), r.x + r.width / 2, r.y + r.height / 2 + 9, TR(kQuickText[i]), fb, EPD_DRAW_ALIGN_CENTER, INK);
    }
    button(kSendPos, g_myPos ? "Send my position (MGRS)" : "My position: no fix yet", true, fb);
    ui::text(ui::font(ui::F_SMALL), 24, H - 34, TR("One tap sends it. Back returns to the chat."), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
}

void sendToChat(const char* text) {
    mesh_service::queueText(g_chatDm ? g_chatDm : mesh::BROADCAST_ADDR, text, g_chatDm ? 0 : g_chatCh);
    g_chatScroll = 0;
    Serial.printf("[comms] send to %s%lx: %s\n", g_chatDm ? "!" : "ch", (unsigned long)(g_chatDm ? g_chatDm : g_chatCh), text);
}

void renderComms(const DeviceState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    g_myPos = s.gpsFix || s.everHadFix;
    g_myLat = s.latitude;
    g_myLon = s.longitude;
    const EpdFont* small = ui::font(ui::F_SMALL);
    const EpdFont* tiny = ui::font(ui::F_TINY);
    if (!mesh_service::enabled()) {
        ui::text(ui::font(ui::F_H1), 24, TOP + 44, TR("Comms"), fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (stealth::active()) {
            ui::text(small, 24, TOP + 78, TR("Stealth: nothing in, nothing out"), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
            ui::text(small, 24, TOP + 150, stealth::listen() ? TR("To listen, turn the radio on in Settings.")
                                                             : TR("Stealth: listen (in Settings) receives."), fb, EPD_DRAW_ALIGN_LEFT, 0x30);
            ui::text(small, 24, TOP + 182, TR("End stealth on Home: hold the switch 5 s."), fb, EPD_DRAW_ALIGN_LEFT, 0x30);
            return;
        }
        ui::text(small, 24, TOP + 78, TR("The radio is off"), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        ui::text(small, 24, TOP + 150, TR("Messages and nodes need the radio."), fb, EPD_DRAW_ALIGN_LEFT, 0x30);
        ui::text(small, 24, TOP + 182, TR("It uses more battery while on."), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
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
        ui::text(small, W / 2, BAR_Y - 23, TR("Stealth: listening, nothing is sent"), fb, EPD_DRAW_ALIGN_CENTER, PAPER);
    }
}

CommsView g_chatFrom = CV_CHATS;   // the list a chat was opened from: Back returns there

void openChat(uint8_t ch, uint32_t dm) {
    if (g_cv == CV_CHATS || g_cv == CV_NODES) g_chatFrom = g_cv;
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
                        g_mapToTarget = true;
                        return TO_MAP;
                    }
                }
                return STAY;
            }
            if (hit(kWrite, x, y)) { g_compose.clear(); if (i18n::lang() == i18n::UK) g_compose.layout = g_compose.lastLetters = kbd::UA; return TO_COMPOSE; }
            if (hit(kQuick, x, y)) { g_cv = CV_QUICK; return STAY; }
            if (hit(kOlder, x, y)) { g_chatScroll += 3; return STAY; }
            if (hit(kNewer, x, y)) { g_chatScroll = g_chatScroll > 3 ? g_chatScroll - 3 : 0; return STAY; }
            return STAY;
        case CV_QUICK:
            for (int i = 0; i < N_QUICK; ++i)
                if (hit(quickRect(i), x, y)) { sendToChat(TR(kQuickText[i])); g_cv = CV_CHAT; return STAY; }
            if (hit(kSendPos, x, y) && g_myPos) {
                char m[64], g[32];
                milgeo::mgrs(g_myLat, g_myLon, g, sizeof g);
                snprintf(m, sizeof m, TR("I am at %s (%.5f, %.5f)"), g, g_myLat, g_myLon);
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
    // (It went to Chats either way: a chat opened from Nodes came back to Chats.)
    if (g_cv == CV_CHAT) { g_cv = g_chatFrom; return true; }
    return false;
}

// --- COMPOSE -----------------------------------------------------------------

void renderCompose(const DeviceState& s, uint8_t* fb) {
    (void)s;
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    g_compose.top = H - g_compose.height();
    g_compose.enterLabel = "SEND";
    char t[48], hdr[64];
    chatTitle(g_chatCh, g_chatDm, t, sizeof t);
    snprintf(hdr, sizeof hdr, TR("TO: %s"), t);
    fit(ui::font(ui::F_SMALL), hdr, W - 140);
    ui::text(ui::font(ui::F_SMALL), 24, TOP + 34, hdr, fb, EPD_DRAW_ALIGN_LEFT, INK);
    char cnt[16];
    snprintf(cnt, sizeof cnt, "%u/%u", (unsigned)strlen(g_compose.text), (unsigned)mesh::MAX_TEXT_LEN);
    ui::text(ui::font(ui::F_TINY), W - 24, TOP + 34, cnt, fb, EPD_DRAW_ALIGN_RIGHT, INK);
    const EpdRect box = {24, TOP + 48, W - 48, g_compose.top - TOP - 64};   // the page margin
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

}  // namespace pg
}  // namespace doba_ui
