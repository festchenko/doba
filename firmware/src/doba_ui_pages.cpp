// DOBA: SOS, SUN, MORE and BATTERY.
#include "doba_ui_shared.h"

namespace doba_ui {
namespace pg {

// --- SOS: a distress call over the mesh -------------------------------------------

bool g_sosOn = false, g_sosArmed = false;
uint32_t g_sosArmedAt = 0, g_sosLast = 0;
int g_sosSent = 0;
const EpdRect kSosBtn = {30, TOP + 300, W - 60, 220};
const EpdRect kSosCancel = {30, H - 130, W - 60, 96};

void wrapAt(const EpdFont* f, int x, int y, int maxW, int lh, const char* text, uint8_t* fb) {
    int k = 0;
    wrap(f, text, maxW, [&](const char* l) {
        ui::text(f, x, y + k * lh, l, fb, EPD_DRAW_ALIGN_LEFT, INK);
        ++k;
    });
}

void sosMessage(const DeviceState& s, char* out, size_t n) {
    char where[32] = "position unknown";
    if (s.gpsFix || s.everHadFix) milgeo::mgrs(s.latitude, s.longitude, where, sizeof where);
    char when[16] = "";
    if (s.utc) {
        struct tm g;
        const time_t t = s.utc;
        gmtime_r(&t, &g);
        snprintf(when, sizeof when, " %02d%02dZ", g.tm_hour, g.tm_min);
    }
    snprintf(out, n, "SOS SOS SOS %s %s%s%s", mesh_service::longName(), where, when,
             s.gpsFix ? "" : (s.everHadFix ? " (last fix)" : ""));
}

void sosSend(const DeviceState& s) {
    // Calling for help is the one thing that ends stealth by itself.
    if (stealth::active()) stealth::set(false);
    if (!mesh_service::enabled()) mesh_service::setEnabled(true);
    char msg[200];
    sosMessage(s, msg, sizeof msg);
    mesh_service::queueText(0xFFFFFFFFu, msg, 0);
    g_sosLast = millis();
    ++g_sosSent;
    Serial.printf("[sos] sent #%d: %s\n", g_sosSent, msg);
}

void sosSubtitle(char* out, size_t n) {
    if (g_sosOn) snprintf(out, n, TR("Active \xC2\xB7 sent %d"), g_sosSent);
    else snprintf(out, n, TR("Hold 5 s to send"));
}

void renderSos(const DeviceState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    char msg[200];
    sosMessage(s, msg, sizeof msg);
    if (!g_sosOn) {
        ui::text(ui::font(ui::F_SMALL), W / 2, TOP + 60, TR("Broadcasts on the mesh, every 5 min:"), fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::frameRound({24, TOP + 84, W - 48, 150}, 8, 2, fb, 0x90, PAPER);
        wrapAt(ui::font(ui::F_BODY), 36, TOP + 124, W - 72, 34, msg, fb);
        const bool armed = g_sosArmed && millis() - g_sosArmedAt < 6000;
        ui::fillRound(kSosBtn, 10, INK, fb);
        ui::text(ui::font(ui::F_BIG), W / 2, kSosBtn.y + 120, TR("SOS"), fb, EPD_DRAW_ALIGN_CENTER, PAPER);
        ui::text(ui::font(ui::F_SMALL), W / 2, kSosBtn.y + 180, armed ? TR("Hold it for 5 seconds to send") : TR("Hold for 5 seconds to send"), fb,
                 EPD_DRAW_ALIGN_CENTER, PAPER);
        ui::text(ui::font(ui::F_TINY), W / 2, kSosBtn.y + kSosBtn.height + 50,
                 stealth::active() ? TR("STEALTH IS ON: sending ends it and reveals you") :
                 mesh_service::enabled() ? TR("radio is on") : TR("radio is off: it will be turned on"), fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::text(ui::font(ui::F_TINY), W / 2, kSosBtn.y + kSosBtn.height + 80,
                 TR("Anyone on the channel within range sees it."), fb, EPD_DRAW_ALIGN_CENTER, INK);
        return;
    }
    epd_fill_rect({0, TOP, W, 290}, INK, fb);
    ui::text(ui::font(ui::F_BIG), W / 2, TOP + 130, TR("SOS ACTIVE"), fb, EPD_DRAW_ALIGN_CENTER, PAPER);
    char line[64];
    const uint32_t left = g_sosLast + SOS_EVERY_MS > millis() ? (g_sosLast + SOS_EVERY_MS - millis()) / 1000 : 0;
    snprintf(line, sizeof line, TR("sent %d · next in %lu:%02lu"), g_sosSent, (unsigned long)(left / 60),
             (unsigned long)(left % 60));
    ui::text(ui::font(ui::F_LABEL), W / 2, TOP + 210, line, fb, EPD_DRAW_ALIGN_CENTER, PAPER);
    ui::frameRound({24, TOP + 320, W - 48, 150}, 8, 2, fb, 0x90, PAPER);
    wrapAt(ui::font(ui::F_BODY), 36, TOP + 360, W - 72, 34, msg, fb);
    ui::text(ui::font(ui::F_SMALL), W / 2, TOP + 530, TR("Stay with the device. Replies show in COMMS."), fb,
             EPD_DRAW_ALIGN_CENTER, INK);
    button(kSosCancel, "CANCEL SOS", false, fb);
}

Go tapSos(int x, int y, const DeviceState& s) {
    if (!g_sosOn) {
        if (!hit(kSosBtn, x, y)) { g_sosArmed = false; return STAY; }
        // Sent only with a finger held 5 s: never by a brush or a pocket.
        if (g_pressMs >= kHoldMs) {
            g_sosArmed = false;
            g_sosOn = true;
            g_sosSent = 0;
            sosSend(s);
        } else {
            g_sosArmed = true;   // a short tap: say how
            g_sosArmedAt = millis();
        }
        return STAY;
    }
    if (hit(kSosCancel, x, y)) {
        g_sosOn = false;
        Serial.println("[sos] cancelled");
        char msg[96];
        snprintf(msg, sizeof msg, "SOS cancelled by %s", mesh_service::longName());
        mesh_service::queueText(0xFFFFFFFFu, msg, 0);
    }
    return STAY;
}

// --- SUN ---------------------------------------------------------------------------

void hhmm(time_t utc, char* out, size_t n) {
    const time_t t = utc + settings::tzMinutes() * 60 + 30;   // to the nearest minute
    struct tm g;
    gmtime_r(&t, &g);
    snprintf(out, n, "%02d:%02d", g.tm_hour, g.tm_min);
}

void sunSubtitle(const DeviceState& s, char* out, size_t n) {
    if (!s.utc || !(s.gpsFix || s.everHadFix)) { snprintf(out, n, TR("needs a GPS fix")); return; }
    const doba_sun::Day d = doba_sun::day(s.latitude, s.longitude, s.utc);
    char t[8];
    if (d.setOk && s.utc < d.set && (!d.riseOk || s.utc > d.rise)) { hhmm(d.set, t, sizeof t); snprintf(out, n, TR("sunset %s"), t); }
    else if (d.riseOk) {
        const time_t r = s.utc > d.rise ? doba_sun::day(s.latitude, s.longitude, s.utc + 86400).rise : d.rise;
        hhmm(r, t, sizeof t);
        snprintf(out, n, TR("sunrise %s"), t);
    } else snprintf(out, n, d.setOk ? "" : TR("polar day/night"));
}

void renderSun(const DeviceState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    if (!s.utc || !(s.gpsFix || s.everHadFix)) {
        ui::text(ui::font(ui::F_KEY), W / 2, TOP + 300, TR("Needs a GPS fix"), fb, EPD_DRAW_ALIGN_CENTER, INK);
        ui::text(ui::font(ui::F_SMALL), W / 2, TOP + 340, TR("for the date, time and place."), fb, EPD_DRAW_ALIGN_CENTER, INK);
        return;
    }
    const doba_sun::Day d = doba_sun::day(s.latitude, s.longitude, s.utc);
    char a[8], b[8], line[64];
    // Daylight left: the headline when the sun is up.
    int y = TOP + 40;
    if (d.riseOk && d.setOk && s.utc >= d.rise && s.utc < d.set) {
        const long left = (long)(d.set - s.utc) / 60;
        ui::text(ui::font(ui::F_SMALL), 24, y + 6, TR("Daylight left"), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        snprintf(line, sizeof line, TR("%ld h %02ld min"), left / 60, left % 60);
        ui::text(ui::font(ui::F_BIG), 20, y + 80, line, fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (d.duskOk) {
            const long l2 = (long)(d.dusk - s.utc) / 60;
            snprintf(line, sizeof line, TR("+ %ld min of twilight after sunset"), l2 - left);
            ui::text(ui::font(ui::F_SMALL), 20, y + 120, line, fb, EPD_DRAW_ALIGN_LEFT, INK);
        }
    } else {
        ui::text(ui::font(ui::F_SMALL), 24, y + 6, TR("Night"), fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        const doba_sun::Day nx = s.utc > d.rise ? doba_sun::day(s.latitude, s.longitude, s.utc + 86400) : d;
        if (nx.riseOk) {
            const long to = (long)(nx.rise - s.utc) / 60;
            snprintf(line, sizeof line, TR("%ld h %02ld min"), to / 60, to % 60);
            ui::text(ui::font(ui::F_BIG), 20, y + 80, line, fb, EPD_DRAW_ALIGN_LEFT, INK);
            ui::text(ui::font(ui::F_SMALL), 20, y + 120, TR("until sunrise"), fb, EPD_DRAW_ALIGN_LEFT, INK);
        }
    }
    // The day's table: after sunset it is tomorrow's — tonight's past times
    // under a "until sunrise" countdown read as the morning ahead.
    const doba_sun::Day td = (d.setOk && s.utc >= d.set) ? doba_sun::day(s.latitude, s.longitude, s.utc + 86400) : d;
    y = TOP + 200;
    epd_fill_rect({24, y, W - 48, 1}, ui::HAIR, fb);
    struct Row { const char* name; bool ok; time_t t; } rows[] = {
        {"FIRST LIGHT", td.dawnOk, td.dawn}, {"SUNRISE", td.riseOk, td.rise},
        {"SUNSET", td.setOk, td.set},        {"LAST LIGHT", td.duskOk, td.dusk},
    };
    for (int i = 0; i < 4; ++i) {
        const int ry = y + 16 + i * 72;
        char rn[64];
        ui::text(ui::font(ui::F_LABEL), 24, ry + 44, ui::sentence(TR(rows[i].name), rn, sizeof rn), fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (rows[i].ok) hhmm(rows[i].t, a, sizeof a); else snprintf(a, sizeof a, "--");
        ui::text(ui::font(ui::F_TITLE), W - 24, ry + 48, a, fb, EPD_DRAW_ALIGN_RIGHT, INK);
        epd_fill_rect({24, ry + 70, W - 48, 1}, ui::HAIR, fb);
    }
    // Sun's bearing at rise and set? Keep it simple: the moon.
    y += 16 + 4 * 72 + 30;
    const double lit = doba_sun::moonLit(s.utc);
    const int mx = 90, my = y + 80, mr = 52;
    epd_fill_circle(mx, my, mr, INK, fb);
    // lit part: an ellipse-ish terminator, drawn as white columns
    const double age = doba_sun::moonAge(s.utc);
    const bool waxing = age < 14.765;
    for (int dx = -mr; dx <= mr; ++dx) {
        const int h = (int)sqrt((double)(mr * mr - dx * dx));
        const double term = mr * (1 - 2 * lit);                // x of the terminator, from the lit edge
        const double xs = waxing ? dx : -dx;                   // lit edge on the right when waxing
        if (xs > term) epd_fill_rect({mx + dx, my - h, 1, 2 * h}, PAPER, fb);
    }
    epd_draw_circle(mx, my, mr, INK, fb);
    char mn[32];
    snprintf(mn, sizeof mn, "%s", TR(doba_sun::moonName(s.utc)));
    ui::capFirst(mn);
    ui::text(ui::font(ui::F_KEY), 170, y + 66, mn, fb, EPD_DRAW_ALIGN_LEFT, INK);
    snprintf(line, sizeof line, TR("%d%% lit · day %d of 29"), (int)lround(lit * 100), (int)age);
    ui::text(ui::font(ui::F_SMALL), 170, y + 104, line, fb, EPD_DRAW_ALIGN_LEFT, INK);
    hhmm(s.utc, b, sizeof b);
    snprintf(line, sizeof line, TR("now %s  ·  UTC%+d  ·  times local"), b, settings::tzMinutes() / 60);
    ui::text(ui::font(ui::F_TINY), W / 2, H - 24, line, fb, EPD_DRAW_ALIGN_CENTER, INK);
}


// --- MORE ----------------------------------------------------------------------

const char* hoursText(float h);
enum MoreRow { M_USB, M_BT, M_WIFI, M_BATT, M_OWNER, M_GPS, M_MODE, M_CARD, M_ABOUT, M_COUNT };
int moreRowY(int r) { return TOP + 4 + SEC_H + r * SROW_H; }

void renderMore(const DeviceState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    section(TOP + 4, "DEVICE", fb);
    for (int r = 0; r < M_COUNT; ++r) {
        const int y = moreRowY(r);
        char sub[64];
        switch (r) {
            case M_USB:
                caption(y, SROW_H, "USB DRIVE", settings::usbDrive() ? "the card shows on a computer" : "off: the card is Doba's", fb);
                toggle(y, SROW_H, settings::usbDrive(), fb);
                break;
            case M_BT:
                // A switch kept for what comes next: nothing in the firmware
                // uses Bluetooth yet, so on or off nothing transmits.
                caption(y, SROW_H, "BLUETOOTH", settings::bluetooth() ? "on: allowed when something needs it" :
                        "off: never switched on", fb);
                toggle(y, SROW_H, settings::bluetooth(), fb);
                break;
            case M_WIFI:
                caption(y, SROW_H, "Wi-Fi", settings::wifi() ? "on: allowed when something needs it" :
                        "off: never switched on", fb);
                toggle(y, SROW_H, settings::wifi(), fb);
                break;
            case M_BATT: {
                doba_batt::Report br;
                doba_batt::report(br);
                if (br.hoursLeft > 0) snprintf(sub, sizeof sub, TR("about %s left"), hoursText(br.hoursLeft));
                else snprintf(sub, sizeof sub, "%s", br.charging ? TR("charging") : TR("measuring..."));
                caption(y, SROW_H, "BATTERY", sub, fb);
                button({CTRL_X, y + (SROW_H - CTRL_H) / 2, CTRL_W, CTRL_H}, "Details  \xE2\x80\xBA", false, fb);
                break;
            }
            case M_OWNER: {
                char o[40];
                snprintf(o, sizeof o, "%.30s", settings::ownerField(0)[0] ? settings::ownerField(0) : settings::ownerSet() ? "..." : "");
                caption(y, SROW_H, "IF FOUND", o[0] ? o : TR("empty: tap to write"), fb);
                button({CTRL_X, y + (SROW_H - CTRL_H) / 2, CTRL_W, CTRL_H}, "Edit  \xE2\x80\xBA", false, fb);
                break;
            }
            case M_GPS:
                snprintf(sub, sizeof sub, s.gpsFix ? TR("fix · %u satellites") : TR("no fix yet"), (unsigned)s.satellites);
                caption(y, SROW_H, "GPS", sub, fb);
                button({CTRL_X, y + (SROW_H - CTRL_H) / 2, CTRL_W, CTRL_H}, "Details  \xE2\x80\xBA", false, fb);
                break;
            case M_MODE:
                // Meshtastic's modem preset. Every node on the mesh has to match;
                // LongFast is what they all use unless told otherwise.
                snprintf(sub, sizeof sub, TR("all nodes must match"));
                caption(y, SROW_H, "RADIO MODE", sub, fb);
                stepper(y, SROW_H, mesh_service::presetName(), fb);
                break;
            case M_CARD:
                snprintf(sub, sizeof sub, s.sdCard == SD_CARD_OK ? TRN(doba_lib::shelfCount(), "%d book", "%d books") : TR("no card"), doba_lib::shelfCount());
                caption(y, SROW_H, "CARD", sub, fb, true);
                break;
            case M_ABOUT:
                {
                    char about[96];
                    snprintf(about, sizeof about, "%s%s", TR("offline backcountry assistant \xC2\xB7 "), __DATE__);
                    caption(y, SROW_H, "DOBA", about, fb, true);
                }
                break;
        }
        epd_fill_rect({24, y + SROW_H - 1, W - 48, 1}, ui::HAIR, fb);
    }
    ui::text(ui::font(ui::F_TINY), W / 2, H - 24, TR("USB drive: copy books and maps from a computer"), fb,
             EPD_DRAW_ALIGN_CENTER, INK);
}

Go tapMore(int x, int y) {
    const int r = (y - moreRowY(0)) / SROW_H;
    if (y < moreRowY(0) || r >= M_COUNT) return STAY;
    if (r == M_USB && toggleHit(x)) {
        const bool on = x >= CTRL_X + CTRL_W - 75;
        settings::setUsbDrive(on);
        if (on) doba_lib::flush();   // the card goes to the computer: nothing left unwritten
        usb_storage::setDriveEnabled(on);
    }
    if (r == M_BT && toggleHit(x)) settings::setBluetooth(x >= CTRL_X + CTRL_W - 75);
    if (r == M_WIFI && toggleHit(x)) settings::setWifi(x >= CTRL_X + CTRL_W - 75);
    if (r == M_GPS && x >= CTRL_X) return TO_GPSDEBUG;
    if (r == M_BATT && x >= CTRL_X) return TO_BATTERY;
    if (r == M_OWNER) {
        g_nameKbd.clear();
        g_ownerField = 0;
        snprintf(g_nameKbd.text, sizeof g_nameKbd.text, "%s", settings::ownerField(0));
        g_fieldOwner = true;
        return TO_SETUP_NAME;
    }
    if (r == M_MODE && x >= CTRL_X) {
        const int n = mesh::PRESET_COUNT, step = stepperHit(x) < 0 ? -1 : 1;
        mesh_service::setPreset((uint8_t)(((mesh_service::presetIndex() + step) % n + n) % n));
    }
    return STAY;
}

// --- BATTERY ---------------------------------------------------------------------
//
// The gauge measures the board as a whole; the split is an estimate from what
// each part is doing (doba_battery.h). The forecast is measured.

const char* hoursText(float h) {
    static char b[32];
    if (h >= 48) snprintf(b, sizeof b, TR("%d days"), (int)(h / 24 + 0.5f));
    else if (h >= 1) snprintf(b, sizeof b, TR("%d h"), (int)(h + 0.5f));
    else snprintf(b, sizeof b, TR("%d min"), (int)(h * 60 + 0.5f));
    return b;
}

void renderBattery(const DeviceState& s, uint8_t* fb) {
    epd_fill_rect({0, TOP, W, H - TOP}, PAPER, fb);
    doba_batt::Report r;
    doba_batt::report(r);
    char big[48], line[128];
    const int pct = r.have ? r.soc : s.batteryPercent;
    snprintf(big, sizeof big, "%d%%", pct);
    ui::text(ui::font(ui::F_TITLE), 24, TOP + 70, big, fb, EPD_DRAW_ALIGN_LEFT, INK);
    // the cell, filled to its level
    const EpdRect cell = {W - 24 - 150, TOP + 28, 140, 56};
    ui::frameRound(cell, 6, 3, fb, INK, PAPER);
    epd_fill_rect({cell.x + cell.width, cell.y + 18, 8, 20}, INK, fb);
    epd_fill_rect({cell.x + 8, cell.y + 8, (cell.width - 16) * std::max(0, std::min(100, pct)) / 100, cell.height - 16}, INK, fb);

    if (r.charging) snprintf(line, sizeof line, "%s", TR("On USB: charging, no forecast"));
    else if (r.hoursLeft > 0) snprintf(line, sizeof line, TR("About %s left at this pace"), hoursText(r.hoursLeft));
    else snprintf(line, sizeof line, "%s", TR("Measuring: a forecast in a few minutes"));
    ui::text(ui::font(ui::F_LABEL), 24, TOP + 130, line, fb, EPD_DRAW_ALIGN_LEFT, INK);
    if (r.have) {
        snprintf(line, sizeof line, TR("%d mA now · %d mA average · %u mAh left · %.2f V"),
                 (int)(r.nowMa + 0.5f), (int)(std::max(0.0f, r.avgMa) + 0.5f), (unsigned)r.remainingMah, r.mv / 1000.0f);
        ui::text(ui::font(ui::F_TINY), 24, TOP + 166, line, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
    }

    section(TOP + 200, "WHERE IT GOES", fb);
    float total = 0;
    for (int i = 0; i < r.parts; ++i) total += r.part[i].ma;
    int y = TOP + 250;
    for (int i = 0; i < r.parts; ++i) {
        const doba_batt::Part& p = r.part[i];
        const int share = total > 0 ? (int)(p.ma * 100 / total + 0.5f) : 0;
        ui::text(ui::font(ui::F_SMALL), 24, y + 24, TR(p.name), fb, EPD_DRAW_ALIGN_LEFT, INK);
        snprintf(line, sizeof line, TR("%d mA · %d%%"), (int)(p.ma + 0.5f), share);
        ui::text(ui::font(ui::F_SMALL), W - 24, y + 24, line, fb, EPD_DRAW_ALIGN_RIGHT, INK);
        const int bw = (W - 48) * share / 100;
        epd_fill_rect({24, y + 36, W - 48, 10}, 0xE0, fb);
        epd_fill_rect({24, y + 36, bw, 10}, INK, fb);
        // what switching it off would give
        if (!r.charging && r.avgMa > p.ma + 1 && r.remainingMah && strcmp(p.name, "Processor and the rest") != 0) {
            snprintf(line, sizeof line, TR("without it: about %s"), hoursText(r.remainingMah / (r.avgMa - p.ma)));
            ui::text(ui::font(ui::F_TINY), 24, y + 72, line, fb, EPD_DRAW_ALIGN_LEFT, ui::GREY);
        }
        y += 92;
    }
    ui::text(ui::font(ui::F_TINY), W / 2, H - 24, TR("measured total · split estimated from typical draws"), fb,
             EPD_DRAW_ALIGN_CENTER, INK);
}

}  // namespace pg
}  // namespace doba_ui
