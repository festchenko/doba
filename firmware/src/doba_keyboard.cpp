// DOBA: on-screen keyboard — see doba_keyboard.h.
#include "doba_keyboard.h"

#include <algorithm>
#include <cstring>

#include "epd_compat.h"
#include "ui_render.h"

namespace kbd {
namespace {

constexpr int W = 540, H = 960;
constexpr int ROW_H = 84;
constexpr int GAP = 4;

// Three letter rows per layout, each a UTF-8 string of single characters.
// Row 3 also carries SHIFT (left) and DEL (right).
const char* const kRows[LAYOUT_COUNT][3] = {
    {"qwertyuiop", "asdfghjkl", "zxcvbnm"},
    {"йцукенгшщзхї", "фівапролджє", "ячсмитьбюґ"},
    {"1234567890", "-/:;()@&\"'", ".,?!#%+=*_"},
};
const char* const kLayoutName[LAYOUT_COUNT] = {"EN", "UA", "123"};

// Next UTF-8 character of s into out (NUL-terminated); returns bytes used.
int nextChar(const char* s, char* out) {
    const unsigned char c = (unsigned char)s[0];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    memcpy(out, s, n);
    out[n] = 0;
    return n;
}

int countChars(const char* s) {
    int n = 0;
    while (*s) { char tmp[5]; s += nextChar(s, tmp); ++n; }
    return n;
}

// Upper case for the letters this keyboard types (ASCII and Cyrillic).
void upper(char* ch) {
    unsigned char* c = (unsigned char*)ch;
    if (c[0] >= 'a' && c[0] <= 'z') { c[0] -= 32; return; }
    if (c[0] == 0xD0 || c[0] == 0xD1) {
        unsigned cp = ((c[0] & 0x1F) << 6) | (c[1] & 0x3F);
        if (cp >= 0x430 && cp <= 0x44F) cp -= 0x20;          // а-я
        else if (cp >= 0x450 && cp <= 0x45F) cp -= 0x50;     // ё ї є ...
        else if (cp == 0x491) cp = 0x490;                    // ґ
        c[0] = (unsigned char)(0xC0 | (cp >> 6));
        c[1] = (unsigned char)(0x80 | (cp & 0x3F));
    } else if (c[0] == 0xD2 && c[1] == 0x91) {
        c[1] = 0x90;                                          // ґ -> Ґ
    }
}

struct Key { int x, y, w, h; char label[8]; int kind; };   // kind: 0 char, 1 shift, 2 del, 3 sym, 4 lang, 5 space, 6 enter

Key mk(int x, int y, int w, int h, const char* label, int kind) {
    Key k;
    k.x = x; k.y = y; k.w = w; k.h = h; k.kind = kind;
    strncpy(k.label, label, sizeof k.label - 1);
    k.label[sizeof k.label - 1] = 0;
    return k;
}

// Every row spans the same width (screen minus a small margin), keys are one
// width per layout (the longest row sets it), shorter rows sit centred, and
// the third row's SHIFT and DEL take whatever is left at its two ends.
constexpr int MARGIN = 6;
int layoutKeys(const Keyboard& k, Key* out) {
    int n = 0;
    const int lay = k.layout;
    const int span = W - 2 * MARGIN;
    int maxCount = 0;
    for (int r = 0; r < 3; ++r) maxCount = std::max(maxCount, countChars(kRows[lay][r]));
    const int keyW = span / maxCount;
    for (int r = 0; r < 3; ++r) {
        const char* row = kRows[lay][r];
        const int count = countChars(row);
        const int y = k.top + r * ROW_H;
        int x;
        if (r == 2 && lay != SYM) {
            // SHIFT | letters | DEL across the full span
            const int letters = std::min(count, maxCount - 2);
            const int side = (span - letters * keyW) / 2;
            Key& s = out[n++];
            s = mk(MARGIN, y, side, ROW_H, k.shift ? "ABC" : "abc", 1);
            x = MARGIN + side;
            const char* p = row;
            for (int i = 0; i < count && *p; ++i) {
                Key& kk = out[n++];
                const int w = (count > letters) ? (span - 2 * side) / count : keyW;
                kk = mk(x, y, w, ROW_H, "", 0);
                p += nextChar(p, kk.label);
                if (k.shift) upper(kk.label);
                x += w;
            }
            out[n++] = mk(x, y, MARGIN + span - x, ROW_H, "DEL", 2);
            continue;
        }
        if (r == 2) {   // symbols: no shift, DEL at the end
            const int w = (span - keyW * 3 / 2) / count;
            x = MARGIN;
            const char* p = row;
            while (*p) {
                Key& kk = out[n++];
                kk = mk(x, y, w, ROW_H, "", 0);
                p += nextChar(p, kk.label);
                x += w;
            }
            out[n++] = mk(x, y, MARGIN + span - x, ROW_H, "DEL", 2);
            continue;
        }
        x = MARGIN + (span - count * keyW) / 2;
        const char* p = row;
        while (*p) {
            Key& kk = out[n++];
            kk = mk(x, y, keyW, ROW_H, "", 0);
            p += nextChar(p, kk.label);
            if (k.shift && lay != SYM) upper(kk.label);
            x += keyW;
        }
    }
    const int y = k.top + 3 * ROW_H;
    const int u = span / 12;
    out[n++] = mk(MARGIN, y, u * 2, ROW_H, "", 3);
    strcpy(out[n - 1].label, lay == SYM ? kLayoutName[k.lastLetters] : "123");
    out[n++] = mk(MARGIN + u * 2, y, u * 2, ROW_H, "", 4);
    strcpy(out[n - 1].label, lay == SYM ? "" : kLayoutName[lay == UA ? EN : lay + 1]);
    out[n++] = mk(MARGIN + u * 4, y, u * 5, ROW_H, "SPACE", 5);
    out[n++] = mk(MARGIN + u * 9, y, span - u * 9, ROW_H, "", 6);
    strncpy(out[n - 1].label, k.enterLabel, sizeof(out[n - 1].label) - 1);
    return n;
}

}  // namespace

void popUtf8(char* s) {
    size_t n = strlen(s);
    while (n > 0) {
        --n;
        if (((unsigned char)s[n] & 0xC0) != 0x80) break;   // a lead byte
    }
    s[n] = 0;
}

int Keyboard::height() const { return 4 * ROW_H; }

void Keyboard::render(uint8_t* fb) const {
    Key keys[48];
    const int n = layoutKeys(*this, keys);
    epd_fill_rect({0, top - 6, W, H - top + 6}, 0xFF, fb);
    epd_fill_rect({0, top - 6, W, 3}, 0x00, fb);
    for (int i = 0; i < n; ++i) {
        const Key& k = keys[i];
        const EpdRect r = {k.x + GAP, k.y + GAP, k.w - 2 * GAP, k.h - 2 * GAP};
        const bool dark = k.kind == 6 || (k.kind == 1 && shift);
        if (dark) epd_fill_rect(r, 0x00, fb);
        else { epd_draw_rect(r, 0x00, fb); epd_draw_rect({r.x + 1, r.y + 1, r.width - 2, r.height - 2}, 0x00, fb); }
        if (!k.label[0]) continue;
        const EpdFont* f = ui::font(k.kind == 0 ? ui::F_KEY : ui::F_SMALL);
        ui::text(f, k.x + k.w / 2, k.y + k.h / 2 + (k.kind == 0 ? 12 : 8), k.label, fb,
                 EPD_DRAW_ALIGN_CENTER, dark ? 0xFF : 0x00);
    }
}

Action Keyboard::tap(int x, int y) {
    Key keys[48];
    const int n = layoutKeys(*this, keys);
    for (int i = 0; i < n; ++i) {
        const Key& k = keys[i];
        if (x < k.x || x >= k.x + k.w || y < k.y || y >= k.y + k.h) continue;
        switch (k.kind) {
            case 0: {
                if (strlen(text) + strlen(k.label) >= sizeof(text)) return NONE;
                strcat(text, k.label);
                if (shift) shift = false;   // one capital, like a phone
                return TYPED;
            }
            case 1: shift = !shift; return CHANGED;
            case 2: popUtf8(text); return DELETED;
            case 3:
                if (layout == SYM) layout = lastLetters;
                else { lastLetters = layout; layout = SYM; }
                return CHANGED;
            case 4:
                if (layout != SYM) layout = layout == UA ? EN : layout + 1;
                return CHANGED;
            case 5:
                if (strlen(text) + 1 < sizeof(text)) strcat(text, " ");
                return TYPED;
            case 6: return ENTER;
        }
    }
    return NONE;
}

}  // namespace kbd
