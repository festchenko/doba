// DOBA: offline library reader — see doba_library.h.
#include "doba_library.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <cstdio>
#include <cstring>

#include "doba_fold_table.h"
#include "epd_compat.h"
#include "sd_bus.h"
#include "ui_render.h"

#ifdef OTP_SIM
#include <zlib.h>
#else
#include "rom/miniz.h"
#endif

namespace doba_lib {
namespace {

constexpr int W = 540, H = 960, TOP = ui::STATUS_H;
constexpr uint8_t INK = 0x00, PAPER = 0xFF;
constexpr int REC = 128, KEY = 56, HDR = 32;
constexpr size_t MAX_TEXT = 1536 * 1024;      // inflated article cap (PSRAM)
constexpr int MAX_BOOKS = 12;
constexpr int RESULTS = 6;
const char* kRoot = "/doba/lib";

struct Book { char dir[32]; char name[40]; uint32_t count; };
Book g_books[MAX_BOOKS];
int g_bookCount = -1;      // -1 = not scanned yet
int g_book = 0;

struct Result { char title[KEY + 1]; uint16_t chunk; uint32_t off, len; };
Result g_results[RESULTS];
int g_resultCount = 0;
char g_lastQuery[256] = "\x01";   // forces the first search

// The open article.
char* g_text = nullptr;          // inflated UTF-8, NUL-terminated
size_t g_textLen = 0;
char g_title[KEY + 1] = {};
struct Line { uint32_t off; uint16_t len; uint8_t kind; };   // a wrapped line
Line* g_lines = nullptr;
int g_lineCount = 0, g_lineCap = 0;
int g_page = 0;
constexpr int MAX_PAGES = 4096;
int* g_pageStarts = nullptr;   // PSRAM: keeps 16 KB out of internal RAM
int g_pageCount = 0;

uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void frame(const EpdRect& r, int t, uint8_t* fb) {
    for (int i = 0; i < t; ++i)
        epd_draw_rect({r.x + i, r.y + i, r.width - 2 * i, r.height - 2 * i}, INK, fb);
}
bool hit(const EpdRect& r, int x, int y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

// --- folding -------------------------------------------------------------------

uint32_t decodeUtf8(const char*& s) {
    const unsigned char c = (unsigned char)*s;
    if (c < 0x80) { ++s; return c; }
    int n = (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : 3;
    uint32_t cp = c & (0x3F >> n);
    ++s;
    for (int i = 0; i < n && (*s & 0xC0) == 0x80; ++i, ++s) cp = (cp << 6) | (*s & 0x3F);
    return cp;
}

int encodeUtf8(uint32_t cp, char* out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | cp >> 6); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

uint32_t foldCp(uint32_t cp) {
    if (cp < 0x80) {
        if ((cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z')) return cp;
        if (cp >= 'A' && cp <= 'Z') return cp + 32;
        return 0x20;
    }
    if (cp >= 0x2000 && cp < 0x2070) return 0x20;
    int lo = 0, hi = kFoldCount;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (kFoldFrom[mid] < cp) lo = mid + 1; else hi = mid;
    }
    return (lo < kFoldCount && kFoldFrom[lo] == cp) ? kFoldTo[lo] : cp;
}

// --- books ---------------------------------------------------------------------

void scan() {
    g_bookCount = 0;
    sdLock();
    File root = SD.open(kRoot);
    if (root && root.isDirectory()) {
        File d;
        while ((d = root.openNextFile()) && g_bookCount < MAX_BOOKS) {
            if (!d.isDirectory()) { d.close(); continue; }
            Book& b = g_books[g_bookCount];
            const char* nm = strrchr(d.name(), '/');
            snprintf(b.dir, sizeof b.dir, "%s", nm ? nm + 1 : d.name());
            d.close();
            char path[96];
            snprintf(path, sizeof path, "%s/%s/index.dti", kRoot, b.dir);
            File ix = SD.open(path, FILE_READ);
            if (!ix) continue;
            uint8_t h[HDR];
            const bool ok = ix.read(h, HDR) == HDR && memcmp(h, "DOBAIX1", 7) == 0;
            ix.close();
            if (!ok) continue;
            b.count = rd32(h + 8);
            snprintf(b.name, sizeof b.name, "%s", b.dir);
            snprintf(path, sizeof path, "%s/%s/info.txt", kRoot, b.dir);
            File info = SD.open(path, FILE_READ);
            if (info) {
                char buf[256] = {};
                info.read((uint8_t*)buf, sizeof buf - 1);
                info.close();
                char* n = strstr(buf, "name=");
                if (n) {
                    n += 5;
                    char* e = strpbrk(n, "\r\n");
                    if (e) *e = 0;
                    snprintf(b.name, sizeof b.name, "%s", n);
                }
            }
            ++g_bookCount;
        }
    }
    if (root) root.close();
    sdUnlock();
}

// Prefix search: up to RESULTS records whose key starts with `key`.
void search(const char* key) {
    g_resultCount = 0;
    if (g_bookCount <= 0) return;
    const Book& b = g_books[g_book];
    char path[96];
    snprintf(path, sizeof path, "%s/%s/index.dti", kRoot, b.dir);
    const size_t klen = strlen(key);
    uint8_t want[KEY] = {};
    memcpy(want, key, klen < KEY ? klen : KEY);
    sdLock();
    File f = SD.open(path, FILE_READ);
    if (!f) { sdUnlock(); return; }
    uint8_t rec[REC];
    uint32_t lo = 0, hi = b.count;
    while (lo < hi) {   // lower bound
        const uint32_t mid = lo + (hi - lo) / 2;
        f.seek(HDR + (uint64_t)mid * REC);
        if (f.read(rec, REC) != REC) break;
        if (memcmp(rec, want, KEY) < 0) lo = mid + 1; else hi = mid;
    }
    f.seek(HDR + (uint64_t)lo * REC);
    char seen[RESULTS][KEY + 1];
    for (uint32_t i = lo; i < b.count && g_resultCount < RESULTS; ++i) {
        if (f.read(rec, REC) != REC) break;
        if (memcmp(rec, want, klen) != 0) break;
        Result& r = g_results[g_resultCount];
        memcpy(r.title, rec + KEY, KEY);
        r.title[KEY] = 0;
        r.chunk = (uint16_t)(rec[112] | rec[113] << 8);
        r.off = rd32(rec + 116);
        r.len = rd32(rec + 120);
        // Redirects share their target: list each article once.
        bool dup = false;
        for (int k = 0; k < g_resultCount; ++k)
            if (g_results[k].chunk == r.chunk && g_results[k].off == r.off) dup = true;
        if (!dup) { memcpy(seen[g_resultCount], r.title, KEY + 1); ++g_resultCount; }
    }
    f.close();
    sdUnlock();
}

bool inflateTo(const uint8_t* src, size_t len, char* dst, size_t cap, size_t* outLen) {
#ifdef OTP_SIM
    z_stream z = {};
    if (inflateInit(&z) != Z_OK) return false;
    z.next_in = (Bytef*)src; z.avail_in = (uInt)len;
    z.next_out = (Bytef*)dst; z.avail_out = (uInt)cap;
    const int r = inflate(&z, Z_FINISH);
    *outLen = z.total_out;
    inflateEnd(&z);
    return r == Z_STREAM_END || r == Z_BUF_ERROR;   // BUF_ERROR: truncated at cap
#else
    static tinfl_decompressor* d = nullptr;
    if (!d) d = (tinfl_decompressor*)heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_8BIT);
    if (!d) return false;
    tinfl_init(d);
    size_t inLen = len, out = cap;
    const tinfl_status st = tinfl_decompress(
        d, src, &inLen, (uint8_t*)dst, (uint8_t*)dst, &out,
        TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    *outLen = out;
    return st == TINFL_STATUS_DONE || st == TINFL_STATUS_HAS_MORE_OUTPUT;
#endif
}

// --- article layout ------------------------------------------------------------

const EpdFont* fontFor(uint8_t kind) {
    return (kind == 'T' || kind == 'H') ? ui::font(ui::F_BODY_H)
         : kind == 'S' ? ui::font(ui::F_KEY) : ui::font(ui::F_BODY);
}
int lineHeight(uint8_t kind) {
    return kind == 'T' ? 46 : kind == 'H' ? 42 : kind == 'S' ? 36 : 30;
}

int widthOf(const EpdFont* f, const char* s, size_t n) {
    char tmp[160];
    if (n >= sizeof tmp) n = sizeof tmp - 1;
    memcpy(tmp, s, n);
    tmp[n] = 0;
    return ui::textWidth(f, tmp);
}

void addLine(uint32_t off, uint16_t len, uint8_t kind) {
    if (g_lineCount == g_lineCap) {
        const int cap = g_lineCap ? g_lineCap * 2 : 2048;
        Line* n = (Line*)heap_caps_realloc(g_lines, cap * sizeof(Line), MALLOC_CAP_SPIRAM);
        if (!n) return;
        g_lines = n;
        g_lineCap = cap;
    }
    g_lines[g_lineCount++] = {off, len, kind};
}

void layout() {
    g_lineCount = 0;
    const int maxW = W - 2 * 18;
    size_t i = 0;
    while (i < g_textLen) {
        size_t e = i;
        while (e < g_textLen && g_text[e] != '\n') ++e;
        const uint8_t kind = (uint8_t)g_text[i];
        const size_t start = i + 1;
        const EpdFont* f = fontFor(kind);
        const int bulletW = kind == 'B' ? 26 : 0;
        const int spaceW = widthOf(f, "a a", 3) - widthOf(f, "aa", 2);
        size_t ls = start, p = start;
        int lw = 0;
        while (p < e) {
            size_t we = p;
            while (we < e && g_text[we] != ' ') ++we;
            const int ww = widthOf(f, g_text + p, we - p);
            const int add = (p == ls ? 0 : spaceW) + ww;
            if (p != ls && lw + add > maxW - bulletW) {
                addLine((uint32_t)ls, (uint16_t)(p - 1 - ls), kind);
                ls = p;
                lw = ww;
            } else {
                lw += add;
            }
            p = we < e ? we + 1 : we;
        }
        if (e > start) addLine((uint32_t)ls, (uint16_t)(e - ls), kind);
        if (kind == 'P' || kind == 'B') addLine(0, 0, 'G');   // paragraph gap
        i = e + 1;
    }
    // Pages: as many lines as fit between the header and the footer.
    g_pageCount = 0;
    if (!g_pageStarts)
        g_pageStarts = (int*)heap_caps_malloc(MAX_PAGES * sizeof(int), MALLOC_CAP_SPIRAM);
    if (!g_pageStarts) return;
    const int bodyTop = TOP + 60, bodyBottom = H - 50;
    int y = bodyTop, k = 0;
    g_pageStarts[g_pageCount++] = 0;
    for (k = 0; k < g_lineCount; ++k) {
        const int h = g_lines[k].kind == 'G' ? 10 : lineHeight(g_lines[k].kind);
        if (y + h > bodyBottom && g_pageCount < MAX_PAGES) {
            g_pageStarts[g_pageCount++] = k;
            y = bodyTop;
        }
        y += h;
    }
}

bool openArticle(const Result& r) {
    const Book& b = g_books[g_book];
    char path[96];
    snprintf(path, sizeof path, "%s/%s/a%04u.dta", kRoot, b.dir, (unsigned)r.chunk);
    if (!g_text) g_text = (char*)heap_caps_malloc(MAX_TEXT + 1, MALLOC_CAP_SPIRAM);
    uint8_t* z = (uint8_t*)heap_caps_malloc(r.len, MALLOC_CAP_SPIRAM);
    if (!g_text || !z) { if (z) heap_caps_free(z); return false; }
    sdLock();
    File f = SD.open(path, FILE_READ);
    bool ok = f && f.seek(r.off) && f.read(z, r.len) == r.len;
    if (f) f.close();
    sdUnlock();
    size_t n = 0;
    ok = ok && inflateTo(z, r.len, g_text, MAX_TEXT, &n);
    heap_caps_free(z);
    if (!ok) return false;
    g_textLen = n;
    g_text[n] = 0;
    snprintf(g_title, sizeof g_title, "%s", r.title);
    layout();
    g_page = 0;
    return true;
}

// --- search screen geometry -------------------------------------------------------

const EpdRect kBookBtn = {12, TOP + 8, W - 24, 56};
const EpdRect kQuery = {12, TOP + 72, W - 24, 64};
constexpr int RES_TOP = TOP + 146, RES_H = 58;

}  // namespace

void fold(const char* in, char* out, size_t cap) {
    size_t n = 0;
    bool space = true;   // drops leading spaces, collapses runs
    while (*in && n + 4 < cap) {
        const uint32_t cp = foldCp(decodeUtf8(in));
        if (cp == 0x20) { if (!space) { out[n++] = ' '; space = true; } continue; }
        n += encodeUtf8(cp, out + n);
        space = false;
    }
    while (n && out[n - 1] == ' ') --n;
    out[n] = 0;
}

int count() {
    if (g_bookCount < 0) scan();
    return g_bookCount;
}

void rescan() { g_bookCount = -1; g_lastQuery[0] = 1; }

void renderSearch(kbd::Keyboard& k, uint8_t* fb) {
    if (g_bookCount < 0) scan();
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    k.top = H - k.height();
    k.enterLabel = "OPEN";
    if (g_bookCount <= 0) {
        ui::text(ui::font(ui::F_TITLE), W / 2, 380, "NO BOOKS", fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::text(ui::font(ui::F_SMALL), W / 2, 430, "copy books to /doba/lib on the card", fb,
                 EPD_DRAW_ALIGN_CENTER, INK);
        return;
    }
    char line[96];
    snprintf(line, sizeof line, "%s  (%lu)  >", g_books[g_book].name,
             (unsigned long)g_books[g_book].count);
    frame(kBookBtn, 2, fb);
    ui::text(ui::font(ui::F_SMALL), kBookBtn.x + 14, kBookBtn.y + 36, line, fb, EPD_DRAW_ALIGN_LEFT, INK);
    frame(kQuery, 3, fb);
    char q[300];
    snprintf(q, sizeof q, "%s_", k.text);
    ui::text(ui::font(ui::F_KEY), kQuery.x + 14, kQuery.y + 44, q, fb, EPD_DRAW_ALIGN_LEFT, INK);

    char key[KEY + 1];
    fold(k.text, key, sizeof key);
    if (strcmp(key, g_lastQuery) != 0) {
        snprintf(g_lastQuery, sizeof g_lastQuery, "%s", key);
        if (key[0]) search(key); else g_resultCount = 0;
    }
    for (int i = 0; i < g_resultCount; ++i) {
        const int y = RES_TOP + i * RES_H;
        ui::text(ui::font(ui::F_KEY), 24, y + 40, g_results[i].title, fb, EPD_DRAW_ALIGN_LEFT, INK);
        epd_fill_rect({12, y + RES_H - 2, W - 24, 1}, INK, fb);
    }
    if (key[0] && g_resultCount == 0)
        ui::text(ui::font(ui::F_SMALL), W / 2, RES_TOP + 60, "nothing starts with that", fb,
                 EPD_DRAW_ALIGN_CENTER, INK);
    k.render(fb);
}

bool tapSearch(kbd::Keyboard& k, int x, int y) {
    if (g_bookCount <= 0) return false;
    if (hit(kBookBtn, x, y)) {
        g_book = (g_book + 1) % g_bookCount;
        g_lastQuery[0] = 1;   // search again in the new book
        return false;
    }
    if (y >= RES_TOP && y < RES_TOP + RESULTS * RES_H && y < k.top - 6) {
        const int i = (y - RES_TOP) / RES_H;
        if (i < g_resultCount) return openArticle(g_results[i]);
        return false;
    }
    if (k.tap(x, y) == kbd::ENTER && g_resultCount > 0) return openArticle(g_results[0]);
    return false;
}

void renderArticle(uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    if (!g_text || !g_pageStarts || g_pageCount == 0) return;
    // Header: the title, small, and the page counter.
    char pg[24];
    snprintf(pg, sizeof pg, "%d/%d", g_page + 1, g_pageCount);
    ui::text(ui::font(ui::F_TINY), W - 14, TOP + 30, pg, fb, EPD_DRAW_ALIGN_RIGHT, INK);
    ui::text(ui::font(ui::F_SMALL), 14, TOP + 32, g_title, fb, EPD_DRAW_ALIGN_LEFT, INK);
    epd_fill_rect({0, TOP + 44, W, 2}, INK, fb);

    const int first = g_pageStarts[g_page];
    const int last = g_page + 1 < g_pageCount ? g_pageStarts[g_page + 1] : g_lineCount;
    int y = TOP + 60;
    char buf[512];
    for (int k = first; k < last; ++k) {
        const Line& l = g_lines[k];
        if (l.kind == 'G') { y += 10; continue; }
        const int h = lineHeight(l.kind);
        size_t n = l.len < sizeof buf - 1 ? l.len : sizeof buf - 1;
        memcpy(buf, g_text + l.off, n);
        buf[n] = 0;
        int x = 18;
        // First line of a bullet gets the bullet; continuation lines indent.
        if (l.kind == 'B') {
            const bool firstOfItem = l.off > 0 && g_text[l.off - 1] == 'B';
            if (firstOfItem) epd_fill_rect({20, y + h / 2 - 6, 8, 8}, INK, fb);
            x += 26;
        }
        ui::text(fontFor(l.kind), x, y + h - 8, buf, fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (l.kind == 'H') epd_fill_rect({18, y + h - 2, W - 36, 2}, INK, fb);
        y += h;
    }
    // Footer: how to page.
    epd_fill_rect({0, H - 44, W, 2}, INK, fb);
    ui::text(ui::font(ui::F_TINY), 18, H - 14, "< tap left", fb, EPD_DRAW_ALIGN_LEFT, INK);
    ui::text(ui::font(ui::F_TINY), W - 18, H - 14, "tap right >", fb, EPD_DRAW_ALIGN_RIGHT, INK);
}

void tapArticle(int x, int y) {
    (void)y;
    if (x > W * 2 / 5) { if (g_page + 1 < g_pageCount) ++g_page; }
    else if (g_page > 0) --g_page;
}

}  // namespace doba_lib
