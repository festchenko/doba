#include "doba_i18n.h"
#include "doba_points.h"

#include <Arduino.h>
#include <SD.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "milgeo.h"
#include "sd_bus.h"

namespace doba_points {
namespace {

constexpr const char* kPath = "/doba/points.txt";
Point g_pts[MAX_POINTS];
int g_count = 0;
int g_target = -1;
char g_targetName[NAME_LEN] = "";   // the target survives a reload by name
bool g_loaded = false;

void save() {
    sdLock();
    SD.mkdir("/doba");
    File f = SD.open(kPath, FILE_WRITE);   // truncates
    if (f) {
        char line[96];
        for (int i = 0; i < g_count; ++i) {
            const int n = snprintf(line, sizeof line, "%s;%.6f;%.6f\n", g_pts[i].name,
                                   g_pts[i].lat, g_pts[i].lon);
            f.write((const uint8_t*)line, n);
        }
        f.close();
    }
    sdUnlock();
}

}  // namespace

void load() {
    g_loaded = true;
    g_count = 0;
    sdLock();
    File f = SD.open(kPath, FILE_READ);
    if (f) {
        char line[96];
        size_t k = 0;
        auto take = [&]() {
            line[k] = 0;
            k = 0;
            char* a = strchr(line, ';');
            char* b = a ? strchr(a + 1, ';') : nullptr;
            if (!a || !b || g_count >= MAX_POINTS) return;
            *a = 0;
            Point& p = g_pts[g_count];
            snprintf(p.name, sizeof p.name, "%s", line);
            p.lat = atof(a + 1);
            p.lon = atof(b + 1);
            if (p.lat >= -90 && p.lat <= 90 && p.lon >= -180 && p.lon <= 180) ++g_count;
        };
        while (f.available()) {
            const int c = f.read();
            if (c < 0) break;
            if (c == '\n' || c == '\r') { if (k) take(); continue; }
            if (k < sizeof line - 1) line[k++] = (char)c;
        }
        if (k) take();
        f.close();
    }
    sdUnlock();
    g_target = -1;
    for (int i = 0; i < g_count && g_targetName[0]; ++i)
        if (!strcmp(g_pts[i].name, g_targetName)) g_target = i;
}

static void ensure() { if (!g_loaded) load(); }
void rescan() { g_loaded = false; }
int count() { ensure(); return g_count; }
const Point* at(int i) { ensure(); return (i >= 0 && i < g_count) ? &g_pts[i] : nullptr; }

int add(const char* name, double lat, double lon) {
    ensure();
    if (g_count >= MAX_POINTS) return -1;
    Point& p = g_pts[g_count];
    if (name && name[0]) {
        // ';' would split the line on reload.
        size_t j = 0;
        for (const char* s = name; *s && j < sizeof p.name - 1; ++s) p.name[j++] = *s == ';' ? ',' : *s;
        p.name[j] = 0;
    } else {
        snprintf(p.name, sizeof p.name, "PT%02d", g_count + 1);
    }
    p.lat = lat;
    p.lon = lon;
    ++g_count;
    save();
    return g_count - 1;
}

void remove(int i) {
    if (i < 0 || i >= g_count) return;
    if (g_target == i) setTarget(-1);
    else if (g_target > i) --g_target;
    memmove(&g_pts[i], &g_pts[i + 1], sizeof(Point) * (g_count - i - 1));
    --g_count;
    save();
}

int target() { ensure(); return g_target; }

void setTarget(int i) {
    g_target = (i >= 0 && i < g_count) ? i : -1;
    snprintf(g_targetName, sizeof g_targetName, "%s", g_target >= 0 ? g_pts[g_target].name : "");
}

bool fromHere(int i, double lat, double lon, double& bearingDeg, double& distM) {
    const Point* p = at(i);
    if (!p) return false;
    milgeo::bearingDistance(lat, lon, p->lat, p->lon, bearingDeg, distM);
    return true;
}

void formatDistance(double m, char* out, size_t n) {
    if (m < 1000) snprintf(out, n, TR("%d m"), (int)(m + 0.5));
    else if (m < 10000) snprintf(out, n, TR("%.2f km"), m / 1000.0);
    else snprintf(out, n, TR("%.1f km"), m / 1000.0);
}

}  // namespace doba_points
