// DOBA: the map's pages: map options and legend, points, nearby places, a point,
// a point typed in as MGRS.
#include "doba_ui_shared.h"

namespace doba_ui {
namespace pg {

// --- MAPOPTS: what the map shows, and its legend --------------------------------------

constexpr int MROW_TOP = TOP + 20, MROW_H = 90, MROWS = 3;
const EpdRect kOpenLegend = {24, TOP + 20 + MROWS * 90 + 30, W - 48, 64};
const EpdRect kOpenPoints = {24, TOP + 20 + MROWS * 90 + 30 + 64 + 16, W - 48, 64};
bool g_legend = false;

// A row that opens a page: the words left, a chevron right, in a hairline box.
void linkRow(const EpdRect& r, const char* label, const char* hint, uint8_t* fb) {
    ui::frameRound(r, 10, 2, fb);
    ui::text(ui::font(ui::F_LABEL), r.x + 20, r.y + (hint ? 28 : r.height / 2 + 9), TR(label), fb, EPD_DRAW_ALIGN_LEFT, INK);
    if (hint) ui::text(ui::font(ui::F_TINY), r.x + 20, r.y + 52, TR(hint), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    const int cx = r.x + r.width - 26, cy = r.y + r.height / 2;
    for (int t = -1; t <= 1; ++t) {
        epd_draw_line(cx - 4 + t, cy - 9, cx + 4 + t, cy, INK, fb);
        epd_draw_line(cx + 4 + t, cy, cx - 4 + t, cy + 9, INK, fb);
    }
}

// One line of the legend: a sample drawn at x 24..96, the words after it.
void legendLine(int y, const char* what, const char* hint, uint8_t* fb) {
    ui::text(ui::font(ui::F_SMALL), 116, y + (hint ? 4 : 14), TR(what), fb, EPD_DRAW_ALIGN_LEFT, INK);
    if (hint) ui::text(ui::font(ui::F_TINY), 116, y + 28, TR(hint), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
}

void poiTag(int cx, int cy, int kind, uint8_t* fb) {
    const EpdFont* f = ui::monoFont(13);
    const char* c = milspec::poiCode(kind);
    const int w = ui::textWidth(f, c) + 12;
    ui::fillRound({cx - w / 2, cy - 12, w, 24}, 6, INK, fb);
    ui::text(f, cx, cy + 6, c, fb, EPD_DRAW_ALIGN_CENTER, PAPER);
}

void renderLegend(const DeviceState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    double lat = 50, lon = 30;
    float mpp = 8;
    milspec::lastView(lat, lon, mpp);
    if (s.gpsFix || s.everHadFix) { lat = s.latitude; lon = s.longitude; }
    float ev, ix, fev, fix;
    milspec::contourInterval(lat, mpp, false, ev, ix);
    milspec::contourInterval(lat, mpp, true, fev, fix);
    char t1[128], t2[128];
    int y = TOP + 6;

    section(y, "RELIEF AT THIS ZOOM", fb);
    y += 52;
    epd_fill_rect({24, y, 72, 1}, 0x44, fb);
    snprintf(t1, sizeof t1, TR("Contour every %g m"), ev);
    snprintf(t2, sizeof t2, TR("flat country: every %g m"), fev);
    legendLine(y - 6, t1, t2, fb);
    y += 56;
    epd_fill_rect({24, y - 1, 72, 3}, 0x44, fb);
    ui::text(ui::font(ui::F_TINY), 60, y - 6, "120", fb, EPD_DRAW_ALIGN_CENTER, INK);
    snprintf(t1, sizeof t1, TR("Bold every %g m, height on it"), ix);
    snprintf(t2, sizeof t2, TR("flat country: every %g m \xC2\xB7 zoom in for finer"), fix);
    legendLine(y - 6, t1, t2, fb);

    y += 48;
    section(y, "GRID AND COMPASS", fb);
    y += 52;
    epd_fill_rect({24, y, 72, 2}, 0x22, fb);
    epd_fill_rect({58, y - 18, 2, 38}, 0x22, fb);
    char sq[4];
    milspec::square(lat, lon, sq, sizeof sq);
    snprintf(t2, sizeof t2, TR("square %s: the 100 km letters, said before the numbers"), sq[0] ? sq : "LC");
    legendLine(y - 6, "MGRS lines, 1 km apart", t2, fb);
    y += 56;
    {
        const float d = milgeo::declination(lat, lon);
        snprintf(t1, sizeof t1, TR("Magnetic north %.1f\xC2\xB0 %s of true"), fabsf(d), d >= 0 ? TR("east") : TR("west"));
        // a needle: N up, turned by the declination
        const float a = d * (float)M_PI / 180.0f;
        const int cx = 60, cy = y + 4;
        for (int k = -1; k <= 1; ++k)
            epd_draw_line(cx + k, cy + 18, cx + (int)(sinf(a) * 22) + k, cy - (int)(cosf(a) * 22), INK, fb);
        ui::text(ui::font(ui::F_TINY), cx + (int)(sinf(a) * 22) + 10, cy - 12, "N", fb, EPD_DRAW_ALIGN_LEFT, INK);
        legendLine(y - 6, t1, "compass az on the map already allows for it", fb);
    }

    y += 48;
    section(y, "MARKS", fb);
    y += 50;
    {   // saved point · going to · the cross · measure from A
        int x = 40;
        for (int k = 0; k < 3; ++k) {
            epd_draw_line(x, y - 12 + k, x - 10 + k, y + 7 - k, INK, fb);
            epd_draw_line(x - 10 + k, y + 7 - k, x + 10 - k, y + 7 - k, INK, fb);
            epd_draw_line(x + 10 - k, y + 7 - k, x, y - 12 + k, INK, fb);
        }
        ui::text(ui::font(ui::F_SMALL), 64, y + 8, TR("Saved point"), fb, EPD_DRAW_ALIGN_LEFT, INK);
        x = 290;
        for (int r = 0; r < 3; ++r) epd_draw_circle(x, y, 14 - r, INK, fb);
        epd_fill_rect({x - 20, y - 1, 40, 3}, INK, fb);
        epd_fill_rect({x - 1, y - 20, 3, 40}, INK, fb);
        ui::text(ui::font(ui::F_SMALL), 318, y + 8, TR("Going to"), fb, EPD_DRAW_ALIGN_LEFT, INK);
        y += 54;
        x = 40;
        epd_fill_rect({x - 16, y - 2, 11, 5}, INK, fb);
        epd_fill_rect({x + 6, y - 2, 11, 5}, INK, fb);
        epd_fill_rect({x - 2, y - 16, 5, 11}, INK, fb);
        epd_fill_rect({x - 2, y + 6, 5, 11}, INK, fb);
        ui::text(ui::font(ui::F_SMALL), 64, y - 2, TR("Cross"), fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(ui::font(ui::F_TINY), 64, y + 20, TR("drag the map: read here"), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        x = 290;
        epd_fill_circle(x, y, 13, INK, fb);
        ui::text(ui::font(ui::F_STRONG), x, y + 6, "A", fb, EPD_DRAW_ALIGN_CENTER, PAPER);
        ui::text(ui::font(ui::F_SMALL), 318, y - 2, TR("Measure from A"), fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(ui::font(ui::F_TINY), 318, y + 20, TR("tap the cross to set"), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    }

    y += 40;
    section(y, "PLACES", fb);
    y += 48;
    const int per = (doba_pois::KINDS - 1 + 1) / 2;
    for (int k = 1; k < doba_pois::KINDS; ++k) {
        const int col = (k - 1) / per, row = (k - 1) % per;
        const int x = 24 + col * 258, yy = y + row * 36;
        poiTag(x + 20, yy, k, fb);
        ui::text(ui::font(ui::F_SMALL), x + 48, yy + 7, doba_pois::kindName(k), fb, EPD_DRAW_ALIGN_LEFT, INK);
    }
}

void renderMapOpts(const DeviceState& s, uint8_t* fb) {
    if (g_legend) { renderLegend(s, fb); return; }
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    // No "up is north / course": the topo tiles are drawn as they are, north up.
    int y = MROW_TOP;
    caption(y, MROW_H, "MGRS GRID", "1 km lines, 10 km zoomed out", fb);
    toggle(y, MROW_H, milspec::gridOn(), fb);
    epd_fill_rect({24, y + MROW_H - 1, W - 48, 1}, ui::HAIR, fb);
    y += MROW_H;
    caption(y, MROW_H, "POINTS", "your saved points", fb);
    toggle(y, MROW_H, milspec::pointsOn(), fb);
    epd_fill_rect({24, y + MROW_H - 1, W - 48, 1}, ui::HAIR, fb);
    y += MROW_H;
    caption(y, MROW_H, "PLACES", doba_pois::available() ? "hospitals, water, food... zoomed in" : "no places file on the card", fb);
    toggle(y, MROW_H, milspec::poisOn(), fb);
    linkRow(kOpenLegend, "Legend", "contours, grid, marks, place letters", fb);
    linkRow(kOpenPoints, "Saved points", nullptr, fb);
    const char* tips[] = {"Hold a finger on the map 1 s to drop a point.",
                          "Drag the map: the strip reads the cross.",
                          "Tap the cross to measure from there."};
    for (int i = 0; i < 3; ++i)
        ui::text(ui::font(ui::F_SMALL), 24, kOpenPoints.y + kOpenPoints.height + 56 + i * 36,
                 TR(tips[i]), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
}

Go tapMapOpts(int x, int y) {
    if (g_legend) return STAY;
    if (hit(kOpenLegend, x, y)) { g_legend = true; return STAY; }
    if (hit(kOpenPoints, x, y)) return TO_POINTS;
    const int r = (y - MROW_TOP) / MROW_H;
    if (y < MROW_TOP || r >= MROWS) return STAY;
    if (r == 0 && toggleHit(x)) milspec::setGridOn(x >= CTRL_X + CTRL_W - 75);
    if (r == 1 && toggleHit(x)) milspec::setPointsOn(x >= CTRL_X + CTRL_W - 75);
    if (r == 2 && toggleHit(x)) milspec::setPoisOn(x >= CTRL_X + CTRL_W - 75);
    return STAY;
}

// A single text field over the keyboard (the callsign).
void renderField(const char* caption, const char* note, kbd::Keyboard& k, const char* enter, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    k.top = H - k.height();
    k.enterLabel = enter;
    ui::text(ui::font(ui::F_SMALL), 24, TOP + 40, TR(caption), fb, EPD_DRAW_ALIGN_LEFT, INK);
    const EpdRect box = {24, TOP + 60, W - 48, 90};   // the page margin, as every other screen
    frame(box, 3, fb);
    char shown[300];
    snprintf(shown, sizeof shown, "%s_", k.text);
    // A long text scrolls: the end, where the typing is, stays in the box.
    const char* vis = shown;
    while (*vis && ui::textWidth(ui::font(ui::F_KEY), vis) > box.width - 32) {
        ++vis;
        while (((uint8_t)*vis & 0xC0) == 0x80) ++vis;
    }
    ui::text(ui::font(ui::F_KEY), box.x + 16, box.y + 58, vis, fb, EPD_DRAW_ALIGN_LEFT, INK);
    if (note) ui::text(ui::font(ui::F_TINY), 24, box.y + box.height + 34, TR(note), fb, EPD_DRAW_ALIGN_LEFT, INK);
    k.render(fb);
}

bool g_fieldOwner = false;   // the SETUP_NAME page edits the owner card instead
int g_ownerField = 0;        // which of its fields: one after another, SAVE moves on
extern const char* const kOwnerCap[settings::OWNER_FIELDS];
const char* const kOwnerCap[settings::OWNER_FIELDS] = {
    "IF FOUND 1/5: NAME", "IF FOUND 2/5: DATE OF BIRTH", "IF FOUND 3/5: BLOOD GROUP",
    "IF FOUND 4/5: ALLERGIES, ILLNESSES", "IF FOUND 5/5: EMERGENCY CONTACT"};
Go tapSetupName(int x, int y) {
    const kbd::Action a = g_nameKbd.tap(x, y);
    if (g_fieldOwner) {
        while (strlen(g_nameKbd.text) > 60) kbd::popUtf8(g_nameKbd.text);
        if (a == kbd::ENTER) {
            settings::setOwnerField(g_ownerField, g_nameKbd.text);
            if (++g_ownerField >= settings::OWNER_FIELDS) { g_fieldOwner = false; return TO_MORE; }
            g_nameKbd.clear();
            snprintf(g_nameKbd.text, sizeof g_nameKbd.text, "%s", settings::ownerField(g_ownerField));
        }
        return STAY;
    }
    while (strlen(g_nameKbd.text) > 36) kbd::popUtf8(g_nameKbd.text);
    if (a == kbd::ENTER && g_nameKbd.text[0]) {
        char sh[20];
        shortName(g_nameKbd.text, sh, sizeof sh);
        mesh_service::setNames(g_nameKbd.text, sh);
        return TO_SETUP;
    }
    return STAY;
}

// --- POINTS ----------------------------------------------------------------------

int g_pointsScroll = 0;
int g_pointSel = -1;
bool g_deleteArmed = false;
const char* g_pointsNote = nullptr;     // a one-off line ("no fix yet")
constexpr int PROW_TOP = TOP + 436, PROW_H = 80, PROW_N = 4;
const EpdRect kMark = {24, TOP + 306, (W - 48 - 14) / 2, 64};
const EpdRect kAdd = {24 + (W - 48 - 14) / 2 + 14, TOP + 306, (W - 48 - 14) / 2, 64};
const EpdRect kUp = {W - 24 - 56 - 12 - 56, H - 78, 56, 52};
const EpdRect kDown = {W - 24 - 56, H - 78, 56, 52};

// A small square button with a chevron: d -1 left/up, +1 right/down.
void chevButton(const EpdRect& r, bool live, bool vertical, int d, uint8_t* fb) {
    const uint8_t c = live ? INK : 0xA0;
    ui::frameRound(r, 8, 2, fb, c, PAPER);
    const int cx = r.x + r.width / 2, cy = r.y + r.height / 2;
    for (int t = -1; t <= 1; ++t) {
        if (vertical) {
            epd_draw_line(cx - 10, cy - d * 4 + t, cx, cy + d * 5 + t, c, fb);
            epd_draw_line(cx, cy + d * 5 + t, cx + 10, cy - d * 4 + t, c, fb);
        } else {
            epd_draw_line(cx - d * 4 + t, cy - 10, cx + d * 5 + t, cy, c, fb);
            epd_draw_line(cx + d * 5 + t, cy, cx - d * 4 + t, cy + 10, c, fb);
        }
    }
}

// --- NEARBY: useful places round you (doba_pois) ---------------------------------
int g_nearby = 0;       // 0 off, 1 the kinds, 2 the places of one kind
int g_nearKind = 0, g_nearPage = 0;
constexpr int NEAR_TOP = TOP + 112, NEAR_ROW = 84;
constexpr int NEAR_MAX = 30;
PsArr<doba_pois::Poi, NEAR_MAX> g_near;
int g_nearCount = 0;
doba_pois::Poi g_each[doba_pois::KINDS];
int g_kindOrder[doba_pois::KINDS], g_kindCount = 0;   // the kinds found, nearest first

void compass8(double deg, char* out, size_t n) {
    static const char* const k[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    snprintf(out, n, "%s", TR(k[(int)((fmod(deg + 360.0, 360.0) + 22.5) / 45.0) % 8]));
}
void nearLine(const doba_pois::Poi& p, char* out, size_t n) {
    char ds[16], az[16], c8[16];
    doba_points::formatDistance(p.dist, ds, sizeof ds);
    azText(p.bearing, az, sizeof az);
    compass8(p.bearing, c8, sizeof c8);
    snprintf(out, n, TR("%s %s \xC2\xB7 az %s"), ds, c8, az);
}
int nearPerPage() { return (H - 90 - NEAR_TOP) / NEAR_ROW; }

void renderNearby(const DeviceState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    double lat, lon;
    if (!here(s, lat, lon)) {
        ui::text(ui::font(ui::F_H1), 24, TOP + 44, TR("Nearby"), fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(ui::font(ui::F_SMALL), 24, TOP + 120, TR("Needs a position: wait for a GPS fix."), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        return;
    }
    if (!doba_pois::available()) {
        ui::text(ui::font(ui::F_H1), 24, TOP + 44, TR("Nearby"), fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(ui::font(ui::F_SMALL), 24, TOP + 120, TR("No list of places on this card."), fb, EPD_DRAW_ALIGN_LEFT, 0x30);
        ui::text(ui::font(ui::F_SMALL), 24, TOP + 150, TR("It is /doba/map/pois.dgp (maps/pois.py)."), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        return;
    }
    const int per = nearPerPage();
    char line[96];
    if (g_nearby == 1) {
        ui::text(ui::font(ui::F_H1), 24, TOP + 44, TR("Nearby"), fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(ui::font(ui::F_SMALL), 24, TOP + 78, TR("Useful places within about 40 km, offline"), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        doba_pois::nearestOfEach(lat, lon, g_each);
        g_kindCount = 0;
        // the fixed order, most urgent first; underground parking with the shelters
        static const uint8_t kOrder[] = {1, 2, 3, 4, 5, 6, 7, 8, 17, 9, 10, 11, 12, 13, 14, 15, 16};
        static_assert(sizeof kOrder == doba_pois::KINDS - 1, "every kind once");
        for (uint8_t k : kOrder) g_kindOrder[g_kindCount++] = k;
        const int pages = (g_kindCount + per - 1) / per;
        if (g_nearPage >= pages) g_nearPage = pages - 1;
        for (int r = 0; r < per && g_nearPage * per + r < g_kindCount; ++r) {
            const int k = g_kindOrder[g_nearPage * per + r];
            const int y = NEAR_TOP + r * NEAR_ROW;
            const doba_pois::Poi& p = g_each[k];
            const bool none = p.dist < 0;
            ui::text(ui::font(ui::F_LABEL), 24, y + 36, doba_pois::kindName(k), fb, EPD_DRAW_ALIGN_LEFT, none ? ui::GREY : INK);
            if (none) snprintf(line, sizeof line, TR("none within 40 km"));
            else {
                char d[48];
                nearLine(p, d, sizeof d);
                snprintf(line, sizeof line, "%s%s%s", d, p.name[0] ? " \xC2\xB7 " : "", p.name);
            }
            fit(ui::font(ui::F_SMALL), line, W - 90);
            ui::text(ui::font(ui::F_SMALL), 24, y + 66, line, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
            if (!none) ui::text(ui::font(ui::F_KEY), W - 26, y + NEAR_ROW / 2 + 12, "\xE2\x80\xBA", fb, EPD_DRAW_ALIGN_RIGHT, 0x80);
            epd_fill_rect({24, y + NEAR_ROW - 1, W - 48, 1}, ui::HAIR, fb);
        }
        if (pages > 1) {
            char pg[16];
            snprintf(pg, sizeof pg, TR("%d of %d"), g_nearPage + 1, pages);
            ui::text(ui::font(ui::F_SMALL), 28, kUp.y + 34, pg, fb, EPD_DRAW_ALIGN_LEFT, 0x40);
            chevButton(kUp, g_nearPage > 0, false, -1, fb);
            chevButton(kDown, g_nearPage + 1 < pages, false, 1, fb);
        }
        return;
    }
    // one kind: the nearest, a tap makes it a point to go to
    ui::text(ui::font(ui::F_H1), 24, TOP + 44, doba_pois::kindName(g_nearKind), fb, EPD_DRAW_ALIGN_LEFT, INK);
    ui::text(ui::font(ui::F_SMALL), 24, TOP + 78, doba_pois::kindHint(g_nearKind), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    g_nearCount = doba_pois::nearest(lat, lon, g_nearKind, g_near, NEAR_MAX);
    const int pages = (g_nearCount + per - 1) / per;
    if (g_nearPage >= pages) g_nearPage = pages ? pages - 1 : 0;
    if (!g_nearCount)
        ui::text(ui::font(ui::F_SMALL), 24, NEAR_TOP + 40, TR("None within about 40 km."), fb, EPD_DRAW_ALIGN_LEFT, 0x30);
    for (int r = 0; r < per && g_nearPage * per + r < g_nearCount; ++r) {
        const doba_pois::Poi& p = g_near[g_nearPage * per + r];
        const int y = NEAR_TOP + r * NEAR_ROW;
        char nm[48];
        snprintf(nm, sizeof nm, "%s", p.name[0] ? p.name : doba_pois::kindName(p.kind));
        fit(ui::font(ui::F_LABEL), nm, W - 90);
        ui::text(ui::font(ui::F_LABEL), 24, y + 36, nm, fb, EPD_DRAW_ALIGN_LEFT, INK);
        nearLine(p, line, sizeof line);
        ui::text(ui::font(ui::F_SMALL), 24, y + 66, line, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        ui::text(ui::font(ui::F_KEY), W - 26, y + NEAR_ROW / 2 + 12, "\xE2\x80\xBA", fb, EPD_DRAW_ALIGN_RIGHT, 0x80);
        epd_fill_rect({24, y + NEAR_ROW - 1, W - 48, 1}, ui::HAIR, fb);
    }
    char pg[32];
    snprintf(pg, sizeof pg, pages > 1 ? TR("%d of %d \xC2\xB7 tap one to go there") : TR("Tap one to go there"), g_nearPage + 1, pages);
    ui::text(ui::font(ui::F_SMALL), 28, kUp.y + 34, pg, fb, EPD_DRAW_ALIGN_LEFT, 0x40);
    if (pages > 1) {
        chevButton(kUp, g_nearPage > 0, false, -1, fb);
        chevButton(kDown, g_nearPage + 1 < pages, false, 1, fb);
    }
}

Go tapNearby(int x, int y, const DeviceState& s) {
    const int per = nearPerPage();
    if (hit(kUp, x, y)) { if (g_nearPage > 0) --g_nearPage; return STAY; }
    if (hit(kDown, x, y)) { ++g_nearPage; return STAY; }   // clamped when drawn
    if (y < NEAR_TOP) return STAY;
    const int r = (y - NEAR_TOP) / NEAR_ROW, i = g_nearPage * per + r;
    if (r >= per) return STAY;
    if (g_nearby == 1) {
        if (i >= g_kindCount) return STAY;
        const int k = g_kindOrder[i];
        if (g_each[k].dist < 0) return STAY;
        g_nearKind = k;
        g_nearPage = 0;
        g_nearby = 2;
        Serial.printf("[pois] kind %s\n", doba_pois::kindName(k));
        return STAY;
    }
    if (i >= g_nearCount) return STAY;
    const doba_pois::Poi& p = g_near[i];
    char nm[24];
    snprintf(nm, sizeof nm, "%.23s", p.name[0] ? p.name : doba_pois::kindName(p.kind));
    const int at = doba_points::add(nm, p.lat, p.lon);
    if (at < 0) { g_pointsNote = "list full"; return STAY; }
    Serial.printf("[pois] point %d '%s'\n", at, nm);
    g_nearby = 0;
    g_pointSel = at;
    g_deleteArmed = false;
    return TO_POINT;
}

void renderPoints(const DeviceState& s, uint8_t* fb) {
    if (g_nearby) { renderNearby(s, fb); return; }
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    double lat, lon;
    const bool have = here(s, lat, lon);
    ui::text(ui::font(ui::F_SMALL), 24, TOP + 34,
             s.gpsFix ? TR("Your position \xC2\xB7 MGRS") : have ? TR("Last position \xC2\xB7 no fix now") : TR("Your position"),
             fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    if (g_pointsNote) {
        ui::text(ui::font(ui::F_SMALL), W - 24, TOP + 34, g_pointsNote, fb, EPD_DRAW_ALIGN_RIGHT, INK);
        g_pointsNote = nullptr;
    }
    char m[32] = "";
    if (have) milgeo::mgrs(lat, lon, m, sizeof m);
    // the grid reference as big as it reads: zone small, the two numbers large
    char zone[16] = "", e[12] = "", nn[12] = "";
    {
        char t[32];
        snprintf(t, sizeof t, "%s", m);
        char* tok[4] = {};
        int k = 0;
        for (char* p = strtok(t, " "); p && k < 4; p = strtok(nullptr, " ")) tok[k++] = p;
        if (k == 4) {
            snprintf(zone, sizeof zone, "%s %s", tok[0], tok[1]);
            snprintf(e, sizeof e, "%s", tok[2]);
            snprintf(nn, sizeof nn, "%s", tok[3]);
        }
    }
    if (zone[0]) {
        ui::text(ui::monoFont(22), 24, TOP + 84, zone, fb, EPD_DRAW_ALIGN_LEFT, 0x40);
        ui::text(ui::valueFont(46), 20, TOP + 152, e, fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(ui::valueFont(46), 20, TOP + 216, nn, fb, EPD_DRAW_ALIGN_LEFT, INK);
    } else {
        ui::text(ui::font(ui::F_H1), 24, TOP + 120, have ? m : TR("No fix yet"), fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (!have) ui::text(ui::font(ui::F_SMALL), 24, TOP + 160, TR("Waiting for satellites"), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    }
    char ll[64], info[64];
    float dep;
    if (have) {
        snprintf(ll, sizeof ll, TR("%.4f\xC2\xB0 %c, %.4f\xC2\xB0 %c \xC2\xB7 decl %+.1f\xC2\xB0"), fabs(lat), lat >= 0 ? 'N' : 'S',
                 fabs(lon), lon >= 0 ? 'E' : 'W', milgeo::declination(lat, lon));
        if (doba_dem::depth(lat, lon, dep))   // on water: depth under you (EMODnet)
            snprintf(info, sizeof info, TR("%d satellites \xC2\xB7 %d m deep"), s.satellites, (int)lroundf(dep));
        else
            snprintf(info, sizeof info, TR("%d satellites \xC2\xB7 %d m high"), s.satellites, (int)lroundf(groundAlt(s)));
        ui::text(ui::font(ui::F_SMALL), 24, TOP + 252, ll, fb, EPD_DRAW_ALIGN_LEFT, 0x30);
        ui::text(ui::font(ui::F_SMALL), 24, TOP + 280, info, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    }
    button(kMark, "Mark here", true, fb);
    button(kAdd, "Enter coords", false, fb);

    const int n = doba_points::count();
    if (g_pointsScroll > n - 1) g_pointsScroll = n > 0 ? ((n - 1) / PROW_N) * PROW_N : 0;
    ui::text(ui::font(ui::F_TINY), 24, PROW_TOP - 14, TR("SAVED POINTS"), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    // Nearby: hospitals, water, food... (maps/pois.py) — a link, top right of the list
    {
        const EpdFont* lf = ui::font(ui::F_LABEL);
        const char* nb = TR("Nearby places \xE2\x80\xBA");
        ui::text(lf, W - 24, PROW_TOP - 12, nb, fb, EPD_DRAW_ALIGN_RIGHT, INK);
    }
    if (n == 0) {
        ui::text(ui::font(ui::F_SMALL), 24, PROW_TOP + 34, TR("Mark here saves where you stand,"), fb, EPD_DRAW_ALIGN_LEFT, 0x30);
        ui::text(ui::font(ui::F_SMALL), 24, PROW_TOP + 62, TR("Enter coords takes a grid reference,"), fb, EPD_DRAW_ALIGN_LEFT, 0x30);
        ui::text(ui::font(ui::F_SMALL), 24, PROW_TOP + 90, TR("or hold a finger on the map."), fb, EPD_DRAW_ALIGN_LEFT, 0x30);
    }
    for (int k = 0; k < PROW_N && g_pointsScroll + k < n; ++k) {
        const int i = g_pointsScroll + k;
        const doba_points::Point* p = doba_points::at(i);
        const int y = PROW_TOP + k * PROW_H;
        const bool tgt = doba_points::target() == i;
        if (tgt) ui::fillRound({16, y + 4, W - 32, PROW_H - 8}, 8, INK, fb);
        const uint8_t fg = tgt ? PAPER : INK, dim = tgt ? PAPER : ui::GREY;
        char nm[40];
        snprintf(nm, sizeof nm, tgt ? "\xE2\x80\xBA %s" : "%s", p->name);
        ui::text(ui::font(ui::F_LABEL), 28, y + 36, nm, fb, EPD_DRAW_ALIGN_LEFT, fg);
        char pm[32];
        milgeo::mgrs(p->lat, p->lon, pm, sizeof pm);
        ui::text(ui::font(ui::F_TINY), 28, y + 64, pm, fb, EPD_DRAW_ALIGN_LEFT, dim);
        if (have) {
            double b, d;
            doba_points::fromHere(i, lat, lon, b, d);
            char az[16], ds[24];
            azText(b, az, sizeof az);
            doba_points::formatDistance(d, ds, sizeof ds);
            ui::text(ui::font(ui::F_LABEL), W - 28, y + 36, ds, fb, EPD_DRAW_ALIGN_RIGHT, fg);
            char azl[24];
            snprintf(azl, sizeof azl, TR("az %s"), az);
            ui::text(ui::font(ui::F_SMALL), W - 28, y + 64, azl, fb, EPD_DRAW_ALIGN_RIGHT, dim);
        }
        if (!tgt) epd_fill_rect({24, y + PROW_H - 1, W - 48, 1}, ui::HAIR, fb);
    }
    if (n > PROW_N) {
        char pg[24];
        snprintf(pg, sizeof pg, TR("%d of %d"), g_pointsScroll / PROW_N + 1, (n + PROW_N - 1) / PROW_N);
        ui::text(ui::font(ui::F_SMALL), 28, kUp.y + 34, pg, fb, EPD_DRAW_ALIGN_LEFT, 0x40);
        chevButton(kUp, g_pointsScroll > 0, false, -1, fb);
        chevButton(kDown, g_pointsScroll + PROW_N < n, false, 1, fb);
    }
}

kbd::Keyboard g_newGrid, g_newName;
bool g_newFocusName = false;
const char* g_newError = nullptr;

Go tapPoints(int x, int y, const DeviceState& s) {
    if (g_nearby) return tapNearby(x, y, s);
    if (y >= PROW_TOP - 46 && y < PROW_TOP + 4 && x > W / 2) { g_nearby = 1; g_nearPage = 0; return STAY; }
    double lat, lon;
    if (hit(kMark, x, y)) {
        if (!here(s, lat, lon)) { g_pointsNote = "no fix yet"; return STAY; }
        const int i = doba_points::add(nullptr, lat, lon);
        g_pointsNote = i < 0 ? "list full" : "marked";
        if (i >= 0) g_pointsScroll = (i / PROW_N) * PROW_N;
        return STAY;
    }
    if (hit(kAdd, x, y)) {
        g_newGrid.clear();
        g_newName.clear();
        g_newFocusName = false;
        g_newError = nullptr;
        // Start from this square: most points are typed in near where you are.
        if (here(s, lat, lon)) {
            char m[32];
            milgeo::mgrs(lat, lon, m, sizeof m);
            if (strlen(m) > 7) { memcpy(g_newGrid.text, m, 7); g_newGrid.text[7] = 0; }
        }
        g_newGrid.layout = kbd::SYM;   // digits first
        return TO_POINT_NEW;
    }
    const int n = doba_points::count();
    if (n > PROW_N && hit(kUp, x, y)) { g_pointsScroll = g_pointsScroll >= PROW_N ? g_pointsScroll - PROW_N : 0; return STAY; }
    if (n > PROW_N && hit(kDown, x, y)) { if (g_pointsScroll + PROW_N < n) g_pointsScroll += PROW_N; return STAY; }
    if (y >= PROW_TOP && y < PROW_TOP + PROW_N * PROW_H) {
        const int i = g_pointsScroll + (y - PROW_TOP) / PROW_H;
        if (i < n) { g_pointSel = i; g_deleteArmed = false; return TO_POINT; }
    }
    return STAY;
}

// --- POINT: one waypoint -----------------------------------------------------------

const EpdRect kGoto = {24, H - 180, W - 48, 72};
const EpdRect kDel = {24, H - 94, (W - 48 - 14) / 2, 64};
const EpdRect kToMap = {24 + (W - 48 - 14) / 2 + 14, H - 94, (W - 48 - 14) / 2, 64};

void arrow(int cx, int cy, int r, float deg, uint8_t c, uint8_t* fb) {
    const float a = deg * (float)M_PI / 180.0f;
    const float sx = sinf(a), sy = -cosf(a);
    const int tx = cx + (int)(sx * r), ty = cy + (int)(sy * r);
    const int bx = cx - (int)(sx * r * 0.35f), by = cy - (int)(sy * r * 0.35f);
    const int px = (int)(-sy * r * 0.28f), py = (int)(sx * r * 0.28f);
    epd_fill_triangle(tx, ty, bx + px, by + py, cx, cy, c, fb);
    epd_fill_triangle(tx, ty, bx - px, by - py, cx, cy, c, fb);
}

void renderPoint(const DeviceState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    const doba_points::Point* p = doba_points::at(g_pointSel);
    if (!p) return;
    {
        char nm[64];
        snprintf(nm, sizeof nm, "%s", p->name);
        fit(ui::font(ui::F_H1), nm, W - 48);
        ui::text(ui::font(ui::F_H1), 24, TOP + 52, nm, fb, EPD_DRAW_ALIGN_LEFT, INK);
    }
    char pm[32];
    milgeo::mgrs(p->lat, p->lon, pm, sizeof pm);
    ui::text(ui::monoFont(22), 24, TOP + 94, pm, fb, EPD_DRAW_ALIGN_LEFT, INK);
    char ll[64];
    snprintf(ll, sizeof ll, "%.5f\xC2\xB0 %c, %.5f\xC2\xB0 %c", fabs(p->lat), p->lat >= 0 ? 'N' : 'S', fabs(p->lon), p->lon >= 0 ? 'E' : 'W');
    ui::text(ui::font(ui::F_SMALL), 24, TOP + 124, ll, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    epd_fill_rect({24, TOP + 144, W - 48, 1}, ui::HAIR, fb);

    double lat, lon;
    const int dialY = TOP + 340, dialR = 140, dialX = 24 + 140;
    if (here(s, lat, lon)) {
        double b, d;
        doba_points::fromHere(g_pointSel, lat, lon, b, d);
        // North-up dial: ticks every 10-00 (60°), N marked, the arrow to the point.
        for (int t = 0; t < 3; ++t) epd_draw_circle(dialX, dialY, dialR - t, INK, fb);
        for (int k = 0; k < 12; ++k) {
            const float a = k * 30.0f * (float)M_PI / 180.0f;
            const int r0 = k % 3 ? dialR - 14 : dialR - 26;
            epd_draw_line(dialX + (int)(sinf(a) * r0), dialY - (int)(cosf(a) * r0),
                          dialX + (int)(sinf(a) * dialR), dialY - (int)(cosf(a) * dialR), INK, fb);
        }
        ui::text(ui::font(ui::F_LABEL), dialX, dialY - dialR + 52, "N", fb, EPD_DRAW_ALIGN_CENTER, INK);
        arrow(dialX, dialY, dialR - 34, (float)b, INK, fb);
        epd_fill_circle(dialX, dialY, 6, PAPER, fb);
        epd_draw_circle(dialX, dialY, 6, INK, fb);
        if (s.gpsFix && s.gpsSpeedKmh >= 2.0f) {   // where you are heading, as a ring tick
            const float a = s.courseDeg * (float)M_PI / 180.0f;
            epd_fill_circle(dialX + (int)(sinf(a) * (dialR + 12)), dialY - (int)(cosf(a) * (dialR + 12)), 8, INK, fb);
        }
        char az[16], deg[16], ds[24];
        azText(b, az, sizeof az);
        snprintf(deg, sizeof deg, "%03d\xC2\xB0", (int)lround(b) % 360);
        doba_points::formatDistance(d, ds, sizeof ds);
        const int tx = 336;
        ui::text(ui::font(ui::F_SMALL), tx, dialY - 104, TR("Azimuth"), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        ui::text(ui::valueFont(30), tx, dialY - 60, az, fb, EPD_DRAW_ALIGN_LEFT, INK);
        ui::text(ui::font(ui::F_SMALL), tx, dialY - 30, deg, fb, EPD_DRAW_ALIGN_LEFT, INK);
        {   // what to set on the compass here: true less the declination
            float mb = (float)b - milgeo::declination(lat, lon);
            if (mb < 0) mb += 360.0f;
            char cm[8], ct[32];
            milgeo::mils(mb, milspec::milsSystem(), cm, sizeof cm);
            snprintf(ct, sizeof ct, TR("compass %s"), cm);
            ui::text(ui::font(ui::F_SMALL), tx, dialY - 6, ct, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        }
        ui::text(ui::font(ui::F_SMALL), tx, dialY + 44, TR("Distance"), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        ui::text(ui::valueFont(28), tx, dialY + 86, ds, fb, EPD_DRAW_ALIGN_LEFT, INK);
        // Height of the point (DEM) and the climb to it.
        float pz;
        bool wet = false;
        if (doba_dem::ground(p->lat, p->lon, pz, wet) && wet) {
            char hz[48];
            snprintf(hz, sizeof hz, TR("Depth %d m"), (int)lroundf(pz));
            ui::text(ui::font(ui::F_SMALL), tx, dialY + 126, hz, fb, EPD_DRAW_ALIGN_LEFT, INK);
        } else if (doba_dem::height(p->lat, p->lon, pz)) {
            char hz[48];
            const int dz = (int)lroundf(pz - groundAlt(s));
            snprintf(hz, sizeof hz, TR("Height %d m"), (int)lroundf(pz));
            ui::text(ui::font(ui::F_SMALL), tx, dialY + 126, hz, fb, EPD_DRAW_ALIGN_LEFT, INK);
            snprintf(hz, sizeof hz, dz >= 0 ? TR("climb %+d m") : TR("drop %d m"), dz);
            ui::text(ui::font(ui::F_SMALL), tx, dialY + 154, hz, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        }
        if (!s.gpsFix)
            ui::text(ui::font(ui::F_SMALL), 24, dialY + dialR + 40, TR("From the last GPS fix"), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    } else {
        ui::text(ui::font(ui::F_SMALL), 24, dialY, TR("No GPS fix yet: the way to it shows when there is one."), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    }
    const bool tgt = doba_points::target() == g_pointSel;
    button(kGoto, tgt ? "Stop guiding" : "Go here", !tgt, fb);
    button(kDel, g_deleteArmed ? "Tap again to delete" : "Delete", g_deleteArmed, fb);
    button(kToMap, "Show on map", false, fb);
    if (!tgt) ui::text(ui::font(ui::F_SMALL), 24, kGoto.y - 16, TR("Go here: the map points you to it"), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
}

Go tapPoint(int x, int y) {
    if (!doba_points::at(g_pointSel)) return TO_POINTS;
    if (hit(kGoto, x, y)) {
        if (doba_points::target() == g_pointSel) { doba_points::setTarget(-1); return STAY; }
        doba_points::setTarget(g_pointSel);
        return TO_MAP;
    }
    if (hit(kDel, x, y)) {
        if (!g_deleteArmed) { g_deleteArmed = true; return STAY; }
        doba_points::remove(g_pointSel);
        g_pointSel = -1;
        return TO_POINTS;
    }
    if (hit(kToMap, x, y)) return TO_MAP;
    g_deleteArmed = false;
    return STAY;
}

// --- POINT_NEW: a waypoint typed in as MGRS ------------------------------------------

// At least 1 km precision (two digit pairs) before a grid counts: the bare
// "32U LC" the field starts with would otherwise be a point 50 km away.
int gridDigits(const char* t) {
    int n = 0;
    for (; *t; ++t) if (*t >= '0' && *t <= '9') ++n;
    return n;
}
bool parseGrid(const char* t, double& la, double& lo) {
    // zone digits (1-2) are not easting/northing digits
    int zone = 0;
    while (t[zone] >= '0' && t[zone] <= '9') ++zone;
    return gridDigits(t) - zone >= 4 && milgeo::parseMgrs(t, la, lo);
}

const EpdRect kGridBox = {24, TOP + 64, W - 48, 76};
const EpdRect kNameBox = {24, TOP + 200, W - 48, 76};

void renderPointNew(const DeviceState& s, uint8_t* fb) {
    (void)s;
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    kbd::Keyboard& k = g_newFocusName ? g_newName : g_newGrid;
    k.top = H - k.height();
    k.enterLabel = "Save";
    auto field = [&](const EpdRect& r, const char* cap, const char* hint, const char* txt, bool focus, const char* empty) {
        ui::text(ui::font(ui::F_SMALL), r.x, r.y - 14, TR(cap), fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (hint) ui::text(ui::font(ui::F_SMALL), r.x + r.width, r.y - 14, TR(hint), fb, EPD_DRAW_ALIGN_RIGHT, ui::GREY);
        ui::frameRound(r, 10, focus ? 3 : 1, fb);
        char shown[300];
        const bool ph = !txt[0] && !focus;
        snprintf(shown, sizeof shown, focus ? "%s_" : "%s", txt[0] ? txt : (focus ? "" : TR(empty)));
        ui::text(ui::font(ui::F_KEY), r.x + 18, r.y + 50, shown, fb, EPD_DRAW_ALIGN_LEFT, ph ? ui::GREY : INK);
    };
    field(kGridBox, "Grid (MGRS)", "e.g. 32U LC 12345 67890", g_newGrid.text, !g_newFocusName, "");
    field(kNameBox, "Name", "optional", g_newName.text, g_newFocusName, "numbered if left empty");
    double la, lo;
    const char* line = g_newError;
    char ok[64];
    if (!line && g_newGrid.text[0] && parseGrid(g_newGrid.text, la, lo)) {
        snprintf(ok, sizeof ok, "= %.5f\xC2\xB0 %c, %.5f\xC2\xB0 %c", fabs(la), la >= 0 ? 'N' : 'S', fabs(lo), lo >= 0 ? 'E' : 'W');
        line = ok;
    }
    if (line) ui::text(ui::font(ui::F_SMALL), 24, kNameBox.y + kNameBox.height + 40, line, fb, EPD_DRAW_ALIGN_LEFT, g_newError ? INK : ui::GREY);
    k.render(fb);
}

Go tapPointNew(int x, int y) {
    if (hit(kGridBox, x, y)) { g_newFocusName = false; return STAY; }
    if (hit(kNameBox, x, y)) { g_newFocusName = true; return STAY; }
    kbd::Keyboard& k = g_newFocusName ? g_newName : g_newGrid;
    const kbd::Action a = k.tap(x, y);
    while (strlen(g_newName.text) > doba_points::NAME_LEN - 1) kbd::popUtf8(g_newName.text);
    while (strlen(g_newGrid.text) > 24) kbd::popUtf8(g_newGrid.text);
    g_newError = nullptr;
    if (a == kbd::ENTER) {
        double la, lo;
        if (!parseGrid(g_newGrid.text, la, lo)) { g_newError = "Needs zone, square and 4 to 10 digits"; g_newFocusName = false; return STAY; }
        const int i = doba_points::add(g_newName.text, la, lo);
        if (i < 0) { g_newError = "The list is full (64 points)"; return STAY; }
        g_pointsScroll = (i / PROW_N) * PROW_N;
        g_pointsNote = "added";
        return TO_POINTS;
    }
    return STAY;
}

}  // namespace pg
}  // namespace doba_ui
