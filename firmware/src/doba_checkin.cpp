#include "doba_checkin.h"

#include <Arduino.h>
#include <Preferences.h>
#include <ctime>

#include "mesh_service.h"
#include "milgeo.h"
#include "settings.h"
#include "stealth.h"

namespace checkin {
namespace {

const uint8_t kSteps[] = {0, 2, 4, 6, 12, 24};
uint8_t g_hours = 0;
bool g_long = false, g_loaded = false, g_timerWake = false;
uint32_t g_last = 0;          // UTC of the last check-in sent
uint32_t g_wakeMs = 0;        // when a check-in wake began
uint32_t g_doneMs = 0;        // a check-in wake's message went to the radio
uint8_t g_prevLight = 2;      // the light before the 72-hour mode

void load() {
    if (g_loaded) return;
    g_loaded = true;
    Preferences p;
    p.begin("doba", true);
    g_hours = p.getUChar("chkh", 0);
    g_long = p.getBool("long72", false);
    g_last = p.getUInt("chkt", 0);
    g_prevLight = p.getUChar("long72bl", 2);
    p.end();
}

void save() {
    Preferences p;
    p.begin("doba", false);
    p.putUChar("chkh", g_hours);
    p.putBool("long72", g_long);
    p.putUInt("chkt", g_last);
    p.putUChar("long72bl", g_prevLight);
    p.end();
}

uint32_t nowUtc() {
    const time_t t = time(nullptr);
    return t > 1600000000 ? (uint32_t)t : 0;
}

}  // namespace

void begin(bool wokeByTimer) {
    load();
    g_timerWake = wokeByTimer && g_hours > 0;
    g_wakeMs = millis();
    if (g_timerWake) Serial.printf("[checkin] woken for the %u-hour check-in\n", g_hours);
}

int hours() { load(); return g_hours; }

void stepHours(int dir) {
    load();
    int i = 0;
    while (i < (int)sizeof kSteps && kSteps[i] != g_hours) ++i;
    const int n = sizeof kSteps;
    i = ((i + dir) % n + n) % n;
    g_hours = kSteps[i];
    if (g_hours && !g_last) g_last = nowUtc();   // the first one: N hours from now
    save();
}

bool longMode() { load(); return g_long; }

void setLongMode(bool on) {
    load();
    if (on == g_long) return;
    g_long = on;
    if (on) {
        g_prevLight = (uint8_t)settings::backlight();
        settings::setBacklight(0);
        if (!g_hours) { g_hours = 6; g_last = nowUtc(); }
    } else {
        settings::setBacklight(g_prevLight);
    }
    save();
}

uint32_t idleSleepMs() { return longMode() ? 60u * 1000 : 3u * 60 * 1000; }

uint32_t secondsToNext() {
    load();
    if (!g_hours) return 0;
    const uint32_t now = nowUtc(), every = g_hours * 3600u;
    if (!now || !g_last) return every;
    const uint32_t due = g_last + every;
    return due > now + 30 ? due - now : 30;
}

void dueNow() { load(); g_last = 0; }

bool sentLast(char* out, unsigned n) {
    load();
    if (!g_last) return false;
    const time_t t = g_last;
    struct tm g;
    localtime_r(&t, &g);
    snprintf(out, n, "%02d:%02d", g.tm_hour, g.tm_min);
    return true;
}

bool tick(const DeviceState& s) {
    load();
    // Woken only for a check-in and it is queued: give the radio 8 s to send it.
    if (g_doneMs) return millis() - g_doneMs > 8000;
    if (!g_hours) return g_timerWake;
    const uint32_t now = nowUtc();
    if (!now) return g_timerWake && millis() - g_wakeMs > 180000;
    const bool due = !g_last || now + 30 >= g_last + g_hours * 3600u;
    if (!due) return false;
    // In stealth nothing is sent: the next one is tried after the next interval.
    // Radio off: a check-in asked for turns it on (as an SOS does).
    static uint32_t radioAskedMs = 0;
    if (!stealth::active() && !mesh_service::enabled()) {
        if (!radioAskedMs) { radioAskedMs = millis() | 1; mesh_service::setEnabled(true); }
        if (millis() - radioAskedMs < 60000) return false;   // the radio coming up
    }
    radioAskedMs = 0;
    if (stealth::active() || !mesh_service::enabled()) {
        g_last = now;
        save();
        return g_timerWake;
    }
    // Woken for it: give the GPS up to 3 minutes for a fix first.
    if (g_timerWake && !s.gpsFix && millis() - g_wakeMs < 180000) return false;
    char where[40] = "position unknown", age[16], last[40] = "", when[12] = "";
    if (s.gpsFix || s.everHadFix) milgeo::mgrs(s.latitude, s.longitude, where, sizeof where);
    if (!s.gpsFix && s.everHadFix)
        snprintf(last, sizeof last, settings::fixAgeText(settings::lastFixUtc(), age, sizeof age, false)
                                        ? " (last fix %s ago)" : " (last fix)", age);
    const time_t t = now;
    struct tm g;
    gmtime_r(&t, &g);
    snprintf(when, sizeof when, " %02d%02dZ", g.tm_hour, g.tm_min);
    char msg[160];
    snprintf(msg, sizeof msg, "OK %s %s%s%s", mesh_service::longName(), where, when, last);
    mesh_service::queueText(0xFFFFFFFFu, msg, 0);
    Serial.printf("[checkin] sent: %s\n", msg);
    g_last = now;
    save();
    // Woken only for this: let the radio get it out, then sleep again.
    if (g_timerWake) g_doneMs = millis() | 1;
    return false;
}

}  // namespace checkin
