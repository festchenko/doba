#include "doba_battery.h"

#include <Arduino.h>
#include <algorithm>

#include "mesh_service.h"
#include "settings.h"
#include "stealth.h"

namespace doba_batt {
namespace {

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
bool g_have = false, g_charging = false;
uint8_t g_soc = 0;
uint16_t g_mv = 0, g_rc = 0;
float g_now = 0, g_avg = -1;
uint32_t g_paints = 0;            // since the last sample
float g_paintsPerMin = 0;
uint32_t g_lastSampleMs = 0;

// Typical draws on this board, mA. Rough on purpose: they only shape the split.
constexpr float GPS_MA = 22;        // L76K / MIA-M10Q tracking
constexpr float RADIO_RX_MA = 6;    // SX1262 listening
constexpr float BT_MA = 4;          // NimBLE advertising
constexpr float PAINT_MAS = 45;     // one e-paper refresh, mA*s
constexpr float LIGHT_MA[4] = {0, 6, 14, 30};

}  // namespace

void notePaint() {
    portENTER_CRITICAL(&g_mux);
    ++g_paints;
    portEXIT_CRITICAL(&g_mux);
}

void sample(uint16_t mv, int16_t ma, uint8_t soc, uint16_t remainingMah, bool charging) {
    const uint32_t now = millis();
    portENTER_CRITICAL(&g_mux);
    const uint32_t paints = g_paints;
    g_paints = 0;
    portEXIT_CRITICAL(&g_mux);
    if (g_lastSampleMs) {
        const float mins = (now - g_lastSampleMs) / 60000.0f;
        if (mins > 0.05f) {
            const float ppm = paints / mins;
            g_paintsPerMin = g_paintsPerMin <= 0 ? ppm : g_paintsPerMin * 0.7f + ppm * 0.3f;
        }
    }
    g_lastSampleMs = now;
    g_have = true;
    g_mv = mv;
    g_soc = soc;
    g_rc = remainingMah;
    // The gauge is negative while discharging; on USB the board runs from the
    // charger and the cell's current says nothing about the board's draw.
    g_charging = charging || ma > 0;
    if (!g_charging) {
        g_now = -(float)ma;
        // ~10 minutes of 30 s samples: alpha 0.05 would be 20 samples
        g_avg = g_avg < 0 ? g_now : g_avg * 0.95f + g_now * 0.05f;
    }
}

void report(Report& r) {
    r = Report{};
    r.have = g_have;
    r.charging = g_charging;
    r.soc = g_soc;
    r.mv = g_mv;
    r.remainingMah = g_rc;
    r.nowMa = g_now;
    r.avgMa = g_avg;
    r.hoursLeft = (!g_charging && g_avg > 1 && g_rc) ? g_rc / g_avg : -1;

    // The split: known parts by state, the rest is the processor and memory.
    Part p[MAX_PARTS];
    int n = 0;
    float known = 0;
    auto add = [&](const char* name, float ma) {
        if (ma <= 0.05f || n >= MAX_PARTS) return;
        p[n++] = {name, ma};
        known += ma;
    };
    add("GPS", GPS_MA);
    if (mesh_service::enabled() && !(stealth::active() && !stealth::listen())) add("Radio (LoRa)", RADIO_RX_MA);
    const int bl = settings::backlight();
    if (!stealth::active()) add("Front light", LIGHT_MA[bl < 0 ? 0 : bl > 3 ? 3 : bl]);
    add("Screen updates", g_paintsPerMin * PAINT_MAS / 60.0f);
    // What is measured and not explained: the processor, memory, card, gauge.
    const float total = (!g_charging && g_avg > 0) ? g_avg : known + 15;
    add("Processor and the rest", std::max(5.0f, total - known));
    // Scale to the measurement, so the parts add up to what the gauge sees.
    if (!g_charging && g_avg > 0 && known > 0) {
        float sum = 0;
        for (int i = 0; i < n; ++i) sum += p[i].ma;
        if (sum > 0) for (int i = 0; i < n; ++i) p[i].ma *= g_avg / sum;
    }
    std::sort(p, p + n, [](const Part& a, const Part& b) { return a.ma > b.ma; });
    r.parts = n;
    for (int i = 0; i < n; ++i) r.part[i] = p[i];
}

}  // namespace doba_batt
