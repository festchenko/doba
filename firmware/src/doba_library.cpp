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
constexpr int MAX_BOOKS = 12;
constexpr int RESULTS = 6;
const char* kRoot = "/doba/lib";

struct Book { char dir[32]; char name[40]; char source[64]; uint32_t count; };
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
                char buf[256] = {};
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
                b.source[0] = 0;
                field("name=", b.name, sizeof b.name);
                field("source=", b.source, sizeof b.source);
            }
            ++g_bookCount;
        }
    }
    if (root) root.close();
    sdUnlock();
}

// Prefix search in one book, appending to g_results (up to RESULTS in all).
void searchBook(int bi, const char* key) {
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
    for (uint32_t i = lo; i < b.count && g_resultCount < RESULTS; ++i) {
        if (f.read(rec, REC) != REC) break;
        if (memcmp(rec, want, klen) != 0) break;
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

bool openArticle(const Result& r) {
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
    Serial.printf("[lib] open '%s' pages=%d links=%d\n", g_title, g_pageCount, g_linkCount);
    return true;
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

enum View { V_HOME, V_SECTION, V_SEARCH };
View g_view = V_HOME;
int g_sec = 0;            // the open section
int g_secPage = 0;        // its list page
int g_scope = -1;         // search scope: -1 all books, else a section
View g_searchFrom = V_HOME;

const EpdRect kSearchBar = {14, TOP + 12, W - 28, 66};
constexpr int EM_Y = TOP + 92, EM_H = 108;
int gridTop() { return g_emergCount ? EM_Y + 28 + EM_H + 16 : TOP + 96; }
int gridRows() { return g_secCount > 12 ? 5 : 4; }
EpdRect cellRect(int i) {
    const int rows = gridRows();
    const int top = gridTop(), gap = 8, cw = (W - 28 - 2 * gap) / 3;
    const int ch = (H - 12 - top - (rows - 1) * gap) / rows;
    return {14 + (i % 3) * (cw + gap), top + (i / 3) * (ch + gap), cw, ch};
}
EpdRect emergRect(int i) {
    const int gap = 10, w = (W - 28 - (g_emergCount - 1) * gap) / g_emergCount;
    return {14 + i * (w + gap), EM_Y + 28, w, EM_H};
}

void renderHome(uint8_t* fb) {
    // Search: one bar, every book.
    frame(kSearchBar, 3, fb);
    doba_icons::draw(doba_icons::I_SEARCH, kSearchBar.x + 38, kSearchBar.y + 33, 38, INK, PAPER, fb);
    ui::text(ui::font(ui::F_LABEL), kSearchBar.x + 74, kSearchBar.y + 43, "Search all books", fb, EPD_DRAW_ALIGN_LEFT, INK);
    char nb[32];
    snprintf(nb, sizeof nb, "%d book%s", g_bookCount, g_bookCount == 1 ? "" : "s");
    ui::text(ui::font(ui::F_TINY), kSearchBar.x + kSearchBar.width - 16, kSearchBar.y + 42, nb, fb, EPD_DRAW_ALIGN_RIGHT, INK);
    // EMERGENCY: straight to the page that matters.
    if (g_emergCount) {
        ui::text(ui::font(ui::F_TINY), 16, EM_Y + 18, "EMERGENCY", fb, EPD_DRAW_ALIGN_LEFT, INK);
        for (int i = 0; i < g_emergCount; ++i) {
            const EpdRect r = emergRect(i);
            epd_fill_rect(r, INK, fb);
            doba_icons::draw(g_emerg[i].icon, r.x + r.width / 2, r.y + 40, 48, PAPER, INK, fb);
            const EpdFont* ef = ui::font(ui::F_SMALL);
            if (ui::textWidth(ef, g_emerg[i].label) > r.width - 14) ef = ui::font(ui::F_TINY);
            ui::text(ef, r.x + r.width / 2, r.y + r.height - 16, g_emerg[i].label, fb, EPD_DRAW_ALIGN_CENTER, PAPER);
        }
    }
    // The sections.
    for (int i = 0; i < g_secCount; ++i) {
        const EpdRect r = cellRect(i);
        frame(r, 2, fb);
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
constexpr int SEC_HEAD = 120, ROW_H = 84, START_H = 112;
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
    snprintf(t, sizeof t, "%.16s", g_books[bi].name);
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
    // START HERE cards
    for (int k = 0, seen = 0; k < x.n && seen < x.starts; ++k) {
        const Entry& e = g_entries[x.first + k];
        if (!e.start) continue;
        ++seen;
        const EpdRect r = {14, y, W - 28, START_H};
        frame(r, 4, fb);
        epd_fill_rect({r.x, r.y, 150, 30}, INK, fb);
        ui::text(ui::font(ui::F_TINY), r.x + 12, r.y + 21, "START HERE", fb, EPD_DRAW_ALIGN_LEFT, PAPER);
        bookTag(e.book, r.x + r.width - 14, r.y + 22, INK, fb);
        ui::text(ui::font(ui::F_KEY), r.x + 16, r.y + 68, e.label, fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(ui::font(ui::F_TINY), r.x + 16, r.y + 96, e.desc, fb, EPD_DRAW_ALIGN_LEFT, INK);
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
            ui::text(ui::font(ui::F_LABEL), 20, y + 36, e.label, fb, EPD_DRAW_ALIGN_LEFT, INK);
            ui::text(ui::font(ui::F_TINY), 20, y + 64, e.desc, fb, EPD_DRAW_ALIGN_LEFT, INK);
            bookTag(e.book, W - 22, y + 36, INK, fb);
            ui::text(ui::font(ui::F_KEY), W - 22, y + 70, ">", fb, EPD_DRAW_ALIGN_RIGHT, INK);
            epd_fill_rect({14, y + ROW_H - 2, W - 28, 1}, INK, fb);
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
const EpdRect kScope = {12, TOP + 8, W - 24, 52};
const EpdRect kQuery = {12, TOP + 68, W - 24, 64};
constexpr int RES_TOP = TOP + 142, RES_H = 60;

uint32_t scopeMask() { return g_scope < 0 ? 0xFFFFFFFFu : g_secs[g_scope].books; }

void renderSearchView(kbd::Keyboard& k, uint8_t* fb) {
    k.top = H - k.height();
    k.enterLabel = "OPEN";
    char line[96];
    if (g_scope < 0) snprintf(line, sizeof line, "IN: ALL BOOKS (%d)", g_bookCount);
    else snprintf(line, sizeof line, "IN: %s   ·   all books >", g_secs[g_scope].name);
    frame(kScope, 2, fb);
    ui::text(ui::font(ui::F_SMALL), kScope.x + 14, kScope.y + 34, line, fb, EPD_DRAW_ALIGN_LEFT, INK);
    frame(kQuery, 3, fb);
    char q[300];
    snprintf(q, sizeof q, "%s_", k.text);
    if (k.text[0])
        ui::text(ui::font(ui::F_KEY), kQuery.x + 14, kQuery.y + 44, q, fb, EPD_DRAW_ALIGN_LEFT, INK);
    else
        ui::text(ui::font(ui::F_SMALL), kQuery.x + 14, kQuery.y + 42, "_  type the start of a title", fb,
                 EPD_DRAW_ALIGN_LEFT, INK);
    char key[KEY + 1];
    fold(k.text, key, sizeof key);
    char want[KEY + 8];
    snprintf(want, sizeof want, "%d|%s", g_scope, key);
    if (strcmp(want, g_lastQuery) != 0) {
        snprintf(g_lastQuery, sizeof g_lastQuery, "%s", want);
        if (key[0]) search(key, scopeMask()); else g_resultCount = 0;
    }
    for (int i = 0; i < g_resultCount; ++i) {
        const int y = RES_TOP + i * RES_H;
        if (y + RES_H > k.top) break;
        ui::text(ui::font(ui::F_KEY), 24, y + 40, g_results[i].title, fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (g_bookCount > 1) bookTag(g_results[i].book, W - 20, y + 22, INK, fb);
        epd_fill_rect({12, y + RES_H - 2, W - 24, 1}, INK, fb);
    }
    if (key[0] && g_resultCount == 0)
        ui::text(ui::font(ui::F_SMALL), W / 2, RES_TOP + 60, "nothing starts with that", fb, EPD_DRAW_ALIGN_CENTER, INK);
    if (!key[0]) {
        ui::text(ui::font(ui::F_SMALL), W / 2, RES_TOP + 50, "e.g.  water, burn, knot, костёр", fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::text(ui::font(ui::F_TINY), W / 2, RES_TOP + 84, "case and accents don't matter", fb, EPD_DRAW_ALIGN_CENTER, INK);
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

// Two arrows and a middle, the same on every reading screen.
void footer(const char* left, const char* mid, const char* midSub, const char* right, uint8_t* fb) {
    const int fy = H - FOOT_H;
    epd_fill_rect({0, fy, W, 2}, INK, fb);
    const EpdFont* k = ui::font(ui::F_KEY);
    if (left) {
        epd_fill_triangle(26, fy + 39, 44, fy + 27, 44, fy + 51, INK, fb);
        ui::text(k, 56, fy + 50, left, fb, EPD_DRAW_ALIGN_LEFT, INK);
    }
    if (right && strcmp(right, "END") != 0) {
        epd_fill_triangle(W - 26, fy + 39, W - 44, fy + 27, W - 44, fy + 51, INK, fb);
    }
    if (right) {
        ui::text(k, strcmp(right, "END") ? W - 56 : W * 5 / 6, fy + 50, right, fb,
                 strcmp(right, "END") ? EPD_DRAW_ALIGN_RIGHT : EPD_DRAW_ALIGN_CENTER, INK);
    }
    epd_fill_rect({W / 3, fy + 14, 1, FOOT_H - 28}, INK, fb);
    epd_fill_rect({W * 2 / 3, fy + 14, 1, FOOT_H - 28}, INK, fb);
    ui::text(ui::font(ui::F_LABEL), W / 2, fy + (midSub ? 36 : 46), mid, fb, EPD_DRAW_ALIGN_CENTER, INK);
    if (midSub) ui::text(ui::font(ui::F_TINY), W / 2, fy + 60, midSub, fb, EPD_DRAW_ALIGN_CENTER, INK);
}

void renderToc(uint8_t* fb) {
    ui::text(ui::font(ui::F_LABEL), 14, TOP + 34, "CONTENTS", fb, EPD_DRAW_ALIGN_LEFT, INK);
    epd_fill_rect({0, TOP + 52, W, 2}, INK, fb);
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
        if (i == cur) epd_fill_rect({0, y + 8, 8, TOC_ROW - 16}, INK, fb);   // you are here
        ui::text(tf, MARGIN + in, y + TOC_ROW / 2 + 8, buf, fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(f, W - MARGIN, y + TOC_ROW / 2 + 8, pn, fb, EPD_DRAW_ALIGN_RIGHT, INK);
        epd_fill_rect({MARGIN + in, y + TOC_ROW - 1, W - 2 * MARGIN - in, 1}, INK, fb);
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
    if (g_view == V_SECTION || (g_view == V_SEARCH && g_searchFrom == V_SECTION))
        snprintf(crumb, sizeof crumb, "%s  >  %s", g_secs[g_sec].name, g_title);
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
    ui::text(cf, 14, TOP + 34, crumb, fb, EPD_DRAW_ALIGN_LEFT, INK);
    frame(kSizeBtn, 2, fb);
#ifdef VFONT
    ui::text(vf_face_cap(&Literata_700, 12.f), kSizeBtn.x + 22, kSizeBtn.y + 30, "A", fb, EPD_DRAW_ALIGN_CENTER, INK);
    ui::text(vf_face_cap(&Literata_700, 21.f), kSizeBtn.x + 50, kSizeBtn.y + 32, "A", fb, EPD_DRAW_ALIGN_CENTER, INK);
#else
    ui::text(ui::font(ui::F_LABEL), kSizeBtn.x + kSizeBtn.width / 2, kSizeBtn.y + 30, "Aa", fb, EPD_DRAW_ALIGN_CENTER, INK);
#endif
    // Rule doubles as a progress bar: thick for the read part.
    epd_fill_rect({0, TOP + 54, W, 2}, INK, fb);
    epd_fill_rect({0, TOP + 52, W * (g_page + 1) / g_pageCount, 6}, INK, fb);

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
           g_page + 1 < g_pageCount ? "NEXT" : "END", fb);
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
        case V_HOME: renderHome(fb); break;
        case V_SECTION: renderSection(fb); break;
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
        case V_HOME:
            if (hit(kSearchBar, x, y)) { g_scope = -1; g_searchFrom = V_HOME; g_view = V_SEARCH; k.clear(); return false; }
            for (int i = 0; i < g_emergCount; ++i)
                if (hit(emergRect(i), x, y)) return openEntry(g_emerg[i].book, g_emerg[i].title);
            for (int i = 0; i < g_secCount; ++i)
                if (hit(cellRect(i), x, y)) { g_sec = i; g_secPage = 0; g_view = V_SECTION; return false; }
            return false;
        case V_SECTION: {
            const Section& sx = g_secs[g_sec];
            if (hit(kSecSearch, x, y)) { g_scope = g_sec; g_searchFrom = V_SECTION; g_view = V_SEARCH; k.clear(); return false; }
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
            if (hit(kScope, x, y)) { g_scope = -1; return false; }
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
        case V_HOME: return false;
        case V_SECTION: g_view = V_HOME; return true;
        case V_SEARCH: g_view = g_searchFrom == V_SECTION ? V_SECTION : V_HOME; return true;
    }
    return false;
}

const char* libraryTitle() {
    switch (g_view) {
        case V_SECTION: return g_secs[g_sec].name;
        case V_SEARCH: return "SEARCH";
        default: return "LIBRARY";
    }
}

namespace {

// The article whose folded title is exactly the target's, in book bi.
bool findIn(int bi, const char* title, Result& out);
bool findExact(const char* title, Result& out) { return findIn(g_book, title, out); }
bool findIn(int bi, const char* title, Result& out) {
    if (g_bookCount <= 0 || bi < 0 || bi >= g_bookCount) return false;
    char key[KEY + 1];
    fold(title, key, sizeof key);
    if (!key[0]) return false;
    uint8_t want[KEY] = {};
    memcpy(want, key, strlen(key) < (size_t)KEY ? strlen(key) : KEY);
    const Book& b = g_books[bi];
    char path[96];
    snprintf(path, sizeof path, "%s/%s/index.dti", kRoot, b.dir);
    bool found = false;
    sdLock();
    File f = SD.open(path, FILE_READ);
    if (f) {
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
        f.close();
    }
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

void tapArticle(int x, int y) {
    if (g_toc) {
        if (y >= H - FOOT_H) {
            if (x < W / 3) { if (g_tocPage > 0) --g_tocPage; }
            else if (x >= W * 2 / 3) { if (g_tocPage + 1 < tocPages()) ++g_tocPage; }
            else g_toc = false;
            return;
        }
        const int row = (y - (BODY_TOP - 8)) / TOC_ROW;
        const int i = g_tocPage * tocRowsPerPage() + row;
        if (y >= BODY_TOP - 8 && row < tocRowsPerPage() && i < g_tocCount) {
            g_page = pageOfLine(g_toc_lines[i]);
            g_toc = false;
            Serial.printf("[lib] contents -> page %d\n", g_page + 1);
        }
        return;
    }
    if (hit({kSizeBtn.x - 8, TOP, kSizeBtn.width + 16, kSizeBtn.height + 14}, x, y)) { cycleSize(); return; }
    if (y >= H - FOOT_H) {            // the footer: back, contents, on
        if (x < W / 3) { if (g_page > 0) --g_page; }
        else if (x >= W * 2 / 3) { if (g_page + 1 < g_pageCount) ++g_page; }
        else if (g_tocCount) {
            g_toc = true;
            int cur = 0;
            for (int i = 0; i < g_tocCount; ++i)
                if (pageOfLine(g_toc_lines[i]) <= g_page) cur = i;
            g_tocPage = cur / tocRowsPerPage();
            Serial.printf("[lib] contents %d\n", g_tocCount);
        }
        return;
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
    if (best >= 0) { follow(best); return; }
    // Anywhere else on the page: the left third goes back, the rest on — a book.
    if (x > W / 3) { if (g_page + 1 < g_pageCount) ++g_page; }
    else if (g_page > 0) --g_page;
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
