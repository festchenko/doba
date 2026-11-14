#pragma once

#include <cstdio>   // snprintf, for units::navDist below

#include <cstdint>
#include <ctime>

// The SD card as the UI reports it (sd_card::sdCardState()). UNREADABLE
// is a card that answered but would not mount: unformatted, or NTFS/APFS. The
// FAT driver reads FAT32 and exFAT (tools/apply-exfat.sh); exFAT is the one
// for big cards.
enum SdCardState : uint8_t {
    SD_CARD_OK = 0,           // mounted (or lent to a USB host)
    SD_CARD_NONE = 1,         // nothing answered: no card in the slot
    SD_CARD_UNREADABLE = 2,   // a card answered, the filesystem did not mount
};

// Single shared snapshot of everything the UI consumes.
// Producers (GPS task, sensors, battery poll) take the lock, update
// their fields, release. Consumers copy the whole struct under the lock.
//
// Plain data — also compiled on the host by tools/preview to render
// screen previews, so no Arduino dependencies in the struct itself.

struct RideState {
    // GPS
    bool     gpsFix = false;
    bool     everHadFix = false;   // lat/lon hold the last known position
    double   latitude = 0.0;
    double   longitude = 0.0;
    // millis() when latitude/longitude last came from a fix: tells a new fix
    // from the same one read twice, and stamps it (the segment matcher).
    uint32_t gpsFixMs = 0;
    float    altitudeM = 0.0f;
    // THE speed: what the dashboard shows, the recorder times "moving" by and
    // the phone gets. Arbitrated by speed_source::publish() from the sources
    // below — wheel sensor, then the device's GPS, then the phone — and never
    // written directly by a producer.
    float    speedKmh = 0.0f;
    uint8_t  speedSource = 0;      // speed_source::Source behind speedKmh
    // The receiver's own ground speed, 0 without a fresh fix (gps_service).
    float    gpsSpeedKmh = 0.0f;
    float    courseDeg = 0.0f;     // heading over ground, valid when moving
    uint8_t  satellites = 0;
    bool     timeValid = false;
    time_t   utc = 0;              // unix time from GPS

    // Terrain elevation at the current position, from the DEM baked into the
    // map tiles (set by the UI task). Used for ascent/grade instead of the
    // noisy GPS-chip altitude.
    bool     mapElevationValid = false;
    float    mapElevationM = 0.0f;

    // SdCardState, mirrored for the status bar's "NO SD" chip.
    uint8_t  sdCard = SD_CARD_OK;

    // Optional Qwiic sensors (aux_sensors.h). All absent-safe: baroValid and
    // compassValid stay false when the chips are not fitted, and every consumer
    // falls back to what it did before.
    bool     baroValid = false;
    float    baroAltM = 0.0f;      // barometric altitude, m
    float    pressurePa = 0.0f;
    float    baroTempC = 0.0f;     // the barometer's own die, NOT air temp
    bool     compassValid = false; // false until the magnetometer has calibrated
    float    compassDeg = 0.0f;    // magnetic heading, degrees clockwise
    bool     deviceMoving = false; // from the accelerometer, independent of GPS

    // Battery
    uint8_t  batteryPercent = 0;
    bool     charging = false;

    // Rider config (mirrored from settings so renderers stay host-safe)
    int16_t  tzMin = -420;
    bool     useMiles = false;   // display units: false = km, true = miles
    bool     clock24h = true;    // status-bar clock: true = 24h, false = 12h
};

// Metric <-> imperial display helpers. Host-safe (used by the preview harness
// too). Storage is always metric; these convert only at render time.
namespace units {
inline double dist(double km, bool miles)  { return miles ? km * 0.621371 : km; }
inline double distM(double m, bool miles)  { return miles ? m * 0.000621371 : m / 1000.0; }
inline double speed(double kmh, bool miles) { return miles ? kmh * 0.621371 : kmh; }
inline double elev(double m, bool miles)   { return miles ? m * 3.28084 : m; }
inline const char* distLabel(bool miles)  { return miles ? "MI" : "KM"; }
inline const char* speedLabel(bool miles) { return miles ? "MPH" : "KM/H"; }
inline const char* elevLabel(bool miles)  { return miles ? "FT" : "M"; }

// Navigation distance-to-turn: feet/miles or metres/km, switching at 1000 of
// the small unit. Used by BOTH the turn banner and the directions list — they
// showed different units when the list open-coded its own formatting.
inline void navDist(char* out, size_t len, float m, bool miles) {
    if (miles) {
        float ft = m * 3.28084f;
        if (ft >= 1000) snprintf(out, len, "%.1f mi", m * 0.000621371f);
        else            snprintf(out, len, "%d ft", (int)(ft + 0.5f));
    } else if (m >= 1000) {
        snprintf(out, len, "%.1f km", m / 1000);
    } else {
        snprintf(out, len, "%d m", (int)(m + 0.5f));
    }
}
}

#ifdef ARDUINO
#include <Arduino.h>

// Copy-out under lock; mutate under lock via with().
class SharedRideState {
public:
    void begin() { mutex_ = xSemaphoreCreateMutex(); }

    RideState snapshot() {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        RideState copy = state_;
        xSemaphoreGive(mutex_);
        return copy;
    }

    template <typename F>
    void with(F&& fn) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        fn(state_);
        xSemaphoreGive(mutex_);
    }

private:
    RideState state_;
    SemaphoreHandle_t mutex_ = nullptr;
};

extern SharedRideState g_state;
#endif
