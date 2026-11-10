// DOBA: offline library reader — see doba_library.h.
#include "doba_library.h"
#include "doba_icons.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <cstdio>
#include <cstring>

#include "doba_fold_table.h"
#include "epd_compat.h"
#include "sd_bus.h"
#include "ui_render.h"
#include <Preferences.h>
#ifdef VFONT
#include "vfont.h"
#include "fonts/vf/literata_450.h"
#include "fonts/vf/literata_700.h"
#endif

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
constexpr int MAX_BOOKS = 32;   // one bit each in a uint32 mask
constexpr int RESULTS = 6;
const char* kRoot = "/doba/lib";

struct Book { char dir[32]; char name[40]; char source[64]; uint32_t count; uint32_t shown; char unit[16]; char about[100]; };
Book g_books[MAX_BOOKS];
int g_bookCount = -1;      // -1 = not scanned yet
int g_book = 0;

struct Result { char title[KEY + 1]; uint16_t chunk; uint32_t off, len; int8_t book; };
Result g_results[RESULTS];
bool findIn(int bi, const char* title, Result& out);
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
constexpr int FOOT_H = 76;     // PREV / NEXT
int* g_pageStarts = nullptr;   // PSRAM: keeps 16 KB out of internal RAM
int g_pageCount = 0;

// Links in the open article, cut out of the text when it opens: the visible
// words stay in g_text, the targets go to g_targets.
struct Link { uint32_t start, end, target; };   // [start, end) in g_text
constexpr int MAX_LINKS = 6000;
constexpr size_t TARGETS_CAP = 192 * 1024;
Link* g_links = nullptr;
int g_linkCount = 0;
char* g_targets = nullptr;
size_t g_targetsLen = 0;
// Where the links on the page on screen are, for taps.
struct Hot { int16_t x0, y0, x1, y1; int link; };
constexpr int MAX_HOT = 96;
Hot g_hot[MAX_HOT];
int g_hotCount = 0;
const char* g_note = nullptr;   // one-off line in the footer ("not in this book")
// Back through followed links.
struct Visit { Result r; int book; int page; };
constexpr int HISTORY = 16;
Visit g_history[HISTORY];
int g_historyCount = 0;
Result g_current = {};
// The contents list of the open article: its headings, as line indices.
bool g_toc = false;                                    // the contents list is up
int g_tocPage = 0;
constexpr int MAX_TOC = 512, TOC_ROW = 60;
int* g_toc_lines = nullptr;
int g_tocCount = 0;

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
            b.source[0] = 0;
            snprintf(b.name, sizeof b.name, "%s", b.dir);
            snprintf(path, sizeof path, "%s/%s/info.txt", kRoot, b.dir);
            File info = SD.open(path, FILE_READ);
            if (info) {
                char buf[640] = {};
                info.read((uint8_t*)buf, sizeof buf - 1);
                info.close();
                auto field = [&](const char* key, char* out, size_t cap) {
                    char* n = strstr(buf, key);
                    if (!n) return;
                    n += strlen(key);
                    const char* e = strpbrk(n, "\r\n");
                    const size_t len = e ? (size_t)(e - n) : strlen(n);
                    snprintf(out, cap, "%.*s", (int)len, n);
                };
                b.source[0] = b.unit[0] = b.about[0] = 0;
                field("name=", b.name, sizeof b.name);
                field("source=", b.source, sizeof b.source);
                field("\nunit=", b.unit, sizeof b.unit);
                field("\nabout=", b.about, sizeof b.about);
                char num[16] = "";
                field("\nshown=", num, sizeof num);
                if (!num[0]) field("\narticles=", num, sizeof num);
                b.shown = num[0] ? (uint32_t)strtoul(num, nullptr, 10) : 0;
            }
            ++g_bookCount;
        }
    }
    if (root) root.close();
    sdUnlock();
}

// Prefix search in one book, appending to g_results (up to RESULTS in all).
void searchBook(int bi, const char* key) {
    const uint32_t t0 = millis();
    struct Took { uint32_t t; ~Took() { if (millis() - t > 50) Serial.printf("[perf] search %lu ms\n", (unsigned long)(millis() - t)); } } took{t0};
    const Book& b = g_books[bi];
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
    for (uint32_t i = lo, scanned = 0; i < b.count && g_resultCount < RESULTS && scanned < 4000; ++i, ++scanned) {
        if (f.read(rec, REC) != REC) break;
        if (memcmp(rec, want, klen) != 0) break;
        if (rec[114] & 2) continue;            // hidden: profiles, Category: pages (library/hidejunk.py)
        Result& r = g_results[g_resultCount];
        memcpy(r.title, rec + KEY, KEY);
        r.title[KEY] = 0;
        r.chunk = (uint16_t)(rec[112] | rec[113] << 8);
        r.off = rd32(rec + 116);
        r.len = rd32(rec + 120);
        r.book = (int8_t)bi;
        // Redirects share their target: list each article once.
        bool dup = false;
        for (int k = 0; k < g_resultCount; ++k)
            if (g_results[k].book == bi && g_results[k].chunk == r.chunk && g_results[k].off == r.off) dup = true;
        if (!dup) ++g_resultCount;
    }
    f.close();
    sdUnlock();
}

// Prefix search over the books in `mask` (bit per book), in book order.
void search(const char* key, uint32_t mask) {
    g_resultCount = 0;
    for (int bi = 0; bi < g_bookCount && g_resultCount < RESULTS; ++bi)
        if (mask & (1u << bi)) searchBook(bi, key);
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

// Article typography. Reading is the one place the device is not "tactical":
// a book face (Literata, made for reading on screens), ragged right, generous
// leading, ~45 characters a line. Three text sizes; everything scales with the
// body's cap height.
constexpr int N_SIZES = 3;
constexpr float kCap[N_SIZES] = {14.f, 16.f, 19.f};   // body cap height, px
const char* const kSizeName[N_SIZES] = {"TEXT: SMALL", "TEXT: MEDIUM", "TEXT: LARGE"};
int g_size = -1;                                       // -1: not read from NVS yet
int textSize() {
    if (g_size < 0) {
        Preferences p;
        p.begin("doba", true);
        g_size = p.getUChar("txtSize", 1);
        p.end();
        if (g_size >= N_SIZES) g_size = 1;
    }
    return g_size;
}
// cap height of a kind, as a multiple of the body's
float capOf(uint8_t kind) {
    return kind == 'T' ? 1.42f : kind == 'H' ? 1.22f : kind == 'S' ? 1.06f : kind == 'F' ? 0.84f : 1.f;
}
const EpdFont* fontFor(uint8_t kind) {
#ifdef VFONT
    const float c = kCap[textSize()] * capOf(kind);
    const bool bold = kind == 'T' || kind == 'H' || kind == 'S';
    return vf_face_cap(bold ? &Literata_700 : &Literata_450, c);
#else
    return (kind == 'T' || kind == 'H') ? ui::font(ui::F_BODY_H)
         : kind == 'S' ? ui::font(ui::F_KEY) : kind == 'F' ? ui::font(ui::F_TINY) : ui::font(ui::F_BODY);
#endif
}
constexpr int MARGIN = 30, BULLET_IN = 26;
int capPx(uint8_t kind) { return (int)(kCap[textSize()] * capOf(kind) + 0.5f); }
// Leading: 2.2 caps for running text (about 1.5 em), tighter for headings.
int lineHeight(uint8_t kind) {
    const float k = kind == 'T' || kind == 'H' ? 1.95f : kind == 'S' ? 2.05f : kind == 'F' ? 2.15f : 2.25f;
    return (int)(kCap[textSize()] * capOf(kind) * k + 0.5f);
}
// Baseline's height above the bottom of the line box: room for descenders,
// the rest of the leading above the caps.
int baselineOf(uint8_t kind) { return (int)((lineHeight(kind) - capPx(kind)) * 0.40f + 0.5f); }
int paraGap() { return (int)(kCap[textSize()] * 0.8f + 0.5f); }

// A picture line: "I<name> <w> <h>"; the pixels are in <book>/img/<name>.dgi.
constexpr int IMG_PAD = 10, IMG_MAX_H = 640;
bool imageLine(const char* s, size_t n, char* name, size_t cap, int& w, int& h) {
    char tmp[64];
    if (n >= sizeof tmp) return false;
    memcpy(tmp, s, n);
    tmp[n] = 0;
    char nm[32];
    if (sscanf(tmp, "%31s %d %d", nm, &w, &h) != 3 || w <= 0 || h <= 0) return false;
    if (h > IMG_MAX_H) h = IMG_MAX_H;
    snprintf(name, cap, "%s", nm);
    return true;
}
int heightOf(const struct Line& l);

int widthOf(const EpdFont* f, const char* s, size_t n) {
    char tmp[512];
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

bool firstLineOf(const Line& l) { return l.off > 0 && (uint8_t)g_text[l.off - 1] == l.kind; }
bool lastLineOf(const Line& l) { return l.off + l.len >= g_textLen || g_text[l.off + l.len] == '\n'; }
int topGap(const Line& l) {
    if (!firstLineOf(l)) return 0;
    const float c = kCap[textSize()];
    return (int)(l.kind == 'T' ? c * 0.6f : l.kind == 'H' ? c * 1.4f : l.kind == 'S' ? c * 0.9f
                 : l.kind == 'F' ? c * 0.5f : 0);
}

// Air under a heading's last line, so it binds to the text it introduces.
int afterGap(const Line& l) {
    if ((l.kind != 'T' && l.kind != 'H' && l.kind != 'S') || !lastLineOf(l)) return 0;
    return (int)(kCap[textSize()] * (l.kind == 'S' ? 0.3f : 0.5f));
}

int heightOf(const Line& l) {
    if (l.kind == 'G') return paraGap();
    if (l.kind == 'I') {
        char nm[32];
        int w, h;
        return imageLine(g_text + l.off, l.len, nm, sizeof nm, w, h) ? h + 2 * IMG_PAD : 0;
    }
    return topGap(l) + lineHeight(l.kind) + afterGap(l);
}

// Draws <book>/img/<name>.dgi centred at y: "DGI1", u16 w, u16 h, then zlib of
// 4-bit greys (0 black .. 15 white), two pixels a byte, rows padded to bytes.
// A picture's bytes from the book's packs: img/index.dgx (sorted: name[10],
// u16 pack, u32 offset, u32 length after "DGX1" u32 count), img/NN.dgp.
// Big books keep their pictures this way — millions of small files would cost
// a cluster each on the card. Caller holds the SD lock.
uint8_t* fromPack(const char* name, size_t& len) {
    char path[128];
    snprintf(path, sizeof path, "%s/%s/img/index.dgx", kRoot, g_books[g_book].dir);
    File ix = SD.open(path, FILE_READ);
    if (!ix) return nullptr;
    uint8_t hdr[8];
    if (ix.read(hdr, 8) != 8 || memcmp(hdr, "DGX1", 4) != 0) { ix.close(); return nullptr; }
    uint32_t lo = 0, hi = hdr[4] | hdr[5] << 8 | hdr[6] << 16 | (uint32_t)hdr[7] << 24;
    char key[10];
    memset(key, 0, sizeof key);
    memcpy(key, name, strnlen(name, 10));
    uint8_t rec[20];
    bool found = false;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        ix.seek(8 + (size_t)mid * 20);
        if (ix.read(rec, 20) != 20) break;
        const int c = memcmp(rec, key, 10);
        if (c == 0) { found = true; break; }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    ix.close();
    if (!found) return nullptr;
    const unsigned pack = rec[10] | rec[11] << 8;
    const uint32_t off = rec[12] | rec[13] << 8 | rec[14] << 16 | (uint32_t)rec[15] << 24;
    len = rec[16] | rec[17] << 8 | rec[18] << 16 | (uint32_t)rec[19] << 24;
    if (len < 9 || len > 4u << 20) return nullptr;
    snprintf(path, sizeof path, "%s/%s/img/%02u.dgp", kRoot, g_books[g_book].dir, pack);
    File f = SD.open(path, FILE_READ);
    if (!f) return nullptr;
    uint8_t* z = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    const bool ok = z && f.seek(off) && f.read(z, len) == len;
    f.close();
    if (!ok && z) { heap_caps_free(z); z = nullptr; }
    return z;
}

void drawImage(const char* name, int y, int maxH, uint8_t* fb) {
    char path[128];
    sdLock();
    size_t zlen = 0;
    uint8_t* z = fromPack(name, zlen);
    bool ok = z != nullptr;
    if (!z) {
        // loose files: small books, and big ones converted before the packs
        if (strlen(name) >= 8)   // sharded: img/ab/abcdef0123.dgi
            snprintf(path, sizeof path, "%s/%s/img/%.2s/%s.dgi", kRoot, g_books[g_book].dir, name, name);
        else
            snprintf(path, sizeof path, "%s/%s/img/%s.dgi", kRoot, g_books[g_book].dir, name);
        File f = SD.open(path, FILE_READ);
        zlen = f ? f.size() : 0;
        z = zlen > 8 ? (uint8_t*)heap_caps_malloc(zlen, MALLOC_CAP_SPIRAM) : nullptr;
        ok = z && f.read(z, zlen) == zlen;
        if (f) f.close();
    }
    sdUnlock();
    if (!ok || memcmp(z, "DGI1", 4) != 0) {
        if (z) heap_caps_free(z);
        epd_fill_rect({W / 2 - 60, y, 120, 2}, INK, fb);   // missing: a mark, not a hole
        Serial.printf("[lib] picture %s missing\n", name);
        return;
    }
    const int w = z[4] | (z[5] << 8), h = z[6] | (z[7] << 8);
    const size_t stride = (size_t)(w + 1) / 2, need = stride * h;
    uint8_t* px = (uint8_t*)heap_caps_malloc(need + 1, MALLOC_CAP_SPIRAM);
    size_t got = 0;
    ok = px && inflateTo(z + 8, zlen - 8, (char*)px, need, &got) && got >= need;
    heap_caps_free(z);
    if (ok) {
        const int x0 = (W - w) / 2;
        const int rows = h < maxH ? h : maxH;
        for (int r = 0; r < rows; ++r) {
            const uint8_t* row = px + stride * r;
            for (int c = 0; c < w; ++c) {
                const uint8_t v = (c & 1) ? (row[c >> 1] & 0x0F) : (row[c >> 1] >> 4);
                if (v != 15) epd_draw_pixel(x0 + c, y + r, v * 17, fb);   // white is the paper
            }
        }
    }
    if (ok) Serial.printf("[lib] picture %s %dx%d\n", name, w, h);
    if (px) heap_caps_free(px);
}

void buildToc();
void layout() {
    g_lineCount = 0;
    const int maxW = W - 2 * MARGIN;
    size_t i = 0;
    while (i < g_textLen) {
        size_t e = i;
        while (e < g_textLen && g_text[e] != '\n') ++e;
        const uint8_t kind = (uint8_t)g_text[i];
        const size_t start = i + 1;
        if (kind == 'I') {   // a picture: one line, its own height
            if (e > start) addLine((uint32_t)start, (uint16_t)(e - start), 'I');
            i = e + 1;
            continue;
        }
        const EpdFont* f = fontFor(kind);
        const int bulletW = kind == 'B' ? BULLET_IN : 0;
        const int spaceW = widthOf(f, "a a", 3) - widthOf(f, "aa", 2);
        size_t ls = start, p = start;
        int lw = 0;
        while (p < e) {
            size_t we = p;
            while (we < e && g_text[we] != ' ') ++we;
            int ww = widthOf(f, g_text + p, we - p);
            // A word wider than the column (a URL, a run of letters) is cut.
            if (ww > maxW - bulletW) {
                if (p != ls) { addLine((uint32_t)ls, (uint16_t)(p - 1 - ls), kind); ls = p; lw = 0; }
                size_t cut = p;
                while (cut < we) {
                    size_t nx = cut + 1;
                    while (nx < we && ((uint8_t)g_text[nx] & 0xC0) == 0x80) ++nx;
                    if (widthOf(f, g_text + p, nx - p) > maxW - bulletW && cut > p) break;
                    cut = nx;
                }
                if (cut < we) {
                    addLine((uint32_t)p, (uint16_t)(cut - p), kind);
                    ls = p = cut;
                    lw = 0;
                    continue;
                }
                ww = widthOf(f, g_text + p, we - p);
            }
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
    const int bodyTop = TOP + 66, bodyBottom = H - FOOT_H - 6;
    int y = bodyTop, k = 0;
    g_pageStarts[g_pageCount++] = 0;
    for (k = 0; k < g_lineCount; ++k) {
        const int h = heightOf(g_lines[k]);
        if (y + h > bodyBottom && g_pageCount < MAX_PAGES) {
            g_pageStarts[g_pageCount++] = k;
            y = bodyTop;
        }
        y += h;
    }
    buildToc();
}

// \x01 visible \x02 target \x03 -> visible, and a Link. In place: the text
// only gets shorter.
void extractLinks() {
    g_linkCount = 0;
    g_targetsLen = 0;
    if (!g_links) g_links = (Link*)heap_caps_malloc(MAX_LINKS * sizeof(Link), MALLOC_CAP_SPIRAM);
    if (!g_targets) g_targets = (char*)heap_caps_malloc(TARGETS_CAP, MALLOC_CAP_SPIRAM);
    size_t w = 0;
    Link cur = {};
    int state = 0;   // 0 text, 1 visible, 2 target
    for (size_t i = 0; i < g_textLen; ++i) {
        const char c = g_text[i];
        if (c == '\x01' && state == 0) { cur.start = (uint32_t)w; state = 1; continue; }
        if (c == '\x02' && state == 1) {
            cur.end = (uint32_t)w;
            cur.target = (uint32_t)g_targetsLen;
            state = 2;
            continue;
        }
        if (state == 2) {
            if (c == '\x03') {
                if (g_targets && g_targetsLen < TARGETS_CAP) g_targets[g_targetsLen++] = 0;
                if (g_links && g_targets && g_linkCount < MAX_LINKS && g_targetsLen <= TARGETS_CAP && cur.end > cur.start)
                    g_links[g_linkCount++] = cur;
                state = 0;
            } else if (g_targets && g_targetsLen < TARGETS_CAP - 1) {
                g_targets[g_targetsLen++] = c;
            }
            continue;
        }
        if (c == '\n') state = 0;   // a link never crosses a line
        if (c == '\x01' || c == '\x02' || c == '\x03') continue;
        g_text[w++] = c;
    }
    g_textLen = w;
    g_text[w] = 0;
}

void addRecent(const char* dir, const char* title);
bool openArticle(const Result& r) {
    const uint32_t t0 = millis();
    g_note = nullptr;
    if (r.book >= 0 && r.book < g_bookCount) g_book = r.book;
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
    extractLinks();
    // Every article names where it came from (info.txt source=, else the book).
    const char* src = b.source[0] ? b.source : b.name;
    const int add = snprintf(g_text + g_textLen, MAX_TEXT + 1 - g_textLen, "\nFSource: %s", src);
    if (add > 0 && g_textLen + add <= MAX_TEXT) g_textLen += add;
    g_text[g_textLen] = 0;
    snprintf(g_title, sizeof g_title, "%s", r.title);
    g_current = r;
    g_toc = false;
    layout();
    g_page = 0;
    Serial.printf("[lib] open '%s' pages=%d links=%d in %lu ms\n", g_title, g_pageCount, g_linkCount,
                  (unsigned long)(millis() - t0));
    addRecent(b.dir, r.title);
    return true;
}

// --- recent: what was read last, newest first (/doba/recent.txt "book|title") -------
constexpr int MAX_RECENT = 30;
struct Recent { char dir[32]; char title[KEY + 1]; };
Recent g_recent[MAX_RECENT];
int g_recentCount = -1;      // -1: not loaded
const char* kRecentPath = "/doba/recent.txt";
void loadRecent() {
    if (g_recentCount >= 0) return;
    g_recentCount = 0;
    sdLock();
    File f = SD.open(kRecentPath, FILE_READ);
    if (f) {
        char line[120];
        size_t n = 0;
        for (int c = f.read();; c = f.read()) {
            if (c < 0 || c == '\n') {
                line[n] = 0;
                char* bar = strchr(line, '|');
                if (bar && g_recentCount < MAX_RECENT) {
                    *bar = 0;
                    Recent& r = g_recent[g_recentCount++];
                    snprintf(r.dir, sizeof r.dir, "%s", line);
                    snprintf(r.title, sizeof r.title, "%s", bar + 1);
                }
                n = 0;
                if (c < 0) break;
            } else if (n + 1 < sizeof line) line[n++] = (char)c;
        }
        f.close();
    }
    sdUnlock();
}
void addRecent(const char* dir, const char* title) {
    loadRecent();
    int at = g_recentCount < MAX_RECENT ? g_recentCount : MAX_RECENT - 1;
    for (int i = 0; i < g_recentCount; ++i)
        if (!strcmp(g_recent[i].dir, dir) && !strcmp(g_recent[i].title, title)) { at = i; break; }
    if (at == 0 && g_recentCount > 0 && !strcmp(g_recent[0].title, title)) return;
    for (int i = at; i > 0; --i) g_recent[i] = g_recent[i - 1];
    snprintf(g_recent[0].dir, sizeof g_recent[0].dir, "%s", dir);
    snprintf(g_recent[0].title, sizeof g_recent[0].title, "%s", title);
    if (at == g_recentCount) ++g_recentCount;
    sdLock();
    File f = SD.open(kRecentPath, FILE_WRITE);
    if (f) {
        for (int i = 0; i < g_recentCount; ++i) { f.print(g_recent[i].dir); f.print("|"); f.print(g_recent[i].title); f.print("\n"); }
        f.close();
    }
    sdUnlock();
}

// --- search screen geometry -------------------------------------------------------

// --- sections: the library's "what do you need?" -----------------------------------
//
// /doba/lib/sections.txt (made by library/sections.py, plain text, editable):
//   S|id|NAME|icon|book,book,...           a section and the books it searches
//   E|id|*|Shown title|book|Article title|one line on what it's for
//                                         an entry ('*' = START HERE)
//   X|LABEL|icon|book|Article title       an EMERGENCY button (up to 3)
// Without the file the sections are still there, searching every book.

constexpr int MAX_SECTIONS = 15, MAX_ENTRIES = 480, MAX_EMERGENCY = 3;
struct Section { char id[16]; char name[24]; doba_icons::Icon icon; uint32_t books; int first, n, starts; };
struct Entry { int8_t sec, book; bool start; char label[48]; char desc[80]; char title[KEY + 1]; };
struct Emergency { char label[20]; doba_icons::Icon icon; int8_t book; char title[KEY + 1]; };
Section g_secs[MAX_SECTIONS];
int g_secCount = 0;
Entry* g_entries = nullptr;
int g_entryCount = 0;
Emergency g_emerg[MAX_EMERGENCY];
int g_emergCount = 0;

int bookIndex(const char* dir) {
    for (int i = 0; i < g_bookCount; ++i)
        if (!strcmp(g_books[i].dir, dir)) return i;
    return -1;
}

void defaultSections() {
    static const struct { const char* id; const char* name; const char* icon; } kDef[] = {
        {"firstaid", "FIRST AID", "firstaid"}, {"water", "WATER", "water"},   {"fire", "FIRE", "fire"},
        {"shelter", "SHELTER", "shelter"},     {"food", "FOOD", "food"},      {"grow", "GROWING", "grow"},
        {"power", "POWER", "power"},           {"nav", "NAVIGATION", "nav"},
        {"hazards", "HAZARDS", "hazard"},      {"weather", "WEATHER", "weather"}, {"tools", "TOOLS", "tools"},
        {"knots", "KNOTS", "knots"},           {"comms", "COMMS", "comms"},   {"reference", "REFERENCE", "books"},
    };
    g_secCount = 0;
    for (const auto& d : kDef) {
        Section& x = g_secs[g_secCount++];
        snprintf(x.id, sizeof x.id, "%s", d.id);
        snprintf(x.name, sizeof x.name, "%s", d.name);
        x.icon = doba_icons::byName(d.icon);
        x.books = 0xFFFFFFFFu;
        x.first = x.n = x.starts = 0;
    }
}

// Split `line` on '|' into at most n fields (in place).
int fields(char* line, char** f, int n) {
    int k = 0;
    f[k++] = line;
    for (char* c = line; *c && k < n; ++c)
        if (*c == '|') { *c = 0; f[k++] = c + 1; }
    return k;
}

void loadSections() {
    defaultSections();
    g_entryCount = g_emergCount = 0;
    if (!g_entries) g_entries = (Entry*)heap_caps_malloc(sizeof(Entry) * MAX_ENTRIES, MALLOC_CAP_SPIRAM);
    char path[64];
    snprintf(path, sizeof path, "%s/sections.txt", kRoot);
    sdLock();
    File f = SD.open(path, FILE_READ);
    if (!f) { sdUnlock(); return; }
    bool fresh = false;
    char line[400];
    size_t k = 0;
    auto take = [&]() {
        line[k] = 0;
        k = 0;
        if (!line[0] || line[0] == '#') return;
        char* fl[8];
        const int n = fields(line, fl, 8);
        if (fl[0][0] == 'S' && n >= 5) {
            if (!fresh) { g_secCount = 0; fresh = true; }   // the file replaces the defaults
            if (g_secCount >= MAX_SECTIONS) return;
            Section& x = g_secs[g_secCount++];
            snprintf(x.id, sizeof x.id, "%s", fl[1]);
            snprintf(x.name, sizeof x.name, "%s", fl[2]);
            x.icon = doba_icons::byName(fl[3]);
            x.books = 0;
            for (char* t = strtok(fl[4], ","); t; t = strtok(nullptr, ",")) {
                const int bi = bookIndex(t);
                if (bi >= 0) x.books |= 1u << bi;
            }
            if (!x.books) x.books = 0xFFFFFFFFu;
            x.first = g_entryCount;
            x.n = x.starts = 0;
        } else if (fl[0][0] == 'E' && n >= 7 && g_entries && g_entryCount < MAX_ENTRIES) {
            int si = -1;
            for (int i = 0; i < g_secCount; ++i) if (!strcmp(g_secs[i].id, fl[1])) si = i;
            const int bi = bookIndex(fl[4]);
            if (si < 0 || bi < 0 || si != g_secCount - 1) return;   // entries follow their section
            Entry& e = g_entries[g_entryCount++];
            e.sec = (int8_t)si;
            e.book = (int8_t)bi;
            e.start = fl[2][0] == '*';
            snprintf(e.label, sizeof e.label, "%s", fl[3]);
            snprintf(e.title, sizeof e.title, "%s", fl[5]);
            snprintf(e.desc, sizeof e.desc, "%s", fl[6]);
            g_secs[si].n++;
            if (e.start) g_secs[si].starts++;
        } else if (fl[0][0] == 'X' && n >= 5 && g_emergCount < MAX_EMERGENCY) {
            const int bi = bookIndex(fl[3]);
            if (bi < 0) return;
            Emergency& x = g_emerg[g_emergCount++];
            snprintf(x.label, sizeof x.label, "%s", fl[1]);
            x.icon = doba_icons::byName(fl[2]);
            x.book = (int8_t)bi;
            snprintf(x.title, sizeof x.title, "%s", fl[4]);
        }
    };
    while (f.available()) {
        const int c = f.read();
        if (c < 0) break;
        if (c == '\n' || c == '\r') { if (k) take(); continue; }
        if (k < sizeof line - 1) line[k++] = (char)c;
    }
    if (k) take();
    f.close();
    sdUnlock();
}

// --- views: HOME (sections), SECTION, SEARCH ----------------------------------------

enum View { V_HOME, V_SECTION, V_SEARCH, V_SHELF, V_BOOK, V_LETTER, V_RECENT };
// V_HOME is the TOPICS tab; the library opens on its books (V_SHELF).
View g_view = V_SHELF;
int g_sec = 0;            // the open section
int g_secPage = 0;        // its list page
int g_scope = -1;         // search scope: -1 all books, else a section
int g_scopeBook = -1;     // or one book
View g_searchFrom = V_HOME;

constexpr int EM_Y = TOP + 194, EM_H = 108;
int gridTop() { return g_emergCount ? EM_Y + 28 + EM_H + 18 : TOP + 200; }
int gridRows() { return g_secCount > 12 ? 5 : 4; }
EpdRect cellRect(int i) {
    const int rows = gridRows();
    const int top = gridTop(), gap = 12, cw = (W - 48 - 2 * gap) / 3;
    const int ch = (H - 24 - top - (rows - 1) * gap) / rows;
    return {24 + (i % 3) * (cw + gap), top + (i / 3) * (ch + gap), cw, ch};
}
EpdRect emergRect(int i) {
    const int gap = 12, w = (W - 48 - (g_emergCount - 1) * gap) / g_emergCount;
    return {24 + i * (w + gap), EM_Y + 28, w, EM_H};
}

// --- BOOKS: the shelf, a book's page, its lists ----------------------------------
//
// The library opens on its books: a cover each (a monogram on a spine, the
// name, how much is inside). A book opens to search-in-this-book and its way
// in: the documents it holds (manuals, PDFs) or, for the big ones, A-Z.
// TOPICS, the survival sections, is the other tab.

struct BookLook { const char* dir; const char* mono; const char* name; };
const BookLook kLooks[] = {
    {"wiki_en", "W", "Wikipedia"},        {"wiki_uk", "УКР", "Вікіпедія"},
    {"med_en", "+", "Medicine"},          {"medicine", "Rx", "Field medicine"},
    {"gov", "US", "US manuals"},          {"disaster", "SOS", "After disaster"},
    {"water", "H2O", "Water"},            {"food", "Fd", "Food"},
    {"knots", "Kn", "Knots"},             {"ifixit", "iFx", "iFixit repair"},
    {"diy_se", "DIY", "Home repair Q&A"}, {"appropedia", "AT", "Appropedia"},
    {"garden_se", "Gr", "Gardening Q&A"}, {"chem_se", "Ch", "Chemistry Q&A"},
    {"wood_se", "Wd", "Woodwork Q&A"},    {"shelf_en", "Lib", "Gutenberg"},
    {"shelf_uk", "Дж", "Вікіджерела"},    {"mine", "Me", "My files"},
};
constexpr int N_LOOKS = sizeof(kLooks) / sizeof(kLooks[0]);

int lookOf(int bi) {
    for (int i = 0; i < N_LOOKS; ++i) if (!strcmp(kLooks[i].dir, g_books[bi].dir)) return i;
    return -1;
}
const char* shortName(int bi) { const int l = lookOf(bi); return l >= 0 ? kLooks[l].name : g_books[bi].name; }
void monogram(int bi, char* out, size_t n) {
    const int l = lookOf(bi);
    if (l >= 0) { snprintf(out, n, "%s", kLooks[l].mono); return; }
    // unknown book: the first two letters of its name
    const char* s = g_books[bi].name;
    char a[8] = {}, b[8] = {};
    const char* p = s;
    uint32_t c1 = decodeUtf8(p), c2 = decodeUtf8(p);
    encodeUtf8(c1, a);
    if (c2 > ' ') encodeUtf8(c2, b);
    snprintf(out, n, "%s%s", a, b);
}
// Shelf order: the table's, unknown books after.
int g_order[MAX_BOOKS];
void sortShelf() {
    int n = 0;
    for (int l = 0; l < N_LOOKS; ++l)
        for (int bi = 0; bi < g_bookCount; ++bi)
            if (!strcmp(kLooks[l].dir, g_books[bi].dir)) g_order[n++] = bi;
    for (int bi = 0; bi < g_bookCount; ++bi)
        if (lookOf(bi) < 0) g_order[n++] = bi;
}
// Cut `t` to `w` px with "..." (whole UTF-8 characters).
void fitText(const EpdFont* f, char* t, int w) {
    if (ui::textWidth(f, t) <= w) return;
    size_t n = strlen(t);
    while (n > 1) {
        --n;
        while (n > 0 && ((uint8_t)t[n] & 0xC0) == 0x80) --n;
        while (n > 0 && t[n - 1] == ' ') --n;
        char keep[4];
        memcpy(keep, t + n, 4);
        memcpy(t + n, "...", 4);
        if (ui::textWidth(f, t) <= w) return;
        memcpy(t + n, keep, 4);
        t[n] = 0;
    }
}
void countText(uint32_t c, char* out, size_t n) {
    if (c >= 1000000) snprintf(out, n, "%.1fM", c / 1e6);
    else if (c >= 10000) snprintf(out, n, "%luk", (unsigned long)(c / 1000));
    else snprintf(out, n, "%lu", (unsigned long)c);
}

// The library's head (the design canvas): a big "Library", the tabs as words
// underlined when chosen, the search box under them.
const EpdRect kTabs = {24, TOP + 64, W - 48, 50};
const EpdRect kAllSearch = {24, TOP + 122, W - 48, 56};
constexpr int SHELF_TOP = TOP + 204, COLS = 3, ROWS = 2, CELL_GAP = 22, COVER_H = 150, CELL_H = 212;
int g_shelfPage = 0;
EpdRect coverRect(int i) {   // the cover; its name and count go under it
    const int cw = (W - 48 - (COLS - 1) * CELL_GAP) / COLS;
    const int k = i % (COLS * ROWS);
    return {24 + (k % COLS) * (cw + CELL_GAP), SHELF_TOP + (k / COLS) * (CELL_H + 22), cw, COVER_H};
}
int shelfPages() { return (g_bookCount + COLS * ROWS - 1) / (COLS * ROWS); }

const char* const kTabNames[3] = {"Books", "Topics", "Recent"};
int tabX(int t) {
    int x = 28;
    for (int i = 0; i < t; ++i) x += ui::textWidth(ui::font(ui::F_LABEL), kTabNames[i]) + 32;
    return x;
}
int tabAt(int x) {
    for (int t = 2; t > 0; --t) if (x >= tabX(t) - 16) return t;
    return 0;
}
void libHead(int which, uint8_t* fb) {
    ui::text(ui::font(ui::F_H1), 24, TOP + 44, "Library", fb, EPD_DRAW_ALIGN_LEFT, INK);
    for (int t = 0; t < 3; ++t) {
        const int x = tabX(t), w = ui::textWidth(ui::font(ui::F_LABEL), kTabNames[t]);
        ui::text(ui::font(ui::F_LABEL), x, TOP + 96, kTabNames[t], fb, EPD_DRAW_ALIGN_LEFT, t == which ? INK : ui::GREY);
        if (t == which) epd_fill_rect({x, TOP + 106, w, 3}, INK, fb);
    }
}
void tabs(int which, uint8_t* fb) { libHead(which, fb); }
void searchBar(const EpdRect& r, const char* label, uint8_t* fb) {
    ui::frameRound(r, 8, 2, fb, 0x90, PAPER);
    const int cx = r.x + 30, cy = r.y + r.height / 2 - 2;
    epd_draw_circle(cx, cy, 8, INK, fb);
    epd_draw_circle(cx, cy, 9, INK, fb);
    for (int t = 0; t < 3; ++t) epd_draw_line(cx + 6 + t, cy + 7, cx + 13 + t, cy + 14, INK, fb);
    char t[80];
    snprintf(t, sizeof t, "%s", label);
    fitText(ui::font(ui::F_SMALL), t, r.width - 70);
    ui::text(ui::font(ui::F_SMALL), r.x + 54, r.y + r.height / 2 + 8, t, fb, EPD_DRAW_ALIGN_LEFT, 0x40);
}

// A cover: a spine, the monogram big, the name, the count.
void cover(int bi, const EpdRect& r, uint8_t* fb, bool label = true) {
    ui::frameRound(r, 3, 2, fb);
    epd_fill_rect({r.x, r.y, 9, r.height}, INK, fb);
    char mono[16], cnt[16];
    monogram(bi, mono, sizeof mono);
    // the monogram in the book face: it has Cyrillic, and reads as a cover
#ifdef VFONT
    const EpdFont* mf = vf_face_cap(&Literata_700, 34.f);
    for (float sz = 32.f; sz >= 18.f && ui::textWidth(mf, mono) > r.width - 30; sz -= 2.f)
        mf = vf_face_cap(&Literata_700, sz);
#else
    const EpdFont* mf = ui::font(ui::F_TITLE);
#endif
    ui::text(mf, r.x + 5 + r.width / 2, r.y + r.height / 2 + 17, mono, fb, EPD_DRAW_ALIGN_CENTER, INK);
    if (!label) return;
    // under the cover: the name, then the count in grey
    char nm[48];
    snprintf(nm, sizeof nm, "%s", shortName(bi));
    fitText(ui::font(ui::F_STRONG), nm, r.width);
    ui::text(ui::font(ui::F_STRONG), r.x, r.y + r.height + 28, nm, fb, EPD_DRAW_ALIGN_LEFT, INK);
    const Book& b = g_books[bi];
    countText(b.shown ? b.shown : b.count, cnt, sizeof cnt);
    char line[40];
    snprintf(line, sizeof line, "%s %s", cnt, b.unit[0] ? b.unit : "articles");
    fitText(ui::font(ui::F_TINY), line, r.width);
    ui::text(ui::font(ui::F_TINY), r.x, r.y + r.height + 52, line, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
}

void pagerRow(int page, int pages, uint8_t* fb);
void renderShelf(uint8_t* fb) {
    libHead(0, fb);
    char lbl[48];
    snprintf(lbl, sizeof lbl, "Search %d books", g_bookCount);
    searchBar(kAllSearch, lbl, fb);
    sortShelf();
    const int pages = shelfPages(), per = COLS * ROWS;
    if (g_shelfPage >= pages) g_shelfPage = pages - 1;
    for (int k = 0; k < per; ++k) {
        const int i = g_shelfPage * per + k;
        if (i >= g_bookCount) break;
        cover(g_order[i], coverRect(k), fb);
    }
    pagerRow(g_shelfPage, pages, fb);
}
int shelfHit(int x, int y) {   // book index, -1 nothing
    const int per = COLS * ROWS;
    for (int k = 0; k < per; ++k) {
        EpdRect r = coverRect(k);
        r.height = CELL_H;
        if (!hit(r, x, y)) continue;
        const int i = g_shelfPage * per + k;
        return i < g_bookCount ? g_order[i] : -1;
    }
    return -1;
}

// A BOOK: its cover and name, search inside, then documents or A-Z.
int g_open = 0;                 // the book on its page
constexpr int MAX_DOCS = 300;
struct Doc { char title[KEY + 1]; char raw[KEY + 1]; uint32_t rec; };
Doc* g_docs = nullptr;
int g_docCount = -1;            // -1: not looked for (this book)
int g_docBook = -1;
const EpdRect kBookSearch = {24, TOP + 104, W - 48, 56};
constexpr int BODY_Y = TOP + 214;     // under the search box (and the A-Z / random links)
const EpdRect kListPrev = {W - 24 - 56 - 12 - 56, H - 78, 56, 52};
const EpdRect kListNext = {W - 24 - 56, H - 78, 56, 52};
constexpr int LIST_ROW = 64;

bool readRec(File& f, uint32_t i, uint8_t* rec) {
    return f.seek(HDR + (uint64_t)i * REC) && f.read(rec, REC) == REC;
}
void recResult(const uint8_t* rec, int bi, Result& r) {
    memcpy(r.title, rec + KEY, KEY);
    r.title[KEY] = 0;
    r.chunk = (uint16_t)(rec[112] | rec[113] << 8);
    r.off = rd32(rec + 116);
    r.len = rd32(rec + 120);
    r.book = (int8_t)bi;
}
// Documents: books of manuals (a few thousand entries at most) list the
// documents themselves — titles before " — " — once each, in order.
// A document's name as a file left it: "the-ten-bushcraft-books" -> "The ten bushcraft books".
void tidyDocName(char* t) {
    int dashes = 0;
    bool space = false;
    for (char* c = t; *c; ++c) { dashes += *c == '-' || *c == '_'; space |= *c == ' '; }
    if (dashes >= 2 || (!space && dashes)) {
        for (char* c = t; *c; ++c) if (*c == '-' || *c == '_') *c = ' ';
    }
    size_t n = strlen(t);
    while (n && (t[n - 1] == ' ' || t[n - 1] == '-' || t[n - 1] == '.')) t[--n] = 0;
    char* c = t;
    while (*c == '.' || *c == ' ') ++c;
    if (c != t) memmove(t, c, strlen(c) + 1);
    if (t[0] >= 'a' && t[0] <= 'z') t[0] -= 32;
}
// "2 Water" before "10 Fire": numbers by value, the rest by letters.
int naturalCmp(const char* a, const char* b) {
    while (*a && *b) {
        if (isdigit((uint8_t)*a) && isdigit((uint8_t)*b)) {
            unsigned long x = strtoul(a, (char**)&a, 10), y = strtoul(b, (char**)&b, 10);
            if (x != y) return x < y ? -1 : 1;
            continue;
        }
        const int ca = tolower((uint8_t)*a), cb = tolower((uint8_t)*b);
        if (ca != cb) return ca - cb;
        ++a, ++b;
    }
    return (uint8_t)*a - (uint8_t)*b;
}
// Documents: books of manuals (a few thousand entries at most) list the
// documents themselves — the names before " — " that have chapters — once
// each. Loose entries (headings a converter split off) stay findable by search.
void findDocs(int bi) {
    if (g_docBook == bi && g_docCount >= 0) return;
    g_docBook = bi;
    g_docCount = 0;
    if (g_books[bi].count > 4000) return;
    if (!g_docs) g_docs = (Doc*)heap_caps_malloc(sizeof(Doc) * MAX_DOCS, MALLOC_CAP_SPIRAM);
    if (!g_docs) return;
    char path[96];
    snprintf(path, sizeof path, "%s/%s/index.dti", kRoot, g_books[bi].dir);
    sdLock();
    File f = SD.open(path, FILE_READ);
    uint8_t rec[REC];
    for (uint32_t i = 0; f && i < g_books[bi].count && g_docCount < MAX_DOCS; ++i) {
        if (!readRec(f, i, rec)) break;
        char t[KEY + 1];
        memcpy(t, rec + KEY, KEY);
        t[KEY] = 0;
        char* dash = strstr(t, " \xE2\x80\x94 ");   // " — "
        if (dash) *dash = 0;
        else if (g_books[bi].count > 60) continue;   // a short book lists everything
        bool seen = false;
        for (int k = g_docCount - 1; k >= 0 && k >= g_docCount - 40; --k)
            if (!strcmp(g_docs[k].raw, t)) { seen = true; break; }
        if (seen) continue;
        Doc& d = g_docs[g_docCount++];
        snprintf(d.raw, sizeof d.raw, "%s", t);
        snprintf(d.title, sizeof d.title, "%s", t);
        tidyDocName(d.title);
        d.rec = i;
    }
    if (f) f.close();
    sdUnlock();
    // Too few to be a shelf of documents: A-Z serves better.
    if (g_docCount < 2 && g_books[bi].count > 60) { g_docCount = 0; return; }
    std::sort(g_docs, g_docs + g_docCount, [](const Doc& x, const Doc& y) { return naturalCmp(x.title, y.title) < 0; });
}

bool cyrillicBook(int bi) { return !strcmp(g_books[bi].dir, "wiki_uk") || !strcmp(g_books[bi].dir, "shelf_uk"); }
const char* const kLatin[] = {"A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z","0-9"};
const char* const kUkr[] = {"А","Б","В","Г","Ґ","Д","Е","Є","Ж","З","И","І","Ї","Й","К","Л","М","Н","О","П","Р","С","Т","У","Ф","Х","Ц","Ч","Ш","Щ","Ю","Я","0-9"};
int letterCount(int bi) { return cyrillicBook(bi) ? 33 : 27; }
const char* letterAt(int bi, int i) { return cyrillicBook(bi) ? kUkr[i] : kLatin[i]; }
EpdRect letterRect(int bi, int i) {
    const int cols = 6, rows = (letterCount(bi) + cols - 1) / cols, gap = 10;
    const int cw = (W - 48 - (cols - 1) * gap) / cols;
    const int ch = std::min(80, (H - 24 - BODY_Y - (rows - 1) * gap) / rows);
    return {24 + (i % cols) * (cw + gap), BODY_Y + (i / cols) * (ch + gap), cw, ch};
}

void renderBookHead(int bi, uint8_t* fb) {
    char t[48];
    snprintf(t, sizeof t, "%s", shortName(bi));
    fitText(ui::font(ui::F_H1), t, W - 48);
    ui::text(ui::font(ui::F_H1), 24, TOP + 44, t, fb, EPD_DRAW_ALIGN_LEFT, INK);
    char cnt[16], sub[160];
    const Book& b = g_books[bi];
    countText(b.shown ? b.shown : b.count, cnt, sizeof cnt);
    snprintf(sub, sizeof sub, "%s %s%s%s", cnt, b.unit[0] ? b.unit : "articles", b.about[0] ? " · " : "", b.about);
    fitText(ui::font(ui::F_SMALL), sub, W - 48);
    ui::text(ui::font(ui::F_SMALL), 24, TOP + 80, sub, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
}

int g_listPage = 0;
int listRows() { return (kListPrev.y - 8 - BODY_Y) / LIST_ROW; }
void pagerRow(int page, int pages, uint8_t* fb) {
    if (pages <= 1) return;
    char p[24];
    snprintf(p, sizeof p, "%d of %d", page + 1, pages);
    ui::text(ui::font(ui::F_SMALL), 28, kListPrev.y + 34, p, fb, EPD_DRAW_ALIGN_LEFT, 0x40);
    auto btn = [&](const EpdRect& r, bool live, bool left) {
        const uint8_t c = live ? INK : 0xA0;
        ui::frameRound(r, 8, 2, fb, c, PAPER);
        const int cx = r.x + r.width / 2, cy = r.y + r.height / 2, d = left ? 1 : -1;
        for (int t = -1; t <= 1; ++t) {
            epd_draw_line(cx + d * 5 + t, cy - 10, cx - d * 5 + t, cy, c, fb);
            epd_draw_line(cx - d * 5 + t, cy, cx + d * 5 + t, cy + 10, c, fb);
        }
    };
    btn(kListPrev, page > 0, true);
    btn(kListNext, page + 1 < pages, false);
}
void listRow(int y, const char* title, uint8_t* fb) {
    char t[KEY + 8];
    snprintf(t, sizeof t, "%s", title);
    fitText(ui::font(ui::F_LABEL), t, W - 90);
    ui::text(ui::font(ui::F_LABEL), 24, y + 40, t, fb, EPD_DRAW_ALIGN_LEFT, INK);
    ui::text(ui::font(ui::F_KEY), W - 26, y + 42, "\xE2\x80\xBA", fb, EPD_DRAW_ALIGN_RIGHT, 0x80);   // ›
    epd_fill_rect({24, y + LIST_ROW - 1, W - 48, 1}, ui::HAIR, fb);
}

// --- a book's first page: its menu ------------------------------------------------
//
// Like Kiwix opening a ZIM on its main page, or a printed book on its contents:
// <book>/menu.txt (library/bookmenu.py, docs2doba.py) lists what the book is
// for, by group:
//   # GROUP                 a header
//   Title|what it is        a row: the article's title, a line under it
// Without one: the book's picks.txt, the topics' titles from it, its documents
// ("Doc — chapter" titles), and for a big book A-Z and a random page.
bool bigBook(int bi) { return g_books[bi].count > 20000; }
bool g_bookAZ = false;          // the A-Z grid is up (from the book's page)
constexpr int MAX_MENU = 480;
struct MenuItem { char title[160]; char sub[100]; bool header; int32_t rec; uint8_t cut; };   // cut: line 2 starts (0: one line)
MenuItem* g_menu = nullptr;
int g_menuCount = 0, g_menuBook = -1;
bool g_menuPicks = false;        // the menu came from picks/topics (a big book's)

void addMenu(const char* title, const char* sub, bool header, int32_t rec = -1) {
    if (g_menuCount >= MAX_MENU || !title[0]) return;
    if (!header)
        for (int k = 0; k < g_menuCount; ++k)
            if (!g_menu[k].header && !strcmp(g_menu[k].title, title)) return;
    MenuItem& m = g_menu[g_menuCount++];
    snprintf(m.title, sizeof m.title, "%s", title);
    snprintf(m.sub, sizeof m.sub, "%s", sub ? sub : "");
    m.header = header;
    m.rec = rec;
    m.cut = 0;
    // A long title (a question) wraps once, at a space.
    const EpdFont* f = ui::font(ui::F_LABEL);
    if (!header && ui::textWidth(f, m.title) > W - 90) {
        int best = 0;
        char t[160];
        for (int i = 1; m.title[i] && i < 150; ++i) {
            if (m.title[i] != ' ') continue;
            memcpy(t, m.title, i);
            t[i] = 0;
            if (ui::textWidth(f, t) <= W - 90) best = i; else break;
        }
        m.cut = (uint8_t)(best ? best + 1 : 0);
    }
}
// Lines of a small text file, one call each.
template <typename F> bool eachLine(const char* path, F fn) {
    sdLock();
    File f = SD.open(path, FILE_READ);
    if (!f) { sdUnlock(); return false; }
    char line[300];
    size_t n = 0;
    for (int c = f.read();; c = f.read()) {
        if (c < 0 || c == '\n') {
            while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) --n;
            line[n] = 0;
            if (n) fn(line);
            n = 0;
            if (c < 0) break;
        } else if (n + 1 < sizeof line) line[n++] = (char)c;
    }
    f.close();
    sdUnlock();
    return true;
}
void findMenu(int bi) {
    if (g_menuBook == bi) return;
    g_menuBook = bi;
    g_menuCount = 0;
    g_menuPicks = false;
    if (!g_menu) g_menu = (MenuItem*)heap_caps_malloc(sizeof(MenuItem) * MAX_MENU, MALLOC_CAP_SPIRAM);
    if (!g_menu) return;
    char path[96];
    snprintf(path, sizeof path, "%s/%s/menu.txt", kRoot, g_books[bi].dir);
    eachLine(path, [](char* l) {
        if (l[0] == '#') { char* t = l + 1; while (*t == ' ') ++t; addMenu(t, "", true); return; }
        char* bar = strchr(l, '|');
        if (bar) *bar = 0;
        addMenu(l, bar ? bar + 1 : "", false);
    });
    if (g_menuCount) return;
    g_menuPicks = true;
    snprintf(path, sizeof path, "%s/%s/picks.txt", kRoot, g_books[bi].dir);
    addMenu("Picked for survival", "", true);
    eachLine(path, [](char* l) { addMenu(l, "", false); });
    if (g_menuCount == 1 && g_entries)
        for (int i = 0; i < g_entryCount; ++i)
            if (g_entries[i].book == bi) addMenu(g_entries[i].title, g_entries[i].desc, false);
    if (g_menuCount > 1) return;
    g_menuCount = 0;
    g_menuPicks = false;
    findDocs(bi);
    if (g_docCount > 0) addMenu("Documents", "", true);
    for (int i = 0; i < g_docCount; ++i) addMenu(g_docs[i].title, "", false, (int32_t)g_docs[i].rec);
}

// a big book's two links, as words under the search box
const EpdRect kBigAZ = {24, TOP + 166, 170, 44};
const EpdRect kBigRandom = {214, TOP + 166, 190, 44};
constexpr int MENU_HEAD_H = 50, MENU_ROW_H = 66, MENU_ROW2_H = 88;
int menuTop(int bi) { return bigBook(bi) ? BODY_Y + 8 : TOP + 176; }
int itemH(const MenuItem& m) { return m.header ? MENU_HEAD_H : (m.sub[0] ? MENU_ROW2_H : MENU_ROW_H) + (m.cut ? 30 : 0); }
// Page starts: as many rows as fit above the pager; a header never ends a page.
int g_menuPageStart[64];
int g_menuPages = 0;
void paginateMenu(int bi) {
    g_menuPages = 0;
    const int top = menuTop(bi), bottom = kListPrev.y - 6;
    int i = 0;
    while (i < g_menuCount && g_menuPages < 64) {
        g_menuPageStart[g_menuPages++] = i;
        int y = top;
        int last = i;
        while (i < g_menuCount && y + itemH(g_menu[i]) <= bottom) { y += itemH(g_menu[i]); last = i; ++i; }
        if (i < g_menuCount && g_menu[last].header && last > g_menuPageStart[g_menuPages - 1]) i = last;
        if (i == g_menuPageStart[g_menuPages - 1]) ++i;
    }
    if (!g_menuPages) g_menuPageStart[g_menuPages++] = 0;
}
void renderMenu(int bi, uint8_t* fb) {
    findMenu(bi);
    paginateMenu(bi);
    if (g_listPage >= g_menuPages) g_listPage = g_menuPages - 1;
    const int from = g_menuPageStart[g_listPage];
    const int to = g_listPage + 1 < g_menuPages ? g_menuPageStart[g_listPage + 1] : g_menuCount;
    int y = menuTop(bi);
    for (int i = from; i < to; ++i) {
        const MenuItem& m = g_menu[i];
        char t[170];
        if (m.header) {
            snprintf(t, sizeof t, "%s", m.title);
            for (char* c = t; *c; ++c) if (*c >= 'a' && *c <= 'z') *c -= 32;
            fitText(ui::font(ui::F_TINY), t, W - 48);
            ui::text(ui::font(ui::F_TINY), 24, y + 38, t, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        } else {
            const int h = itemH(m);
            const EpdFont* lf = ui::font(ui::F_LABEL);
            char two[164];
            int ty = y + (m.sub[0] ? 38 : 42);
            if (m.cut) {
                snprintf(two, sizeof two, "%.*s", (int)m.cut - 1, m.title);
                ui::text(lf, 24, ty, two, fb, EPD_DRAW_ALIGN_LEFT, INK);
                ty += 30;
                snprintf(two, sizeof two, "%s", m.title + m.cut);
            } else {
                snprintf(two, sizeof two, "%s", m.title);
            }
            fitText(lf, two, W - 90);
            ui::text(lf, 24, ty, two, fb, EPD_DRAW_ALIGN_LEFT, INK);
            if (m.sub[0]) {
                char sub[104];
                snprintf(sub, sizeof sub, "%s", m.sub);
                fitText(ui::font(ui::F_SMALL), sub, W - 90);
                ui::text(ui::font(ui::F_SMALL), 24, ty + 30, sub, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
            }
            ui::text(ui::font(ui::F_KEY), W - 26, y + h / 2 + 12, "\xE2\x80\xBA", fb, EPD_DRAW_ALIGN_RIGHT, 0x80);
            epd_fill_rect({24, y + h - 1, W - 48, 1}, ui::HAIR, fb);
        }
        y += itemH(m);
    }
    pagerRow(g_listPage, g_menuPages, fb);
}
// The tapped menu row, or -1.
int menuHit(int bi, int x, int y) {
    (void)x;
    const int from = g_menuPageStart[g_listPage];
    const int to = g_listPage + 1 < g_menuPages ? g_menuPageStart[g_listPage + 1] : g_menuCount;
    int ry = menuTop(bi);
    for (int i = from; i < to; ++i) {
        const int h = itemH(g_menu[i]);
        if (y >= ry && y < ry + h) return g_menu[i].header ? -1 : i;
        ry += h;
    }
    return -1;
}
void renderBigBar(uint8_t* fb) {
    const EpdFont* f = ui::font(ui::F_LABEL);
    const char* az = "Browse A\xE2\x80\x93Z";
    ui::text(f, kBigAZ.x, kBigAZ.y + 30, az, fb, EPD_DRAW_ALIGN_LEFT, INK);
    epd_fill_rect({kBigAZ.x, kBigAZ.y + 37, ui::textWidth(f, az), 2}, INK, fb);
    ui::text(f, kBigRandom.x, kBigRandom.y + 30, "Random page", fb, EPD_DRAW_ALIGN_LEFT, INK);
    epd_fill_rect({kBigRandom.x, kBigRandom.y + 37, ui::textWidth(f, "Random page"), 2}, INK, fb);
}

void renderBook(uint8_t* fb) {
    const int bi = g_open;
    renderBookHead(bi, fb);
    char lbl[64];
    snprintf(lbl, sizeof lbl, "Search %s", shortName(bi));
    searchBar(kBookSearch, lbl, fb);
    findMenu(bi);
    if (!g_bookAZ && (g_menuCount || bigBook(bi))) {
        if (bigBook(bi)) renderBigBar(fb);
        if (g_menuCount) renderMenu(bi, fb);
        else ui::text(ui::font(ui::F_SMALL), W / 2, BODY_Y + 140, "Search above, or browse A-Z.", fb, EPD_DRAW_ALIGN_CENTER, INK);
        return;
    }
    ui::text(ui::font(ui::F_TINY), 24, BODY_Y - 14, "BROWSE A\xE2\x80\x93Z", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    for (int i = 0; i < letterCount(bi); ++i) {
        const EpdRect r = letterRect(bi, i);
        frame(r, 2, fb);
        ui::text(ui::font(ui::F_LABEL), r.x + r.width / 2, r.y + r.height / 2 + 12, letterAt(bi, i), fb,
                 EPD_DRAW_ALIGN_CENTER, INK);
    }
}

// A-Z: the index is sorted, so any prefix is one run of records. A letter
// of Wikipedia holds a million titles: a run too long to page through splits
// into its next letters ("A" -> "Aa", "Ab"...), the way a dictionary's thumb
// tabs do, until it is short enough to list.
constexpr int MAX_KIDS = 36, LIST_MAX = 64, MAX_DEPTH = 12;
struct Kid { char label[28]; uint32_t from, to; bool more; };
struct Level { char key[KEY + 1]; char label[28]; uint32_t from, to; bool group; };
Kid g_kids[MAX_KIDS];
int g_kidCount = 0;
Level g_levels[MAX_DEPTH];
int g_depth = 0;                 // g_levels[g_depth - 1] is on screen
uint32_t g_letterFrom = 0, g_letterTo = 0;

uint32_t lowerIn(File& f, const char* key, size_t klen, uint32_t lo, uint32_t hi) {
    uint8_t want[KEY] = {}, rec[REC];
    memcpy(want, key, std::min(klen, (size_t)KEY));
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (!readRec(f, mid, rec)) break;
        if (memcmp(rec, want, KEY) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}
int utf8Len(uint8_t c) { return c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }
// A tab's label from its (folded, lower-case) key: "ab" -> "Ab", "a " -> "A_".
void keyLabel(const char* key, size_t len, const char* tail, char* out, size_t cap) {
    char k[KEY + 1];
    len = std::min(len, (size_t)KEY);
    memcpy(k, key, len);
    k[len] = 0;
    const char* p = k;
    uint32_t cp = decodeUtf8(p);
    if (cp >= 'a' && cp <= 'z') cp -= 32;
    else if (cp >= 0x430 && cp <= 0x44F) cp -= 0x20;
    else if (cp >= 0x450 && cp <= 0x45F) cp -= 0x50;
    else if (cp == 0x491) cp = 0x490;
    char first[8];
    first[encodeUtf8(cp, first)] = 0;
    snprintf(out, cap, "%s%s%s", len ? first : "", p, tail);
    for (char* c = out; *c; ++c) if (*c == ' ') *c = '_';
}
// Not of the book's alphabet: Cyrillic in a Latin book, Latin in a Ukrainian one.
bool oddLetter(int bi, uint8_t c) {
    if (cyrillicBook(bi)) return (c >= 'a' && c <= 'z') || (c >= 0x80 && (c < 0xD0 || c > 0xD2));
    return c >= 0x80;
}
void findKids(int bi) {
    struct Took { uint32_t t; ~Took() { if (millis() - t > 50) Serial.printf("[perf] a-z split %lu ms\n", (unsigned long)(millis() - t)); } } took{millis()};
    g_kidCount = 0;
    const Level& lv = g_levels[g_depth - 1];
    if (lv.to - lv.from <= (uint32_t)LIST_MAX) return;
    const size_t L = strlen(lv.key);
    if (L + 4 >= (size_t)KEY) return;
    char path[96];
    snprintf(path, sizeof path, "%s/%s/index.dti", kRoot, g_books[bi].dir);
    sdLock();
    File f = SD.open(path, FILE_READ);
    uint8_t rec[REC];
    uint32_t pos = lv.from;
    while (f && pos < lv.to && g_kidCount < MAX_KIDS) {
        if (!readRec(f, pos, rec)) break;
        Kid& k = g_kids[g_kidCount];
        if (g_kidCount == MAX_KIDS - 1) {          // the rest, together
            k.from = pos; k.to = lv.to; k.more = true;
            snprintf(k.label, sizeof k.label, "MORE");
            ++g_kidCount;
            break;
        }
        char key[KEY + 1] = {};
        memcpy(key, lv.key, L);
        size_t kl;
        if (rec[L] == 0) {                      // the prefix itself
            key[L] = 1; kl = L + 1;
            keyLabel(lv.key, L, "", k.label, sizeof k.label);
        } else if (!lv.group && oddLetter(bi, rec[L])) {   // letters of another alphabet: one tab
            if (cyrillicBook(bi) && rec[L] < 0xD0) { key[L] = (char)0xD0; kl = L + 1; } else kl = 0;
            snprintf(k.label, sizeof k.label, "OTHER");
        } else if (!lv.group && rec[L] > ' ' && rec[L] < 'a') {   // signs and digits: one tab each kind
            const bool digit = rec[L] >= '0' && rec[L] <= '9';
            key[L] = digit ? ':' : rec[L] < '0' ? '0' : 'a';
            kl = L + 1;
            keyLabel(lv.key, L, digit ? "0-9" : "#", k.label, sizeof k.label);
        } else {
            const int n = utf8Len(rec[L]);
            memcpy(key + L, rec + L, n);
            kl = L + n;
            keyLabel(key, kl, "", k.label, sizeof k.label);
            key[kl - 1]++;                      // just past every key with it
        }
        k.from = pos;
        k.to = kl ? lowerIn(f, key, kl, pos + 1, lv.to) : lv.to;
        k.more = !lv.group && ((rec[L] > ' ' && rec[L] < 'a') || oddLetter(bi, rec[L]));   // a group: its records share only the prefix
        if (!k.label[0]) snprintf(k.label, sizeof k.label, "?");
        pos = k.to;
        ++g_kidCount;
    }
    if (f) f.close();
    sdUnlock();
}
// Show the run [from, to) of records starting with `key` (folded).
void pushLevel(int bi, const char* key, const char* label, uint32_t from, uint32_t to, bool group = false) {
    if (g_depth >= MAX_DEPTH) g_depth = MAX_DEPTH - 1;
    Level& lv = g_levels[g_depth++];
    snprintf(lv.key, sizeof lv.key, "%s", key);
    snprintf(lv.label, sizeof lv.label, "%s", label);
    lv.from = from;
    lv.to = to;
    lv.group = group;
    findKids(bi);
    // one way on only: take it
    while (g_kidCount == 1 && !g_kids[0].more && g_depth < MAX_DEPTH && g_kids[0].to - g_kids[0].from == to - from) {
        char k2[KEY + 1];
        snprintf(k2, sizeof k2, "%s", lv.key);
        // the kid's key: the first record's key up to one more character
        char path[96];
        snprintf(path, sizeof path, "%s/%s/index.dti", kRoot, g_books[bi].dir);
        uint8_t rec[REC] = {};
        sdLock();
        File f = SD.open(path, FILE_READ);
        const bool ok = f && readRec(f, g_kids[0].from, rec);
        if (f) f.close();
        sdUnlock();
        const size_t L = strlen(k2);
        if (!ok || rec[L] == 0 || L + 4 >= (size_t)KEY) break;
        memcpy(k2 + L, rec + L, utf8Len(rec[L]));
        k2[L + utf8Len(rec[L])] = 0;
        snprintf(lv.key, sizeof lv.key, "%s", k2);
        snprintf(lv.label, sizeof lv.label, "%s", g_kids[0].label);
        findKids(bi);
    }
    g_letterFrom = lv.from;
    g_letterTo = lv.to;
    g_listPage = 0;
    Serial.printf("[lib] index '%s': %lu entries, %d tabs\n", lv.label, (unsigned long)(lv.to - lv.from), g_kidCount);
}
bool popLevel(int bi) {
    if (g_depth <= 1) { g_depth = 0; return false; }
    --g_depth;
    findKids(bi);
    g_letterFrom = g_levels[g_depth - 1].from;
    g_letterTo = g_levels[g_depth - 1].to;
    g_listPage = 0;
    return true;
}
void setLetter(int bi, int i) {
    const char* L = letterAt(bi, i);
    char path[96];
    snprintf(path, sizeof path, "%s/%s/index.dti", kRoot, g_books[bi].dir);
    sdLock();
    File f = SD.open(path, FILE_READ);
    const uint32_t count = g_books[bi].count;
    uint32_t from = 0, to = 0;
    char key[16] = "";
    if (f && !strcmp(L, "0-9")) {
        to = lowerIn(f, "a", 1, 0, count);   // digits and signs sort before letters
    } else if (f) {
        fold(L, key, sizeof key);
        from = lowerIn(f, key, strlen(key), 0, count);
        char next[16];
        snprintf(next, sizeof next, "%s", key);
        next[strlen(next) - 1]++;
        to = lowerIn(f, next, strlen(next), from, count);
    }
    if (f) f.close();
    sdUnlock();
    g_depth = 0;
    pushLevel(bi, key, L, from, to);
}
const int KID_COLS = 4, KID_GAP = 8;
EpdRect kidRect(int i) {
    const int cw = (W - 24 - (KID_COLS - 1) * KID_GAP) / KID_COLS, ch = 66;
    return {12 + (i % KID_COLS) * (cw + KID_GAP), BODY_Y - 22 + (i / KID_COLS) * (ch + KID_GAP), cw, ch};
}
void renderLetter(uint8_t* fb) {
    const int bi = g_open;
    renderBookHead(bi, fb);
    const Level& lv = g_levels[g_depth > 0 ? g_depth - 1 : 0];
    const uint32_t n = g_letterTo - g_letterFrom;
    char cnt[16], h[64];
    countText(n, cnt, sizeof cnt);
    snprintf(h, sizeof h, "%s", lv.label);
    ui::text(ui::font(ui::F_TITLE), 20, kBookSearch.y + 50, h, fb, EPD_DRAW_ALIGN_LEFT, INK);
    snprintf(h, sizeof h, "%s title%s%s", cnt, n == 1 ? "" : "s", g_kidCount ? " - pick the next letters" : "");
    ui::text(ui::font(ui::F_TINY), W - 16, kBookSearch.y + 48, h, fb, EPD_DRAW_ALIGN_RIGHT, INK);
    if (g_kidCount) {
        for (int i = 0; i < g_kidCount; ++i) {
            const EpdRect r = kidRect(i);
            const Kid& k = g_kids[i];
            frame(r, 2, fb);
            char t[32];
            snprintf(t, sizeof t, "%s", k.label);
            const EpdFont* f = ui::font(ui::F_LABEL);
            while (ui::textWidth(f, t) > r.width - 10 && strlen(t) > 1) {
                size_t m = strlen(t) - 1;
                while (m > 0 && ((uint8_t)t[m] & 0xC0) == 0x80) --m;
                t[m] = 0;
            }
            ui::text(f, r.x + r.width / 2, r.y + 34, t, fb, EPD_DRAW_ALIGN_CENTER, INK);
            countText(k.to - k.from, cnt, sizeof cnt);
            ui::text(ui::font(ui::F_TINY), r.x + r.width / 2, r.y + 58, cnt, fb, EPD_DRAW_ALIGN_CENTER, INK);
        }
        return;
    }
    const int per = listRows() + 1;
    const int pages = n ? (int)((n + per - 1) / per) : 1;
    if (g_listPage >= pages) g_listPage = pages - 1;
    char path[96];
    snprintf(path, sizeof path, "%s/%s/index.dti", kRoot, g_books[bi].dir);
    sdLock();
    File f = SD.open(path, FILE_READ);
    uint8_t rec[REC];
    for (int r = 0; r < per; ++r) {
        const uint32_t i = g_letterFrom + (uint32_t)(g_listPage * per + r);
        if (i >= g_letterTo || !f || !readRec(f, i, rec)) break;
        char t[KEY + 1];
        memcpy(t, rec + KEY, KEY);
        t[KEY] = 0;
        listRow(BODY_Y - 30 + r * LIST_ROW, t, fb);
    }
    if (f) f.close();
    sdUnlock();
    if (!n) ui::text(ui::font(ui::F_SMALL), W / 2, BODY_Y + 120, "nothing under this letter", fb, EPD_DRAW_ALIGN_CENTER, INK);
    pagerRow(g_listPage, pages, fb);
}
bool openRecord(int bi, uint32_t i) {
    char path[96];
    snprintf(path, sizeof path, "%s/%s/index.dti", kRoot, g_books[bi].dir);
    sdLock();
    File f = SD.open(path, FILE_READ);
    uint8_t rec[REC];
    const bool ok = f && readRec(f, i, rec);
    if (f) f.close();
    sdUnlock();
    if (!ok) return false;
    Result r;
    recResult(rec, bi, r);
    g_historyCount = 0;
    return openArticle(r);
}

constexpr int RECENT_TOP = TOP + 196, RECENT_ROW = 86;
int recentRows() { return (kListPrev.y - 8 - RECENT_TOP) / RECENT_ROW; }
void renderRecent(uint8_t* fb) {
    tabs(2, fb);
    loadRecent();
    if (!g_recentCount) {
        ui::text(ui::font(ui::F_LABEL), W / 2, TOP + 300, "Nothing read yet", fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::text(ui::font(ui::F_SMALL), W / 2, TOP + 344, "Pages you open show up here.", fb, EPD_DRAW_ALIGN_CENTER, INK);
        return;
    }
    const int per = recentRows(), pages = (g_recentCount + per - 1) / per;
    if (g_listPage >= pages) g_listPage = pages - 1;
    for (int r = 0; r < per && g_listPage * per + r < g_recentCount; ++r) {
        const Recent& e = g_recent[g_listPage * per + r];
        const int y = RECENT_TOP + r * RECENT_ROW;
        char t[KEY + 8];
        snprintf(t, sizeof t, "%s", e.title);
        fitText(ui::font(ui::F_LABEL), t, W - 90);
        ui::text(ui::font(ui::F_LABEL), 24, y + 38, t, fb, EPD_DRAW_ALIGN_LEFT, INK);
        const int bi = bookIndex(e.dir);
        ui::text(ui::font(ui::F_SMALL), 24, y + 68, bi >= 0 ? shortName(bi) : e.dir, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        ui::text(ui::font(ui::F_KEY), W - 26, y + RECENT_ROW / 2 + 12, "\xE2\x80\xBA", fb, EPD_DRAW_ALIGN_RIGHT, 0x80);
        epd_fill_rect({24, y + RECENT_ROW - 1, W - 48, 1}, ui::HAIR, fb);
    }
    pagerRow(g_listPage, pages, fb);
}

void renderHome(uint8_t* fb) {
    tabs(1, fb);
    char lbl[48];
    snprintf(lbl, sizeof lbl, "Search all %d books", g_bookCount);
    searchBar(kAllSearch, lbl, fb);
    // EMERGENCY: straight to the page that matters.
    if (g_emergCount) {
        ui::text(ui::font(ui::F_TINY), 24, EM_Y + 18, "EMERGENCY", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        for (int i = 0; i < g_emergCount; ++i) {
            const EpdRect r = emergRect(i);
            ui::fillRound(r, 8, INK, fb);
            doba_icons::draw(g_emerg[i].icon, r.x + r.width / 2, r.y + 40, 48, PAPER, INK, fb);
            const EpdFont* ef = ui::font(ui::F_SMALL);
            if (ui::textWidth(ef, g_emerg[i].label) > r.width - 14) ef = ui::font(ui::F_TINY);
            ui::text(ef, r.x + r.width / 2, r.y + r.height - 16, g_emerg[i].label, fb, EPD_DRAW_ALIGN_CENTER, PAPER);
        }
    }
    // The sections.
    for (int i = 0; i < g_secCount; ++i) {
        const EpdRect r = cellRect(i);
        ui::frameRound(r, 8, 2, fb);
        const int isz = r.height > 150 ? 70 : r.height > 125 ? 58 : 50;
        doba_icons::draw(g_secs[i].icon, r.x + r.width / 2, r.y + r.height / 2 - 16, isz, INK, PAPER, fb);
        const EpdFont* f = ui::font(ui::F_SMALL);
        if (ui::textWidth(f, g_secs[i].name) > r.width - 12) f = ui::font(ui::F_TINY);
        ui::text(f, r.x + r.width / 2, r.y + r.height - 16, g_secs[i].name, fb, EPD_DRAW_ALIGN_CENTER, INK);
        if (g_secs[i].n) {
            char c[8];
            snprintf(c, sizeof c, "%d", g_secs[i].n);
            ui::text(ui::font(ui::F_TINY), r.x + r.width - 10, r.y + 24, c, fb, EPD_DRAW_ALIGN_RIGHT, INK);
        }
    }
}

// SECTION: header, START HERE, the guides, search in the section.
constexpr int SEC_HEAD = 120, ROW_H = 112, START_H = 124;
const EpdRect kSecPrev = {14, H - 88, 90, 76};
const EpdRect kSecSearch = {114, H - 88, W - 228, 76};
const EpdRect kSecNext = {W - 104, H - 88, 90, 76};

int secRowsPerPage(const Section& x) {
    const int avail = kSecPrev.y - 8 - (TOP + SEC_HEAD) - (x.starts ? x.starts * (START_H + 10) + 8 : 0);
    return avail / ROW_H > 1 ? avail / ROW_H : 1;
}
int secPages(const Section& x) {
    const int rest = x.n - x.starts, per = secRowsPerPage(x);
    return rest <= 0 ? 1 : (rest + per - 1) / per;
}

void bookTag(int bi, int xr, int y, uint8_t fg, uint8_t* fb) {
    if (bi < 0 || bi >= g_bookCount) return;
    char t[24];
    snprintf(t, sizeof t, "%.16s", shortName(bi));
    ui::text(ui::font(ui::F_TINY), xr, y, t, fb, EPD_DRAW_ALIGN_RIGHT, fg);
}

void renderSection(uint8_t* fb) {
    const Section& x = g_secs[g_sec];
    doba_icons::draw(x.icon, 64, TOP + 60, 84, INK, PAPER, fb);
    ui::text(ui::font(ui::F_TITLE), 124, TOP + 58, x.name, fb, EPD_DRAW_ALIGN_LEFT, INK);
    char sub[64];
    int nb = 0;
    for (int i = 0; i < g_bookCount; ++i) if (x.books & (1u << i)) ++nb;
    if (x.n) snprintf(sub, sizeof sub, "%d guides · %d book%s", x.n, nb, nb == 1 ? "" : "s");
    else snprintf(sub, sizeof sub, "%d book%s · search inside", nb, nb == 1 ? "" : "s");
    ui::text(ui::font(ui::F_SMALL), 124, TOP + 94, sub, fb, EPD_DRAW_ALIGN_LEFT, INK);
    epd_fill_rect({14, TOP + SEC_HEAD - 8, W - 28, 3}, INK, fb);
    int y = TOP + SEC_HEAD;
    // Guides as a contents page: a number, the title in the book face, a line
    // or two on what is inside. START HERE ones first, in a frame.
#ifdef VFONT
    const EpdFont* tf = vf_face_cap(&Literata_700, 15.f);
    const EpdFont* df = vf_face_cap(&Literata_450, 12.f);
#else
    const EpdFont* tf = ui::font(ui::F_LABEL);
    const EpdFont* df = ui::font(ui::F_TINY);
#endif
    const EpdFont* nf = ui::font(ui::F_TINY);
    auto row = [&](const Entry& e, int num, int y, int h, bool start) {
        const int x0 = start ? 28 : 20, right = W - 22;
        char n[16];
        snprintf(n, sizeof n, "%d.%d", g_sec + 1, num);
        if (start) {
            frame({14, y, W - 28, h}, 3, fb);
            epd_fill_rect({14, y, 140, 26}, INK, fb);
            ui::text(nf, 26, y + 19, "START HERE", fb, EPD_DRAW_ALIGN_LEFT, PAPER);
        } else {
            ui::text(nf, x0, y + 20, n, fb, EPD_DRAW_ALIGN_LEFT, INK);
        }
        bookTag(e.book, right, y + 20, INK, fb);
        char t[64];
        snprintf(t, sizeof t, "%s", e.label);
        while (ui::textWidth(tf, t) > right - x0 - 30 && strlen(t) > 4) {
            size_t k = strlen(t) - 4;
            while (k > 0 && ((uint8_t)t[k] & 0xC0) == 0x80) --k;
            memcpy(t + k, "...", 4);
        }
        ui::text(tf, x0, y + 52, t, fb, EPD_DRAW_ALIGN_LEFT, INK);
        // the description, up to two lines, cut at a word
        const char* d = e.desc;
        int ly = y + 78;
        for (int line = 0; line < 2 && *d; ++line) {
            char buf[96];
            int best = 0, i = 0;
            while (d[i] && i < (int)sizeof buf - 1) {
                buf[i] = d[i];
                buf[i + 1] = 0;
                if (ui::textWidth(df, buf) > right - x0 - 30) break;
                if (d[i] == ' ' || !d[i + 1]) best = i + 1;
                ++i;
            }
            if (!best) best = i;
            memcpy(buf, d, best);
            buf[best] = 0;
            ui::text(df, x0, ly, buf, fb, EPD_DRAW_ALIGN_LEFT, INK);
            d += best;
            while (*d == ' ') ++d;
            ly += 24;
        }
        ui::text(ui::font(ui::F_KEY), right, y + h / 2 + 22, ">", fb, EPD_DRAW_ALIGN_RIGHT, INK);
        if (!start) epd_fill_rect({14, y + h - 2, W - 28, 1}, INK, fb);
    };
    for (int k = 0, seen = 0; k < x.n && seen < x.starts; ++k) {
        const Entry& e = g_entries[x.first + k];
        if (!e.start) continue;
        ++seen;
        row(e, k + 1, y, START_H, true);
        y += START_H + 10;
    }
    if (x.starts) y += 8;
    // the rest, a page at a time
    const int per = secRowsPerPage(x);
    int idx = 0;
    for (int k = 0; k < x.n; ++k) {
        const Entry& e = g_entries[x.first + k];
        if (e.start) continue;
        if (idx >= g_secPage * per && idx < (g_secPage + 1) * per) {
            row(e, k + 1, y, ROW_H, false);
            y += ROW_H;
        }
        ++idx;
    }
    if (!x.n) {
        ui::text(ui::font(ui::F_SMALL), W / 2, TOP + 300, "No guides picked for this yet.", fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::text(ui::font(ui::F_SMALL), W / 2, TOP + 334, "Search its books below.", fb, EPD_DRAW_ALIGN_CENTER, INK);
    }
    // footer: pages and search
    const int pages = secPages(x);
    frame(kSecSearch, 3, fb);
    doba_icons::draw(doba_icons::I_SEARCH, kSecSearch.x + 36, kSecSearch.y + 38, 34, INK, PAPER, fb);
    ui::text(ui::font(ui::F_SMALL), kSecSearch.x + 64, kSecSearch.y + 46, "SEARCH HERE", fb, EPD_DRAW_ALIGN_LEFT, INK);
    if (pages > 1) {
        frame(kSecPrev, 2, fb);
        frame(kSecNext, 2, fb);
        ui::text(ui::font(ui::F_KEY), kSecPrev.x + 45, kSecPrev.y + 50, g_secPage > 0 ? "<" : "", fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::text(ui::font(ui::F_KEY), kSecNext.x + 45, kSecNext.y + 50, g_secPage + 1 < pages ? ">" : "", fb, EPD_DRAW_ALIGN_CENTER, INK);
        char pg[16];
        snprintf(pg, sizeof pg, "%d/%d", g_secPage + 1, pages);
        ui::text(ui::font(ui::F_TINY), W / 2, kSecPrev.y - 8, pg, fb, EPD_DRAW_ALIGN_CENTER, INK);
    }
}

// SEARCH: the query, its scope, results with their book.
const EpdRect kScope = {24, TOP + 4, W - 48, 50};
const EpdRect kQuery = {24, TOP + 60, W - 48, 64};
constexpr int RES_TOP = TOP + 142, RES_H = 60;

uint32_t scopeMask() { return g_scopeBook >= 0 ? 1u << g_scopeBook : g_scope < 0 ? 0xFFFFFFFFu : g_secs[g_scope].books; }

void renderSearchView(kbd::Keyboard& k, uint8_t* fb) {
    k.top = H - k.height();
    k.enterLabel = "OPEN";
    char line[96];
    if (g_scopeBook >= 0) snprintf(line, sizeof line, "In %s  \xC2\xB7  search all books instead", shortName(g_scopeBook));
    else if (g_scope < 0) snprintf(line, sizeof line, "In all %d books", g_bookCount);
    else snprintf(line, sizeof line, "In %s  \xC2\xB7  search all books instead", g_secs[g_scope].name);
    fitText(ui::font(ui::F_SMALL), line, kScope.width - 8);
    ui::text(ui::font(ui::F_SMALL), kScope.x + 4, kScope.y + 34, line, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    ui::frameRound(kQuery, 8, 2, fb);
    char q[300];
    snprintf(q, sizeof q, "%s|", k.text);
    if (k.text[0])
        ui::text(ui::font(ui::F_KEY), kQuery.x + 18, kQuery.y + 44, q, fb, EPD_DRAW_ALIGN_LEFT, INK);
    else
        ui::text(ui::font(ui::F_SMALL), kQuery.x + 18, kQuery.y + 40, "Type the start of a title", fb,
                 EPD_DRAW_ALIGN_LEFT, ui::GREY);
    char key[KEY + 1];
    fold(k.text, key, sizeof key);
    char want[KEY + 16];
    snprintf(want, sizeof want, "%d|%d|%s", g_scope, g_scopeBook, key);
    if (strcmp(want, g_lastQuery) != 0) {
        snprintf(g_lastQuery, sizeof g_lastQuery, "%s", want);
        if (key[0]) search(key, scopeMask()); else g_resultCount = 0;
    }
    for (int i = 0; i < g_resultCount; ++i) {
        const int y = RES_TOP + i * RES_H;
        if (y + RES_H > k.top) break;
        char rt[KEY + 8];
        snprintf(rt, sizeof rt, "%s", g_results[i].title);
        fitText(ui::font(ui::F_LABEL), rt, W - 150);
        ui::text(ui::font(ui::F_LABEL), 24, y + 40, rt, fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (g_bookCount > 1 && g_scopeBook < 0)
            ui::text(ui::font(ui::F_TINY), W - 24, y + 38, shortName(g_results[i].book), fb, EPD_DRAW_ALIGN_RIGHT, ui::GREY);
        epd_fill_rect({24, y + RES_H - 1, W - 48, 1}, ui::HAIR, fb);
    }
    if (key[0] && g_resultCount == 0)
        ui::text(ui::font(ui::F_SMALL), 24, RES_TOP + 50, "Nothing starts with that", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    if (!key[0]) {
        ui::text(ui::font(ui::F_SMALL), 24, RES_TOP + 50, "For example: water, burn, knot, вогнище", fb, EPD_DRAW_ALIGN_LEFT, 0x40);
        ui::text(ui::font(ui::F_SMALL), 24, RES_TOP + 80, "Case and accents don\u2019t matter", fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    }
    k.render(fb);
}

bool openEntry(int book, const char* title) {
    Result r;
    if (!findIn(book, title, r)) {
        Serial.printf("[lib] missing '%s' in book %d\n", title, book);
        g_note = "not on this card";
        return false;
    }
    g_historyCount = 0;
    return openArticle(r);
}

}  // namespace

int count() {
    if (g_bookCount < 0) { scan(); loadSections(); }
    return g_bookCount;
}

void rescan() { g_bookCount = -1; g_view = V_HOME; }

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

// --- article chrome: header, footer, contents ----------------------------------
namespace {

constexpr int BODY_TOP = TOP + 66;
const EpdRect kSizeBtn = {W - 86, TOP + 6, 78, 42};   // "Aa": the text size

int pageOfLine(int k) {
    int lo = 0, hi = g_pageCount - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (g_pageStarts[mid] <= k) lo = mid; else hi = mid - 1;
    }
    return lo;
}
void buildToc() {
    g_tocCount = 0;
    if (!g_toc_lines) g_toc_lines = (int*)heap_caps_malloc(MAX_TOC * sizeof(int), MALLOC_CAP_SPIRAM);
    if (!g_toc_lines) return;
    for (int k = 0; k < g_lineCount && g_tocCount < MAX_TOC; ++k) {
        const Line& l = g_lines[k];
        if ((l.kind == 'H' || l.kind == 'S') && (l.off == 0 || g_text[l.off - 1] == (char)l.kind))
            g_toc_lines[g_tocCount++] = k;
    }
}
int tocRowsPerPage() { return (H - FOOT_H - BODY_TOP - 8) / TOC_ROW; }
int tocPages() { const int r = tocRowsPerPage(); return g_tocCount ? (g_tocCount + r - 1) / r : 1; }

// A heading's whole text (it may wrap over lines), cut with "..." to fit maxW.
void headingText(const Line& l, const EpdFont* f, int maxW, char* out, size_t cap) {
    size_t e = l.off;
    while (e < g_textLen && g_text[e] != '\n') ++e;
    size_t n = e - l.off < cap - 4 ? e - l.off : cap - 4;
    memcpy(out, g_text + l.off, n);
    out[n] = 0;
    if (ui::textWidth(f, out) <= maxW) return;
    while (n > 0) {
        do { --n; } while (n > 0 && ((uint8_t)out[n] & 0xC0) == 0x80);
        memcpy(out + n, "...", 4);
        if (ui::textWidth(f, out) <= maxW) return;
    }
}

// Back, the place, on: the same on every reading screen. Words, not boxes.
void footer(const char* left, const char* mid, const char* midSub, const char* right, uint8_t* fb) {
    const int fy = H - FOOT_H;
    epd_fill_rect({24, fy, W - 48, 1}, ui::HAIR, fb);
    const EpdFont* k = ui::font(ui::F_LABEL);
    auto chev = [&](int cx, int cy, int d) {   // d: -1 points left
        for (int t = -1; t <= 1; ++t) {
            epd_draw_line(cx - d * 5 + t, cy - 9, cx + d * 4 + t, cy, INK, fb);
            epd_draw_line(cx + d * 4 + t, cy, cx - d * 5 + t, cy + 9, INK, fb);
        }
    };
    char l[16], r[16];
    auto titled = [](const char* w, char* out) {   // "PREV" -> "Prev"
        snprintf(out, 16, "%s", w);
        for (char* c = out + 1; *c; ++c) if (*c >= 'A' && *c <= 'Z') *c += 32;
    };
    if (left) {
        titled(left, l);
        chev(32, fy + 38, -1);
        ui::text(k, 46, fy + 46, l, fb, EPD_DRAW_ALIGN_LEFT, INK);
    }
    if (right) {
        titled(right, r);
        const bool close = !strcmp(right, "CLOSE");
        if (!close) chev(W - 32, fy + 38, 1);
        ui::text(k, close ? W - 28 : W - 46, fy + 46, r, fb, EPD_DRAW_ALIGN_RIGHT, INK);
    }
    char m[32];
    snprintf(m, sizeof m, "%s", mid);
    if (!strcmp(mid, "CLOSE")) snprintf(m, sizeof m, "Close");
    ui::text(ui::font(ui::F_SMALL), W / 2, fy + (midSub ? 34 : 44), m, fb, EPD_DRAW_ALIGN_CENTER, 0x30);
    if (midSub) {
        char ms[24];
        snprintf(ms, sizeof ms, "%s", !strcmp(midSub, "CONTENTS") ? "Contents" : midSub);
        ui::text(ui::font(ui::F_TINY), W / 2, fy + 58, ms, fb, EPD_DRAW_ALIGN_CENTER, ui::GREY);
    }
}

void renderToc(uint8_t* fb) {
    ui::text(ui::font(ui::F_LABEL), MARGIN, TOP + 34, "Contents", fb, EPD_DRAW_ALIGN_LEFT, INK);
    epd_fill_rect({MARGIN, TOP + 52, W - 2 * MARGIN, 1}, ui::HAIR, fb);
    const int per = tocRowsPerPage();
    if (g_tocPage >= tocPages()) g_tocPage = tocPages() - 1;
    int cur = -1;   // the part being read: the last heading at or before this page
    for (int i = 0; i < g_tocCount; ++i)
        if (pageOfLine(g_toc_lines[i]) <= g_page) cur = i;
    const EpdFont* f = ui::font(ui::F_SMALL);
#ifdef VFONT
    const EpdFont* fh = vf_face_cap(&Literata_700, 15.f);
    const EpdFont* fs = vf_face_cap(&Literata_450, 15.f);
#else
    const EpdFont* fh = ui::font(ui::F_LABEL);
    const EpdFont* fs = ui::font(ui::F_SMALL);
#endif
    char buf[160], pn[12];
    int y = BODY_TOP - 8;
    for (int i = g_tocPage * per; i < g_tocCount && i < (g_tocPage + 1) * per; ++i) {
        const Line& l = g_lines[g_toc_lines[i]];
        const int in = l.kind == 'S' ? 22 : 0;
        snprintf(pn, sizeof pn, "%d", pageOfLine(g_toc_lines[i]) + 1);
        const int pw = ui::textWidth(f, pn);
        const EpdFont* tf = l.kind == 'S' ? fs : fh;
        headingText(l, tf, W - 2 * MARGIN - in - pw - 24, buf, sizeof buf);
        if (i == cur) epd_fill_rect({MARGIN - 18, y + 10, 5, TOC_ROW - 20}, INK, fb);   // you are here
        ui::text(tf, MARGIN + in, y + TOC_ROW / 2 + 8, buf, fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(f, W - MARGIN, y + TOC_ROW / 2 + 8, pn, fb, EPD_DRAW_ALIGN_RIGHT, INK);
        epd_fill_rect({MARGIN + in, y + TOC_ROW - 1, W - 2 * MARGIN - in, 1}, ui::HAIR, fb);
        y += TOC_ROW;
    }
    char mid[24];
    snprintf(mid, sizeof mid, "%d / %d", g_tocPage + 1, tocPages());
    footer(g_tocPage > 0 ? "UP" : nullptr, "CLOSE", mid, g_tocPage + 1 < tocPages() ? "DOWN" : nullptr, fb);
}

}  // namespace

void renderArticle(uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    g_hotCount = 0;
    if (!g_text || !g_pageStarts || g_pageCount == 0) return;
    if (g_toc) { renderToc(fb); return; }
    // Header: where this came from, and the text size button.
    char crumb[KEY + 40];
    const View from = g_view == V_SEARCH ? g_searchFrom : g_view;
    if (from == V_SECTION)
        snprintf(crumb, sizeof crumb, "%s  \xE2\x80\xBA  %s", g_secs[g_sec].name, g_title);
    else if (from == V_BOOK || from == V_LETTER || (g_view == V_SEARCH && g_scopeBook >= 0))
        snprintf(crumb, sizeof crumb, "%s  \xE2\x80\xBA  %s", shortName(g_current.book >= 0 ? g_current.book : g_open), g_title);
    else
        snprintf(crumb, sizeof crumb, "%s", g_title);
    // Never under the button: cut at a character, mark the cut.
    const EpdFont* cf = ui::font(ui::F_SMALL);
    const int room = kSizeBtn.x - 14 - 12;
    if (ui::textWidth(cf, crumb) > room) {
        size_t n = strlen(crumb);
        while (n > 0) {
            do { --n; } while (n > 0 && ((uint8_t)crumb[n] & 0xC0) == 0x80);
            crumb[n] = 0;
            char t[KEY + 48];
            snprintf(t, sizeof t, "%s...", crumb);
            if (ui::textWidth(cf, t) <= room) { snprintf(crumb, sizeof crumb, "%s", t); break; }
        }
    }
    ui::text(cf, MARGIN, TOP + 32, crumb, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    ui::text(ui::font(ui::F_LABEL), kSizeBtn.x + kSizeBtn.width - 4, kSizeBtn.y + 28, "Aa", fb, EPD_DRAW_ALIGN_RIGHT, INK);
    // A hairline that is also the progress: black for the part read.
    epd_fill_rect({MARGIN, TOP + 50, W - 2 * MARGIN, 1}, ui::HAIR, fb);
    epd_fill_rect({MARGIN, TOP + 49, (W - 2 * MARGIN) * (g_page + 1) / g_pageCount, 3}, INK, fb);

    const int first = g_pageStarts[g_page];
    const int last = g_page + 1 < g_pageCount ? g_pageStarts[g_page + 1] : g_lineCount;
    int y = BODY_TOP;
    char buf[512];
    // Links are in text order, like the lines: start the scan where this page does.
    int li = 0;
    if (first < g_lineCount)
        while (li < g_linkCount && g_links[li].end <= g_lines[first].off) ++li;
    for (int k = first; k < last; ++k) {
        const Line& l = g_lines[k];
        if (l.kind == 'G') { y += paraGap(); continue; }
        if (l.kind == 'I') {
            char nm[32];
            int iw, ih;
            if (imageLine(g_text + l.off, l.len, nm, sizeof nm, iw, ih)) {
                drawImage(nm, y + IMG_PAD, ih, fb);
                y += ih + 2 * IMG_PAD;
            }
            continue;
        }
        y += topGap(l);
        const int h = lineHeight(l.kind);
        size_t n = l.len < sizeof buf - 1 ? l.len : sizeof buf - 1;
        memcpy(buf, g_text + l.off, n);
        buf[n] = 0;
        int x = MARGIN;
        // First line of a bullet gets the bullet; continuation lines indent.
        if (l.kind == 'B') {
            const char c0 = g_text[l.off];
            const bool numbered = c0 == '(' || ((c0 >= '0' && c0 <= '9') || (c0 >= 'a' && c0 <= 'z'))
                                  && l.len > 1 && (g_text[l.off + 1] == ')' || g_text[l.off + 1] == '.');
            if (firstLineOf(l) && !numbered)   // "(1)", "a." carry their own label
                epd_fill_rect({MARGIN + 3, y + h - baselineOf('B') - (int)(capPx('B') * 0.36f) - 3, 7, 7}, INK, fb);
            x += BULLET_IN;
        }
        const EpdFont* f = fontFor(l.kind);
        ui::text(f, x, y + h - baselineOf(l.kind), buf, fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (l.kind == 'F' && firstLineOf(l)) epd_fill_rect({MARGIN, y - 4, 60, 1}, INK, fb);
        // Links on this line: underlined, and remembered for taps.
        const uint32_t ls = l.off, le = l.off + l.len;
        for (int j = li; j < g_linkCount && g_links[j].start < le; ++j) {
            const Link& L = g_links[j];
            if (L.end <= ls) continue;
            const uint32_t a = L.start > ls ? L.start : ls, b = L.end < le ? L.end : le;
            const int x0 = x + widthOf(f, g_text + ls, a - ls);
            const int x1 = x + widthOf(f, g_text + ls, b - ls);
            const int ul = y + h - baselineOf(l.kind) + 4;      // just under the baseline
            epd_fill_rect({x0, ul, x1 - x0, 3}, INK, fb);
            if (g_hotCount < MAX_HOT)
                g_hot[g_hotCount++] = {(int16_t)x0, (int16_t)y, (int16_t)x1, (int16_t)(y + h), j};
        }
        while (li < g_linkCount && g_links[li].end <= le) ++li;
        y += h + afterGap(l);
    }
    // Footer: back a page, where you are (tap: contents), on a page.
    char pg[24];
    snprintf(pg, sizeof pg, "%d / %d", g_page + 1, g_pageCount);
    footer(g_page > 0 ? "PREV" : nullptr, pg, g_tocCount ? "CONTENTS" : nullptr,
           g_page + 1 < g_pageCount ? "NEXT" : "CLOSE", fb);
    if (g_note) {
        const int tw = ui::textWidth(ui::font(ui::F_SMALL), g_note) + 24;
        epd_fill_rect({(W - tw) / 2, H - FOOT_H - 50, tw, 42}, INK, fb);
        ui::text(ui::font(ui::F_SMALL), W / 2, H - FOOT_H - 20, g_note, fb, EPD_DRAW_ALIGN_CENTER, PAPER);
        g_note = nullptr;
    }
}


void renderSearch(kbd::Keyboard& k, uint8_t* fb) {
    if (g_bookCount < 0) { scan(); loadSections(); }
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    if (g_bookCount <= 0) {
        doba_icons::draw(doba_icons::I_SHELF, W / 2, 300, 120, INK, PAPER, fb);
        ui::text(ui::font(ui::F_TITLE), W / 2, 430, "NO BOOKS", fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::text(ui::font(ui::F_SMALL), W / 2, 474, "copy books to /doba/lib on the card", fb, EPD_DRAW_ALIGN_CENTER, INK);
        return;
    }
    switch (g_view) {
        case V_SHELF: renderShelf(fb); break;
        case V_HOME: renderHome(fb); break;
        case V_RECENT: renderRecent(fb); break;
        case V_SECTION: renderSection(fb); break;
        case V_BOOK: renderBook(fb); break;
        case V_LETTER: renderLetter(fb); break;
        case V_SEARCH: renderSearchView(k, fb); break;
    }
    if (g_note && g_view != V_SEARCH) {
        const int tw = ui::textWidth(ui::font(ui::F_SMALL), g_note) + 24;
        epd_fill_rect({(W - tw) / 2, H - 150, tw, 42}, INK, fb);
        ui::text(ui::font(ui::F_SMALL), W / 2, H - 120, g_note, fb, EPD_DRAW_ALIGN_CENTER, PAPER);
        g_note = nullptr;
    }
}

bool tapSearch(kbd::Keyboard& k, int x, int y) {
    if (g_bookCount <= 0) return false;
    switch (g_view) {
        case V_SHELF: {
            if (hit(kTabs, x, y)) { const int t = tabAt(x); g_view = t == 1 ? V_HOME : t == 2 ? V_RECENT : V_SHELF; g_listPage = 0; return false; }
            if (hit(kAllSearch, x, y)) { g_scope = -1; g_scopeBook = -1; g_searchFrom = V_SHELF; g_view = V_SEARCH; k.clear(); return false; }
            if (shelfPages() > 1 && hit(kListPrev, x, y)) { if (g_shelfPage > 0) --g_shelfPage; return false; }
            if (shelfPages() > 1 && hit(kListNext, x, y)) { if (g_shelfPage + 1 < shelfPages()) ++g_shelfPage; return false; }
            const int b = shelfHit(x, y);
            if (b >= 0) { g_open = b; g_listPage = 0; g_bookAZ = false; g_view = V_BOOK; Serial.printf("[lib] book %s\n", g_books[b].dir); }
            return false;
        }
        case V_BOOK: {
            const int bi = g_open;
            if (hit(kBookSearch, x, y)) { g_scope = -1; g_scopeBook = bi; g_searchFrom = V_BOOK; g_view = V_SEARCH; k.clear(); return false; }
            findMenu(bi);
            if (!g_bookAZ && (g_menuCount || bigBook(bi))) {
                if (bigBook(bi) && hit(kBigAZ, x, y)) { g_bookAZ = true; return false; }
                if (bigBook(bi) && hit(kBigRandom, x, y)) {
                    uint32_t i = (uint32_t)random((long)g_books[bi].count);
                    {   // not a hidden page (a profile, a Category:) and not a redirect
                        char path[96];
                        snprintf(path, sizeof path, "%s/%s/index.dti", kRoot, g_books[bi].dir);
                        sdLock();
                        File f = SD.open(path, FILE_READ);
                        uint8_t rec[REC];
                        for (int t = 0; f && t < 40 && readRec(f, i, rec) && (rec[114] & 3); ++t)
                            i = (uint32_t)random((long)g_books[bi].count);
                        if (f) f.close();
                        sdUnlock();
                    }
                    Serial.printf("[lib] random %lu\n", (unsigned long)i);
                    return openRecord(bi, i);
                }
                paginateMenu(bi);
                if (g_menuPages > 1 && hit(kListPrev, x, y)) { if (g_listPage > 0) --g_listPage; return false; }
                if (g_menuPages > 1 && hit(kListNext, x, y)) { if (g_listPage + 1 < g_menuPages) ++g_listPage; return false; }
                const int i = menuHit(bi, x, y);
                if (i < 0) return false;
                if (g_menu[i].rec >= 0) return openRecord(bi, (uint32_t)g_menu[i].rec);
                return openEntry(bi, g_menu[i].title);
            }
            for (int i = 0; i < letterCount(bi); ++i)
                if (hit(letterRect(bi, i), x, y)) { setLetter(bi, i); g_view = V_LETTER; return false; }
            return false;
        }
        case V_LETTER: {
            if (g_kidCount) {
                for (int i = 0; i < g_kidCount; ++i)
                    if (hit(kidRect(i), x, y)) {
                        const Kid k = g_kids[i];
                        char key[KEY + 1];
                        snprintf(key, sizeof key, "%s", g_levels[g_depth - 1].key);
                        if (!k.more) {   // the tab's key: its first record, one character on
                            char path[96];
                            snprintf(path, sizeof path, "%s/%s/index.dti", kRoot, g_books[g_open].dir);
                            uint8_t rec[REC] = {};
                            sdLock();
                            File f = SD.open(path, FILE_READ);
                            const bool ok = f && readRec(f, k.from, rec);
                            if (f) f.close();
                            sdUnlock();
                            const size_t L = strlen(key);
                            if (ok && rec[L] && L + 4 < (size_t)KEY) {
                                memcpy(key + L, rec + L, utf8Len(rec[L]));
                                key[L + utf8Len(rec[L])] = 0;
                            }
                        }
                        pushLevel(g_open, key, k.label, k.from, k.to, k.more && strcmp(k.label, "MORE") != 0);
                        return false;
                    }
                return false;
            }
            const int per = listRows() + 1;
            const uint32_t n = g_letterTo - g_letterFrom;
            const int pages = n ? (int)((n + per - 1) / per) : 1;
            if (pages > 1 && hit(kListPrev, x, y)) { if (g_listPage > 0) --g_listPage; return false; }
            if (pages > 1 && hit(kListNext, x, y)) { if (g_listPage + 1 < pages) ++g_listPage; return false; }
            const int top = BODY_Y - 30, r = (y - top) / LIST_ROW;
            const uint32_t i = g_letterFrom + (uint32_t)(g_listPage * per + r);
            if (y >= top && r < per && i < g_letterTo) return openRecord(g_open, i);
            return false;
        }
        case V_RECENT: {
            if (hit(kTabs, x, y)) { const int t = tabAt(x); g_view = t == 1 ? V_HOME : t == 2 ? V_RECENT : V_SHELF; g_listPage = 0; return false; }
            loadRecent();
            const int per = recentRows(), pages = (g_recentCount + per - 1) / per;
            if (pages > 1 && hit(kListPrev, x, y)) { if (g_listPage > 0) --g_listPage; return false; }
            if (pages > 1 && hit(kListNext, x, y)) { if (g_listPage + 1 < pages) ++g_listPage; return false; }
            const int r = (y - RECENT_TOP) / RECENT_ROW, i = g_listPage * per + r;
            if (y >= RECENT_TOP && r < per && i < g_recentCount) {
                const int bi = bookIndex(g_recent[i].dir);
                if (bi < 0) { g_note = "book not on this card"; return false; }
                char t[KEY + 1];
                snprintf(t, sizeof t, "%s", g_recent[i].title);
                return openEntry(bi, t);
            }
            return false;
        }
        case V_HOME:
            if (hit(kTabs, x, y)) { const int t = tabAt(x); g_view = t == 1 ? V_HOME : t == 2 ? V_RECENT : V_SHELF; g_listPage = 0; return false; }
            if (hit(kAllSearch, x, y)) { g_scope = -1; g_scopeBook = -1; g_searchFrom = V_HOME; g_view = V_SEARCH; k.clear(); return false; }
            for (int i = 0; i < g_emergCount; ++i)
                if (hit(emergRect(i), x, y)) return openEntry(g_emerg[i].book, g_emerg[i].title);
            for (int i = 0; i < g_secCount; ++i)
                if (hit(cellRect(i), x, y)) { g_sec = i; g_secPage = 0; g_view = V_SECTION; return false; }
            return false;
        case V_SECTION: {
            const Section& sx = g_secs[g_sec];
            if (hit(kSecSearch, x, y)) { g_scope = g_sec; g_scopeBook = -1; g_searchFrom = V_SECTION; g_view = V_SEARCH; k.clear(); return false; }
            const int pages = secPages(sx);
            if (pages > 1 && hit(kSecPrev, x, y)) { if (g_secPage > 0) --g_secPage; return false; }
            if (pages > 1 && hit(kSecNext, x, y)) { if (g_secPage + 1 < pages) ++g_secPage; return false; }
            int ry = TOP + SEC_HEAD;
            for (int kk = 0, seen = 0; kk < sx.n && seen < sx.starts; ++kk) {
                const Entry& e = g_entries[sx.first + kk];
                if (!e.start) continue;
                ++seen;
                if (y >= ry && y < ry + START_H) return openEntry(e.book, e.title);
                ry += START_H + 10;
            }
            if (sx.starts) ry += 8;
            const int per = secRowsPerPage(sx);
            int idx = 0;
            for (int kk = 0; kk < sx.n; ++kk) {
                const Entry& e = g_entries[sx.first + kk];
                if (e.start) continue;
                if (idx >= g_secPage * per && idx < (g_secPage + 1) * per) {
                    if (y >= ry && y < ry + ROW_H) return openEntry(e.book, e.title);
                    ry += ROW_H;
                }
                ++idx;
            }
            return false;
        }
        case V_SEARCH:
            if (hit(kScope, x, y)) { g_scope = -1; g_scopeBook = -1; return false; }
            if (y >= RES_TOP && y < RES_TOP + RESULTS * RES_H && y < k.top - 6) {
                const int i = (y - RES_TOP) / RES_H;
                if (i < g_resultCount) { g_historyCount = 0; return openArticle(g_results[i]); }
                return false;
            }
            if (k.tap(x, y) == kbd::ENTER && g_resultCount > 0) { g_historyCount = 0; return openArticle(g_results[0]); }
            return false;
    }
    return false;
}

bool libraryBack() {
    switch (g_view) {
        case V_SHELF: return false;
        case V_HOME: return false;
        case V_RECENT: return false;
        case V_SECTION: g_view = V_HOME; return true;
        case V_BOOK:
            if (g_bookAZ) { g_bookAZ = false; g_listPage = 0; return true; }
            g_view = V_SHELF; return true;
        case V_LETTER:
            if (popLevel(g_open)) return true;
            g_view = V_BOOK; g_listPage = 0; return true;
        case V_SEARCH: g_view = g_searchFrom; return true;
    }
    return false;
}

const char* libraryTitle() {
    switch (g_view) {
        case V_SECTION: return g_secs[g_sec].name;
        case V_SEARCH: return "Search";
        default: return "";        // the page names itself, big
    }
}

namespace {

// The article whose folded title is exactly the target's, in book bi.
bool findIn(int bi, const char* title, Result& out);
bool findExact(const char* title, Result& out) { return findIn(g_book, title, out); }
bool findIn(int bi, const char* title, Result& out) {
    if (g_bookCount <= 0 || bi < 0 || bi >= g_bookCount) return false;
    // As library/dobafold.py key(): the whole title folded (up to 240 bytes);
    // up to 55 bytes that is the key. A longer one is its first 46 bytes,
    // \x01 and 8 letters of an FNV-1a hash of the whole fold, so long titles
    // that start alike (Wikisource chapters) stay apart. Books converted
    // before that cut long keys at 55 bytes: that is tried second.
    char key[256];
    fold(title, key, sizeof key);
    if (!key[0]) return false;
    auto cutAt = [&](size_t n) {
        while (n > 0 && ((uint8_t)key[n] & 0xC0) == 0x80) --n;
        while (n > 0 && key[n - 1] == ' ') --n;
        return n;
    };
    size_t full = strlen(key);
    if (full > 240) { full = cutAt(240); key[full] = 0; }
    uint8_t want[KEY] = {}, old[KEY] = {};
    bool twoKeys = false;
    if (full <= (size_t)KEY - 1) {
        memcpy(want, key, full);
    } else {
        uint64_t h = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < full; ++i) h = (h ^ (uint8_t)key[i]) * 0x100000001B3ull;
        static const char kB32[] = "abcdefghijklmnopqrstuvwxyz234567";
        const size_t c = cutAt(46);
        memcpy(want, key, c);
        want[c] = 0x01;
        for (int i = 0; i < 8; ++i) want[c + 1 + i] = (uint8_t)kB32[(h >> (5 * i)) & 31];
        const size_t o = cutAt(KEY - 1);
        memcpy(old, key, o);
        twoKeys = true;
    }
    const Book& b = g_books[bi];
    char path[96];
    snprintf(path, sizeof path, "%s/%s/index.dti", kRoot, b.dir);
    bool found = false;
    sdLock();
    File f = SD.open(path, FILE_READ);
    for (int pass = 0; f && !found && pass < (twoKeys ? 2 : 1); ++pass) {
        if (pass == 1) memcpy(want, old, KEY);
        uint8_t rec[REC];
        uint32_t lo = 0, hi = b.count;
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            f.seek(HDR + (uint64_t)mid * REC);
            if (f.read(rec, REC) != REC) break;
            if (memcmp(rec, want, KEY) < 0) lo = mid + 1; else hi = mid;
        }
        if (lo < b.count && f.seek(HDR + (uint64_t)lo * REC) && f.read(rec, REC) == REC &&
            memcmp(rec, want, KEY) == 0) {
            memcpy(out.title, rec + KEY, KEY);
            out.title[KEY] = 0;
            out.chunk = (uint16_t)(rec[112] | rec[113] << 8);
            out.off = rd32(rec + 116);
            out.len = rd32(rec + 120);
            out.book = (int8_t)bi;
            found = true;
        }
    }
    if (f) f.close();
    sdUnlock();
    return found;
}

// "#Heading": a jump inside the article to the heading of that name.
bool jumpToHeading(const char* name) {
    char want[KEY], have[KEY];
    fold(name, want, sizeof want);
    for (int k = 0; k < g_lineCount; ++k) {
        const Line& l = g_lines[k];
        if (l.kind != 'H' && l.kind != 'S' && l.kind != 'T') continue;
        if (!(l.off == 0 || g_text[l.off - 1] == (char)l.kind)) continue;   // its first line
        char text[KEY];
        size_t e = l.off, n = 0;
        while (e < g_textLen && g_text[e] != '\n' && n < sizeof text - 1) text[n++] = g_text[e++];
        text[n] = 0;
        fold(text, have, sizeof have);
        if (!strcmp(want, have)) {
            if (g_historyCount == HISTORY) {
                memmove(g_history, g_history + 1, sizeof(Visit) * (HISTORY - 1));
                --g_historyCount;
            }
            g_history[g_historyCount++] = {g_current, g_book, g_page};
            g_page = pageOfLine(k);
            Serial.printf("[lib] jump '%s' -> page %d\n", name, g_page + 1);
            return true;
        }
    }
    return false;
}

void follow(int link) {
    const char* target = g_targets + g_links[link].target;
    if (target[0] == '#') {
        if (!jumpToHeading(target + 1)) g_note = "not in this article";
        return;
    }
    Result r;
    if (!findExact(target, r)) {
        Serial.printf("[lib] no '%s' in this book\n", target);
        g_note = "not in this book";
        return;
    }
    const Visit v = {g_current, g_book, g_page};
    if (g_historyCount == HISTORY) {
        memmove(g_history, g_history + 1, sizeof(Visit) * (HISTORY - 1));
        --g_historyCount;
    }
    g_history[g_historyCount++] = v;
    if (!openArticle(r)) { --g_historyCount; g_note = "can't open it"; }
}

}  // namespace

bool openTitle(const char* title) {
    count();
    for (int bi = 0; bi < g_bookCount; ++bi) {
        Result r;
        if (findIn(bi, title, r)) { g_book = bi; return openArticle(r); }
    }
    Serial.printf("[lib] no '%s'\n", title);
    return false;
}

// Bigger or smaller text, staying on the same words.
void cycleSize() {
    const uint32_t at = g_lineCount && g_page < g_pageCount ? g_lines[g_pageStarts[g_page]].off : 0;
    g_size = (textSize() + 1) % N_SIZES;
    Preferences p;
    p.begin("doba", false);
    p.putUChar("txtSize", (uint8_t)g_size);
    p.end();
    layout();
    g_page = 0;
    for (int k = 0; k < g_lineCount; ++k)
        if (g_lines[k].off >= at && g_lines[k].kind != 'G') { g_page = pageOfLine(k); break; }
    g_note = kSizeName[g_size];
    Serial.printf("[lib] text size %d pages=%d\n", g_size, g_pageCount);
}

bool tapArticle(int x, int y) {
    if (g_toc) {
        if (y >= H - FOOT_H) {
            if (x < W / 3) { if (g_tocPage > 0) --g_tocPage; }
            else if (x >= W * 2 / 3) { if (g_tocPage + 1 < tocPages()) ++g_tocPage; }
            else g_toc = false;
            return false;
        }
        const int row = (y - (BODY_TOP - 8)) / TOC_ROW;
        const int i = g_tocPage * tocRowsPerPage() + row;
        if (y >= BODY_TOP - 8 && row < tocRowsPerPage() && i < g_tocCount) {
            g_page = pageOfLine(g_toc_lines[i]);
            g_toc = false;
            Serial.printf("[lib] contents -> page %d\n", g_page + 1);
        }
        return false;
    }
    if (hit({kSizeBtn.x - 8, TOP, kSizeBtn.width + 16, kSizeBtn.height + 14}, x, y)) { cycleSize(); return false; }
    if (y >= H - FOOT_H) {            // the footer: back, contents, on
        if (x < W / 3) { if (g_page > 0) --g_page; }
        else if (x >= W * 2 / 3) {
            if (g_page + 1 < g_pageCount) ++g_page;
            else return true;   // CLOSE on the last page: done with it
        }
        else if (g_tocCount) {
            g_toc = true;
            int cur = 0;
            for (int i = 0; i < g_tocCount; ++i)
                if (pageOfLine(g_toc_lines[i]) <= g_page) cur = i;
            g_tocPage = cur / tocRowsPerPage();
            Serial.printf("[lib] contents %d\n", g_tocCount);
        }
        return false;
    }
    // A link, with a finger's slack round it; the nearest wins.
    int best = -1, bestD = 1 << 30;
    for (int i = 0; i < g_hotCount; ++i) {
        const Hot& h = g_hot[i];
        const int dx = x < h.x0 - 10 ? h.x0 - 10 - x : x > h.x1 + 10 ? x - h.x1 - 10 : 0;
        const int dy = y < h.y0 - 4 ? h.y0 - 4 - y : y > h.y1 + 4 ? y - h.y1 - 4 : 0;
        if (dx || dy) continue;
        const int cx = (h.x0 + h.x1) / 2, cy = (h.y0 + h.y1) / 2;
        const int d = (x - cx) * (x - cx) + 4 * (y - cy) * (y - cy);
        if (d < bestD) { bestD = d; best = h.link; }
    }
    if (best >= 0) { follow(best); return false; }
    // Anywhere else on the page: the left third goes back, the rest on — a book.
    if (x > W / 3) { if (g_page + 1 < g_pageCount) ++g_page; }
    else if (g_page > 0) --g_page;
    return false;
}

bool articleBack() {
    if (g_toc) { g_toc = false; return true; }
    while (g_historyCount > 0) {
        const Visit v = g_history[--g_historyCount];
        g_book = v.book;
        if (openArticle(v.r)) {
            g_page = v.page < g_pageCount ? v.page : 0;
            return true;
        }
    }
    return false;
}

void leaveArticle() { g_historyCount = 0; g_toc = false; }

}  // namespace doba_lib
