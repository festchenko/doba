#include "doba_i18n.h"
#include "ui_render.h"
#include "stealth.h"
#include "doba_topo_bg.h"
#include "milgeo.h"    // DOBA
#include "milspec.h"   // DOBA
#include "epd_compat.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "config.h"
#include "ride_state.h"
#include "speed_source.h"
// Faces at the design system's ACTUAL sizes. The drop's specimens are drawn 1:1
// with the panel, so its CSS px are device px — and the previously compiled
// faces were 2.1-2.4x those sizes (Impact_40 rendered a 68 px cap where the
// drop's 40 px Impact wants 29). Everything was therefore roughly double the
// intended scale, which is why captions collided and rows had to be truncated.
//
//   Arial_L   label      = drop's arialbold_14   (cap 11)
//   Arial_B   body       = drop's arialbold_20   (cap 15)
//   Impact_S  clock      = drop's impact_28      (cap 20)
//   Impact_T  row/button = drop's impact_40      (cap 29)
//   Impact_V  cell value = drop's impact_64      (cap 46)
//   Impact_H  hero       = drop's impact_128     (cap 95)
#include "vfont_faces.h"



namespace {
// Defined further down with the other formatters; declared here so the status
// bar and the CLOCK dashboard field share one implementation.
void formatClock(char* out, size_t len, const RideState& s);
}

namespace ui {

void text(const EpdFont* font, int x, int y, const char* str, uint8_t* fb,
          EpdFontFlags align, uint8_t color) {
#ifdef OTP_SIM
    // DOBA: in Ukrainian, the simulator lists every drawn string that still
    // has English words in it (the i18n check; content shows up here too).
    if (str && i18n::lang() == i18n::UK) {
        int run = 0;
        bool hit = false;
        for (const char* c = str; *c && !hit; ++c) {
            run = ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')) ? run + 1 : 0;
            hit = run >= 3;
        }
        if (hit) {
            static uint32_t seen[512];
            uint32_t h = 2166136261u;
            for (const char* c = str; *c; ++c) h = (h ^ (uint8_t)*c) * 16777619u;
            bool dup = false;
            for (uint32_t v : seen) dup |= v == h;
            static int k = 0;
            if (!dup) { seen[k++ % 512] = h; Serial.printf("[i18n] drawn '%s'\n", str); }
        }
    }
#endif
    EpdFontProperties props = epd_font_properties_default();
    props.flags = align;
    props.fg_color = color >> 4;
    epd_write_string(font, str, &x, &y, fb, &props);
}

int textWidth(const EpdFont* font, const char* str) {
    EpdFontProperties props = epd_font_properties_default();
    EpdRect r = epd_get_string_rect(font, str, 0, 0, 0, &props);
    return r.width;
}

// Tracked-out label: draws characters individually with a few extra
// pixels of letterspacing, matching the design. UTF-8 aware.
// Rendered width of a tracked label. Same arithmetic as label() below, exposed
// so callers can pick a font that FITS before drawing — tracking adds 3 px per
// character, so a plain textWidth() underestimates by enough to overflow a
// half-width cell on a long caption like "MOVING TIME".
int labelWidth(const EpdFont* font, const char* str) {
    if (!font) font = &Arial_L;
    constexpr int TRACK = 3;
    char ch[8];
    int total = -TRACK;
    for (const char* p = str; *p;) {
        int len = 1;
        if (*p & 0x80) { while ((p[len] & 0xC0) == 0x80) len++; }
        memcpy(ch, p, len);
        ch[len] = 0;
        p += len;
        total += textWidth(font, ch) + TRACK;
    }
    return total < 0 ? 0 : total;
}

void label(int cx, int y, const char* str, uint8_t* fb, uint8_t color,
           const EpdFont* font) {
    if (!font) font = &Arial_L;
    constexpr int TRACK = 3;
    char ch[8];
    auto nextChar = [&](const char*& p) {
        int len = 1;
        if (*p & 0x80) {
            while ((p[len] & 0xC0) == 0x80) len++;
        }
        memcpy(ch, p, len);
        ch[len] = 0;
        p += len;
    };

    int total = -TRACK;
    for (const char* p = str; *p;) {
        nextChar(p);
        total += textWidth(font, ch) + TRACK;
    }

    int x = cx - total / 2;
    for (const char* p = str; *p;) {
        nextChar(p);
        text(font, x, y, ch, fb, EPD_DRAW_ALIGN_LEFT, color);
        x += textWidth(font, ch) + TRACK;
    }
}

void valueWithUnit(const EpdFont* valueFont, int x0, int x1, int baselineY,
                   const char* value, const char* unit, uint8_t* fb,
                   uint8_t color) {
    int vw = textWidth(valueFont, value);
    int uw = unit && unit[0] ? textWidth(&Arial_L, unit) + 10 : 0;
    int startX = x0 + ((x1 - x0) - (vw + uw)) / 2;
    if (startX < x0) startX = x0;   // never spill past the left bound / screen edge
    text(valueFont, startX, baselineY, value, fb, EPD_DRAW_ALIGN_LEFT, color);
    if (uw) {
        text(&Arial_L, startX + vw + 10, baselineY, unit, fb,
             EPD_DRAW_ALIGN_LEFT, color);
    }
}

namespace {

}  // namespace

// DOBA: the tactical status band — black, white monospace, every item a word
// rather than a glyph: "14:25  SAT 10  REC ... TITLE ... BAT 87%". The
// OpenTrailPaper band (clock, signal dots, sensor chips, battery icon) is kept
// below as statusBarOtp() for reference.
void statusBarOtp(const RideState& s, uint8_t* fb, const char* title);

// DOBA: a crossed-out eye left of the clock while stealth is on.
static int stealthMark(int rightEdge, int cy, uint8_t fg, uint8_t* fb) {
    if (!stealth::active()) return rightEdge;
    const int cx = rightEdge - 18;
    epd_draw_circle(cx, cy, 11, fg, fb);
    epd_draw_circle(cx, cy, 10, fg, fb);
    epd_fill_circle(cx, cy, 4, fg, fb);
    for (int t = -1; t <= 1; ++t) epd_draw_line(cx - 15, cy + 13 + t, cx + 15, cy - 13 + t, fg, fb);
    return rightEdge - 40;
}

// DOBA: the light band from the design canvas — no black bar. A sub-page:
// "‹ Back" on the left (the tap target, x < BACK_W), its name in the middle,
// clock and battery in grey on the right.
void statusBarBack(const RideState& s, uint8_t* fb, const char* title) {
    const int W = epd_rotated_display_width();
    const int H = STATUS_H;
    const uint8_t FG = 0x00, DIM = GREY;
    epd_fill_rect({0, 0, W, H}, 0xFF, fb);
    const int base = 40;
    const EpdFont* f = ui::font(ui::F_SMALL);
    const EpdFont* fb2 = ui::font(ui::F_LABEL);
    // a thin chevron, then the word
    const int cx = 22, cy = base - 8;
    for (int t = 0; t < 2; ++t) {
        epd_draw_line(cx + 9 + t, cy - 10, cx + t, cy, FG, fb);
        epd_draw_line(cx + t, cy, cx + 9 + t, cy + 10, FG, fb);
    }
    text(fb2, cx + 18, base, TR("Back"), fb, EPD_DRAW_ALIGN_LEFT, FG);
    char clock[10];
    formatClock(clock, sizeof(clock), s);
    char right[32];
    if (s.batteryPercent == 0) snprintf(right, sizeof right, "%s", clock);
    else snprintf(right, sizeof right, "%s · %u%%%s", clock, s.batteryPercent, s.charging ? "+" : "");
    text(f, W - 18, base, right, fb, EPD_DRAW_ALIGN_RIGHT, DIM);
    const int rx = W - 18 - textWidth(f, right) - 10;
    if (title && title[0]) {
        // the name, between the back word and the clock, cut if it must be
        char t[48];
        sentence(title, t, sizeof t);
        const int lx = BACK_W + 16, room = rx - lx - 10;
        while (textWidth(fb2, t) > room && strlen(t) > 3) {
            size_t n = strlen(t) - 4;
            while (n > 0 && ((uint8_t)t[n] & 0xC0) == 0x80) --n;
            memcpy(t + n, "...", 4);
        }
        text(fb2, (lx + rx) / 2, base, t, fb, EPD_DRAW_ALIGN_CENTER, FG);
    }
    stealthMark(rx, cy, FG, fb);
}

void statusBar(const RideState& s, uint8_t* fb, const char* title) {
    const int W = epd_rotated_display_width();
    const int H = STATUS_H;
    const uint8_t FG = 0x00, DIM = GREY;
    epd_fill_rect({0, 0, W, H}, 0xFF, fb);
    const int base = 42;
    char clock[10];
    formatClock(clock, sizeof(clock), s);
    const EpdFont* cf = vf_face_digit(VF_MONO, 18);
    text(cf, 26, base, clock, fb, EPD_DRAW_ALIGN_LEFT, FG);
    const EpdFont* f = ui::font(ui::F_SMALL);
    // right: what matters about the device, in grey, worst news first
    char right[64];
    const char* flag = nullptr;
    if (s.sdCard == SD_CARD_UNREADABLE) flag = TR("SD error");
    else if (s.sdCard != SD_CARD_OK) flag = TR("No SD");
    char gps[32];
    if (s.gpsFix) snprintf(gps, sizeof gps, TR("GPS %u"), (unsigned)s.satellites);
    else snprintf(gps, sizeof gps, "%s", TR("No fix"));
    char bat[12];
    if (s.batteryPercent == 0) snprintf(bat, sizeof bat, "--%%");
    else snprintf(bat, sizeof bat, "%u%%%s", s.batteryPercent, s.charging ? "+" : "");
    if (title && title[0]) snprintf(right, sizeof right, "%s · %s", title, bat);
    else if (flag) snprintf(right, sizeof right, "%s · %s · %s", flag, gps, bat);
    else snprintf(right, sizeof right, "%s · %s", gps, bat);
    text(f, W - 26, base, right, fb, EPD_DRAW_ALIGN_RIGHT, flag ? FG : DIM);
    stealthMark(W - 26 - textWidth(f, right) - 10, base - 8, FG, fb);
}

// --- Screentones ----------------------------------------------------------
// Tone, not grey. A mid grey inside a 1 Hz region ghosts (the driver's model
// loses lightened pixels), so anything that must read as "less than black"
// while it changes is built from 1-bit pixels instead.
void fillTone(const EpdRect& r, Tone t, uint8_t* fb) {
    for (int y = r.y; y < r.y + r.height; ++y) {
        for (int x = r.x; x < r.x + r.width; ++x) {
            bool on = false;
            switch (t) {
                // 25%: one pixel in every 2x2. This was one in every 4x4 —
                // 6.25%, four times lighter than the name says, which is why a
                // greyed-out cell barely read as greyed out at all.
                case TONE_25: on = ((x & 1) == 0) && ((y & 1) == 0); break;
                case TONE_30: on = ((x + y) % 3) == 0; break;         // 45 deg
                case TONE_50: on = ((x >> 1) + (y >> 1)) % 2 == 0; break;
                case TONE_33: on = (y % 3) == 0; break;               // scanline
            }
            if (on) epd_fill_rect({x, y, 1, 1}, INK, fb);
        }
    }
}

// --- CELL -----------------------------------------------------------------
// Specimen: 240 x 150, border 2, pad 16. Label top-LEFT (arialbold_14, tracked),
// value left-aligned 10 px below it in impact, unit baseline-aligned 6 px along.
// Nothing here is centred — the drop aligns the whole cell to its left padding.
// Fine-grained on purpose: the rider asked for values as large as the box
// allows, and a coarse ladder lands well under the ceiling (46 -> 69 -> 95 left
// a 60 px gap where a 66 px face would have fitted).
//
// STRICTLY DESCENDING by digit height, and it has to stay that way — the two
// things that read this ladder both assume it. valueFontIndex() returns the
// FIRST face that fits, so an out-of-order entry hands back a smaller face than
// the cell could have taken; worse, the dashboard equalises a size class by
// keeping the LARGEST INDEX any of its cells needed, which is only "the
// smallest face" if the order holds — otherwise a class renders at a face too
// big for one of its cells and the value overflows the box. Impact_H (95) sat
// ahead of Impact_C (120), so both happened at once.
// Heights: 158, 120, 95, 81, 69, 58, 46, 30, 15.
const EpdFont* const kValueLadder[VALUE_LADDER_N] = {
    &Impact_XL, &Impact_C, &Impact_H, &Impact_B, &Impact_M,
    &Impact_A,  &Impact_V, &Impact_T, &Arial_B};

// Index of the largest ladder face that fits; 3 (the smallest) if none do.
int valueFontIndex(const EpdFont* const* ladder, const char* value, int availW,
                   int availH, int unitW) {
    for (int i = 0; i < VALUE_LADDER_N; ++i) {
        if (epdc_digit_height(ladder[i]) <= availH &&
            textWidth(ladder[i], value) + unitW <= availW)
            return i;
    }
    return VALUE_LADDER_N - 1;
}

const EpdFont* const kLabelLadder[LABEL_LADDER_N] = {&ArialBold_20, &Arial_B,
                                                     &Arial_L};

void cell(const EpdRect& r, const char* labelStr, const char* value,
          const char* unit, uint8_t* fb, bool stale, const EpdFont* forced,
          const EpdFont* forcedLabel) {
    for (int b = 0; b < RULE; ++b)
        epd_draw_rect({r.x + b, r.y + b, r.width - 2 * b, r.height - 2 * b}, INK, fb);

    // NO DATA: scanline tone behind the VALUE only. Running it across the whole
    // cell (as a 10%-opacity wash does on screen) buried the label here — this
    // is 1-bit, so the tone is as black as the type sitting on it.
    // A field whose sensor is not connected is GREYED OUT: the system's dot tone
    // across the whole cell, drawn first so the caption and the no-data rule sit
    // on top of it in ink. The dot (25%, 4 px pitch) rather than the scanline —
    // the scanline is dense enough to bury a caption at this size, and the point
    // is to read as inactive, not as damaged.
    if (stale) {
        const EpdRect tone = {r.x + RULE, r.y + RULE, r.width - 2 * RULE, r.height - 2 * RULE};
        fillTone(tone, TONE_25, fb);
    }
    const bool toneValue = false;

    const int lx = r.x + CELL_PAD;
    const int ly = r.y + CELL_PAD + Arial_B.ascender + 6;
    // Labels step down too. "HEART RATE" at 20 pt is wider than a 240 px cell,
    // and a caption that runs into its neighbour is worse than a smaller one.
    const int labelAvail = r.width - 2 * CELL_PAD;
    const EpdFont* lf = forcedLabel;
    if (!lf) {
        lf = kLabelLadder[LABEL_LADDER_N - 1];
        for (int i = 0; i < LABEL_LADDER_N; ++i)
            if (labelWidth(kLabelLadder[i], labelStr) <= labelAvail) {
                lf = kLabelLadder[i];
                break;
            }
    }
    const int lw = labelWidth(lf, labelStr);
    label(lx + lw / 2, ly, labelStr, fb, INK, lf);

    // Value box: everything below the label, less the bottom padding.
    const int top = ly + 10;
    const int bot = r.y + r.height - CELL_PAD;
    const int avail = r.width - 2 * CELL_PAD;
    const int unitW = (unit && unit[0]) ? textWidth(&Arial_B, unit) + 4 : 0;

    // Impact_M (the sheet face) is the top step: on real glass the drop's cell
    // size read small, and a tall cell has the room for it.
    const EpdFont* vf = forced;
    if (!vf) {
        const int idx = valueFontIndex(kValueLadder, value, avail, bot - top, unitW);
        vf = kValueLadder[idx];
    }
    int vh = epdc_digit_height(vf);
    // CENTRE the value in the space below the label. The specimen hangs it off
    // the bottom padding, which is right in a 150 px cell where the number all
    // but fills the box — but wrong the moment a cell is tall (a field left
    // alone after its neighbours' sensors dropped out inherits the whole
    // panel), where it stranded the value at the very bottom under a void.
    const int baseline = top + ((bot - top) + vh) / 2;

    // The value and its unit are ONE object, centred in the cell as a pair —
    // centring the number alone would push it off-axis by half the unit's
    // width. The label stays pinned top-left; only the figure is centred.
    const int vw = textWidth(vf, value);
    const int startX = r.x + (r.width - vw - unitW) / 2;
    if (toneValue) {
        // NO DATA is drawn, not typed. A hyphen's ink is a short thin bar
        // whatever face it is set in, so "--" at the same font as its
        // neighbours still rendered a fraction of their size and read as a
        // typographic bug. A rule scaled to the digit height sits at the weight
        // the number would have had, which is what makes the cell read as
        // "no source" rather than "broken".
        const int barW = vh * 5 / 8, barH = vh / 6 < 4 ? 4 : vh / 6;
        const int gap = barW / 3;
        const int totalW = barW * 2 + gap;
        const int bx = r.x + (r.width - totalW - unitW) / 2;
        const int by = baseline - vh / 2 - barH / 2;
        epd_fill_rect({bx, by, barW, barH}, INK, fb);
        epd_fill_rect({bx + barW + gap, by, barW, barH}, INK, fb);
        if (unitW)
            text(&Arial_B, bx + totalW + 4, baseline, unit, fb,
                 EPD_DRAW_ALIGN_LEFT, INK);
    } else {
        text(vf, startX, baseline, value, fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (unitW)
            text(&Arial_B, startX + vw + 4, baseline, unit, fb,
                 EPD_DRAW_ALIGN_LEFT, INK);
    }
    (void)vh;
}

}  // namespace ui

namespace {

// Impact carries NO lowercase glyphs, so any caller-supplied string set in it
// silently loses every lowercase character ("Turn left onto Valencia" drew as
// "T V S"). Uppercase before drawing; this has bitten list titles, sheet heroes
// and the nav banner in turn.
void upperCopy(char* out, size_t len, const char* in) {
    size_t n = 0;
    for (; in[n] && n < len - 1; ++n)
        out[n] = (in[n] >= 'a' && in[n] <= 'z') ? (char)(in[n] - 32) : in[n];
    out[n] = 0;
}

// Local time of day, 24 h or 12 h per the rider's setting. Shared by the status
// bar and the CLOCK dashboard field so the two can never disagree about the
// format — they are inches apart on the same screen.
void formatClock(char* out, size_t len, const RideState& s) {
    snprintf(out, len, "--:--");
    if (!s.timeValid) return;
    time_t local = s.utc + (time_t)s.tzMin * 60;
    struct tm tmv;
    gmtime_r(&local, &tmv);
    if (s.clock24h) {
        snprintf(out, len, "%d:%02d", tmv.tm_hour, tmv.tm_min);
    } else {
        int h = tmv.tm_hour % 12;
        if (h == 0) h = 12;
        snprintf(out, len, "%d:%02d%c", h, tmv.tm_min, tmv.tm_hour < 12 ? 'a' : 'p');
    }
}

}  // namespace

// Defined below with the list helpers; declared here so the sheet can clamp
// its body lines to the content column.
static void fitText(const EpdFont* font, const char* str, int maxW,
                    char* out, size_t outSize);

// Primary buttons are solid, secondary are outlined; one primary per screen.
static void sheetButton(const EpdRect& r, const char* text, bool primary,
                        uint8_t* fb) {
    if (primary) {
        epd_fill_rect(r, ui::INK, fb);
    } else {
        for (int e = 0; e < 3; ++e)   // specimen: secondary border is 3 px
            epd_draw_rect({r.x + e, r.y + e, r.width - 2 * e, r.height - 2 * e},
                          ui::INK, fb);
    }
    // Specimen sets button text in impact_40; step down only if it will not fit.
    const EpdFont* f = ui::textWidth(&Impact_T, text) <= r.width - 16
                           ? &Impact_T : &Impact_T;
    ui::text(f, r.x + r.width / 2, r.y + r.height / 2 + epdc_digit_height(f) / 2,
             text, fb, EPD_DRAW_ALIGN_CENTER, primary ? ui::PAPER : ui::INK);
}

void ui_render_update_overlay(const char* phase, int pct, uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();

    // Centered modal card.
    const int bw = W - 56, bh = 340;
    const int bx = (W - bw) / 2, by = (H - bh) / 2;
    const int innerW = bw - 16;   // text must stay inside this (with margin)
    epd_fill_rect({bx, by, bw, bh}, 0xFF, fb);
    for (int i = 0; i < 4; ++i) epd_draw_rect({bx + i, by + i, bw - 2 * i, bh - 2 * i}, 0x00, fb);

    // Pick the largest font that keeps a centered string inside the card.
    auto fitFont = [&](const char* s) -> const EpdFont* {
        return ui::textWidth(&Arial_B, s) <= innerW ? &Arial_B : &Arial_L;
    };

    // Inverted title band
    epd_fill_rect({bx, by, bw, 76}, 0x00, fb);
    ui::text(fitFont("UPDATING FIRMWARE"), W / 2, by + 50, "UPDATING FIRMWARE", fb,
             EPD_DRAW_ALIGN_CENTER, 0xFF);

    char sub[48];
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    snprintf(sub, sizeof(sub), "%s  %d%%", phase, pct);
    ui::text(fitFont(sub), W / 2, by + 138, sub, fb, EPD_DRAW_ALIGN_CENTER, 0x00);

    // Progress bar
    const int pbx = bx + 40, pby = by + 170, pbw = bw - 80, pbh = 44;
    epd_draw_rect({pbx, pby, pbw, pbh}, 0x00, fb);
    epd_draw_rect({pbx + 1, pby + 1, pbw - 2, pbh - 2}, 0x00, fb);
    int fillw = (pbw - 8) * pct / 100;
    if (fillw > 0) epd_fill_rect({pbx + 4, pby + 4, fillw, pbh - 8}, 0x00, fb);

    ui::text(&Arial_L, W / 2, by + 270, "Keep the app open and the", fb,
             EPD_DRAW_ALIGN_CENTER, 0x00);
    ui::text(&Arial_L, W / 2, by + 300, "device nearby until it finishes.", fb,
             EPD_DRAW_ALIGN_CENTER, 0x00);
}

// Copy `str` into `out`, truncating with a trailing ".." if it would exceed
// `maxW` px in `font`. UTF-8 safe (never cuts mid-character). Use for any
// variable-length text (sensor/route names, turn instructions) so it can never
// run past its bounds.
static void fitText(const EpdFont* font, const char* str, int maxW,
                    char* out, size_t outSize) {
    if (ui::textWidth(font, str) <= maxW || outSize < 4) {
        snprintf(out, outSize, "%s", str);
        return;
    }
    size_t n = strlen(str);
    while (n > 1) {
        n--;
        while (n > 0 && ((unsigned char)str[n] & 0xC0) == 0x80) n--;  // UTF-8 boundary
        snprintf(out, outSize, "%.*s..", (int)n, str);
        if (ui::textWidth(font, out) <= maxW) return;
    }
    snprintf(out, outSize, "..");
}

namespace {
}  // namespace

// iOS-style pill switch: black filled pill with a white knob on the right
// when on; white outlined pill with a black knob on the left when off.
// SWITCH — drawn 120 x 52, no radii (the system forbids them, and the driver
// has no antialiasing to make a curve read as one anyway). State is the whole
// rectangle inverted, which is the one state cue a DU pass renders cleanly.
// The hit rect is 120 x 88; only the drawn shape is this size.
// SWITCH — 120 x 52, border 2, split into two halves; the ACTIVE half is filled
// and carries its word in reverse. Not a single filled rectangle: the specimen
// shows position, so "off" is still a legible state rather than an empty box.
void ui_settings_toggle(int x, int y, int w, int h, bool on, uint8_t* fb,
                        const char* onText, const char* offText) {
    for (int b = 0; b < ui::RULE; ++b)
        epd_draw_rect({x + b, y + b, w - 2 * b, h - 2 * b}, ui::INK, fb);
    const int half = w / 2;
    const int fx = on ? x : x + half;      // ON fills the left half, OFF the right
    epd_fill_rect({fx + ui::RULE, y + ui::RULE, half - ui::RULE,
                   h - 2 * ui::RULE}, ui::INK, fb);
    // Only the filled half is labelled — the empty half is left blank, which is
    // what makes the switch read as a position rather than two competing words.
    ui::label(fx + half / 2, y + h / 2 + 6, on ? onText : offText, fb, ui::PAPER);
}

void ui_render_gps_debug(const GpsDebugView& g, uint8_t* fb) {
    const int W = epd_rotated_display_width();

    // No rule here: kMenuRowTop sits on the status band, whose own 3 px rule
    // is already the separator. Drawing one too made two lines with a gap.

    struct Line {
        const char* label;
        char value[40];
    } lines[16];
    int n = 0;
#define DBG_LINE(lbl, ...)                                            \
    do {                                                              \
        lines[n].label = lbl;                                         \
        snprintf(lines[n].value, sizeof(lines[n].value), __VA_ARGS__); \
        n++;                                                          \
    } while (0)

    DBG_LINE("MODULE", "%s", g.moduleDetected ? g.module : "NOT DETECTED");
    DBG_LINE("NMEA BYTES", "%lu", (unsigned long)g.chars);
    DBG_LINE("CHECKSUM", "%lu / %lu", (unsigned long)g.passedCksum,
             (unsigned long)g.failedCksum);
    DBG_LINE("WITH FIX", "%lu", (unsigned long)g.withFix);
    DBG_LINE("SATS IN VIEW", "%d", g.satsInView);
    DBG_LINE("SATS IN USE", "%d", g.satsInUse);
    if (g.bestSnr > 0) DBG_LINE("BEST SIGNAL", "%d dB-Hz", g.bestSnr);
    else DBG_LINE("BEST SIGNAL", "--");
    if (g.hdop > 0) DBG_LINE("HDOP", "%.1f", g.hdop);
    else DBG_LINE("HDOP", "--");
    if (g.locValid) {
        DBG_LINE("FIX", "yes · age %lu ms", (unsigned long)g.locAgeMs);
        DBG_LINE("LAT / LON", "%.5f  %.5f", g.lat, g.lon);
        DBG_LINE("ALT / SPEED", "%.0f %s · %.1f %s",
                 units::elev(g.altM, g.useMiles), units::elevLabel(g.useMiles),
                 units::speed(g.speedKmh, g.useMiles), units::speedLabel(g.useMiles));
    } else {
        DBG_LINE("FIX", "NO FIX");
        DBG_LINE("LAT / LON", "--");
        DBG_LINE("ALT / SPEED", "--");
    }
    if (g.hour >= 0) DBG_LINE("UTC TIME", "%02d:%02d:%02d", g.hour, g.minute,
                              g.second);
    else DBG_LINE("UTC TIME", "--");
#undef DBG_LINE

    // This page repaints every second via the fast 1-bit DU refresh, which
    // snaps any gray to white — so every mark here must be pure black.
    // Spacing is derived so all rows fit between the header and the bottom.
    // Label left, value right, on the content column — the same left/right
    // cluster the status bar uses. Tracked uppercase labels, values in the body
    // face; rules inset to 492 like every other row on the device.
    const int y0 = ui::STATUS_H + ui::STEP;
    const int step = n > 0 ? (960 - ui::MARGIN - ui::STEP - y0) / n : 40;
    const int lx = ui::CONTENT_X + ui::CELL_PAD;
    const int rx = ui::CONTENT_X + ui::CONTENT_W - ui::CELL_PAD;
    for (int i = 0; i < n; ++i) {
        const int y = y0 + i * step + step - ui::STEP;
        // Fit the label to whatever the value leaves — a long label and a long
        // value previously overlapped in the middle of the row.
        const int room = rx - ui::textWidth(&Arial_B, lines[i].value)
                         - ui::STEP - lx;
        char lab[32];
        fitText(&Arial_L, lines[i].label, room, lab, sizeof(lab));
        ui::label(lx + ui::labelWidth(&Arial_L, lab) / 2, y, lab, fb);
        ui::text(&Arial_B, rx, y, lines[i].value, fb, EPD_DRAW_ALIGN_RIGHT,
                 ui::INK);
        if (i > 0) epd_fill_rect({0, y0 + i * step, W, 1}, ui::INK, fb);
    }
}

// Bottom sheet: ~360 px tall, drawn over the live screen behind it.
// SHEET geometry, shared with the end-of-ride modal: a 520 px panel anchored to
// the bottom with a 6 px top rule and 24 px side padding. The buttons sit on the
// same rows the summary uses, so all three modals are the same object.
const EpdRect kPowerSheet = {0, 440, 540, 520};
const EpdRect kPowerShutdown = {24, 700, 492, 96};
const EpdRect kPowerCancel = {24, 816, 492, 96};

// Draws the shared modal shell: scrim over the live screen, bottom panel, rule.
// The end-of-ride sheet, the power dialog and the navigation prompt are ONE
// component with different words in it.
// `scrim` tones what is behind the sheet, to push it back and say the modal has
// the input. Pass false when what is behind IS the thing being decided about —
// the route preview under the NAVIGATE prompt, where a 50% checker over fine map
// lines is exactly what stops the rider recognising the route they are being
// asked to accept.
static void sheetShell(uint8_t* fb, bool scrim = true) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();
    const int y = H - 520;
    // Scrim EVERYTHING above the sheet, status band included. This used to
    // start at STATUS_H to keep the clock and battery readable, which left the
    // band crisp over a toned screen — on the power dialog (and the POWERING
    // OFF band that shutdownDevice() paints straight over it) the device looked
    // half still-on. The end-of-ride summary already tones from y = 0; all the
    // sheets now read as the same object. The sheet's own words sit on the
    // white panel below, so nothing the modal says is under the checker.
    if (scrim) fillTone({0, 0, W, y}, ui::TONE_50, fb);
    epd_fill_rect({0, y, W, H - y}, ui::PAPER, fb);
    epd_fill_rect({0, y, W, 6}, ui::INK, fb);
}

// Title + hero line + one detail line, at the summary's sizes exactly.
// Returns the baseline of the detail line (or where it would be), so a caller
// can add a line under it: the hero steps down a size for long text, which
// moves everything below it.
static int sheetHead(const char* label, const char* hero, const char* detail,
                     uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();
    const int x = ui::MARGIN;
    int y = (H - 520) + 6 + 40;

    const int lw = ui::labelWidth(&Arial_L, label);
    ui::label(x + lw / 2, y + Arial_L.ascender, label, fb);
    y += Arial_L.ascender + 8;

    // Impact carries no lowercase, no '_' and no '.' beyond its ASCII run, so a
    // route filename ("mission_dolores.gpx") drew as a row of blanks. Uppercase
    // it, turn separators into spaces and drop the extension — which is also
    // how a rider would read the name aloud.
    // DOBA: the vector faces carry Cyrillic, so other scripts stay (they were
    // dropped, and "ВИМКНУТИ?" drew as a lone "?"); capitals as ui::upper().
    char sep[96];
    size_t n = 0;
    for (const char* p = hero; *p && n < sizeof(sep) - 1; ++p) {
        char c = *p;
        if (c == '.' && (strcasecmp(p, ".gpx") == 0)) break;
        if (c == '_' || c == '-' || c == '.') c = ' ';
        if ((unsigned char)c < ' ') continue;
        sep[n++] = c;
    }
    sep[n] = 0;
    char clean[96];
    ui::upper(sep, clean, sizeof clean);

    // Step the hero DOWN to fit rather than truncating it: a route called
    // "MISSION DOL.." tells the rider less than the same name a size smaller.
    // Descending, like every other step-down ladder here: Impact_B (81) is
    // TALLER than Impact_M (69), so listing M first stepped a name that would
    // have fitted the bigger face straight down to it.
    static const EpdFont* const heroSteps[] = {&Impact_B, &Impact_M, &Impact_T};
    const EpdFont* hf = &Impact_T;
    for (const EpdFont* f : heroSteps)
        if (ui::textWidth(f, clean) <= W - 2 * ui::MARGIN) { hf = f; break; }
    char fit[96];
    fitText(hf, clean, W - 2 * ui::MARGIN, fit, sizeof(fit));
    const int heroH = epdc_digit_height(hf);
    ui::text(hf, x, y + heroH, fit, fb, EPD_DRAW_ALIGN_LEFT, ui::INK);
    y += heroH + 12;

    if (detail && detail[0]) {
        char line[72];
        fitText(&Arial_B, detail, W - 2 * ui::MARGIN, line, sizeof(line));
        ui::text(&Arial_B, x, y + Arial_B.ascender, line, fb,
                 EPD_DRAW_ALIGN_LEFT, ui::INK);
    }
    return y + Arial_B.ascender;
}

void ui_render_power_sheet(uint8_t* fb) {
    sheetShell(fb);
    sheetHead(TR("Power"), TR("SHUT DOWN?"),
              TR("Switch on again with POWER."),
              fb);
    sheetButton(kPowerShutdown, TR("SHUT DOWN"), true, fb);
    sheetButton(kPowerCancel, TR("CANCEL"), false, fb);
}


// Instant acknowledgment band for a tap that triggers seconds of work before
// the next repaint (saving a ride, powering off). Painted over whatever is on
// the glass; without it the press got no visible response and read as a
// missed tap. Same geometry as the farewell screen's band.
void ui_render_busy_band(const char* title, const char* sub, uint8_t* fb,
                         int bandY, int bandH) {
    const int W = epd_rotated_display_width();
    epd_fill_rect({0, bandY, W, bandH}, 0x00, fb);
    epd_fill_rect({0, bandY - 3, W, 3}, 0xFF, fb);
    epd_fill_rect({0, bandY + bandH, W, 3}, 0xFF, fb);
    ui::label(W / 2, bandY + bandH / 2 - 8, title, fb, 0xFF, &Arial_B);
    ui::text(&Arial_L, W / 2, bandY + bandH / 2 + 32, sub, fb,
             EPD_DRAW_ALIGN_CENTER, 0xFF);
}

void ui_render_shutdown_ack(bool savingRide, uint8_t* fb) {
    ui_render_busy_band("POWERING OFF",
                        savingRide ? "saving track..." : "one moment...", fb);
}

static void drawAppIcon(int cx, int topY, int height, uint8_t* fb, bool halo);

// DOBA: the wordmark — "D.O.B.A" and what it stands for, in a ruled box.
static const char* kDobaLine = "DURABLE OFFLINE BACKCOUNTRY ASSISTANT";
static void drawDobaMark(int cx, int topY, int height, uint8_t* fb, bool halo = false) {
    (void)height;
    const EpdFont* big = &Impact_M;
    const EpdFont* sub = &Arial_L;
    const int w = 470, h = 190, x = cx - w / 2;
    if (halo) epd_fill_rect({x - 8, topY - 8, w + 16, h + 16}, 0xFF, fb);
    epd_fill_rect({x, topY, w, h}, 0xFF, fb);
    for (int t = 0; t < 4; ++t) epd_draw_rect({x + t, topY + t, w - 2 * t, h - 2 * t}, 0x00, fb);
    ui::text(big, cx, topY + 112, "D.O.B.A", fb, EPD_DRAW_ALIGN_CENTER, 0x00);
    epd_fill_rect({x + 24, topY + 146, w - 48, 2}, 0x00, fb);
    ui::text(sub, cx, topY + 174, kDobaLine, fb, EPD_DRAW_ALIGN_CENTER, 0x00);
}

// Boot progress screen. Drawn from setup() as each subsystem comes up, so the
// glass shows what the firmware is doing instead of holding the previous
// session's image for the several seconds setup() takes. That wait is worst
// exactly when something is wrong — a failing SD mount alone burns ~1.5 s in
// sdWait() timeouts — which is precisely when a blank-looking device is most
// alarming.
//
// Composition: the mark and wordmark sit on the upper third, the step list runs
// from the optical centre, and the version anchors the foot. Only 0x00 and the
// 0x22/0x33 greys are used — this panel renders 0x44 and lighter as plain white.
void ui_render_boot_screen(const char* version, const char* const* lines,
                           const int8_t* state, const char (*detail)[28],
                           const uint32_t* ms, int count, uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();
    epd_fill_rect({0, 0, W, H}, ui::PAPER, fb);

    // Kernel-style boot log. The rider cannot fix a boot problem, but they can
    // SEE one — which step is slow, which subsystem failed, how long the card
    // took. That is worth more on a device with no console than a progress bar
    // is, and it is why the timings are on the glass rather than only in the
    // diag file.
    // Mark and wordmark first — this is still the device's front door, and the
    // log is what it is doing, not what it is. Both sized to leave the log the
    // lower two thirds.
    const int cx = W / 2;
    drawDobaMark(cx, ui::MARGIN + 20, 200, fb);   // DOBA
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "doba · otp %s · esp32-s3 · 540x960 epd", version);
    ui::text(&Arial_L, cx, 296, hdr, fb, EPD_DRAW_ALIGN_CENTER, ui::INK);

    int y = 320;
    epd_fill_rect({0, y, W, ui::RULE}, ui::INK, fb);
    y += ui::STEP + Arial_L.ascender + 6;

    const int lineH = Arial_L.ascender + 14;
    const int statusX = W - ui::MARGIN;
    for (int i = 0; i < count; ++i) {
        char stamp[16];
        snprintf(stamp, sizeof(stamp), "[%3lu.%02lu]",
                 (unsigned long)(ms[i] / 1000), (unsigned long)((ms[i] % 1000) / 10));
        ui::text(&Arial_L, ui::MARGIN, y, stamp, fb, EPD_DRAW_ALIGN_LEFT, ui::INK);

        char body[64];
        if (detail && detail[i] && detail[i][0])
            snprintf(body, sizeof(body), "%s  %s", lines[i], detail[i]);
        else
            snprintf(body, sizeof(body), "%s", lines[i]);

        // Status bracket, right-aligned: the column the eye scans.
        const char* st = state[i] == BOOT_OK ? "[ OK ]"
                       : state[i] == BOOT_FAIL ? "[FAIL]" : "[ .. ]";

        // Clamp the body to what is left before that column. A long detail used
        // to run straight under the bracket — "42 rides[ OK ]" with no gap —
        // and the status is the part that has to stay readable, because it is
        // the whole reason someone is looking at this screen.
        const int bodyX = ui::MARGIN + 82;
        const int bodyMax = statusX - ui::textWidth(&Arial_B, st) - ui::STEP - bodyX;
        char fitted[64];
        fitText(&Arial_B, body, bodyMax, fitted, sizeof(fitted));
        ui::text(&Arial_B, bodyX, y, fitted, fb, EPD_DRAW_ALIGN_LEFT, ui::INK);
        ui::text(&Arial_B, statusX, y, st, fb, EPD_DRAW_ALIGN_RIGHT, ui::INK);
        y += lineH;
    }

    // Cursor block on the line still running, so a long step reads as busy
    // rather than as a frozen list.
    if (count > 0 && state[count - 1] == BOOT_PENDING)
        epd_fill_rect({ui::MARGIN, y - Arial_L.ascender, 10, Arial_L.ascender},
                      ui::INK, fb);
    // DOBA: contour paper below the log; the mark and the log stay on plain paper.
    topo_bg::draw({0, y + 10, W, H - y - 10}, fb, 3);
}

void ui_render_mesh_notice(const char* from, const char* channel,
                           const char* text, uint8_t* fb) {
    // The band the rider already reads as "the device is telling you
    // something" (turn, pause). Static while it is up, so it costs one paint
    // arriving and one leaving.
    const int W = epd_rotated_display_width();
    const int top = ui::STATUS_H;
    epd_fill_rect({0, top, W, 138}, 0x00, fb);

    // Speech bubble: a rounded body and a tail, white on the band.
    const int bx = 36, by = top + 40, bw = 48, bh = 34, r = 8;
    epd_fill_rect({bx + r, by, bw - 2 * r, bh}, 0xFF, fb);
    epd_fill_rect({bx, by + r, bw, bh - 2 * r}, 0xFF, fb);
    epd_fill_circle(bx + r, by + r, r, 0xFF, fb);
    epd_fill_circle(bx + bw - r - 1, by + r, r, 0xFF, fb);
    epd_fill_circle(bx + r, by + bh - r - 1, r, 0xFF, fb);
    epd_fill_circle(bx + bw - r - 1, by + bh - r - 1, r, 0xFF, fb);
    epd_fill_triangle(bx + 10, by + bh - 1, bx + 24, by + bh - 1, bx + 8,
                      by + bh + 14, 0xFF, fb);

    const int textX = 104;
    const int textW = W - textX - ui::MARGIN;

    char head[80], fromU[40], chanU[16], line[96];
    upperCopy(fromU, sizeof(fromU), from);
    upperCopy(chanU, sizeof(chanU), channel);
    snprintf(head, sizeof(head), "%s · %s", fromU, chanU);
    fitText(&Arial_L, head, textW, line, sizeof(line));
    ui::text(&Arial_L, textX, top + 34, line, fb, EPD_DRAW_ALIGN_LEFT, 0xFF);

    // The message: first line breaks at the last space that fits, the rest
    // is fitted (with an ellipsis) onto the second.
    char l1[96] = "", l2[96] = "";
    const size_t n = strlen(text);
    if (ui::textWidth(&Arial_B, text) <= textW) {
        snprintf(l1, sizeof(l1), "%s", text);
    } else {
        size_t cut = 0, fit = 0;
        for (size_t i = 0; i < n && i + 1 < sizeof(l1); ++i) {
            char probe[96];
            memcpy(probe, text, i + 1);
            probe[i + 1] = 0;
            if (ui::textWidth(&Arial_B, probe) > textW) break;
            fit = i + 1;
            if (text[i] == ' ') cut = i;
        }
        if (!cut) cut = fit;                       // one long word: hard break
        // Never split a UTF-8 sequence across the two lines.
        while (cut > 0 && ((uint8_t)text[cut] & 0xC0) == 0x80) cut--;
        snprintf(l1, sizeof(l1), "%.*s", (int)cut, text);
        const char* rest = text + cut;
        while (*rest == ' ') rest++;
        fitText(&Arial_B, rest, textW, l2, sizeof(l2));
    }
    ui::text(&Arial_B, textX, top + 80, l1, fb, EPD_DRAW_ALIGN_LEFT, 0xFF);
    if (l2[0])
        ui::text(&Arial_B, textX, top + 120, l2, fb, EPD_DRAW_ALIGN_LEFT, 0xFF);
}

// DOBA: a calibration card — the sixteen framebuffer levels as bars, with their
// numbers, to see on the real glass which ones the panel tells apart.
void ui_render_grey_test(uint8_t* fb) {
    const int W = epd_rotated_display_width(), H = epd_rotated_display_height();
    epd_fill_rect({0, 0, W, H}, 0xFF, fb);
    const int top = 70, bh = (H - top - 20) / 16;
    ui::text(&Arial_B, W / 2, 45, "GREY LEVELS 0-15", fb, EPD_DRAW_ALIGN_CENTER, 0x00);
    for (int v = 0; v < 16; ++v) {
        const int y = top + v * bh;
        epd_fill_rect({130, y, W - 150, bh - 6}, (uint8_t)(v * 17), fb);
        epd_draw_rect({130, y, W - 150, bh - 6}, 0x00, fb);
        char n[8];
        snprintf(n, sizeof n, "%2d %s", v, v == 0 ? "BLK" : v == 5 ? "DRK" : v == 10 ? "LGT" : v == 15 ? "WHT" : "");
        ui::text(&Arial_B, 14, y + bh / 2 + 6, n, fb, EPD_DRAW_ALIGN_LEFT, 0x00);
    }
}


void ui::fillRound(const EpdRect& r, int rad, uint8_t color, uint8_t* fb) {
    if (r.width <= 0 || r.height <= 0) return;
    rad = std::max(0, std::min(rad, std::min(r.width, r.height) / 2));
    if (!rad) { epd_fill_rect(r, color, fb); return; }
    // Row by row: the inset of each row from the corner circle, measured at
    // the pixel's centre, so a pill reads as one smooth curve.
    for (int i = 0; i < r.height; ++i) {
        const int d = std::min(i, r.height - 1 - i);   // rows from the nearer edge
        int inset = 0;
        if (d < rad) {
            const float dy = rad - d - 0.5f;
            inset = (int)lroundf(rad - sqrtf(std::max(0.f, (float)rad * rad - dy * dy)));
        }
        epd_fill_rect({r.x + inset, r.y + i, r.width - 2 * inset, 1}, color, fb);
    }
}
void ui::frameRound(const EpdRect& r, int rad, int t, uint8_t* fb, uint8_t color, uint8_t inside) {
    fillRound(r, rad, color, fb);
    fillRound({r.x + t, r.y + t, r.width - 2 * t, r.height - 2 * t}, rad - t, inside, fb);
}

// Capitals for a section head, Cyrillic too (the table keeps one spelling
// for a word used as a head and as a caption).
void ui::upper(const char* in, char* out, size_t n) {
    size_t o = 0;
    for (const uint8_t* p = (const uint8_t*)in; *p && o + 3 < n;) {
        uint8_t a = p[0], b = p[1];
        if (a < 0x80) { out[o++] = (a >= 'a' && a <= 'z') ? a - 32 : a; ++p; continue; }
        if (a == 0xD0 && b >= 0xB0 && b <= 0xBF) b -= 0x20;                    // а..п
        else if (a == 0xD1 && b >= 0x80 && b <= 0x8F) { a = 0xD0; b += 0x20; }  // р..я
        else if (a == 0xD1 && b >= 0x90 && b <= 0x9F) { a = 0xD0; b -= 0x10; }  // ѐ..џ: є і ї
        else if (a == 0xD2 && b == 0x91) b = 0x90;                                 // ґ
        if (b && (a & 0xE0) == 0xC0) { out[o++] = a; out[o++] = b; p += 2; continue; }
        out[o++] = *p++;
    }
    out[o] = 0;
}

// A Cyrillic letter at p (two UTF-8 bytes): its case, and in place to lower.
static bool cyrUpper(const unsigned char* p) {
    return (p[0] == 0xD0 && ((p[1] >= 0x90 && p[1] <= 0xAF) || p[1] == 0x84 || p[1] == 0x86 || p[1] == 0x87)) ||
           (p[0] == 0xD2 && p[1] == 0x90);
}
static bool cyrLower(const unsigned char* p) {
    return (p[0] == 0xD0 && p[1] >= 0xB0 && p[1] <= 0xBF) || (p[0] == 0xD1 && p[1] >= 0x80 && p[1] <= 0x9F) ||
           (p[0] == 0xD2 && p[1] == 0x91);
}
static void cyrToLower(unsigned char* p) {
    if (p[0] == 0xD0 && p[1] >= 0x90 && p[1] <= 0x9F) p[1] += 0x20;
    else if (p[0] == 0xD0 && p[1] >= 0xA0 && p[1] <= 0xAF) { p[0] = 0xD1; p[1] -= 0x20; }
    else if (p[0] == 0xD0 && (p[1] == 0x84 || p[1] == 0x86 || p[1] == 0x87)) { p[0] = 0xD1; p[1] += 0x10; }
    else if (p[0] == 0xD2 && p[1] == 0x90) p[1] = 0x91;
}

// First letter to a capital, Latin or Cyrillic ("молодий місяць" -> "Молодий...").
void ui::capFirst(char* t) {
    unsigned char* p = (unsigned char*)t;
    if (p[0] >= 'a' && p[0] <= 'z') { p[0] -= 32; return; }
    if (p[0] == 0xD0 && p[1] >= 0xB0 && p[1] <= 0xBF) p[1] -= 0x20;
    else if (p[0] == 0xD1 && p[1] >= 0x80 && p[1] <= 0x8F) { p[0] = 0xD0; p[1] += 0x20; }
    else if (p[0] == 0xD1 && (p[1] == 0x94 || p[1] == 0x96 || p[1] == 0x97)) { p[0] = 0xD0; p[1] -= 0x10; }
    else if (p[0] == 0xD2 && p[1] == 0x91) p[1] = 0x90;
}

const char* ui::sentence(const char* in, char* out, size_t n) {
    snprintf(out, n, "%s", in ? in : "");
    for (const char* c = out; *c; ++c) if (*c >= 'a' && *c <= 'z') return out;   // already mixed
    // DOBA: Ukrainian too — "БАТАРЕЯ" -> "Батарея", "ЯКЩО ЗНАЙШЛИ" -> "Якщо знайшли".
    {
        bool cyr = false;
        for (const unsigned char* c = (const unsigned char*)out; c[0] && c[1]; ++c) {
            if (cyrLower(c)) return out;   // already mixed
            if (cyrUpper(c)) cyr = true;
        }
        if (cyr) {
            static const char* const kKeepUk[] = {"ДСНС", "МОН", "ЗСУ", "НС", "ГПС"};
            bool first = true;
            unsigned char* w = (unsigned char*)out;
            while (*w) {
                if (!(w[1] && (cyrUpper(w) || cyrLower(w)))) {
                    if ((*w >= 'A' && *w <= 'Z') || (*w >= '0' && *w <= '9')) first = false;
                    ++w;
                    continue;
                }
                unsigned char* e = w;
                while (e[0] && e[1] && (cyrUpper(e) || cyrLower(e) || (e[0] == 0xE2 && e[1] == 0x80 && e[2] == 0x99))) e += (e[0] == 0xE2 ? 3 : 2);
                bool keep = false;
                for (const char* k : kKeepUk)
                    if ((size_t)(e - w) == strlen(k) && !strncmp((const char*)w, k, e - w)) keep = true;
                if (!keep)
                    for (unsigned char* c = first ? w + 2 : w; c < e; c += (c[0] == 0xE2 ? 3 : 2))
                        if (cyrUpper(c)) cyrToLower(c);
                first = false;
                w = e;
            }
            return out;
        }
    }
    static const char* const kKeep[] = {"GPS", "USB", "SOS", "UTC", "MGRS", "EU", "UA", "US", "RU", "BT",
                                        "SD", "LED", "ID", "OK", "AZ", "DEM", "MHZ", "LORA", "QR", "DM", "UK"};
    bool first = true;
    for (char* w = out; *w;) {
        if (!((*w >= 'A' && *w <= 'Z') || (*w >= '0' && *w <= '9'))) { ++w; continue; }
        char* e = w;
        while ((*e >= 'A' && *e <= 'Z') || (*e >= '0' && *e <= '9')) ++e;
        bool keep = false;
        for (const char* k : kKeep)
            if ((size_t)(e - w) == strlen(k) && !strncmp(w, k, e - w)) keep = true;
        for (char* c = w; c < e && !keep; ++c)
            if (*c >= '0' && *c <= '9') keep = true;   // "6400", "EU_868": codes
        if (*e == '_') keep = true;
        if (!keep)
            for (char* c = (first ? w + 1 : w); c < e; ++c) if (*c >= 'A' && *c <= 'Z') *c += 32;
        first = false;
        w = e;
    }
    return out;
}

const EpdFont* ui::monoFont(int px) {
#ifdef VFONT
    return vf_face_digit(VF_MONO, px);
#else
    return &Arial_B;
#endif
}
const EpdFont* ui::valueFont(int px) {
#ifdef VFONT
    return vf_face_digit(VF_VALUES, px);
#else
    return &Impact_V;
#endif
}

// DOBA: see ui_render.h.
const EpdFont* ui::font(ui::FontRole r) {
    switch (r) {
#ifdef VFONT
        case F_TINY:   return vf_face_digit(VF_LABELS, 11);   // DOBA: Inter, small grey lines
#else
        case F_TINY:   return &Arial_L;
#endif
        case F_SMALL:  return &Arial_B;
        case F_LABEL:  return &ArialBold_14;
        case F_KEY:    return &ArialBold_20;
#ifdef VFONT
        case F_BODY:   return vf_face_digit(VF_LABELS, 17);
        case F_BODY_H: return vf_face_digit(VF_VALUES, 24);
#else
        case F_BODY:   return &ArialBold_14;
        case F_BODY_H: return &ArialBold_20;
#endif
#ifdef VFONT
        case F_TITLE:  return vf_face_digit(VF_STRONG, 21);   // DOBA: headings in Inter
        case F_MONO:   return vf_face_digit(VF_MONO, 15);     // coordinates, times
        case F_H1:     return vf_face_digit(VF_STRONG, 28);
        case F_STRONG: return vf_face_digit(VF_STRONG, 13);
#else
        case F_H1:     return &Impact_T;
        case F_STRONG: return &Arial_B;
        case F_TITLE:  return &Impact_T;
        case F_MONO:   return &Arial_B;
#endif
        case F_BIG:    return &Impact_V;
    }
    return &Arial_B;
}
