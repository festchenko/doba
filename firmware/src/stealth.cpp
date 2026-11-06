#include "stealth.h"

#include <Arduino.h>
#include <Preferences.h>

#include "ble_sensors.h"
#include "ble_server.h"
#include "mesh_service.h"

namespace stealth {
namespace {
int8_t g_on = -1, g_listen = -1;

bool readFlag(const char* key, bool def, int8_t& cache) {
    if (cache < 0) {
        Preferences p;
        p.begin("doba", true);
        cache = p.getBool(key, def) ? 1 : 0;
        p.end();
    }
    return cache == 1;
}
void writeFlag(const char* key, bool on, int8_t& cache) {
    cache = on ? 1 : 0;
    Preferences p;
    p.begin("doba", false);
    p.putBool(key, on);
    p.end();
}
bool radioBefore() {
    Preferences p;
    p.begin("doba", true);
    const bool v = p.getBool("stRadio", true);
    p.end();
    return v;
}
void saveRadioBefore(bool on) {
    Preferences p;
    p.begin("doba", false);
    p.putBool("stRadio", on);
    p.end();
}

// Apply the radio side of the current state.
void applyRadio(bool entering) {
    if (active()) {
        if (entering) saveRadioBefore(mesh_service::enabled());
        // Listening keeps the receiver as the user had it; otherwise it sleeps.
        if (!listen()) mesh_service::setEnabled(false);
        else if (!mesh_service::enabled() && radioBefore()) mesh_service::setEnabled(true);
    } else {
        mesh_service::setEnabled(radioBefore());
    }
}
}  // namespace

bool active() { return readFlag("stealth", false, g_on); }
bool listen() { return readFlag("stealthRx", true, g_listen); }
bool transmitAllowed() { return !active(); }

void set(bool on) {
    if (on == active()) return;
    writeFlag("stealth", on, g_on);
    applyRadio(on);
    ble_server::applyStealth();
    ble_sensors::applyStealth();
    Serial.printf("[stealth] %s%s\n", on ? "on" : "off", on && listen() ? " (listening)" : "");
}

void setListen(bool on) {
    writeFlag("stealthRx", on, g_listen);
    if (active()) applyRadio(false);
}

}  // namespace stealth
