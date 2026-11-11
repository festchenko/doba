// DOBA: see doba_i18n.h.
#include "doba_i18n.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>

namespace i18n {
namespace {

struct Entry {
    const char* en;
    const char* uk;
};
#include "doba_i18n_uk.inc"   // kUk[]: sorted by en (strcmp), kUkCount

Lang g_lang = EN;
bool g_loaded = false;

const char* find(const char* en) {
    size_t lo = 0, hi = kUkCount;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const int c = strcmp(kUk[mid].en, en);
        if (c == 0) return kUk[mid].uk;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return nullptr;
}

}  // namespace

void load() {
    Preferences p;
    if (p.begin("doba", true)) {
        g_lang = p.getUChar("lang", EN) == UK ? UK : EN;
        p.end();
    }
    g_loaded = true;
}

Lang lang() {
    if (!g_loaded) load();
    return g_lang;
}

void setLang(Lang l) {
    g_lang = l;
    g_loaded = true;
    Preferences p;
    if (p.begin("doba", false)) {
        p.putUChar("lang", (uint8_t)l);
        p.end();
    }
}

const char* tr(const char* en) {
    if (!en || lang() == EN) return en;
    const char* uk = find(en);
#ifdef OTP_SIM
    bool ascii = true;
    for (const char* c = en; *c; ++c) ascii &= (unsigned char)*c < 0x80;
    if (!uk && ascii && en[0]) {   // the simulator lists what is still English
        static char last[8][64];
        static int k = 0;
        bool seen = false;
        for (auto& l : last) seen |= !strncmp(l, en, sizeof l - 1);
        if (!seen) {
            snprintf(last[k++ % 8], sizeof last[0], "%s", en);
            Serial.printf("[i18n] miss '%s'\n", en);
        }
    }
#endif
    return uk ? uk : en;
}

// Ukrainian: 1, 21, 31... one; 2-4, 22-24... few; the rest many.
const char* trn(long n, const char* one, const char* many) {
    const long a = n < 0 ? -n : n;
    if (lang() == EN) return a == 1 ? one : many;
    const char* forms = find(one);   // "одна|дві-чотири|п'ять"
    if (!forms) return a == 1 ? one : many;
    static char buf[4][96];
    static int slot = 0;
    char* out = buf[slot++ % 4];
    const int m10 = a % 10, m100 = a % 100;
    const int which = (m10 == 1 && m100 != 11) ? 0 : (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) ? 1 : 2;
    const char* p = forms;
    for (int i = 0; i < which; ++i) {
        const char* bar = strchr(p, '|');
        if (!bar) break;
        p = bar + 1;
    }
    const char* e = strchr(p, '|');
    const size_t len = e ? (size_t)(e - p) : strlen(p);
    snprintf(out, sizeof buf[0], "%.*s", (int)(len < sizeof buf[0] ? len : sizeof buf[0] - 1), p);
    return out;
}

}  // namespace i18n
