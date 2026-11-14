#include "sd_bus.h"

#ifndef OTP_SIM
#include <Arduino.h>
#include <cstring>

#include "diag.h"

namespace {

// Per holder task: how often others waited behind it, the longest and the
// total wait. A handful of tasks ever touch the card; 8 slots is plenty.
struct Waits { char name[12]; uint32_t n, maxMs, totalMs, uiN, uiMaxMs; };
Waits g_waits[8];
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

void sdNoteWait(uint32_t waitedMs, TaskHandle_t holder) {
    if (waitedMs < 5) return;
    const char* who = pcTaskGetName(holder);
    const char* me = pcTaskGetName(nullptr);
    const bool ui = me && !strcmp(me, "ui");
    portENTER_CRITICAL(&g_mux);
    int i = 0;
    for (; i < 8 && g_waits[i].name[0] && strncmp(g_waits[i].name, who, sizeof g_waits[i].name - 1); ++i) {}
    if (i < 8) {
        Waits& w = g_waits[i];
        if (!w.name[0]) strncpy(w.name, who ? who : "?", sizeof w.name - 1);
        ++w.n;
        w.totalMs += waitedMs;
        if (waitedMs > w.maxMs) w.maxMs = waitedMs;
        if (ui) { ++w.uiN; if (waitedMs > w.uiMaxMs) w.uiMaxMs = waitedMs; }
    }
    portEXIT_CRITICAL(&g_mux);
    // The ones a person feels: the UI held up for a tenth of a second or more.
    static uint32_t lastLog = 0;
    if (ui && waitedMs >= 100 && millis() - lastLog > 10000) {
        lastLog = millis();
        diag::log("sd: ui waited %lu ms for the card behind '%s'", (unsigned long)waitedMs, who ? who : "?");
    }
}

void sdWaitReport() {
    Serial.println("[sd] waits behind each holder: count, max ms, total ms; the UI's count and max");
    for (const Waits& w : g_waits) {
        if (!w.name[0]) continue;
        Serial.printf("  %-10s n=%lu max=%lu total=%lu  ui n=%lu max=%lu\n", w.name, (unsigned long)w.n,
                      (unsigned long)w.maxMs, (unsigned long)w.totalMs, (unsigned long)w.uiN,
                      (unsigned long)w.uiMaxMs);
    }
}
#endif
