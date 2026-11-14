#include "ui_dashboard.h"
#include "doba_places.h"
#include "milgeo.h"

#include <Arduino.h>
#include <stdarg.h>
#include <SD.h>
#include <Update.h>
#include <Wire.h>
#include "epd_compat.h"
#include "map_base_cache.h"
#include <TouchDrvGT911.hpp>

#include "config.h"
#include "ride_state.h"
#include "ride_recorder.h"
#include "sd_bus.h"
#include "sd_diagnostics.h"
#include "gps_service.h"
#include "board_power.h"
#include "doba_battery.h"
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp32-hal-tinyusb.h>   // usb_persist_restart / RESTART_BOOTLOADER
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include "ui_render.h"
#include "map_view.h"
#include "topo_layer.h"   // DOBA
#include "doba_ui.h"
#include "stealth.h"      // DOBA
#include "milspec.h"      // DOBA
#include "vfont.h"        // DOBA: font cache stats
#include "doba_i18n.h"    // DOBA: lang command
#include "doba_points.h" // DOBA
#include "doba_dem.h"    // DOBA
#include "doba_library.h" // DOBA
#include "map_tiles.h"
#include "map_store.h"
#include "i2c_bus.h"
#include "usb_storage.h"
#include <esp_random.h>

#include "lora_radio.h"
#include "mesh_service.h"
#include "routes.h"
#include "settings.h"
#include "speed_source.h"
#include "aux_sensors.h"
#include "diag.h"
#include "crash_report.h"
#include "memfault_service.h"
#include "smooth_epd.h"
#include "power_mgmt.h"
#include "dash_config.h"
#include "live_view.h"

namespace {

TouchDrvGT911 touch;
bool touchOk = false;
int refreshCount = 0;

// Shadow of the last frame actually pushed to glass. The panel must not
// repaint when nothing changed (design: "static chrome never repaints"),
// so identical frames are dropped before any epd update.
uint8_t* shadowFb = nullptr;
size_t fbSize = 0;

enum Screen { SCREEN_DASH, SCREEN_MAP, SCREEN_SUMMARY, SCREEN_MENU,
              SCREEN_ROUTES, SCREEN_HISTORY,
              SCREEN_SETTINGS, SCREEN_GPSDEBUG, SCREEN_DIRECTIONS,
              SCREEN_MAPLAYERS,
              // DOBA: the launcher, comms and library (doba_ui.h).
              SCREEN_HOME, SCREEN_COMMS, SCREEN_COMPOSE, SCREEN_LIBRARY,
              SCREEN_ARTICLE, SCREEN_SETUP, SCREEN_SETUP_NAME, SCREEN_POINTS,
              SCREEN_POINT, SCREEN_POINT_NEW, SCREEN_MAPOPTS, SCREEN_SOS, SCREEN_SUN, SCREEN_MORE, SCREEN_BATTERY };
#ifdef OTP_SIM
// DOBA: the inherited OTP sim suites were written for a dashboard boot; they set
// DOBA_START=dash (sim/tests/conftest.py). The device always boots to HOME.
static Screen dobaStartScreen() {
    const char* s = getenv("DOBA_START");
    return (s && strcmp(s, "dash") == 0) ? SCREEN_DASH : SCREEN_HOME;
}
Screen screen = dobaStartScreen();
#else
Screen screen = SCREEN_HOME;   // DOBA: boot lands on the main screen
#endif
// Which cycle slot the rider is on (indexes dash_config::pages, which now
// includes the MAP as a movable page). Deliberately NOT reset by navigation:
// leaving for the menu or settings and coming back lands on the page the
// rider was on — the page is a place, not a mode.
int dashPage = 0;

// First slot that is an actual dashboard (not the map) — where "back to the
// data page" lands when the current slot is the map.
int firstContentPage() {
    for (int i = 0; i < dash_config::pageCount(); ++i)
        if (dash_config::page(i).kind != DP_MAP) return i;
    return 0;
}

int mapSlot() {
    for (int i = 0; i < dash_config::pageCount(); ++i)
        if (dash_config::page(i).kind == DP_MAP) return i;
    return dash_config::pageCount() - 1;
}
RideSummary pendingSummary;
// The summary sheet is doing boot-recovery duty: it shows the ride a reset cut
// short, and its buttons resolve that ride (CONTINUE / SAVE / DISCARD) instead
// of stopping a live one.
bool recoveryPrompt = false;

float mapMpp = 2.0f;  // map zoom: 1/2/4/8/16/32 m per px (wide levels show tiles)
// DOBA: the last map frame was a topo one. Topo packs carry zooms 9-16, so the
// map may zoom out further than the vector maps' 32 m/px (z9 is ~190 m/px here).
bool mapTopoActive = false;
// DOBA: the grey calibration card is up (console `greytest`; any touch or
// key, or `greytest off`, takes it down).
bool greyTestShown = false;
inline float mapMaxMpp() { return mapTopoActive ? 200.0f : 32.0f; }
// Whether the next map frame may read tiles off the card. See renderMapScreen:
// the first pass after any change is drawn from RAM alone so it lands at once.
bool mapFullPass = false;
// Which zoom button is showing its pressed state (0 none, 1 in, 2 out). Held
// from the tap until a COMPLETE map frame has been drawn.
int zoomHeld = 0;
bool mapTrackUp = false;
// DOBA: how far the map has been dragged off the position, metres east/north.
float mapPanE = 0, mapPanN = 0;
bool mapPanned() { return mapPanE != 0 || mapPanN != 0; }
float mapTestHeading = -1.0f;   // console `heading`: stands in for the real one

MapBaseCache mapCache;
bool mapCacheOn = true;
bool mapCacheAllocated = false;
MapBaseKey mapViewKey;              // the view on screen
uint32_t mapStillSince = 0;         // when it last moved
uint32_t mapPrerenderNotBefore = 0;
uint32_t mapCacheHits = 0, mapCacheMisses = 0, mapPrerenders = 0, mapPrerendersCut = 0;
// How long the view must hold before the zooms either side are worth rendering. While
// riding it never gets here, which is the point: those frames would go unused.
constexpr uint32_t kMapStillMs = 3000;
constexpr uint32_t kMapPrerenderRetryMs = 5000;
// A cached layer is four things, one after the other. The base layer's pixels at one
// bit each: it is pure black and white, and PSRAM spent here comes straight out of the
// tile cache, which is what wide views live on. What the tile pass found, so a frame
// that skips it still reports the same map. Its POIs, which go on top of the base but
// come out of that pass too. And where its water is.
constexpr int kMapCachePois = 512;
struct MapLayerFacts {
    int32_t poiCount, features, tilePolys, projectedTiles, wantedTiles, parks, waters;
    decltype(MapScreenData::clsCount) classes;
};
const uint8_t* mapFrameBase = nullptr;   // this frame's cached layer, if it has one
const uint8_t* mapFrameWater = nullptr;  // and where its water is, for a zoom's scrub
uint8_t* mapPrerenderFb = nullptr;       // a full framebuffer to prerender into
uint8_t* mapLayerBeingKept = nullptr;

// Serial test hooks: drive the UI over the CDC serial port to profile the map
// without physical taps. Toggle timing logs with 't'; single-char commands
// injected in the task loop mirror button presses. See pollSerialCommands().
bool dbgTiming = false;

// When the "Start navigation?" prompt appeared (for the accept settle guard).
uint32_t navPromptShownAt = 0;

// Turn-by-turn banner rect (top of map/dashboard); tapping it ends nav.
const EpdRect kNavBanner = {0, ui::STATUS_H, 540, 138};
// onlyWithinM > 0 limits the banner to an APPROACHING turn — pages whose
// content the band would bury (music, workout) get it as a popup instead of
// a fixture. bandDrawnThisFrame feeds the dismissal scrub: any top-band
// overlay leaving the glass needs its ghost cleared.
void drawNavBanner(uint8_t* fb, float onlyWithinM = 0);
void drawPauseBanner(const RideState& s, uint8_t* fb);
void drawMeshNotice(uint8_t* fb);
bool bandDrawnThisFrame = false;

// The private-channel message popup (mesh_service::latestNotice). Up from
// arrival for MESH_NOTICE_MS, or until tapped; noticeSeq is the last one
// shown, so each message pops up once.
mesh_service::Notice notice;
uint32_t noticeSeq = 0;
bool noticeUp = false;
uint32_t noticeUntilMs = 0;
// A turn this close keeps the band: the message can wait ten seconds, the
// turn cannot.
constexpr float kNoticeYieldsToTurnM = 200.0f;
bool meshNoticeShown() {
    if (!noticeUp) return false;
    if (routes::navActive()) {
        char instr[routes::MANEUVER_TEXT];
        float dist = 0;
        if (routes::nextTurn(instr, sizeof(instr), dist) &&
            dist <= kNoticeYieldsToTurnM)
            return false;
    }
    return true;
}
void buildMapScreenData(const RideState& s, MapScreenData& map,
                        bool finalFrame = false);

// Which screen the Sensors page was opened from, so back returns there.

// Same for the upcoming-directions list, reached by tapping the turn banner.
Screen directionsFrom = SCREEN_MAP;

// Set from the GT911 home-button callback (fires inside touch.getPoint()).
volatile bool homeKeyPressed = false;

// Interrupt flags. ISRs only set these; all I2C reads and logic stay in the
// UI task. A slow fallback poll (see the task loop) covers the case where an
// INT line doesn't behave as expected, so input can never be dropped.
volatile bool touchIrq = false;      // GT911 INT (GPIO3)
volatile bool boardBtnIrq = false;   // BOOT edge or expander INT (GPIO38)

// Wakes the UI task from its idle block. Everything the loop services is either
// interrupt-driven (touch, buttons) or on its own slower timer (redraw 1 Hz,
// touch-poll fallback 200 ms, elevation 2.5 s), so the task has no reason to
// spin — it blocks here and the ISRs below release it.
//
// MUST be a dedicated semaphore, NEVER the task's built-in notification slot.
// ESP-IDF drivers use the CALLING task's notification slot to wait for
// completion — including the SPI master behind the SD card and the panel's DMA
// waits — and this task does both SD reads and panel painting. An input
// interrupt landing mid-transfer would corrupt a driver's completion handshake.
SemaphoreHandle_t uiWake = nullptr;

// Idle block length. 200 ms is the loop's own touch-poll backstop, so nothing
// inside it is starved; input latency is unaffected because the ISRs release
// the semaphore immediately.
constexpr uint32_t UI_IDLE_TICK_MS = 200;

inline void IRAM_ATTR uiWakeFromIsr() {
    if (!uiWake) return;
    BaseType_t hpw = pdFALSE;
    xSemaphoreGiveFromISR(uiWake, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

// DOBA: touch is sampled by a task of its own (touchTask), not by the UI loop.
// The UI loop is busy for half a second at a time while the panel repaints,
// and a quick tap that went down and up inside that window was simply never
// seen: the next read found no finger. The task catches the whole tap and
// queues it; the loop takes taps off the queue whenever it gets round to them.
struct TouchEv { int16_t x0, y0, x1, y1; uint32_t downMs, upMs; };
QueueHandle_t g_touchQ = nullptr;
TaskHandle_t g_touchTask = nullptr;
void IRAM_ATTR onTouchIrq() {
    touchIrq = true;
    if (g_touchTask) {
        BaseType_t hpw = pdFALSE;
        vTaskNotifyGiveFromISR(g_touchTask, &hpw);
        if (hpw) portYIELD_FROM_ISR();
    }
    uiWakeFromIsr();
}
void IRAM_ATTR onBoardBtnIrq() { boardBtnIrq = true; uiWakeFromIsr(); }

// Bottom-sheet overlay. Named for its first user, the power/shutdown dialog
// (opened by holding BOOT 1.5 s); the unpair-phone confirmation (Settings >
// PHONE) is the same modal with different words, so it shares every bit of
// the overlay plumbing — tap routing, Home-key dismissal, holding still, the
// scrub on close — and only sheetKind says which words to draw.
bool powerOverlay = false;
enum SheetKind { SHEET_POWER, SHEET_UNPAIR };
SheetKind sheetKind = SHEET_POWER;

// Backlight: 4 levels cycled by the GPIO48 button.
// Off / Low / Med / Bright. Low is deliberately very dim — it is for reading the
// panel at night, where 50/255 was still dazzling; e-paper needs far less
// frontlight than an emissive display to become legible.
const uint8_t kBacklightPWM[4] = {0, 12, 90, 230};
void applyBacklight(int level) {
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    // Belt and braces: a pad hold left over from shutdownDevice() freezes the
    // pin and silently swallows every duty write (main.cpp also releases it).
    gpio_hold_dis((gpio_num_t)BOARD_BL_EN);
    if (level > 0) {
        // Keep the LEDC clock and the pad's PWM function alive through
        // automatic light sleep while lit; released again when turned off so
        // the dark device keeps its full light-sleep savings.
        power_mgmt::frontlightHold(true);
        gpio_sleep_sel_dis((gpio_num_t)BOARD_BL_EN);
        analogWrite(BOARD_BL_EN, kBacklightPWM[level]);
    } else {
        analogWrite(BOARD_BL_EN, 0);
        gpio_sleep_sel_en((gpio_num_t)BOARD_BL_EN);
        power_mgmt::frontlightHold(false);
    }
}

namespace {
// Mean of a few gauge current samples (mA, negative = discharging). The gauge
// averages over ~1 s itself; three of them steady it against the CPU's noise.
int avgBatteryMa() {
    int sum = 0, n = 0;
    for (int i = 0; i < 3; ++i) {
        delay(1000);
        uint16_t mv; int16_t ma;
        if (board_read_power(mv, ma)) { sum += ma; ++n; }
    }
    return n ? sum / n : 0;
}

// Edges seen on the GPS RX pin in `ms`. The module bursts its sentences once a
// second, so a powered GPS gives hundreds of edges and a dead one gives none.
int gpsRxEdges(uint32_t ms) {
    int edges = 0, last = gpio_get_level((gpio_num_t)BOARD_GPS_RXD);
    for (uint32_t t0 = millis(); millis() - t0 < ms; ) {
        int v = gpio_get_level((gpio_num_t)BOARD_GPS_RXD);
        if (v != last) { ++edges; last = v; }
    }
    return edges;
}

void radioRailOffProbe() {
    const int maOn = avgBatteryMa();
    const int edgesOn = gpsRxEdges(1200);
    board_radio_power(false);
    uint8_t out0 = 0xFF, cfg0 = 0xFF;
    i2cLock();
    Wire.beginTransmission(0x20); Wire.write(0x02);   // XL9555 output port 0
    bool okOut = Wire.endTransmission(false) == 0 && Wire.requestFrom(0x20, 1) == 1;
    if (okOut) out0 = Wire.read();
    Wire.beginTransmission(0x20); Wire.write(0x06);   // config port 0 (1 = input)
    bool okCfg = Wire.endTransmission(false) == 0 && Wire.requestFrom(0x20, 1) == 1;
    if (okCfg) cfg0 = Wire.read();
    i2cUnlock();
    const int maOff = avgBatteryMa();
    const int edgesOff = gpsRxEdges(1200);
    diag::log("sleep: radio rail off -> xl9555 out0=0x%02x cfg0=0x%02x (io%d %s, %s); "
              "battery %dmA -> %dmA (delta %d); gps rx edges %d -> %d",
              out0, cfg0, IOEXP_PIN_RADIO_POWER,
              (okOut && !(out0 & (1 << IOEXP_PIN_RADIO_POWER))) ? "low" : "NOT LOW",
              (okCfg && !(cfg0 & (1 << IOEXP_PIN_RADIO_POWER))) ? "output" : "NOT OUTPUT",
              maOn, maOff, maOff - maOn, edgesOn, edgesOff);
}

// DOBA: a power-off from the menu on battery cuts the battery (ship mode), and
// then only the POWER key brings Doba back; auto-sleep and power-off on USB
// stay in deep sleep, woken by BOOT. The farewell screen says which.
static bool g_offByPowerKey = false;
}  // namespace

// The map the device leaves on the glass when it powers off. Logs what it got, because
// this frame is the one nobody can watch being built.
void renderFarewell(uint8_t* fb) {
    memset(fb, 0xFF, fbSize);
    mapMpp = 32.0f;   // max zoom-out (1/2/4/8/16/32 m per px)
    // Always north-up for the farewell. This image sits on the glass for
    // days; rotated to whatever heading the last fix happened to have, it is
    // unreadable as a map of where the bike is. (Deep sleep follows
    // immediately, so mutating the live setting here is harmless.)
    mapTrackUp = false;
    RideState s = g_state.snapshot();
    // Save the last known position on the way down, so the next boot's
    // GPS warm-start seed is as fresh as possible.
    if (s.everHadFix) settings::setLastPosition(s.latitude, s.longitude);
    MapScreenData map = {};
    const uint32_t t0 = millis();
    buildMapScreenData(s, map, /*finalFrame=*/true);
    // The widest zoom only suits a card that has that much map. Past what was
    // downloaded it is blank paper, with the map a patch in the middle, and this frame
    // stays up for days. So go in to the widest view the card can fill. A card with
    // nothing that fills any view keeps the widest, since that shows the most it has.
    if (map.projectedTiles < map.wantedTiles) {
        for (float mpp = 16.0f; mpp >= 2.0f; mpp /= 2.0f) {
            if (!map_store::coversView(map.centerLat, map.centerLon, mpp)) continue;
            mapMpp = mpp;
            map = MapScreenData();
            buildMapScreenData(s, map, /*finalFrame=*/true);
            break;
        }
    }
    const uint32_t t1 = millis();
    ui_render_map_features(map, s, fb);
    diag::log("farewell: %d m/px project=%lu draw=%lu tiles=%d/%d polys=%d water=%d parks=%d%s%s at=%.5f,%.5f",
              (int)mapMpp, (unsigned long)(t1 - t0), (unsigned long)(millis() - t1), map.projectedTiles,
              map.wantedTiles, map.featureCount, map.waterCount, map.parkCount,
              map.partial ? " CUT" : "", map.hasMap ? "" : " NOMAP", map.centerLat, map.centerLon);
    doba_ui::renderOffCard(s, g_offByPowerKey, fb);   // DOBA: the paper card
}

void shutdownDevice(uint8_t* fb, const char* reason) {
    // Hold light sleep off for the whole shutdown, and never release it.
    //
    // LOAD-BEARING, not just a bus guard. Automatic light sleep re-arms the RTC
    // timer wake source on EVERY tickless idle — esp_pm's vApplicationSleep
    // calls esp_sleep_enable_timer_wakeup() before each nap — and that trigger
    // lives in the same global sleep config esp_deep_sleep_start() later
    // consumes. If an idle window lands between our disable-everything below
    // and the deep-sleep call, the timer trigger comes back and deep sleep
    // honours it: the device wakes seconds later and boots, "reset: wake from
    // sleep". Taking the lock first means no idle can arm anything from here on.
    power_mgmt::busyAcquire();   // deliberately never released — we don't return
    {
        bool usb = false;
        board_usb_power_present(usb);
        g_offByPowerKey = strstr(reason, "user") != nullptr && !usb;
    }

    // Answer the tap NOW, before the slow part. Saving the ride and flushing
    // the diag log are seconds of SD work, and with nothing on the glass in
    // that window the press read as a miss (and got pressed again). The paint
    // is asynchronous on this backend, so the band is drawing while the ride
    // saves — acknowledgment costs no extra wall-clock.
    ui_render_shutdown_ack(ride_recorder::isRecording(), fb);
    epdc_paint();

    // Log WHY we're powering off and flush it to SD before deep sleep, so the
    // next boot's log distinguishes a user shutdown / auto-sleep from a reset
    // or a power loss (which leave no such line).
    diag::log("shutdown: %s", reason);
    if (ride_recorder::isRecording()) {
        ride_recorder::stopRide(true);  // never lose a ride to power-off
    }
    diag::flushToSD();

    // Leave a static farewell on the glass — e-paper keeps it for free.
    // Full-screen map backdrop (last known position) with a POWERED OFF plate.
    // Force the widest zoom so the farewell shows the broadest area overview
    // (we're deep-sleeping right after, so mutating mapMpp is harmless).
    renderFarewell(fb);
    if (shadowFb) memcpy(shadowFb, fb, fbSize);
    // The shutdown screen has to survive on the glass for days with no power, so
    // it gets the one place a HARD clear is genuinely worth its cost: scrub to
    // white, then paint the final image with everything driven from a known
    // state. epdc_clear() leaves our framebuffer alone (unlike epdiy's
    // fullclear, which wiped it and needed a save/restore around the call).
    epdc_clear();
    epdc_paint();
    // MUST come before deep sleep. On the EPD_Painter backend epdc_paint() only
    // hands the frame to the driver's paint task and returns while the rows are
    // still being clocked out, so powering down here caught the panel mid-drive
    // and left the farewell screen half-written. (The epdiy backend painted
    // inline, which is why this never used to be needed.)
    epdc_paint_wait();

    // Then let the panel's rails come down. Without this the TPS65185 sits
    // generating VPOS/VNEG/VGH/VGL for the entire sleep — the enables are latched
    // in the expander, which is on the always-on 3V3, and the driver's own
    // power-off timer never gets to run because we stop the CPU first. This
    // waits out the driver's stock idle countdown, deliberately armed by the
    // paint above and never shortened — see epd_compat.h for the mid-drive
    // rail-cut race that shortening it caused. The image survives regardless:
    // e-paper needs no power to hold a frame, which is the whole premise of the
    // farewell screen.
    epdc_power_off_wait();
    // Verify the rails are actually down (and force them if not) BEFORE the
    // final SD flush, so the readback lands in the card's log. A device
    // measured ~30 mA "off" for hours; the blind wait alone was not evidence.
    epdc_power_off_verify();
    // Then the ESP32 side of the panel bus. The driver leaves CKV/SPV high,
    // and gpio_deep_sleep_hold_en() below would hold them high into the
    // panel's dead logic rail for the whole sleep.
    epdc_park_pins();
    // Frontlight off and HELD: analogWrite() only parks the LEDC duty at 0,
    // and a digital pad floats in deep sleep unless held. The factory sleep
    // sequence zeroes this pin too (vendor examples/factory ui_sleep()).
    analogWrite(BOARD_BL_EN, 0);
    pinMode(BOARD_BL_EN, OUTPUT);
    digitalWrite(BOARD_BL_EN, LOW);
    gpio_hold_en((gpio_num_t)BOARD_BL_EN);

    // Drain users, then KEEP the bus lock through deep sleep. Releasing it
    // after SD.end() lets recorder/diag/MSC/LoRa tasks start new work while the
    // card is unmounted and power-down is in progress.
    sdLock();
    // Cut the GPS + LoRa rail HERE, before the final flush, and log whether it
    // actually went down. The off drain is a steady ~32 mA with the panel
    // verified off and its pins parked, which is the size of a GPS still
    // searching, and nothing has ever confirmed this rail drops. The sdLock
    // above keeps LoRa SPI quiet across the cut. Evidence: the expander's
    // port-0 output/config bits, the battery current before and after, and
    // whether the GPS is still talking on RX afterwards.
    radioRailOffProbe();
    diag::log("sleep: final SD flush; mounted=%d host=%d CS=%d/%d bl=%d",
              ride_recorder::sdMounted(), usb_storage::hostActive(),
              digitalRead(BOARD_SD_CS), digitalRead(BOARD_LORA_CS),
              digitalRead(BOARD_BL_EN));
    diag::flushToSD();
    diag::checkpoint("entering deep sleep");
    diag::storageStage(6);
    SD.end();
    diag::drainDriverLogs();
    diag::checkpoint("SD unmount diagnostics");
    digitalWrite(BOARD_SD_CS, HIGH);
    digitalWrite(BOARD_LORA_CS, HIGH);
    gpio_hold_en((gpio_num_t)BOARD_SD_CS);
    // sdUnlock intentionally omitted: no bus work may begin before CPU stops.

    // Peripherals down, matching the factory sleep sequence
    i2cLock(); touch.sleep(); i2cUnlock();
    digitalWrite(BOARD_TOUCH_RST, LOW);
    gpio_hold_en((gpio_num_t)BOARD_TOUCH_RST);
    gpio_deep_sleep_hold_en();
    board_radio_power(false);
    // With the GPS + LoRa rail cut, nothing facing it may stay high: the
    // deep-sleep hold freezes every pad as it is, and a held-high line feeds
    // the dead rail through the parts' input clamps for the whole sleep. The
    // UART TX to the GPS idles high, and LoRa CS was just driven high above.
    // Outputs go low; inputs from the radios get pulled down rather than left
    // floating. MOSI/SCLK/MISO are shared with the SD card, and low is safe for
    // it too; SD CS stays high.
    for (int p : {BOARD_GPS_TXD, BOARD_LORA_CS, BOARD_LORA_RST,
                  BOARD_SPI_MOSI, BOARD_SPI_SCLK}) {
        gpio_reset_pin((gpio_num_t)p);
        gpio_pullup_dis((gpio_num_t)p);
        gpio_set_direction((gpio_num_t)p, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)p, 0);
    }
    for (int p : {BOARD_GPS_RXD, BOARD_LORA_IRQ, BOARD_LORA_BUSY, BOARD_SPI_MISO}) {
        gpio_reset_pin((gpio_num_t)p);
        gpio_pullup_dis((gpio_num_t)p);
        gpio_pulldown_en((gpio_num_t)p);
    }

    // Wait for BOOT to come back up before arming the wake on it. The power
    // dialog is opened by HOLDING BOOT, so the button can still be down when the
    // rider taps Shut down — and ext0 waits for LOW, which a held button already
    // satisfies. Bounded, so a stuck button can't hang the shutdown.
    for (uint32_t t0 = millis();
         digitalRead(BOARD_BOOT_BTN) == LOW && millis() - t0 < 3000; ) {
        delay(20);
    }

    // ext0 wakes on GPIO0 going LOW, so the pin MUST be held high while asleep.
    // pinMode(INPUT_PULLUP) sets the *digital* pull-up, and that does not survive
    // the pad switching to RTC-domain control on the way into deep sleep — the
    // pin then floats, reads LOW, and the device wakes immediately. That is the
    // "pressing Shut down just restarts it" bug, and it hit auto-sleep too:
    // 2026-08-02 shows three auto-sleeps waking after exactly 1 s and a manual
    // power-off waking after 3 s, while one power-off that happened to settle
    // high stayed asleep 4 h 16 m. Intermittent because a floating pin is.
    rtc_gpio_pullup_en((gpio_num_t)BOARD_BOOT_BTN);
    rtc_gpio_pulldown_dis((gpio_num_t)BOARD_BOOT_BTN);

    // Start from NO wake sources. The enabled-trigger set is global and sticky:
    // anything enabled earlier in the boot — the PM tickless-idle timer above
    // all, but also the optional GPS UART wakeup — is still armed here, and
    // esp_deep_sleep_start() honours every one of it. That is why power-off kept
    // "restarting by itself after ~10 s": the last light sleep's RTC alarm was
    // simply re-armed for deep sleep. The button is the ONLY way back on.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BOARD_BOOT_BTN, 0);
    diag::storageStage(7);
    if (g_offByPowerKey) {
        board_battery_ship_mode();   // the lights go out here on battery
        delay(1500);                 // still running: USB arrived meanwhile
    }
    esp_deep_sleep_start();
}

// Screen-space route overlay (rebuilt each map render)
constexpr int MAX_ROUTE_SCREEN_PTS = 4096;
int16_t* routeScreenPts = nullptr;

// Navigate and Ride History page through a listing read from the card once —
// when the screen opens, on REFRESH, or when the card or the ride being
// recorded changes — never on the 1 Hz redraw: the card runs at 4 MHz under
// sdLock, and touch is only read between loop passes. Paging just indexes the
// copy. Both arrays live in PSRAM, allocated on first use.
constexpr int kRouteListMax = 128;
constexpr int kRideListMax = 256;
char (*routeFiles)[routes::MAX_NAME] = nullptr;
int routeFileCount = 0;
ride_recorder::RideFile* rideFiles = nullptr;
int rideFileCount = 0;
bool listStale = true;    // re-read the card on the next render
int listPage = 0;         // which page of kMenuRowCount rows is showing
// What the last frame showed, so a tap maps to the row the rider saw: the
// whole list's length, and how many pinned rows ("Clear route", "Recording in
// progress") sit ahead of the files.
int listTotal = 0;
int listPinned = 0;

// Long-press tracking
uint32_t touchDownAt = 0;
volatile bool touchWasDown = false;   // DOBA: written by touchTask
void touchTask(void*) {
    int16_t x0 = 0, y0 = 0, lx = 0, ly = 0;
    uint32_t downAt = 0;
    bool down = false;
    for (;;) {
        // While a finger is on the glass: every 15 ms. Otherwise wait for the
        // GT911's interrupt, with a slow poll in case one is missed.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(down ? 15 : 250));
        int16_t x[1], y[1];
        i2cLock();
        const bool now = touch.getPoint(x, y, 1) > 0;
        i2cUnlock();
        touchIrq = false;
        if (now) {
            lx = x[0]; ly = y[0];
            if (!down) { x0 = lx; y0 = ly; downAt = millis(); }
        } else if (down) {
            const TouchEv ev = {x0, y0, lx, ly, downAt, millis()};
            xQueueSend(g_touchQ, &ev, 0);
            if (uiWake) xSemaphoreGive(uiWake);
        }
        down = now;
        touchWasDown = now;
    }
}

// Inactivity auto-sleep: any input refreshes this; the task deep-sleeps after
// a quiet period (unless riding / navigating / phone connected).
uint32_t lastActivityMs = 0;
constexpr uint32_t AUTO_SLEEP_MS = 10 * 60 * 1000;   // 10 minutes idle
// Console override for bench/battery testing. UI task owns both this flag and
// the command parser. RAM only: every reset restores automatic shutdown.
bool autoSleepEnabled = true;
// Set by any input so the next loop iteration redraws immediately instead of
// waiting for the 1 Hz periodic tick — otherwise an in-place change (zoom, a
// map toggle) can sit up to a second before the panel repaints.
bool forceDraw = false;
// Last touch/button. Used to back off the barometer sample while the rider is
// interacting, so it does not contend with the recorder for the SD lock.
uint32_t lastUiInputMs = 0;
void noteActivity() {
    greyTestShown = false;   // DOBA: any touch or key takes the grey card down
    lastActivityMs = millis();
    lastUiInputMs = millis();
    forceDraw = true;
    if (uiWake) xSemaphoreGive(uiWake);   // redraw now, don't wait out the block
}

// The periodic redraw. Screens that show live readings (the map, the ride
// page, SOS, GPS details, the sun clock) tick every second. Every other page
// only changes on a tap or an event (which force a frame), or through its
// status band: the clock's minute, the battery, the fix. Those redraw when
// the minute turns, and every 30 s at the latest — a full render a second for
// nothing was most of the UI task's CPU on a page being read.
bool liveScreen(Screen sc) {
    return sc == SCREEN_MAP || sc == SCREEN_DASH || sc == SCREEN_SOS || sc == SCREEN_GPSDEBUG ||
           sc == SCREEN_SUN || sc == SCREEN_SUMMARY || sc == SCREEN_HOME || sc == SCREEN_DIRECTIONS;
}
bool tickDue(Screen sc, uint32_t lastDraw) {
    const uint32_t since = millis() - lastDraw;
    if (liveScreen(sc)) return since >= 1000;
    if (since >= 30000) return true;
    static int lastMinute = -1;
    const int minute = (int)((time(nullptr) / 60) % 60);
    if (since >= 1000 && minute != lastMinute) { lastMinute = minute; return true; }
    return false;
}

// Panel refresh.
//
// This used to be ~260 lines of waveform policy: DU vs GL16 vs GC16, ghostDebt
// accounting, a deferred settle-clean, region-scoped two-phase flashes through
// white, a changed-region differ, and a pass that flattened the framebuffer to
// 1-bit. ALL of it existed to work around epdiy's differential DU — a pixel that
// is black before and after gets no waveform, DU is not DC-balanced, and neither
// DU nor GL16 can restore a white pixel it never drove. See
// investigations/display-ghosting.md for the full history.
//
// EPD_Painter makes every one of those workarounds unnecessary: it has its own
// delta engine, per-panel measured waveform tables, and an explicit DC-balance
// pass. Soaked on hardware with a 1 Hz partial update, the white background held
// with no drift — which is what none of our epdiy schemes achieved.
//
// So the whole policy reduces to: skip identical frames, otherwise paint.
// How much clean area one dirty rectangle may swallow before it is split, in
// pixels. Deliberately generous: the driver stops scanning at 32 rectangles and
// leaves everything past that unscrubbed, so a tolerance low enough to produce
// tight rects would silently skip the bottom of a busy transition — the exact
// residue this is here to remove. ~24 clean rows of the panel's 960 px width,
// which merges a map's scattered street rows into a handful of bands while
// still leaving an unchanged half-screen alone.
constexpr int kDirtyClearTolerance = 24 * 960;

// Set for one refresh() when greyed-out cells are arriving on the glass.
bool hardClearToned = false;
// Set for one refresh() when a zoom needs its water scrubbed: where that water is.
const uint8_t* scrubBits = nullptr;

bool refresh(bool screenChanged, bool fastInPage, bool listFast,
             bool forceClean = false, bool gc16 = false, bool fullFlash = false) {
    // screenChanged/fastInPage/listFast/forceClean/gc16 each selected an epdiy
    // waveform. The driver makes that choice itself now, so they survive only to
    // keep the ~15 call sites unchanged.
    //
    // fullFlash is the exception and IS still live — this comment used to claim
    // it was inert while the code below acted on it, because the scrub was
    // removed and then put back when residue showed up on the map, and only the
    // code changed. See the epdc_clear_dirty() call for what it now does.
    (void)screenChanged; (void)fastInPage; (void)listFast;
    (void)forceClean; (void)gc16;

    uint8_t* fb = epdc_framebuffer();

    // Identical frame: never touch the panel. Cheaper than letting the delta
    // engine discover there is nothing to do, and it keeps the 1 Hz idle path
    // free of panel activity entirely.
    if (shadowFb && memcmp(fb, shadowFb, fbSize) == 0) return false;
    if (shadowFb) memcpy(shadowFb, fb, fbSize);

    const uint32_t tw0 = millis();
    // Scrub before painting the map. A map is nearly all fine dark lines on
    // white; the dashboard it replaces is large filled blocks and heavy type.
    // Driving straight from one to the other leaves the dashboard's residue
    // sitting under the streets, and on this panel that reads as grey haze
    // exactly where map detail needs contrast.
    //
    // The residue is NOT over-driven black — the driver's delta engine can't
    // drive a black pixel black again (see epdc_clear_dirty). It is black that
    // failed to fully erase in one light pulse, which the driver has already
    // recorded as white, so no later frame will ever touch it again. A clear is
    // what resyncs the driver's model to the glass.
    //
    // SCOPED, not the whole panel: only the rectangles that actually differ
    // between the glass and this frame. Paid only when ENTERING the map — not
    // on the way out, not when the power sheet opens over it, and not on the
    // 1 Hz map redraws.
    if (fullFlash) epdc_clear_dirty(kDirtyClearTolerance);
    if (hardClearToned) {
        const EpdRect* rects = nullptr;
        const int count = ui_render_dashboard_toned_rects(&rects);
        epdc_clear_rects(rects, count);
        hardClearToned = false;
    }
    if (scrubBits) {
        epdc_scrub_bits(scrubBits);
        scrubBits = nullptr;
    }
    doba_batt::notePaint();   // DOBA: the battery page counts repaints
    epdc_paint();

    if (dbgTiming)
        diag::log("refresh: %lums", (unsigned long)(millis() - tw0));
    refreshCount++;
    return true;
}


// Screens that are pure black/white AND update in place can use the fast
// DU path between frames. Screens with gray tones must take GL16.
bool screenIsFast(Screen s, bool overlay) {
    if (overlay) return false;  // power sheet has gray subtitle
    return s == SCREEN_DASH || s == SCREEN_MAP || s == SCREEN_GPSDEBUG;
}

// The menu / list / settings screens are pure black/white, so both their page
// changes and any in-place updates can take the fast DU path instead of the
// slower GL16 — this is what makes "switching menu state" feel snappy.
bool screenListFast(Screen s) {
    return s == SCREEN_MENU || s == SCREEN_ROUTES ||
           s == SCREEN_HISTORY || s == SCREEN_SETTINGS ||
           s == SCREEN_DIRECTIONS || s == SCREEN_MAPLAYERS;
}

bool inRect(const EpdRect& r, int x, int y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

void handlePowerTap(int x, int y);
void ackPagerTap(const EpdRect& r);

// Open Navigate or Ride History on its first page, with a fresh read of the
// card.
void enterPagedList(Screen s) {
    screen = s;
    listPage = 0;
    listStale = true;
}

// The pager footer of Navigate / Ride History (ui_render_pager). True when the
// tap landed in the footer band, whether or not it hit a button, so a missed
// arrow never falls through to "back to the menu".
bool listPagerTap(int x, int y) {
    if (y < kPagerTop) return false;
    if (inRect(kPagerUp, x, y)) {
        if (listPage > 0) { --listPage; ackPagerTap(kPagerUp); }
    } else if (inRect(kPagerDown, x, y)) {
        if ((listPage + 1) * kMenuRowCount < listTotal) {
            ++listPage;
            ackPagerTap(kPagerDown);
        }
    } else if (inRect(kPagerAction, x, y)) {
        listStale = true;   // REFRESH: re-read the card
        ackPagerTap(kPagerAction);
    }
    // The new page draws now, not at the next 1 Hz tick: the screen itself
    // doesn't change, so nothing else would hurry it.
    forceDraw = true;
    return true;
}

// DOBA: which doba_ui page a screen is, and acting on where a page leads.
doba_ui::Page dobaPage() {
    switch (screen) {
        case SCREEN_COMMS:   return doba_ui::COMMS;
        case SCREEN_COMPOSE: return doba_ui::COMPOSE;
        case SCREEN_LIBRARY: return doba_ui::LIBRARY;
        case SCREEN_ARTICLE: return doba_ui::ARTICLE;
        case SCREEN_SETUP:   return doba_ui::SETUP;
        case SCREEN_SETUP_NAME: return doba_ui::SETUP_NAME;
        case SCREEN_POINTS:  return doba_ui::POINTS;
        case SCREEN_POINT:   return doba_ui::POINT;
        case SCREEN_POINT_NEW: return doba_ui::POINT_NEW;
        case SCREEN_MAPOPTS: return doba_ui::MAPOPTS;
        case SCREEN_SOS: return doba_ui::SOS;
        case SCREEN_SUN: return doba_ui::SUN;
        case SCREEN_MORE: return doba_ui::MORE;
        case SCREEN_BATTERY: return doba_ui::BATTERY;
        default:             return doba_ui::HOME;
    }
}
void enterPagedList(Screen s);
void startOrStopTrack();
void dobaGo(doba_ui::Go g) {
    switch (g) {
        case doba_ui::STAY:        break;
        case doba_ui::TO_HOME:     screen = SCREEN_HOME; break;
        case doba_ui::TO_MAP:      screen = SCREEN_MAP; break;
        case doba_ui::TO_POSITION: screen = SCREEN_POINTS; break;
        case doba_ui::TO_LIBRARY:  screen = SCREEN_LIBRARY; break;
        case doba_ui::TO_ARTICLE:  screen = SCREEN_ARTICLE; break;
        case doba_ui::TO_COMMS:    screen = SCREEN_COMMS; break;
        case doba_ui::TO_COMPOSE:  screen = SCREEN_COMPOSE; break;
        case doba_ui::TO_TRACKS:   enterPagedList(SCREEN_HISTORY); break;
        case doba_ui::TO_ROUTES:   enterPagedList(SCREEN_ROUTES); break;
        case doba_ui::TO_SETTINGS: screen = SCREEN_SETTINGS; break;
        case doba_ui::TO_RECORD:   startOrStopTrack(); break;
        case doba_ui::TO_SETUP:    screen = SCREEN_SETUP; break;
        case doba_ui::TO_SETUP_NAME: screen = SCREEN_SETUP_NAME; break;
        case doba_ui::TO_POINTS:   screen = SCREEN_POINTS; break;
        case doba_ui::TO_POINT:    screen = SCREEN_POINT; break;
        case doba_ui::TO_POINT_NEW: screen = SCREEN_POINT_NEW; break;
        case doba_ui::TO_MAPOPTS:  screen = SCREEN_MAPOPTS; break;
        case doba_ui::TO_SOS:      screen = SCREEN_SOS; break;
        case doba_ui::TO_SUN:      screen = SCREEN_SUN; break;
        case doba_ui::TO_MORE:     screen = SCREEN_MORE; break;
        case doba_ui::TO_BATTERY:  screen = SCREEN_BATTERY; break;
        case doba_ui::TO_GPSDEBUG: screen = SCREEN_GPSDEBUG; break;
        case doba_ui::TO_POWER:    sheetKind = SHEET_POWER; powerOverlay = true; break;
    }
}

// Navigate up one level in the screen hierarchy (front button, short press).
void goBack() {
    if (routes::navPending()) {
        routes::dismissNav();
        return;
    }
    if (powerOverlay) {
        powerOverlay = false;
        return;
    }
    switch (screen) {
        case SCREEN_GPSDEBUG: screen = SCREEN_MORE; break;   // DOBA
        case SCREEN_MAPLAYERS: screen = SCREEN_MAP; break;
        case SCREEN_DIRECTIONS: screen = directionsFrom; break;
        case SCREEN_SETTINGS: screen = SCREEN_SETUP; break;  // DOBA: OTP's settings are "MORE"
        case SCREEN_ROUTES:
        case SCREEN_HISTORY:  screen = SCREEN_HOME; break;   // DOBA: was the menu
        case SCREEN_HOME:
        case SCREEN_COMMS:
        case SCREEN_COMPOSE:
        case SCREEN_LIBRARY:
        case SCREEN_SETUP: case SCREEN_SETUP_NAME: case SCREEN_POINTS:
        case SCREEN_POINT: case SCREEN_POINT_NEW: case SCREEN_MAPOPTS: case SCREEN_SOS: case SCREEN_SUN: case SCREEN_MORE: case SCREEN_BATTERY:
        case SCREEN_ARTICLE:  dobaGo(doba_ui::back(dobaPage())); break;
        case SCREEN_MENU:     screen = SCREEN_DASH; break;
        case SCREEN_MAP:      screen = SCREEN_HOME; break;   // DOBA: the map is a page of Doba's home
        // Back on the summary resumes the ride (non-destructive); SAVE / DISCARD
        // must be tapped explicitly.
        case SCREEN_SUMMARY:
            // Backing out of the recovery prompt settles it the safe way:
            // finalize and keep the ride. Same philosophy as power-off.
            if (recoveryPrompt) {
                recoveryPrompt = false;
                ride_recorder::resolveRecovery(ride_recorder::RECOVERY_SAVE);
            }
            screen = SCREEN_DASH;
            break;
        case SCREEN_DASH:     break;  // already home
    }
}

// DOBA: the menu's old row 0, shared with the main screen's RECORD tile.
void startOrStopTrack() {
    if (ride_recorder::isRecording()) {
        pendingSummary = ride_recorder::summary();
        screen = SCREEN_SUMMARY;
    } else {
        ride_recorder::startRide();
        dashPage = firstContentPage();
        screen = SCREEN_DASH;
    }
}

// Paint the acknowledgment band NOW, before a tap handler runs seconds of SD
// work (finalizing or discarding a FIT). The paint is asynchronous, so the
// band draws while the work runs; the next frame repaints over it.
void paintBusyBand(const char* title) {
    // 440/180: start at the summary sheet's top edge and cover its label and
    // hero line whole — a half-clipped hero read as a rendering glitch.
    ui_render_busy_band(title, "one moment...", epdc_framebuffer(), 440, 180);
    epdc_paint();
}

// BOOT short-press: start a ride, or stop it (via the save/discard summary).
void toggleRide() {
    if (ride_recorder::isRecording()) {
        pendingSummary = ride_recorder::summary();
        screen = SCREEN_SUMMARY;
    } else {
        recoveryPrompt = false;   // startRide() saves any still-pending ride
        ride_recorder::startRide();
        screen = SCREEN_DASH;
    }
}

// Side-key short-press: step the backlight Off -> Low -> Med -> Bright -> Off.
void cycleBacklight() {
    if (stealth::active()) return;   // DOBA: no light in stealth, not even by the side key
    int next = (settings::backlight() + 1) & 3;
    settings::setBacklight(next);
    applyBacklight(next);
}

// Answer a zoom tap NOW, before the map is re-projected.
//
// Re-projecting pulls tiles off the SD card and takes the best part of a
// second, and until this existed nothing on the panel moved in that time — so a
// tap that had registered looked exactly like one that had missed, and the
// natural response was to tap again (zooming twice). Painting the button in its
// pressed state is a ~76 px change the driver's delta engine drives almost
// immediately; the full redraw that follows puts it back.
//
// Through refresh() rather than epdc_paint() directly, so the shadow buffer
// tracks what is actually on the glass. Otherwise a tap at either end of the
// zoom range — where mapMpp is clamped and the rest of the frame is unchanged —
// would leave the button stuck black: the next frame would compare equal to the
// shadow and never repaint.
void ackZoomTap(bool zoomIn) {
    if (screen != SCREEN_MAP) return;
    uint8_t* fb = epdc_framebuffer();
    ui_map_draw_zoom_button(zoomIn, true, fb);
    refresh(false, true, false);
    // Stay pressed until the map has actually caught up. A single flash before
    // the redraw answers "did it hear me?" but not "is it still working?", and
    // on a cold zoom the outer tiles can take a moment to arrive. Held down, the
    // button is doing the job a spinner would.
    zoomHeld = zoomIn ? 1 : 2;
}

// Answer a pager tap (arrow or the action button) NOW, the way ackZoomTap
// answers a zoom: fill the button black on the glass before the page is
// rebuilt. The next page only changes a few rows of text, which on e-paper is
// easy to miss, so without this a tap that registered looked like one that
// hadn't. The redraw that follows draws the button normally again. Through
// refresh() so the shadow buffer matches the glass.
void ackPagerTap(const EpdRect& r) {
    uint8_t* fb = epdc_framebuffer();
    epd_fill_rect(r, ui::INK, fb);
    refresh(false, true, false);
}

// DOBA: "heard you, working on it". A tap that opens a book, a page of the
// shelf or the map reads the card before anything new can be drawn, and on
// e-paper a second of nothing reads as a missed tap. A small black pill with
// an hourglass goes up in the status bar at once; the redraw that follows
// paints over it.
void ackBusy() {
    // A ring of dots in the middle of the screen, graded from faint to black
    // like a spinner caught mid-turn, on a white disc so it reads on any page.
    uint8_t* fb = epdc_framebuffer();
    const int cx = 540 / 2, cy = 960 / 2;
    epd_fill_circle(cx, cy, 62, ui::INK, fb);
    epd_fill_circle(cx, cy, 59, ui::PAPER, fb);
    static const uint8_t kGrey[10] = {0xE0, 0xD0, 0xC0, 0xB0, 0x98, 0x80, 0x60, 0x40, 0x20, 0x00};
    for (int k = 0; k < 10; ++k) {
        const float a = -1.5708f + k * 0.6283f;          // 36 degrees apart, from the top
        const int x = cx + (int)lroundf(36 * cosf(a)), y = cy + (int)lroundf(36 * sinf(a));
        epd_fill_circle(x, y, 3 + k / 3, kGrey[k], fb);
    }
    refresh(false, true, false);
}

// DOBA: a finger drawn across the map body moves the map with it. Released,
// not live: e-paper redraws once, where the finger let go.
bool mapDrag(int x0, int y0, int x1, int y1) {
    if (screen != SCREEN_MAP || powerOverlay) return false;
    const int dx = x1 - x0, dy = y1 - y0;
    if (dx * dx + dy * dy < 30 * 30) return false;
    if (y0 < ui::STATUS_H || y0 >= ui::MAP_STRIP_TOP) return false;
    mapPanE -= dx * mapMpp;
    mapPanN += dy * mapMpp;
    Serial.printf("[map] drag %d,%d -> %.0f m E %.0f m N\n", dx, dy, mapPanE, mapPanN);
    ackBusy();   // the new tiles come off the card
    return true;
}

// DOBA: a finger held on the map body drops a waypoint there and opens it.
bool mapLongPress(int x, int y) {
    if (screen != SCREEN_MAP) return false;
    if (y < ui::STATUS_H + 10 || y > ui::MAP_STRIP_TOP - 10) return false;
    if (x >= kMapZoom.zoomX - 10) return false;          // the button column
    double lat, lon;
    if (!milspec::screenToLatLon(x, y, lat, lon)) return false;
    const int i = doba_points::add(nullptr, lat, lon);
    if (i < 0) return false;
    Serial.printf("[pts] long-press point %d at %.6f,%.6f\n", i, lat, lon);
    doba_ui::openPoint(i);
    screen = SCREEN_POINT;
    return true;
}

bool isDobaSubPage(Screen sc) {
    switch (sc) {
        case SCREEN_COMMS: case SCREEN_COMPOSE: case SCREEN_LIBRARY: case SCREEN_ARTICLE:
        case SCREEN_SETUP: case SCREEN_SETUP_NAME: case SCREEN_POINTS: case SCREEN_POINT:
        case SCREEN_POINT_NEW: case SCREEN_MAPOPTS: case SCREEN_SOS: case SCREEN_SUN: case SCREEN_MORE: case SCREEN_BATTERY:
        case SCREEN_MAP: case SCREEN_SETTINGS: case SCREEN_GPSDEBUG: case SCREEN_MAPLAYERS:
            return true;
        default:
            return false;
    }
}

void handleTap(int x, int y) {
    // The "Start navigation?" prompt owns every tap while it is up. START
    // begins navigation; any other tap dismisses it, so it can never trap
    // the UI.
    if (routes::navPending()) {
        // Ignore taps for a moment after the prompt appears so a touch that
        // was already in progress when the route arrived can't auto-accept
        // it. The user must deliberately tap START.
        if (millis() - navPromptShownAt < 700) return;
        if (inRect(kPowerShutdown, x, y)) {
            routes::startNav();
            screen = SCREEN_MAP;
        } else {
            // LATER: don't just stop navigating — fully unload the route so it
            // isn't drawn on the map. It's still saved on the SD card, so it can
            // be started later from Saved Routes.
            routes::clearRoute();
        }
        return;
    }

    // DOBA: Doba's sub-pages carry a BACK chevron at the left of the band (the
    // Home key still goes back too; this is the one a new user finds).
    if (y < ui::STATUS_H && x < ui::BACK_W + 20 && isDobaSubPage(screen)) {
        goBack();
        return;
    }

    // No BACK strip to hit-test: the Home button below the glass goes back from
    // every sub-screen, so the strip was a full-width control duplicating a key
    // the rider's thumb is already on, for 96 px of every page.

    // Tapping the turn banner opens the rest of the route's directions. It used
    // to end navigation, which is a destructive action on the one control a
    // rider is most likely to hit by accident — and it threw away the thing they
    // probably wanted, which is to see what is coming up. Navigation is ended
    // from the menu instead.
    // A message popup over the band is dismissed by tapping it, ahead of what
    // the band would otherwise do underneath (directions, resume).
    if (meshNoticeShown() && inRect(kNavBanner, x, y) &&
        (screen == SCREEN_DASH || screen == SCREEN_MAP)) {
        noticeUp = false;
        noteActivity();
        return;
    }

    if (routes::navActive() && inRect(kNavBanner, x, y) &&
        (screen == SCREEN_DASH || screen == SCREEN_MAP)) {
        directionsFrom = screen;
        screen = SCREEN_DIRECTIONS;
        noteActivity();
        return;
    }

    // Tapping the AUTO-PAUSED banner (same band, shown when nav isn't) resumes
    // the ride by hand. Clearing the pause is also what restarts the sensor
    // hunt — its gate is "recording and not parked" — so a rider tapping at
    // the end of a coffee stop gets their meter and strap reconnecting while
    // they clip in, instead of waiting for GPS to notice they're rolling.
    if (!routes::navActive() && inRect(kNavBanner, x, y) &&
        (screen == SCREEN_DASH || screen == SCREEN_MAP) &&
        g_state.snapshot().ridePaused) {
        ride_recorder::manualResume();
        noteActivity();
        return;
    }

    switch (screen) {
        case SCREEN_DASH:
        case SCREEN_MAP:
            if (screen == SCREEN_MAP) {
                int dx = x - kMapCompass.cx,
                    dy = y - mapCompassCy(routes::navActive()) - (mapTopoActive ? 56 : 0);
                if (dx * dx + dy * dy <= kMapCompass.r * kMapCompass.r) {
                    mapTrackUp = !mapTrackUp;
                    break;
                }
            }
            if (screen == SCREEN_MAP && x >= kMapZoom.zoomX &&
                x < kMapZoom.zoomX + kMapZoom.size) {
                // DOBA: the layers button opens Doba's map options (OTP's were
                // cycling layers: bike routes, toilets, bike shops).
                if (mapPanned() && y >= mapCentreY() && y < mapCentreY() + kMapZoom.size) {
                    mapPanE = mapPanN = 0;
                    milspec::measureClear();
                    Serial.println("[map] centre");
                    break;
                }
                if (y >= kMapZoom.layersY && y < kMapZoom.layersY + kMapZoom.size) {
                    screen = SCREEN_MAPOPTS;
                    break;
                }
                if (y >= kMapZoom.zoomInY && y < kMapZoom.zoomInY + kMapZoom.size) {
                    if (mapMpp > 1.0f) mapMpp /= 2.0f;
                    Serial.printf("[map] zoom in -> %.2f m/px\n", mapMpp);
                    ackZoomTap(true);
                    break;
                }
                if (y >= kMapZoom.zoomOutY && y < kMapZoom.zoomOutY + kMapZoom.size) {
                    if (mapMpp < mapMaxMpp()) mapMpp *= 2.0f;
                    Serial.printf("[map] zoom out -> %.2f m/px\n", mapMpp);
                    ackZoomTap(false);
                    break;
                }
            }
            // Status bar opens the menu. On the map, the DATA STRIP along the
            // bottom (below MAP_STRIP_TOP, clear of the zoom buttons at y 560
            // and 640) goes to the dashboard. The body of either screen does
            // nothing.
            //
            // Tapping anywhere used to flip, which made the map hostile to use:
            // every miss of the compass or a zoom button threw the rider off the
            // map, and on the dashboard a stray glove brush did the same. The
            // flip now has deliberate targets — that strip, and the Home key,
            // which is the only way back to the map.
            // DOBA: a tap on the cross of a dragged map sets A there (or clears
            // it); the strip then measures from A to wherever the cross goes.
            if (screen == SCREEN_MAP && mapTopoActive && mapPanned() &&
                abs(x - 270) < 56 && abs(y - milspec::kCrossY) < 56) {
                milspec::measureToggleAtCross();
                Serial.printf("[map] measure %s\n", milspec::measureOn() ? "A set" : "off");
                break;
            }
            if (y < ui::STATUS_H) screen = SCREEN_HOME;   // DOBA: was the menu
            else if (screen == SCREEN_MAP && y >= ui::MAP_STRIP_TOP) {
                screen = SCREEN_POINTS;   // DOBA: the strip opens POSITION (no ride dashboard)
            }
            break;
        case SCREEN_SUMMARY:
            if (inRect(kResumeButton, x, y)) {
                if (recoveryPrompt) {
                    // CONTINUE: reopen the interrupted file and keep recording.
                    // Resuming replays the whole record stream first.
                    paintBusyBand("RESUMING TRACK");
                    recoveryPrompt = false;
                    ride_recorder::resolveRecovery(ride_recorder::RECOVERY_CONTINUE);
                }
                // Live ride: it never stopped recording — just go back to the
                // dashboard and keep going.
                screen = SCREEN_DASH;
            } else if (inRect(kSaveButton, x, y)) {
                paintBusyBand("SAVING TRACK");
                if (recoveryPrompt) {
                    recoveryPrompt = false;
                    ride_recorder::resolveRecovery(ride_recorder::RECOVERY_SAVE);
                } else {
                    ride_recorder::stopRide(true);
                }
                screen = SCREEN_DASH;
            } else if (inRect(kDiscardButton, x, y)) {
                paintBusyBand("DISCARDING TRACK");
                if (recoveryPrompt) {
                    recoveryPrompt = false;
                    ride_recorder::resolveRecovery(ride_recorder::RECOVERY_DISCARD);
                } else {
                    ride_recorder::stopRide(false);
                }
                screen = SCREEN_DASH;
            }
            break;
        case SCREEN_MENU: {
            int row = (y - kMenuRowTop) / kMenuRowH;
            if (y >= kMenuRowTop && row >= 0 && row < kMenuRowCount) {
                switch (row) {
                    case 0: startOrStopTrack(); break;
                    case 1: enterPagedList(SCREEN_ROUTES); break;
                    case 3: enterPagedList(SCREEN_HISTORY); break;
                    case 4: screen = SCREEN_SETTINGS; break;
                }
            } else {
                screen = SCREEN_DASH;  // header or footer: back to ride
            }
            break;
        }
        case SCREEN_ROUTES: {
            if (listPagerTap(x, y)) break;
            int row = (y - kMenuRowTop) / kMenuRowH;
            if (y >= kMenuRowTop && row >= 0 && row < kMenuRowCount) {
                int idx = listPage * kMenuRowCount + row;
                if (idx < listPinned) {
                    routes::clearRoute();
                } else if (idx < listTotal && idx - listPinned < routeFileCount) {
                    if (routes::load(routeFiles[idx - listPinned])) {
                        screen = SCREEN_MAP;
                    }
                }
            } else {
                screen = SCREEN_HOME;   // DOBA
            }
            break;
        }
        case SCREEN_HISTORY:
            // Rows aren't actions (yet): any tap outside the pager goes back.
            if (!listPagerTap(x, y)) screen = SCREEN_HOME;   // DOBA
            break;
        case SCREEN_HOME:
        case SCREEN_COMMS:
        case SCREEN_COMPOSE:
        case SCREEN_LIBRARY:
        case SCREEN_SETUP: case SCREEN_SETUP_NAME: case SCREEN_POINTS:
        case SCREEN_POINT: case SCREEN_POINT_NEW: case SCREEN_MAPOPTS: case SCREEN_SOS: case SCREEN_SUN: case SCREEN_MORE: case SCREEN_BATTERY:
        case SCREEN_ARTICLE: {  // DOBA
            const int bl = settings::backlight();
            // The pages whose taps read the card: home (into the library or the
            // map), the library (books, shelves, letters) and an article (links).
            if (screen == SCREEN_HOME || (screen == SCREEN_LIBRARY && !doba_lib::typing()))
                ackBusy();
            {   // DOBA: what a tap cost before the redraw (opening a book page reads the card)
                const uint32_t t0 = micros();
                const int pg = (int)dobaPage();
                dobaGo(doba_ui::tap(dobaPage(), x, y, g_state.snapshot()));
                const uint32_t ms = (micros() - t0) / 1000;
                if (ms > 30) Serial.printf("[perf] tap on page %d (%d,%d) took %lu ms\n", pg, x, y, (unsigned long)ms);
            }
            if (settings::backlight() != bl) applyBacklight(settings::backlight());
            break;
        }
        case SCREEN_SETTINGS: {
            int row = (y - kMenuRowTop) / kSettingsRowH;
            bool inRows = y >= kMenuRowTop && row >= 0 && row < kSettingsRowCount;
            // BACKLIGHT is NOT a toggle row: ui_render_settings draws it with the
            // -/+ stepper (four levels don't fit a switch), so it must be hit-
            // tested like one. It used to be listed here, which meant only the
            // toggle band — overlapping the + button — did anything: "-" fell
            // through to the "tap anywhere else" branch and bounced the rider
            // out to the menu instead of dimming the light.
            bool toggleRow = row == kSettingsUnitsRow || row == kSettingsUsbRow ||
                             row == kSettingsOfflineRow || row == kSettingsMeshRow;
            bool minus = x >= kSettingsMinusX && x < kSettingsMinusX + kSettingsBtn;
            bool plus = x >= kSettingsPlusX && x < kSettingsPlusX + kSettingsBtn;
            // Per-row, because UNITS draws a wider switch: a fixed rect would
            // leave its left third dead, and a stray tap there falls through to
            // the do-nothing branch with no feedback.
            const int togX = inRows ? settingsToggleX(row) : kSettingsToggleX;
            const int togW = inRows ? settingsToggleW(row) : kSettingsToggleW;
            bool toggleHit = x >= togX && x < togX + togW;
            bool edited = false;
            if (inRows && !toggleRow && (minus || plus)) {
                int dir = plus ? 1 : -1;
                // DOBA: row 0 is the mils system: either button flips it.
                if (row == 0) milspec::setMilsSystem(milspec::milsSystem() == 6400 ? 6000 : 6400);
                if (row == 1) settings::setTzMinutes(settings::tzMinutes() + dir * 30);
                if (row == kSettingsBacklightRow) {
                    // Clamped, not wrapped: with a visible -/+ pair, "+" at
                    // Bright wrapping round to Off would read as a fault. (The
                    // side key still cycles — that one has nowhere else to go.)
                    int bl = settings::backlight() + dir;
                    if (bl < 0) bl = 0;
                    if (bl > 3) bl = 3;
                    settings::setBacklight(bl);
                    applyBacklight(bl);
                }
                edited = true;
            } else if (inRows && toggleRow && toggleHit) {
                if (row == kSettingsUnitsRow) {
                    settings::setUseMiles(!settings::useMiles());
                } else if (row == kSettingsUsbRow) {
                    bool on = !settings::usbDrive();
                    settings::setUsbDrive(on);
                    usb_storage::setDriveEnabled(on);
                } else if (row == kSettingsMeshRow) {
                    // REQUEST, not apply: setEnabled() stages the change for the
                    // mesh task, which owns the radio. Doing the bring-up from
                    // the touch handler would drive SPI from a second task and
                    // is exactly what sd_bus.h warns about. Persisting is the
                    // task's job too (applyEnabled -> settings::setMeshEnabled),
                    // so nothing is written here.
                    mesh_service::setEnabled(!mesh_service::enabled());
                } else {   // show offline sensor fields
                    settings::setShowOffline(!settings::showOffline());
                }
                edited = true;
            }
            if (edited) {
                g_state.with([](RideState& st) {
                    st.tzMin = (int16_t)settings::tzMinutes();
                    st.useMiles = settings::useMiles();
                    st.clock24h = settings::clock24h();
                    st.showOffline = settings::showOffline();
                });
            } else if (y >= kMenuRowTop && row == kSettingsGpsRow) {
                screen = SCREEN_GPSDEBUG;
            }
            // Anything else on this screen does NOTHING. Settings is the one
            // place where a stray touch used to navigate away mid-edit — reach
            // for a stepper, miss it by a few pixels, and you were back on the
            // menu with the change unmade. The capacitive Home key (goBack())
            // is the only way out.
            break;
        }
        case SCREEN_MAPLAYERS: {
            // A row flips its layer; the map picks the mask up on its next
            // frame. Anything else does nothing — Home goes back to the map,
            // as on Settings, so a near-miss never navigates away mid-edit.
            const int row = ui_map_layers_hit(x, y);
            if (row >= 0) {
                settings::setMapLayers(settings::mapLayers() ^ ui_map_layer_bit(row));
            }
            break;
        }
        case SCREEN_GPSDEBUG:
            // Experimental: a tap in the top band runs the smooth-drive
            // pipeline self-test; anywhere else navigates back to Settings.
            if (y < 150) {
                smooth_epd::selfTest();
                // The self-test drove the panel directly, behind the driver's
                // back, so its record of what is on glass is now wrong. A clear
                // puts both back in agreement at white; zeroing shadowFb (a
                // value the renderer never produces) forces a full redraw.
                epdc_clear();
                if (shadowFb) memset(shadowFb, 0, fbSize);
            } else {
                screen = SCREEN_SETTINGS;
            }
            break;
    }
}

void handlePowerTap(int x, int y) {
    if (inRect(kPowerShutdown, x, y)) {
        uint8_t* fb = epdc_framebuffer();
        shutdownDevice(fb, "user power-off (dialog)");  // does not return
    } else {
        // CANCEL, or a tap anywhere outside the sheet, dismisses it.
        powerOverlay = false;
    }
}

// The letters in a rider's map marker: the first two of their name, "Al" for
// Alex, so two riders sharing an initial still read apart. A name nobody set
// ("OpenTrail 1a2b", "Meshtastic 1a2b") would give everyone the same letters,
// so those fall back to the short name, which is unique-ish by default.
static void peerInitials(const mesh_service::Node& nd, char out[3]) {
    const char* name = nd.longName;
    if (!strncmp(name, "OpenTrail ", 10) || !strncmp(name, "Meshtastic ", 11))
        name = "";
    for (const char* src : {name, (const char*)nd.shortName}) {
        int n = 0;
        for (const char* c = src; *c && n < 2; ++c) {
            if (!isalnum((unsigned char)*c)) continue;
            out[n] = n ? (char)tolower((unsigned char)*c) : (char)toupper((unsigned char)*c);
            ++n;
        }
        out[n] = 0;
        if (n) return;
    }
    snprintf(out, 3, "?");
}

// Riders on our private mesh channels who have told us where they are (see
// MapPeer). Public-channel positions are left off: those are strangers, and a
// busy public mesh would cover the map in them.
void buildMapPeers(double lat, double lon, float rot, bool useMiles,
                   MapScreenData& map) {
    static MapPeer peers[16];
    int count = 0;
    const uint32_t now = millis();
    const int n = mesh_service::nodeCount();
    for (int i = 0; i < n && count < (int)(sizeof(peers) / sizeof(peers[0])); ++i) {
        mesh_service::Node nd;
        if (!mesh_service::nodeAt(i, nd)) break;
        if (!nd.hasPosition || nd.positionChannel == 0) continue;
        const uint32_t age = now - nd.positionMs;
        if (age > MESH_MAP_MAX_AGE_MS) continue;

        // Project at a scale that cannot overflow the int16 screen coordinates
        // — a rider 50 km off at 2 m/px is 25,000 px away. Only the direction
        // matters once they are off the screen, and that survives the rescale.
        const double dy = (nd.latitude - lat) * 111320.0;
        const double dx = (nd.longitude - lon) * 111320.0 * cos(lat * M_PI / 180.0);
        const float distM = (float)sqrt(dx * dx + dy * dy);
        const float mpp = distM / mapMpp > 8000.0f ? distM / 8000.0f : mapMpp;
        int16_t x, y;
        map_tiles::geoToScreen(nd.latitude, nd.longitude, lat, lon, mpp,
                               map.riderX, map.riderY, rot, x, y);

        // Inside the map with room for the marker, clear of the top band when
        // something is drawn in it; otherwise pinned further in, so the
        // pointer reaching out of it toward them stays on the glass too.
        const int bandBottom = ui::STATUS_H + (map.navBannerVisible ? 138 : 0);
        MapPeer& p = peers[count];
        p.offscreen = x < 24 || x > 540 - 24 || y < bandBottom + 24 ||
                      y > ui::MAP_STRIP_TOP - 24;
        const int left = 84, right = 540 - 84;
        const int top = bandBottom + 84, bottom = ui::MAP_STRIP_TOP - 84;
        if (p.offscreen) {
            const float vx = x - map.riderX, vy = y - map.riderY;
            float t = 1.0f;
            if (vx > 0) t = fminf(t, (right - map.riderX) / vx);
            if (vx < 0) t = fminf(t, (left - map.riderX) / vx);
            if (vy > 0) t = fminf(t, (bottom - map.riderY) / vy);
            if (vy < 0) t = fminf(t, (top - map.riderY) / vy);
            // Slide in along the same line until it is clear of the chrome
            // drawn over the map edge (compass, zoom column, scale bar), so the
            // marker is neither buried nor mistaken for the compass needle.
            const int compY = mapCompassCy(map.navBannerVisible) + (map.topo ? 56 : 0);
            auto underChrome = [&](float px, float py) {
                const float cdx = px - kMapCompass.cx, cdy = py - compY;
                const float cr = kMapCompass.r + 30;
                if (cdx * cdx + cdy * cdy < cr * cr) return true;
                if (px > kMapZoom.zoomX - 50 && py > kMapZoom.layersY - 50 &&
                    py < kMapZoom.zoomOutY + kMapZoom.size + 50) return true;
                if (px < 190 + 50 && py > ui::MAP_STRIP_TOP - 48 - 50) return true;
                return false;
            };
            // In 4 px steps along the line, no nearer the rider than 80 px.
            const float len = sqrtf(vx * vx + vy * vy);
            const float edge = t;
            for (float k = edge; k * len > 80.0f; k -= 4.0f / len) {
                t = k;
                // The marker and the tip of its pointer (78 px further out).
                const float cx = map.riderX + vx * k, cy = map.riderY + vy * k;
                if (!underChrome(cx, cy) &&
                    !underChrome(cx + vx / len * 78, cy + vy / len * 78)) break;
            }
            x = (int16_t)(map.riderX + vx * t);
            y = (int16_t)(map.riderY + vy * t);
        }
        p.x = x;
        p.y = y;
        p.stale = age > MESH_MAP_FRESH_MS;
        peerInitials(nd, p.initials);
        // Off the screen, how far: which way AND whether it is a wait at the
        // next corner or a ride to catch up.
        p.label[0] = 0;
        if (p.offscreen) {
            units::navDist(p.label, sizeof(p.label), distM, useMiles);
            for (char* c = p.label; *c; ++c) *c = (char)toupper((unsigned char)*c);
        }
        count++;
    }
    map.peers = peers;
    map.peerCount = count;
}

MapBaseKey mapKeyFor(const MapScreenData& map) {
    MapBaseKey k;
    k.lat = map.centerLat + mapPanN / 111320.0;
    k.lon = map.centerLon + mapPanE / (111320.0 * cos(map.centerLat * M_PI / 180.0));
    k.mpp = map.metersPerPixel;
    k.rotDeg = map.northDeg;
    k.layers = map.layers;
    k.tiles = map_store::generation();
    k.hasMap = map.hasMap;
    return k;
}

void mapCacheAllocate() {
    if (mapCacheAllocated) return;
    mapCacheAllocated = true;
    const size_t bytes = fbSize / 4 + sizeof(MapLayerFacts) + kMapCachePois * sizeof(MapPoi) +
                         fbSize / 8;
    for (int i = 0; i < MapBaseCache::kSlots; ++i)
        mapCache.attach(i, (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    mapPrerenderFb = (uint8_t*)heap_caps_malloc(fbSize, MALLOC_CAP_SPIRAM);
}

MapLayerFacts* cachedFacts(const uint8_t* layer) { return (MapLayerFacts*)(layer + fbSize / 4); }
MapPoi* cachedPois(const uint8_t* layer) {
    return (MapPoi*)(layer + fbSize / 4 + sizeof(MapLayerFacts));
}

// Four framebuffer bytes, so eight pixels, to one byte and back. White is 1.
void mapLayerPack(const uint8_t* fb, uint8_t* layer) {
    for (size_t i = 0; i < fbSize / 4; ++i) {
        uint8_t bits = 0;
        for (int b = 0; b < 4; ++b) {
            const uint8_t px = fb[i * 4 + b];
            bits |= (uint8_t)(((px & 0x80) ? 2 : 0) | ((px & 0x08) ? 1 : 0)) << (2 * b);
        }
        layer[i] = bits;
    }
}

void mapLayerExpand(const uint8_t* layer, uint8_t* fb) {
    static uint32_t table[256];
    if (!table[0xFF]) {
        for (int v = 0; v < 256; ++v) {
            uint8_t out[4];
            for (int b = 0; b < 4; ++b) {
                const int two = (v >> (2 * b)) & 3;
                out[b] = (uint8_t)(((two & 2) ? 0xF0 : 0) | ((two & 1) ? 0x0F : 0));
            }
            memcpy(&table[v], out, 4);
        }
    }
    uint32_t* words = (uint32_t*)fb;
    for (size_t i = 0; i < fbSize / 4; ++i) words[i] = table[layer[i]];
}

void mapLayerKeep(const uint8_t* fb) { mapLayerPack(fb, mapLayerBeingKept); }

uint8_t* cachedWater(const uint8_t* layer) {
    return (uint8_t*)cachedPois(layer) + kMapCachePois * sizeof(MapPoi);
}

// What a stored layer needs besides its pixels. Call straight after drawing it.
void mapCacheKeepExtras(uint8_t* layer, const MapScreenData& map) {
    const int n = map.poiCount < kMapCachePois ? map.poiCount : kMapCachePois;
    if (n) memcpy(cachedPois(layer), map.pois, n * sizeof(MapPoi));
    MapLayerFacts* facts = cachedFacts(layer);
    facts->poiCount = n;
    facts->features = map.featureCount;
    facts->tilePolys = map.tilePolys;
    facts->projectedTiles = map.projectedTiles;
    facts->wantedTiles = map.wantedTiles;
    facts->parks = map.parkCount;
    facts->waters = map.waterCount;
    memcpy(facts->classes, map.clsCount, sizeof(facts->classes));

    uint8_t* bits = cachedWater(layer);
    memset(bits, 0, fbSize / 8);
    if (const uint8_t* mask = ui_map_dither_mask()) {
        for (size_t i = 0; i < fbSize; ++i) {
            if (mask[i]) bits[i >> 3] |= (uint8_t)(1 << (i & 7));
        }
    }
}

void buildMapScreenData(const RideState& s, MapScreenData& map,
                        bool finalFrame) {
    map.riderX = 270;
    map.riderY = 430;
    // Track-up: the world rotates so travel direction is up; the rider
    // arrow points up and the compass needle shows where north went.
    // Which way the device is pointing. Course-over-ground is a DIRECTION OF
    // TRAVEL: it is meaningless standing still and jittery at walking pace,
    // which is why a track-up map used to spin while the rider waited at a
    // light. The compass has no such problem and is used below 5 km/h, if one
    // is fitted and has finished calibrating.
    const bool useCompass = s.compassValid && s.speedKmh < 5.0f;
    const float heading = mapTestHeading >= 0.0f ? mapTestHeading
                                                 : useCompass ? s.compassDeg : s.courseDeg;
    float rot = mapTrackUp ? -heading : 0.0f;
    map.headingDeg = mapTrackUp ? 0.0f : heading;
    map.northDeg = rot;
    map.trackUp = mapTrackUp;
    // Tell the map renderer to drop the compass below the turn banner when one
    // is drawn over the top of the viewport. The pause banner sits in the same
    // band, so it displaces the compass the same way.
    map.navBannerVisible = routes::navActive() || s.ridePaused ||
                           meshNoticeShown();
    map.metersPerPixel = mapMpp;
    map.layers = settings::mapLayers();

    // Position priority: the device's own current GPS fix; else the connected
    // phone's recent location (fallback when our receiver is cold); else the
    // last known device position; else the persisted/default. Never jump to the
    // default mid-ride.
    double lat = DEFAULT_MAP_LAT, lon = DEFAULT_MAP_LON;
    bool phoneRecent = s.phoneFixValid && (millis() - s.phoneFixMs) < 15000;
    map.phonePosition = phoneRecent && !s.gpsFix;
    if (s.gpsFix) {
        lat = s.latitude;
        lon = s.longitude;
    } else if (phoneRecent) {
        lat = s.phoneLat;
        lon = s.phoneLon;
    } else if (s.everHadFix) {
        lat = s.latitude;
        lon = s.longitude;
    } else {
        settings::lastPosition(lat, lon);
    }
    map.centerLat = lat;
    map.centerLon = lon;
    // DOBA: a topo pack under the rider takes over the base layer. Its tiles
    // are drawn 1:1, so the zoom snaps to the tile zoom nearest the one asked
    // for (the zoom buttons still halve/double; the snap keeps them on tile
    // zooms), and the map is north-up: a raster is not rotated.
    map.topo = topo::covers(lat, lon);
    mapTopoActive = map.topo;
    if (map.topo) {
        // DOBA: the first topo view opens at z14 (~6 m/px), not at the closest
        // level, so + and - both do something from the start.
        static bool opened = false;
        if (!opened) {
            opened = true;
            if (mapMpp == 2.0f) mapMpp = topo::nativeMpp(14, lat);
        }
        // Down to ~0.4 m/px (z18): drawn from z17 detail tiles where the card
        // has them, else from z16, the last steps by enlarging the tiles.
        const int zmax = topo::maxZoomAt(lat, lon);
        // DOBA: no enlarging past the deepest tiles. A z17 tile blown up 2x
        // turns its fine anti-aliased lines into dotted ones on four greys
        // (seen on the device at 1:3 500), and costs a second per screen.
        const float nMax = topo::nativeMpp(zmax, lat);
        if (mapMpp < nMax * 0.75f) mapMpp = nMax;
        else mapMpp = topo::nativeMpp(std::min(topo::zoomFor(lat, mapMpp), zmax), lat);
        map.metersPerPixel = mapMpp;
        rot = 0.0f;
        map.northDeg = 0.0f;
        map.trackUp = false;
        map.headingDeg = heading;
    }
    // DOBA: dragged: the view stays north-up and the position moves on the
    // screen (it may leave it); everything is still drawn round it.
    map.zoomInEnd = map.topo ? mapMpp <= topo::nativeMpp(18, lat) * 1.01f : mapMpp <= 1.0f;
    map.zoomOutEnd = mapMpp * 2.0f > mapMaxMpp() * 1.01f;
    // DOBA: the place under the view (the position, or the middle when dragged)
    if (map.topo) {
        const double vlat = lat + mapPanN / 111320.0;
        const double vlon = lon + mapPanE / (111320.0 * cos(lat * M_PI / 180.0));
        static double qLat = 999, qLon = 999;
        static char qName[32] = "", qSub[32] = "";
        double b0, moved;
        milgeo::bearingDistance(qLat, qLon, vlat, vlon, b0, moved);
        if (qLat > 90 || moved > 150) {
            qLat = vlat;
            qLon = vlon;
            double d, b;
            if (doba_places::nearest(vlat, vlon, qName, sizeof qName, d, b)) {
                static const char* const kDir[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
                // the spot relative to the place: from the place, towards here
                const char* dir = kDir[(int)lround(fmod(b + 180.0, 360.0) / 45.0) % 8];
                if (d < 800) snprintf(qSub, sizeof qSub, "%s", mapPanned() ? TR("map centre") : TR("you are here"));
                else if (d < 10000) snprintf(qSub, sizeof qSub, TR("%.1f km %s of it"), d / 1000.0, TR(dir));
                else snprintf(qSub, sizeof qSub, TR("%.0f km %s of it"), d / 1000.0, TR(dir));
            } else {
                qName[0] = qSub[0] = 0;
            }
        }
        snprintf(map.place, sizeof map.place, "%s", qName);
        snprintf(map.placeSub, sizeof map.placeSub, "%s", qSub);
    }
    if (mapPanned()) {
        map.panned = true;
        rot = 0.0f;
        map.northDeg = 0.0f;
        map.trackUp = false;
        map.headingDeg = heading;
        map.riderX = 270 - (int)lroundf(mapPanE / mapMpp);
        map.riderY = 430 + (int)lroundf(mapPanN / mapMpp);
    }
    // The farewell frame is the one frame with no "next one".
    //
    // Every rule below exists because another frame is coming: draw only what
    // is already in RAM, and bail the instant a finger lands, because `partial`
    // brings us straight back. Nothing brings the shutdown screen back — it is
    // painted, the rails come down, and it sits on the glass for days. Run it
    // cached-only and it draws whatever the cache happened to hold, which after
    // riding at a close zoom is the tile under the rider and little else; abort
    // it on touch and it stops after one tile, since power-off is REACHED by
    // touching (the dialog) or holding BOOT. That is the whole bug: the farewell
    // asked for the widest zoom and then refused to read the card for it.
    //
    // So: read the card, ignore the finger, and bound it by the clock instead —
    // a power-off must not hang, and tiles come nearest-first, so a deadline
    // costs the far edge rather than the middle.
    static uint32_t frameDeadline;
    // 12 s, not the 4 it was: the widest view is about 2 MB, and the card manages about
    // 400 KB/s, so a cold one takes 5 s and 4 s left only the tiles around the rider.
    frameDeadline = millis() + 12000;
    map.hasMap = map.topo || map_store::coversPosition(lat, lon);

    // With this view's base layer already finished, the tiles have nothing left to
    // give: skip them. Reading them here would be worse than wasted, because the
    // zooms prerendered either side push this view's tiles out of RAM, and a frame
    // that needs none of them would spend seconds on the card fetching them back.
    mapFrameBase = nullptr;
    if (mapCacheOn && !finalFrame) {
        mapCacheAllocate();
        mapFrameBase = mapCache.find(mapKeyFor(map), millis());
    }
    if (mapFrameBase) {
        const MapLayerFacts* facts = cachedFacts(mapFrameBase);
        map.pois = cachedPois(mapFrameBase);
        map.poiCount = facts->poiCount;
        map.featureCount = facts->features;
        map.tilePolys = facts->tilePolys;
        map.projectedTiles = facts->projectedTiles;
        map.wantedTiles = facts->wantedTiles;
        map.parkCount = facts->parks;
        map.waterCount = facts->waters;
        memcpy(map.clsCount, facts->classes, sizeof(facts->classes));
    } else if (map.topo) {
        // DOBA: no vector features under a topo base layer.
        map.features = nullptr; map.featureCount = 0;
        map.water = nullptr; map.waterCount = 0;
        map.parks = nullptr; map.parkCount = 0;
        map.pois = nullptr; map.poiCount = 0;
    } else {
        map_store::renderInto(lat, lon, mapMpp, map.riderX, map.riderY, rot, map,
                              finalFrame
                                  ? []() -> bool { return millis() < frameDeadline; }
                                  : []() -> bool { return !touchIrq && !touchWasDown; },
                              /*cachedOnly=*/finalFrame ? false : !mapFullPass);
    }

    if (routes::active() && routeScreenPts) {
        int n = routes::pointCount();
        if (n > MAX_ROUTE_SCREEN_PTS) n = MAX_ROUTE_SCREEN_PTS;
        for (int i = 0; i < n; ++i) {
            double plat, plon;
            routes::point(i, plat, plon);
            map_tiles::geoToScreen(plat, plon, lat, lon, mapMpp, map.riderX,
                                   map.riderY, rot, routeScreenPts[i * 2],
                                   routeScreenPts[i * 2 + 1]);
        }
        map.route = routeScreenPts;
        map.routePointCount = n;
        map.riddenPointCount = routes::progressIndex() + 1;
        map.showRemaining = true;
        map.remainingKm = routes::remainingKm();
    }

    // Not on the farewell frame: that one stays on the glass for days, long
    // after anyone is still where it would show them.
    if (!finalFrame) buildMapPeers(lat, lon, rot, s.useMiles, map);
}

// The map frame's base layer, from the cache if it can be, kept for next time if not.
void renderMapWithCache(const MapScreenData& map, const RideState& s, uint8_t* fb) {
    const uint32_t now = millis();
    const MapBaseKey key = mapKeyFor(map);
    if (!mapSameView(key, mapViewKey)) {
        mapViewKey = key;
        mapStillSince = now;
    }
    if (!mapCacheOn) {
        ui_render_map(map, s, fb);
        return;
    }
    mapFrameWater = nullptr;
    if (mapFrameBase) {
        mapCacheHits++;
        mapFrameWater = cachedWater(mapFrameBase);
        mapLayerExpand(mapFrameBase, fb);
        ui_render_map(map, s, fb, /*baseDrawn=*/true);
        return;
    }
    mapCacheMisses++;
    // A partial frame is missing tiles, so it is not worth keeping.
    uint8_t* save = map.partial ? nullptr : mapCache.claim(now);
    mapLayerBeingKept = save;
    ui_render_map(map, s, fb, false, save ? mapLayerKeep : nullptr);
    if (save) {
        mapCacheKeepExtras(save, map);
        mapCache.commit(save, key, now);
        mapFrameWater = cachedWater(save);
    }
}

void renderMapScreen(const RideState& s, uint8_t* fb) {
    MapScreenData map = {};
    for (int i = 0; i < 3; ++i) map.stripFields[i] = dash_config::mapField(i);
    uint32_t m0 = millis();
    buildMapScreenData(s, map);
    uint32_t m1 = millis();

    // A finger arrived while this frame was being built. Don't spend the panel
    // on it — one paint is 130-400 ms, and that is 130-400 ms in which nothing
    // acknowledges the touch. Put the LAST PAINTED frame back and return, so
    // refresh() finds nothing changed, skips the panel, and the next thing to
    // reach the glass is the pressed zoom button.
    //
    // Restoring from shadowFb rather than just returning is the whole point: the
    // caller memsets the framebuffer to white before this runs, so returning
    // early leaves a BLANK screen — status bar, data fields and all — with only
    // the acknowledgement drawn on it. shadowFb is by definition what is
    // currently on the glass, so this is a no-op repaint rather than a wipe.
    if ((touchIrq || touchWasDown) && shadowFb) {
        memcpy(fb, shadowFb, fbSize);
        forceDraw = true;
        mapFullPass = true;      // and let the retry read whatever it missed
        if (dbgTiming) diag::log("map: frame dropped for a touch");
        return;
    }
    renderMapWithCache(map, s, fb);
    // ui_render_map redraws the buttons unpressed, so re-assert the held one on
    // top of it. Released only once the frame is complete — a partial frame
    // means tiles are still coming.
    if (zoomHeld) {
        if (map.partial) ui_map_draw_zoom_button(zoomHeld == 1, true, fb);
        else             zoomHeld = 0;
    }
    // Two-pass rendering, and the reason a zoom lands instantly.
    //
    // The FIRST pass after any change draws only tiles already in RAM. Zooming
    // does not change which ground is under the rider — only its scale — so the
    // middle of the map is always already resident and can be re-projected and
    // painted in milliseconds. If that left anything out (the newly exposed
    // outer ring, or a frame a finger interrupted), the frame comes back marked
    // partial and the NEXT one is allowed to read the card.
    //
    // Steady state costs nothing: once everything on screen is cached, the
    // cached-only pass IS the complete frame and there is no second one.
    if (map.partial) { forceDraw = true; mapFullPass = true; }
    else             { mapFullPass = false; }
    if (dbgTiming)
        diag::log("map: project=%lu draw=%lu polys=%d (tiles=%d base=%d) "
                  "cls[maj=%d pri=%d sec=%d ter=%d min=%d path=%d] ntiles=%d/%d%s "
                  "mpp=%d pois=%d/%d at=%.5f,%.5f peers=%d",
                  (unsigned long)(m1 - m0), (unsigned long)(millis() - m1),
                  map.featureCount, map.tilePolys, map.featureCount - map.tilePolys,
                  map.clsCount[0], map.clsCount[1], map.clsCount[2], map.clsCount[3],
                  map.clsCount[4], map.clsCount[5],
                  map.projectedTiles, map.wantedTiles,
                  map.partial ? " cut" : "", (int)mapMpp,
                  map.poiCount, map_tiles::projectStats().poisDropped,
                  map.centerLat, map.centerLon, map.peerCount);
    drawNavBanner(fb);
    drawPauseBanner(s, fb);
    drawMeshNotice(fb);
}

// With the view holding still and nothing else to do, render one zoom either side, one
// per call, so a tap only has to put it on the panel. Runs in the UI task's idle time and
// gives way to a finger, which is checked all the way through.
void mapPrerenderStep() {
    if (!mapCacheOn || screen != SCREEN_MAP || powerOverlay || routes::navPending() ||
        zoomHeld) return;
    const uint32_t now = millis();
    if (now - mapStillSince < kMapStillMs || (int32_t)(now - mapPrerenderNotBefore) < 0) return;
    if (touchIrq || touchWasDown) return;

    for (const float mpp : {mapMpp / 2.0f, mapMpp * 2.0f}) {
        if (mpp < 1.0f || mpp > 32.0f) continue;
        MapBaseKey want = mapViewKey;
        want.mpp = mpp;
        if (mapCache.find(want, now)) continue;
        if (!mapPrerenderFb) return;

        const float liveMpp = mapMpp;
        const bool liveFullPass = mapFullPass;
        mapMpp = mpp;
        mapFullPass = true;      // this one may read the card, and has the time to
        MapScreenData map = {};
        const RideState s = g_state.snapshot();
        buildMapScreenData(s, map);
        mapMpp = liveMpp;
        mapFullPass = liveFullPass;

        // The bike can have moved since mapViewKey, onto a layer that is already kept.
        // That frame has its counts but no features, so there is nothing here to draw.
        if (mapFrameBase) return;
        // Only now: a claim gives up a slot's layer, and that is worth doing for a
        // frame that is going to be drawn.
        uint8_t* px = mapCache.claim(millis());
        if (!px) return;

        memset(mapPrerenderFb, 0xFF, fbSize);
        const bool done = !map.partial && ui_render_map_base(
            map, mapPrerenderFb, []() -> bool { return !touchIrq && !touchWasDown; });
        if (done) {
            mapLayerPack(mapPrerenderFb, px);
            mapCacheKeepExtras(px, map);
            mapCache.commit(px, mapKeyFor(map), millis());
            mapPrerenders++;
        } else {
            mapPrerendersCut++;
            mapPrerenderNotBefore = millis() + kMapPrerenderRetryMs;
        }
        return;
    }
}

// One full map frame through the cache, as the UI task would make it.
void mapTestFrame(const RideState& s, MapScreenData& map, uint8_t* fb) {
    map = MapScreenData();
    for (int i = 0; i < 3; ++i) map.stripFields[i] = dash_config::mapField(i);
    memset(fb, 0xFF, fbSize);
    mapFullPass = true;
    buildMapScreenData(s, map);
    renderMapWithCache(map, s, fb);
    vTaskDelay(1);
}

// Console `maptest`: for each heading in track-up, a frame drawn from scratch must match
// the one drawn from its cached layer byte for byte, a turn within tolerance must reuse
// the layer, and one past it must not. Runs at the current zoom, on real tiles.
void mapSelfTest() {
    mapCacheAllocate();
    uint8_t* fresh = epdc_framebuffer();
    uint8_t* cached = mapPrerenderFb;
    if (!mapCacheOn || !cached) {
        Serial.println("[maptest] needs mapcache on");
        return;
    }
    const bool liveTrackUp = mapTrackUp;
    const float liveHeading = mapTestHeading;
    mapTrackUp = true;
    const RideState s = g_state.snapshot();
    MapScreenData map = {};
    int failed = 0;
    for (const float heading : {0.f, 1.f, 30.f, 45.f, 89.f, 90.f, 135.f, 180.f, 225.f, 270.f,
                                315.f, 359.f}) {
        mapCache.clear();
        mapTestHeading = heading;
        const uint32_t t0 = millis();
        mapTestFrame(s, map, fresh);
        const uint32_t freshMs = millis() - t0;
        const bool cold = mapFrameBase == nullptr;
        const bool partial = map.partial;
        const int tiles = map.projectedTiles, wanted = map.wantedTiles;

        const uint32_t t1 = millis();
        mapTestFrame(s, map, cached);
        const uint32_t cachedMs = millis() - t1;
        const bool sameHit = mapFrameBase != nullptr;
        size_t differ = 0;
        for (size_t i = 0; i < fbSize; ++i) differ += fresh[i] != cached[i];

        // 360 is not a heading anything produces, but wrapping past north must still hit.
        mapTestHeading = fmodf(heading + 1.0f, 360.0f);
        mapTestFrame(s, map, cached);
        const bool nearHit = mapFrameBase != nullptr;
        mapTestHeading = fmodf(heading + 3.0f, 360.0f);
        mapTestFrame(s, map, cached);
        const bool farHit = mapFrameBase != nullptr;

        const bool ok = cold && !partial && sameHit && differ == 0 && nearHit && !farHit;
        failed += !ok;
        Serial.printf("[maptest] %s heading %3.0f: tiles %d/%d, %lu ms fresh, %lu ms cached, "
                      "%u bytes differ, +1 deg %s, +3 deg %s%s\n",
                      ok ? "ok  " : "FAIL", heading, tiles, wanted, (unsigned long)freshMs,
                      (unsigned long)cachedMs, (unsigned)differ, nearHit ? "reused" : "redrawn",
                      farHit ? "reused" : "redrawn", partial ? " PARTIAL" : "");
    }
    mapTrackUp = liveTrackUp;
    mapTestHeading = liveHeading;
    mapFullPass = false;
    mapCache.clear();
    forceDraw = true;
    Serial.printf("[maptest] %s at %d m/px\n", failed ? "FAILED" : "all passed", (int)mapMpp);
}

// Console `farewell`: the power-off screen without the power-off, held for a look.
void farewellTest() {
    const float liveMpp = mapMpp;
    const bool liveTrackUp = mapTrackUp;
    uint8_t* fb = epdc_framebuffer();
    renderFarewell(fb);
    if (shadowFb) memcpy(shadowFb, fb, fbSize);
    epdc_clear();
    epdc_paint();
    epdc_paint_wait();
    vTaskDelay(pdMS_TO_TICKS(30000));
    mapMpp = liveMpp;
    mapTrackUp = liveTrackUp;
    forceDraw = true;
}

// Turn-by-turn banner across the top, shown on the map AND the dashboard while
// navigating. Tapping it (kNavBanner) opens the directions list.
//
// Units come from the shared state, the same source the directions list reads,
// rather than settings:: directly. Both stay in sync today (the settings screen
// and the phone's BLE settings write both), but reading one fact from two
// places is what made the list and the banner disagree in the first place.
void drawNavBanner(uint8_t* fb, float onlyWithinM) {
    if (!routes::navActive()) return;
    char instr[routes::MANEUVER_TEXT];
    float dist = 0;
    if (routes::nextTurn(instr, sizeof(instr), dist)) {
        if (onlyWithinM > 0 && dist > onlyWithinM) return;
        ui_render_nav_banner(instr, dist, g_state.snapshot().useMiles, fb);
        bandDrawnThisFrame = true;
    }
}

// Auto-pause banner in the same band, on the same two screens. The turn banner
// wins when both apply — a rider stopped AT a turn needs the instruction, and
// the status bar's PAUSED chip still covers the pause.
void drawPauseBanner(const RideState& s, uint8_t* fb) {
    if (routes::navActive() || !s.ridePaused) return;
    ui_render_pause_banner(fb);
    bandDrawnThisFrame = true;
}

// A private-channel message, over whatever else is in the band for its ten
// seconds (except a turn that is close — see meshNoticeShown).
void drawMeshNotice(uint8_t* fb) {
    if (!meshNoticeShown()) return;
    ui_render_mesh_notice(notice.from, notice.channelName, notice.text, fb);
    bandDrawnThisFrame = true;
}





// Keep the page in range when the list shrinks under it (a route cleared,
// a REFRESH that found fewer files).
void clampListPage() {
    const int last = listTotal > 0 ? (listTotal - 1) / kMenuRowCount : 0;
    if (listPage > last) listPage = last;
    if (listPage < 0) listPage = 0;
}

void renderListScreen(uint8_t* fb) {
    ListRow rows[kMenuRowCount] = {};
    int count = 0;
    const char* title = "";
    const char* footer = "";
    bool paged = false;   // Navigate / Ride History: rows + ui_render_pager

    switch (screen) {
        case SCREEN_ROUTES: {
            title = "ROUTES";
            paged = true;
            const bool sd = ride_recorder::sdMounted();
            footer = sd ? "put .gpx files in /routes on the SD card"
                        : "no SD card";
            if (!routeFiles)
                routeFiles = (char(*)[routes::MAX_NAME])heap_caps_malloc(
                    kRouteListMax * routes::MAX_NAME, MALLOC_CAP_SPIRAM);
            static bool listedWithCard = false;
            if (listStale || sd != listedWithCard) {
                routeFileCount =
                    sd && routeFiles ? routes::list(routeFiles, kRouteListMax)
                                     : 0;
                listedWithCard = sd;
                listStale = false;
            }
            // The active route is a pinned "Clear route" row ahead of the
            // files, so it leads page 1.
            listPinned = routes::active() ? 1 : 0;
            listTotal = listPinned + routeFileCount;
            clampListPage();
            for (int i = listPage * kMenuRowCount;
                 i < listTotal && count < kMenuRowCount; ++i, ++count) {
                ListRow& r = rows[count];
                if (i < listPinned) {
                    snprintf(r.title, sizeof(r.title), "Clear route");
                    snprintf(r.subtitle, sizeof(r.subtitle),
                             "%s · %.1f %s left", routes::activeName(),
                             units::dist(routes::remainingKm(),
                                         settings::useMiles()),
                             settings::useMiles() ? "mi" : "km");
                    r.inverted = true;
                } else {
                    snprintf(r.title, sizeof(r.title), "%s",
                             routeFiles[i - listPinned]);
                    snprintf(r.subtitle, sizeof(r.subtitle),
                             "tap to follow this route");
                }
            }
            break;
        }
        case SCREEN_DIRECTIONS: {
            title = "DIRECTIONS";
            footer = routes::navActive() ? "distances update as you move"
                                         : "navigation has ended";
            int n = routes::upcomingCount();
            for (int i = 0; i < n && count < (int)(sizeof(rows) / sizeof(rows[0]));
                 ++i) {
                char instr[routes::MANEUVER_TEXT];
                float dm = 0;
                if (!routes::upcomingTurn(i, instr, sizeof(instr), dm)) break;
                snprintf(rows[count].title, sizeof(rows[0].title), "%s", instr);
                RideState s = g_state.snapshot();
                char dtxt[16];
                units::navDist(dtxt, sizeof(dtxt), dm, s.useMiles);
                snprintf(rows[count].subtitle, sizeof(rows[0].subtitle),
                         "in %s", dtxt);
                rows[count].inverted = (i == 0);   // the turn you are riding to
                count++;
            }
            if (count == 0) {
                snprintf(rows[count].title, sizeof(rows[0].title), "No turns ahead");
                snprintf(rows[count].subtitle, sizeof(rows[0].subtitle),
                         "you are on the last leg");
                count++;
            }
            break;
        }
        case SCREEN_HISTORY: {
            title = "TRACKS";
            footer = "tracks are .fit files in /rides on the SD card";
            paged = true;
            const bool sd = ride_recorder::sdMounted();
            if (!rideFiles)
                rideFiles = (ride_recorder::RideFile*)heap_caps_malloc(
                    kRideListMax * sizeof(ride_recorder::RideFile),
                    MALLOC_CAP_SPIRAM);

            // A ride in progress gets a pinned row at the top; listRides()
            // leaves its still-open, unfinalized FIT file out. Starting or
            // finishing one changes the files, so it re-reads the card too.
            const char* activeFile = ride_recorder::currentRideFile();
            static char listedActive[sizeof(ride_recorder::RideFile::name)];
            static bool listedWithCard = false;
            if (listStale || sd != listedWithCard ||
                strcmp(activeFile, listedActive) != 0) {
                rideFileCount =
                    sd && rideFiles
                        ? ride_recorder::listRides(rideFiles, kRideListMax)
                        : 0;
                snprintf(listedActive, sizeof(listedActive), "%s", activeFile);
                listedWithCard = sd;
                listStale = false;
            }
            listPinned = activeFile[0] ? 1 : 0;
            listTotal = listPinned + rideFileCount;
            clampListPage();
            for (int i = listPage * kMenuRowCount;
                 i < listTotal && count < kMenuRowCount; ++i, ++count) {
                ListRow& r = rows[count];
                if (i < listPinned) {
                    RideState s = g_state.snapshot();
                    snprintf(r.title, sizeof(r.title), "Recording");   // DOBA: fits the mono face
                    uint32_t secs = s.elapsedS;
                    snprintf(r.subtitle, sizeof(r.subtitle),
                             "%.1f %s · %lu:%02lu:%02lu",
                             units::distM(s.distanceM, s.useMiles),
                             units::distLabel(s.useMiles),
                             (unsigned long)(secs / 3600),
                             (unsigned long)(secs / 60 % 60),
                             (unsigned long)(secs % 60));
                    r.inverted = true;
                } else {
                    const ride_recorder::RideFile& f = rideFiles[i - listPinned];
                    snprintf(r.title, sizeof(r.title), "%s", f.name);
                    snprintf(r.subtitle, sizeof(r.subtitle), "%lu KB",
                             (unsigned long)(f.size / 1024));
                }
            }
            break;
        }
        default:
            break;
    }
    // Directions rows ARE turns, so each one leads with its maneuver arrow.
    ui_render_list(title, rows, count, footer, fb, screen == SCREEN_DIRECTIONS,
                   paged);
    if (paged)
        ui_render_pager(listPage * kMenuRowCount, count, listTotal, "REFRESH",
                        fb);
}

}  // namespace

// Boot progress trace. ui_dashboard::begin() drives the panel through a lot of
// steps that only ever ran once epdc_begin() started succeeding, and a hang in
// any of them stops setup() before the tasks are created — which looks, from the
// outside, exactly like a dead device: blank screen and total serial silence.
// The last line printed says where it stopped.
//
// Gated with the rest of the bring-up aids: this is diagnostic scaffolding, and
// the shipping t5s3-pro build (the one CI releases) should not carry it.
#ifdef EPDC_BOOT_WAIT
#define EPDC_STEP(msg) Serial.printf("[ui] %lu ms: %s\n", (unsigned long)millis(), msg)
#else
#define EPDC_STEP(msg) ((void)0)
#endif


namespace ui_dashboard {

// Boot progress state. Kept tiny and static — this runs before any allocation
// we control, and a boot screen that can fail to allocate is worse than none.
namespace {
const char* bootLines[16];
int8_t      bootState[16];
// Per-step detail ("30436 MB free", "CASIC", "107 tiles") and the millis() at
// which it resolved — the boot log shows both.
char        bootDetail[16][28];
uint32_t    bootMs[16];
// Detail staged for the NEXT status line, so a resolve can print name,
// finding and result together on one new line.
char        bootPending[28] = "";
int         bootCount = 0;
bool        bootScreenLive = false;
}

// Panel-only bring-up, split out of begin() so setup() can show progress while
// the slow subsystems (SD, GPS, BLE) start. Everything here is display/input;
// nothing touches the SD card or the radios.
bool beginPanel() {
    // Panel up. Board config, VCOM, waveform tables and rotation all live inside
    // the compat layer now (see epd_compat.cpp) — including the NVS-stored
    // per-board tuning EPD_Painter loads at the end of its own begin().
    if (!epdc_begin()) {
        Serial.println("[ui] display init FAILED");
        return false;
    }

    EPDC_STEP("panel up");

    if (settings::panelScan()) epdc_scan_enable(true);

    // Backlight — level set in Settings, applied here at boot.
    pinMode(BOARD_BL_EN, OUTPUT);
    applyBacklight(settings::backlight());

    // Deep-sleep shutdown latches the touch RST pin LOW (gpio_hold) to keep
    // the GT911 in reset. Release that hold on boot, or after a wake the
    // controller stays reset and touch never works (can't reach map/settings).
    gpio_hold_dis((gpio_num_t)BOARD_TOUCH_RST);
    gpio_deep_sleep_hold_dis();

    EPDC_STEP("backlight");

    touch.setPins(BOARD_TOUCH_RST, BOARD_TOUCH_INT);
    touchOk = touch.begin(Wire, GT911_SLAVE_ADDRESS_L, BOARD_SDA, BOARD_SCL);
    if (!touchOk) Serial.println("[ui] GT911 touch not found");

    EPDC_STEP("touch");

    // Interrupt-drive the inputs. The GT911 pulses its INT on a touch event;
    // the XL9555 pulls its INT low when a button changes; BOOT is a plain
    // GPIO edge. ISRs just set a flag that the task acts on.
    // Must exist before any ISR can fire (they no-op on null, but the task
    // blocks on it immediately).
    uiWake = xSemaphoreCreateBinary();

    if (touchOk) {
        g_touchQ = xQueueCreate(8, sizeof(TouchEv));
        xTaskCreatePinnedToCore(touchTask, "touch", 3072, nullptr, 3, &g_touchTask, 0);
    }
    attachInterrupt(digitalPinToInterrupt(BOARD_TOUCH_INT), onTouchIrq, FALLING);
    attachInterrupt(digitalPinToInterrupt(BOARD_BOOT_BTN), onBoardBtnIrq, CHANGE);
    pinMode(BOARD_PCA9535_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BOARD_PCA9535_INT), onBoardBtnIrq,
                    FALLING);

    // The capacitive Home button below the display is a GT911 key, reported
    // via this callback during touch.getPoint(). It navigates back.
    touch.setHomeButtonCallback([](void*) { homeKeyPressed = true; }, nullptr);

    // Scrub before the first paint. E-paper holds its image with no power, so
    // what is physically on the glass right now is the shutdown screen from the
    // last ride (or another firmware's screen entirely) while the driver's record
    // says white. One pass did not shift it on hardware; four does.
    EPDC_STEP("about to clear x4");
    epdc_clear(4);
    EPDC_STEP("cleared");

    fbSize = epd_width() / 2 * epd_height();
    shadowFb = (uint8_t*)heap_caps_malloc(fbSize, MALLOC_CAP_SPIRAM);

    routeScreenPts = (int16_t*)heap_caps_malloc(
        MAX_ROUTE_SCREEN_PTS * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);

    EPDC_STEP("buffers");
    bootScreenLive = true;
    bootStatus("Display", true);
    return true;
}

namespace {
// Repaint the boot screen from the current step table. Safe before beginPanel()
// (no framebuffer yet) and after the dashboard takes over (bootScreenLive).
void bootRepaint() {
    if (!bootScreenLive) return;
    uint8_t* fb = epdc_framebuffer();
    if (!fb) return;
    EPDC_STEP("bootRepaint: render");
    ui_render_boot_screen(FIRMWARE_VERSION, bootLines, bootState, bootDetail,
                          bootMs, bootCount, fb);
    EPDC_STEP("bootRepaint: paint");
    epdc_paint();
    EPDC_STEP("bootRepaint: wait");
    // WAIT for the rows to finish clocking out. epdc_paint() is asynchronous on
    // the EPD_Painter backend — it hands the frame to the driver's paint task
    // and returns mid-drive — so leaving one in flight means the NEXT paint is
    // issued into a panel that is still being driven, and the driver wedges:
    // the glass keeps whatever was on it and never updates again.
    //
    // This bit when the "Maps" step moved the last boot repaint from main.cpp
    // (where ~3 s of SD scan plus the whole BLE init happened to follow it, so
    // the paint always landed first) to the end of ui_dashboard::begin(), a few
    // hundred ms before the UI task's first frame. The panel then froze on the
    // boot screen for the rest of the session while the firmware ran on
    // perfectly happily behind it — console answering, GPS logging, 140 s uptime.
    //
    // Same rule shutdownDevice() already follows for the farewell screen. It
    // belongs here rather than at the handover because EVERY boot repaint is a
    // full-screen paint, and any of them can be the last one before something
    // else drives the panel. Bounded at 6 s inside epdc_paint_wait(), so this
    // cannot turn a slow panel into a hung boot.
    epdc_paint_wait();
    EPDC_STEP("bootRepaint: painted");
    if (shadowFb) memcpy(shadowFb, fb, fbSize);   // keep the delta engine honest
    EPDC_STEP("bootRepaint: done");
}

// Index of `step` in the table, or -1. Compared by pointer first since every
// caller passes the same string literal, with a strcmp fallback.
int bootFind(const char* step) {
    for (int i = 0; i < bootCount; ++i)
        if (bootLines[i] == step || strcmp(bootLines[i], step) == 0) return i;
    return -1;
}
}  // namespace

// Announce a step that is ABOUT to run, so the glass shows what the device is
// working on rather than sitting on the previous step until this one finishes.
// Matters most for the SD mount, which can take seconds and used to look like a
// freeze. Pair every bootStep() with a bootStatus() carrying the same literal.
void bootStep(const char* step) {
    if (!bootScreenLive) return;
    if (bootCount >= (int)(sizeof(bootLines) / sizeof(*bootLines))) return;
    bootLines[bootCount] = step;
    bootState[bootCount] = BOOT_PENDING;
    bootDetail[bootCount][0] = 0;
    bootMs[bootCount] = millis();
    ++bootCount;
    bootRepaint();
}

// Resolve a step to a tick or a cross. Updates the pending line in place when
// bootStep() announced it, otherwise appends a completed one (so callers that
// never announce still work).
void bootStatus(const char* step, bool ok) {
    if (!bootScreenLive) return;
    // APPEND, never rewrite. A log that goes back and edits a line it already
    // printed is not a log — you cannot tell what the device knew and when. The
    // announce line stays as it was; the result gets its own line, carrying
    // whatever detail was staged for it.
    if (bootCount >= (int)(sizeof(bootLines) / sizeof(*bootLines))) return;
    const int i = bootCount++;
    bootLines[i] = step;
    bootState[i] = ok ? BOOT_OK : BOOT_FAIL;
    bootMs[i] = millis();
    snprintf(bootDetail[i], sizeof(bootDetail[i]), "%s", bootPending);
    bootPending[0] = 0;
    bootRepaint();
    // Put the boot log on the CARD, step by step, as soon as there is a card to
    // put it on.
    //
    // Until this existed, a boot that died never left a trace anywhere. The
    // reset reason and the core-dump summary are logged in the first
    // milliseconds of setup(), but: the USB CDC does not exist yet (TinyUSB
    // starts from the UI task, after the SD mount), a CDC with no host attached
    // silently discards writes anyway, and the only caller of flushToSD() was
    // the BLE server task — which is created AFTER this function runs. So the
    // one failure mode you most need a log for was the one that produced none,
    // and diagnosing a boot loop meant guessing. No-ops before the card mounts
    // and while a ride is recording.
    diag::flushToSD();
}

// Attach detail to a step already announced ("30436 MB free", "CASIC"), so the
// boot log reports what each subsystem actually found rather than only that it
// was looked at.
void bootDetailFor(const char* step, const char* fmt, ...) {
    (void)step;   // staged for the next status line, not attached to a past one
    if (!bootScreenLive) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(bootPending, sizeof(bootPending), fmt, ap);
    va_end(ap);
}

bool begin() {
    // Load the best downloaded map for our last-known spot, else the embedded
    // default. New maps arrive from the phone over BLE (see map_store).
    //
    // ANNOUNCED, like the SD mount and for the same reason: indexing the tiles
    // on the card is the longest thing left in setup() — ~4 s with 107 tiles,
    // and it grows with the card — and it used to run with the glass showing a
    // fully ticked list and an unmoving bar. Nothing said the device was still
    // working, so the last four seconds of every boot looked like a hang.
    // Read the rider's dashboard layout off the card before the first frame, so
    // the panel never flashes the default and then rearrange itself.
    dash_config::begin();

    bootStep("Maps");
    double mlat = DEFAULT_MAP_LAT, mlon = DEFAULT_MAP_LON;
    settings::lastPosition(mlat, mlon);
    map_store::begin(mlat, mlon);
    EPDC_STEP("map_store::begin returned");
    // NO tile count here. Counting means walking every tile directory on the
    // card, and this is the boot path — the exact place map_store::tileCount()
    // documents itself as never belonging. Having it here (twice, once per
    // argument) put a full card walk back into boot immediately after the scan
    // it was meant to have removed, which is what kept the boot loop alive.
    bootDetailFor("Maps", "ready");
    EPDC_STEP("bootDetailFor done");
    bootStatus("Maps", true);
    EPDC_STEP("bootStatus done");
    EPDC_STEP("map loaded — begin() done");
    // Hand the panel over to the dashboard: further bootStatus() calls no-op,
    // and the task's first refresh() paints the real UI over the boot screen.
    bootScreenLive = false;
    // The boot screen has sat there for seconds, in greys, and shows through a scrub.
    // A boot already flashes, so one more costs nothing.
    if (epdc_scan_enabled()) epdc_clear();
    return true;
}

// Flash a firmware.bin dropped on the SD card, then reboot. Reads straight from
// SD into the flash writer — no PSRAM staging (unlike the BLE OTA). ESP32 A/B
// OTA partitions still protect the running image if the flash fails. To update:
// copy the new firmware.bin to the SD card root and power the device on.
void applySdUpdate() {
    if (!ride_recorder::sdMounted()) return;

    // Locked for the whole check-rename-read sequence. This runs on the UI task
    // with the mesh task already live on the shared SPI bus; the previous
    // unlocked SD.exists() here could interleave with a LoRa transfer and wedge
    // the card mid-command — the silent boot freeze right after "Maps ready".
    // Holding the lock across the flash also keeps every other card user off
    // the bus while the firmware is being read, which is what you want anyway.
    sdLock();
    if (!SD.exists("/firmware.bin")) { sdUnlock(); return; }

    // Rename first so a bad/interrupted flash can never boot-loop re-trying it.
    SD.remove("/firmware.applied");
    SD.rename("/firmware.bin", "/firmware.applied");
    File f = SD.open("/firmware.applied", FILE_READ);
    if (!f) { sdUnlock(); return; }
    size_t size = f.size();
    if (size < 4096) { f.close(); sdUnlock(); return; }

    auto drawProgress = [&](int pct, bool first) {
        uint8_t* fb = epdc_framebuffer();
        memset(fb, 0xFF, fbSize);
        ui_render_update_overlay("Installing", pct, fb);
        refresh(first, !first, false);   // GL16 on first frame, fast DU for progress
    };
    drawProgress(0, true);
    diag::log("sd update: flashing %u bytes from SD", (unsigned)size);

    bool ok = false;
    if (Update.begin(size)) {
        uint8_t buf[4096];
        size_t written = 0;
        int lastPct = -10;
        while (written < size) {
            size_t n = f.read(buf, sizeof(buf));
            if (n == 0) break;
            if (Update.write(buf, n) != n) break;
            written += n;
            int pct = (int)(written * 100 / size);
            if (pct - lastPct >= 10) { lastPct = pct; drawProgress(pct, false); }
        }
        ok = (written == size) && Update.end(true);
    }
    f.close();
    if (ok) {
        SD.remove("/firmware.applied");   // done — don't keep it around
        diag::log("sd update OK — rebooting into new firmware");
        diag::flushToSD();   // takes sdLock recursively — same task, fine
        drawProgress(100, false);
        epdc_paint_wait();   // same async-paint hazard as shutdownDevice(): the
                             // delay below is a guess, not a completion signal
        delay(600);
        esp_restart();
    } else {
        // Leave /firmware.applied (not /firmware.bin) so it isn't retried; the
        // device keeps running its current, untouched image.
        diag::log("sd update FAILED (err %d) — keeping current firmware",
                  Update.getError());
    }
    sdUnlock();
}

// Reboot into the ROM serial bootloader (download mode) for hands-free
// reflashing. On the S3, usb_persist_restart(RESTART_BOOTLOADER) calls
// usb_switch_to_cdc_jtag() before restarting, so the board comes back on the
// USB-Serial-JTAG peripheral rather than TinyUSB: a *different* USB device on a
// different port (…usbmodem2101, product "USB JTAG/serial debug unit"). That is
// the point — unlike our OTG port, that bridge has DTR/RTS wired to GPIO0/EN,
// so the host can reset us back into the app afterwards with no button press.
// tools/flash.py drives both halves; `pio run -t upload` goes through it.
static void rebootToBootloader() {
    Serial.println("[cmd] entering download mode — flash now");
    Serial.flush();
    delay(100);
    usb_persist_restart(RESTART_BOOTLOADER);
}

// After an `agnss <n>` command, the next n bytes on the console are raw AGNSS
// ephemeris (not console text) piped straight into the GPS inject stream.
static long agnssRxRemaining = 0;

// Console word -> sensor kind. KIND_ALL for "all", KIND_BAD for anything else
// (which the caller then treats as a MAC address).
constexpr int KIND_ALL = -1, KIND_BAD = -2;

// `sensors` — the saved MAC per kind, the MAC of each live link, and every
// device seen this session. The Sensors screen labels a row with the sensor's
// name (upgraded to the DIS "Manufacturer Model" on first connect and kept in
// NVS), so two identical straps are indistinguishable there: this is the only
// place you can check that the thing feeding the dashboard is actually yours.
// Everything the mesh node knows about itself, its neighbours and its traffic.
//
// Exists because the failure modes here are all silent: a node on the wrong
// frequency slot, the wrong channel key or a dead SX1262 all present the same
// way — nothing arrives — and the counters are the only thing that tells those
// apart. rxOther climbing with rx flat means the band is busy but nothing is on
// OUR channel (wrong name or key). Everything flat means the radio is not
// hearing at all.
static void printMeshReport() {
    Serial.printf("[mesh] radio %s, messaging %s\n",
                  mesh_service::radioOk() ? "UP" : "DOWN",
                  mesh_service::enabled() ? "on" : "OFF");
    char id[16];
    mesh::nodeIdString(mesh_service::nodeNum(), id, sizeof(id));
    Serial.printf("[mesh] node %s '%s' (%s)\n", id, mesh_service::longName(),
                  mesh_service::shortName());
    {
        // Compare with the "Public Key" a Meshtastic app shows for this node.
        uint8_t pub[32];
        char b64[48];
        if (mesh_service::publicKey(pub) && mesh::base64(pub, sizeof(pub), b64, sizeof(b64)))
            Serial.printf("[mesh] public key %s\n", b64);
        else
            Serial.printf("[mesh] public key not loaded yet\n");
    }
    uint8_t psk[16];
    mesh::defaultPsk(mesh_service::channelPskIndex(), psk);
    // Channel and modem on separate lines because they are separate settings, and
    // matching only one of them is the classic way to end up completely deaf: the
    // channel name decides the frequency, the modem decides whether you can
    // demodulate what arrives there.
    Serial.printf("[mesh] channel '%s'%s key %u -> %.4f MHz, hash 0x%02x\n",
                  mesh_service::channelName(),
                  mesh_service::channelFollowsPreset() ? " (from modem)" : " (custom)",
                  mesh_service::channelPskIndex(), mesh_service::frequencyMHz(),
                  mesh::channelHash(mesh_service::channelName(), psk, sizeof(psk)));
    const mesh::ModemPreset& mp = mesh::preset(mesh_service::presetIndex());
    Serial.printf("[mesh] modem %s: SF%u BW%.0f CR4/%u\n", mp.name, mp.sf, mp.bwKhz,
                  mp.cr);
    // The band, and where in it. Slot numbers are 1-based as Meshtastic shows
    // them; "pinned" means the rider chose the number, otherwise the name did.
    const mesh::Region& rg = mesh::region(mesh_service::regionIndex());
    Serial.printf("[mesh] region %s %.3f-%.3f MHz, slot %u/%u%s, %d dBm (limit %d)\n",
                  rg.name, rg.startMHz, rg.endMHz,
                  (unsigned)mesh_service::activeSlot(),
                  (unsigned)mesh_service::slotCount(),
                  mesh_service::freqSlot() ? " pinned" : "",
                  mesh_service::txPowerDbm(), mesh_service::txPowerLimitDbm());

    // Every channel we can decrypt. All share the frequency above — only the
    // primary's name decides that; the rest are told apart by their hash.
    for (int i = 0; i < mesh::MAX_CHANNELS; ++i) {
        mesh_service::ChannelInfo ci;
        if (!mesh_service::channelAt(i, ci)) continue;
        char keydesc[24];
        if (ci.pskLen == 0)      snprintf(keydesc, sizeof(keydesc), "unencrypted");
        else if (ci.pskLen == 1) snprintf(keydesc, sizeof(keydesc), "well-known #%u",
                                          ci.psk[0]);
        else                     snprintf(keydesc, sizeof(keydesc), "%u-byte key",
                                          (unsigned)ci.pskLen);
        Serial.printf("[mesh] ch%d %-14s hash 0x%02x  %s%s\n", i,
                      ci.name[0] ? ci.name : "(unnamed)", ci.hash, keydesc,
                      i == 0 ? "  <- primary, sets the frequency" : "");
    }

    mesh_service::Stats s = mesh_service::stats();
    Serial.printf("[mesh] rx=%u dropped=%u rxOther=%u dup=%u | tx=%u txFail=%u acks=%u\n",
                  (unsigned)s.rx, (unsigned)s.rxDropped,
                  (unsigned)s.rxOtherChannel, (unsigned)s.rxDuplicate,
                  (unsigned)s.tx, (unsigned)s.txFailed, (unsigned)s.acksRx);
    Serial.printf("[mesh] pkiRx=%u pkiFailed=%u nodeInfoReplies=%u\n",
                  (unsigned)s.pkiRx, (unsigned)s.pkiFailed,
                  (unsigned)s.nodeInfoReplies);

    const int nn = mesh_service::nodeCount();
    Serial.printf("[mesh] %d neighbour%s:\n", nn, nn == 1 ? "" : "s");
    for (int i = 0; i < nn; ++i) {
        mesh_service::Node n;
        if (!mesh_service::nodeAt(i, n)) continue;
        mesh::nodeIdString(n.num, id, sizeof(id));
        Serial.printf("  %s %-20s %-5s %4d dBm snr %3d %d hop%s, %lus ago%s\n", id,
                      n.longName[0] ? n.longName : "(no NodeInfo yet)",
                      n.shortName, n.rssi, n.snr, n.hops, n.hops == 1 ? "" : "s",
                      (unsigned long)((millis() - n.lastHeardMs) / 1000),
                      n.hasPublicKey ? "  [key]" : "");
        if (n.hasPosition) {
            Serial.printf("             %.5f,%.5f  %dm  %u sats%s  (%lus ago)\n",
                          n.latitude, n.longitude, (int)n.altitudeM, n.satsInView,
                          n.precisionBits < 32 ? "  IMPRECISE" : "",
                          (unsigned long)((millis() - n.positionMs) / 1000));
        }
    }

    const int mc = mesh_service::messageCount();
    Serial.printf("[mesh] %d message%s (%d unread):\n", mc, mc == 1 ? "" : "s",
                  mesh_service::unreadCount());
    static const char* kTx[] = {"queued", "sent", "ACKED", "FAILED"};
    for (int i = 0; i < mc; ++i) {
        mesh_service::Message m;
        if (!mesh_service::messageAt(i, m)) continue;
        mesh::nodeIdString(m.outgoing ? m.to : m.from, id, sizeof(id));
        Serial.printf("  %s %s %-9s %lus ago: %s\n", m.outgoing ? "->" : "<-", id,
                      m.outgoing ? kTx[m.status & 3] : "",
                      (unsigned long)((millis() - m.ms) / 1000), m.text);
    }
}


static void printConsoleHelp() {
    Serial.println("commands:");
    Serial.println("  help                 this list");
    Serial.println("  cold [unaided]       GPS cold-start test (re-seed unless 'unaided')");
    Serial.println("  gpsoff <sec>         cut GPS power for N s, then re-seed (retention test)");
    Serial.println("  gpsver               query GPS module firmware version");
    Serial.println("  gpsraw <on|off>      echo raw receiver bytes");
    Serial.println("  sd                   SD mount/USB ownership state (see diag for failures)");
    Serial.println("  sdtest [seconds|status] SD/sleep/phone verification, 30..1800s (default 180)");
    Serial.println("  crashlog [panic]     report status; panic deliberately reboots (no active ride)");
    Serial.println("  memfault [export]    offline crash status or non-destructive SDK chunk export");
    Serial.println("  diag                dump retained boot/recent/flash diagnostics (no SD needed)");
    Serial.println("  diag sd [file] [bytes] [offset]  read an SD log over serial (default:");
    Serial.println("                      today's, last 128 KiB; offset reads from there)");
    Serial.println("  diag sd ls          list /logs files and sizes");
    Serial.println("  pm [on|off]         report PM/driver locks, or request light-sleep A/B");
    Serial.println("  bleinterval [status|fast]  measured link or transient fast request");
    Serial.println("  autosleep [on|off]   idle auto-shutdown, this boot only (default on)");
    Serial.println("  usbdrive [on|off]    expose the SD to a host; off takes the card back");
    Serial.println("  power                battery voltage + draw (mA) + full fuel-gauge state");
    Serial.println("  aux                  optional Qwiic sensors: baro / accel / compass");
    Serial.println("  sensors              paired/connected MACs + everything seen this session");
    Serial.println("  mesh                 node state, neighbours, messages, counters");
    Serial.println("  mesh send <text>     broadcast a message to the channel");
    Serial.println("  mesh preset [name]   list / set the modem preset (how fast)");
    Serial.println("  mesh rssi            sample the noise floor (is RX alive?)");
    Serial.println("  mesh private <name>  new private channel, random 256-bit key");
    Serial.println("  mesh forget <slot>   delete a private channel");
    Serial.println("  mesh channel <name> [key]   set the channel (where — retunes)");
    Serial.println("  mesh <on|off>        power the LoRa radio");
    Serial.println("  autopause [sec|off]  ride timer pause after N s stopped (0/off disables)");
    Serial.println("  wheel [mm]           speed-sensor wheel circumference (default 2105)");
    Serial.println("  workout <list|load <f>|start|pause|resume|stop|skip|back|goto <n>|ftp [W]>");
    Serial.println("  sensorsleep [on|off] sensor-link sleep experiment, this boot; status without argument");
    Serial.println("  sleepexp [on|off]    re-arm the light-sleep-with-phone experiment (this boot)");
    Serial.println("  scan <on|off>        force the sensor scan (what the Sensors screen does)");
    Serial.println("  disconnect <kind>    drop the link (hr|power|cadence|radar|all); stays paired");
    Serial.println("  forget <kind|mac>    unpair and drop (hr|power|cadence|radar|all|aa:bb:..)");
    Serial.println("  bootloader           reboot into download mode for flashing");
    Serial.println("  reboot               restart the device");
    Serial.println("  timing               toggle frame-timing logs");
    Serial.println("  screen <map|dash|menu|settings|gps>   switch UI screen");
    Serial.println("  zoom <in|out>        map zoom     back   go back");
}

// Dispatch one console line (first word = command, rest = args).
static void runConsoleLine(char* line) {
    char* cmd = strtok(line, " \t");
    if (!cmd) return;
    char* arg = strtok(nullptr, " \t");

    if (!strcasecmp(cmd, "help") || !strcmp(cmd, "?")) {
        printConsoleHelp();
    } else if (!strcasecmp(cmd, "cold")) {
        bool aided = !(arg && !strcasecmp(arg, "unaided"));
        Serial.printf("[cmd] GPS cold-start test (%s)\n", aided ? "aided" : "unaided");
        gps_service::forceColdStart(aided);
    } else if (!strcasecmp(cmd, "gpsoff")) {
        int sec = arg ? atoi(arg) : 5;
        if (sec < 1) sec = 1;
        Serial.printf("[cmd] GPS power off %ds, then re-seed\n", sec);
        gps_service::powerCycleTest(sec * 1000);
    } else if (!strcasecmp(cmd, "gpsver")) {
        Serial.println("[cmd] querying GPS version (watch for $GPTXT)");
        gps_service::queryVersion();
    } else if (!strcasecmp(cmd, "gpssend")) {
        if (!arg) { Serial.println("[cmd] gpssend <nmea-body, no $ or *cksum>"); return; }
        Serial.printf("[cmd] -> $%s\n", arg);
        gps_service::sendNmeaCommand(arg);
    } else if (!strcasecmp(cmd, "gpsraw")) {
        bool on = !(arg && !strcasecmp(arg, "off"));
        gps_service::setRawEcho(on);
        Serial.printf("[cmd] raw GPS echo %s\n", on ? "ON" : "OFF");
    } else if (!strcasecmp(cmd, "agnss")) {
        // Test hook: stream N raw AGNSS ephemeris bytes over serial into the
        // same pipe the BLE path uses. After this line, the next N bytes are
        // taken verbatim (see pollSerialCommands), not parsed as commands.
        long n = arg ? atol(arg) : 0;
        if (n <= 0) { Serial.println("[cmd] agnss <byte-count>, then send raw bytes"); return; }
        agnssRxRemaining = n;
        gps_service::agnssBegin();
        Serial.printf("[cmd] AGNSS: send %ld raw bytes now\n", n);
    } else if (!strcasecmp(cmd, "memfault")) {
        if (!arg || !strcasecmp(arg,"status")) memfault_service::requestStatus();
        else if (!strcasecmp(arg,"export")) memfault_service::requestStatus(true);
        else Serial.println("[memfault] use memfault [status|export]");
    } else if (!strcasecmp(cmd, "crashlog")) {
        if(arg && !strcasecmp(arg,"panic"))crash_report::requestTestPanic();
        else if(!arg || !strcasecmp(arg,"status"))crash_report::requestStatus();
        else Serial.println("[crashlog] use crashlog [status|panic]");
    } else if (!strcasecmp(cmd, "power")) {
        uint16_t mv = 0; int16_t ma = 0;
        if (board_read_power(mv, ma))
            Serial.printf("[power] %umV %dmA (%s)\n", mv, ma,
                          ma < 0 ? "discharging" : "charging/idle");
        else
            Serial.println("[power] fuel gauge init failed at boot "
                           "(raw registers still read below)");
        // Always dump the full gauge state: when the battery percentage is
        // missing, the status words and the raw SOC are the whole diagnosis.
        char rep[200];
        board_gauge_report(rep, sizeof(rep));
        Serial.printf("[gauge] %s\n", rep);
    } else if (!strcasecmp(cmd, "aux")) {
        char line[160];
        aux_sensors::report(line, sizeof(line));
        Serial.printf("[cmd] %s\n", line);
    } else if (!strcasecmp(cmd, "mesh")) {
        if (arg && !strcasecmp(arg, "send")) {
            // The rest of the LINE, not the next token — a test message has
            // spaces in it. strtok with an empty delimiter set returns the
            // remainder, which is exactly what is wanted here.
            char* text = strtok(nullptr, "");
            while (text && *text == ' ') ++text;
            if (!text || !*text) {
                Serial.println("[cmd] mesh send <text>   (broadcasts to the channel)");
                return;
            }
            const uint32_t id = mesh_service::queueText(mesh::BROADCAST_ADDR, text);
            if (id) Serial.printf("[cmd] queued packet 0x%08x: %s\n", (unsigned)id, text);
            else    Serial.println("[cmd] NOT queued (radio off, or outbox full)");
        } else if (arg && !strcasecmp(arg, "tx")) {
            // mesh tx <dest hex|all> <channel> <text>: a message to anyone, on any slot.
            char* d = strtok(nullptr, " \t");
            char* c = strtok(nullptr, " \t");
            char* text = strtok(nullptr, "");
            while (text && *text == ' ') ++text;
            if (!d || !c || !text || !*text) { Serial.println("[cmd] mesh tx <dest hex|all> <channel> <text>"); return; }
            const uint32_t dest = !strcasecmp(d, "all") ? mesh::BROADCAST_ADDR : (uint32_t)strtoul(d, nullptr, 16);
            const uint32_t id = mesh_service::queueText(dest, text, (uint8_t)atoi(c));
            Serial.printf("[cmd] mesh tx id=%u\n", (unsigned)id);
        } else if (arg && !strcasecmp(arg, "preset")) {
            const char* i = strtok(nullptr, " \t");
            if (i) mesh_service::setPreset((uint8_t)atoi(i));
            Serial.printf("[cmd] mesh preset %s\n", mesh_service::presetName());
        } else if (arg && !strcasecmp(arg, "share")) {
            const char* i = strtok(nullptr, " \t");
            const char* on = strtok(nullptr, " \t");
            if (i && on) mesh_service::setSharesLocation((uint8_t)atoi(i), !strcasecmp(on, "on"));
            Serial.printf("[cmd] mesh share %s %s\n", i ? i : "?", on ? on : "?");
        } else if (arg && !strcasecmp(arg, "chan")) {
            // mesh chan <slot> <hex psk> <name, spaces allowed>: a private channel with a given key.
            const char* i = strtok(nullptr, " \t");
            const char* hx = strtok(nullptr, " \t");
            char* nm = strtok(nullptr, "");
            while (nm && *nm == ' ') ++nm;
            uint8_t psk[mesh::MAX_PSK_LEN] = {};
            size_t n = 0;
            for (size_t k = 0; hx && hx[k] && hx[k + 1] && n < sizeof psk; k += 2) {
                char two[3] = {hx[k], hx[k + 1], 0};
                psk[n++] = (uint8_t)strtoul(two, nullptr, 16);
            }
            const bool ok = i && nm && mesh_service::setPrivateChannel((uint8_t)atoi(i), nm, psk, n);
            Serial.printf("[cmd] mesh chan %s\n", ok ? "ok" : "refused");
        } else if (arg && !strcasecmp(arg, "private")) {
            // Create a private channel with a random 256-bit key. The same thing
            // the app's QR flow does, reachable without a phone.
            char* want = strtok(nullptr, " \t");
            if (!want) {
                Serial.println("[cmd] mesh private <name>   (new random 256-bit key)");
                Serial.println("      mesh forget <slot>    (delete one)");
                return;
            }
            const int slot = mesh_service::firstFreeChannel();
            if (slot < 0) { Serial.println("[cmd] no free channel slots"); return; }
            uint8_t key[32];
            for (int i = 0; i < 32; i += 4) {
                const uint32_t r = esp_random();
                memcpy(key + i, &r, 4);
            }
            if (!mesh_service::setPrivateChannel((uint8_t)slot, want, key, sizeof(key))) {
                Serial.println("[cmd] could not create the channel");
                return;
            }
            Serial.printf("[cmd] creating '%s' in slot %d with a random 256-bit key\n",
                          want, slot);
            Serial.println("      share it from the app to let someone join");
        } else if (arg && !strcasecmp(arg, "forget")) {
            char* want = strtok(nullptr, " \t");
            if (!want) { Serial.println("[cmd] mesh forget <slot>"); return; }
            const int slot = atoi(want);
            if (!mesh_service::deletePrivateChannel((uint8_t)slot))
                Serial.println("[cmd] slot 0 is the primary and cannot be forgotten");
            else
                Serial.printf("[cmd] forgetting channel %d and its messages\n", slot);
        } else if (arg && !strcasecmp(arg, "rssi")) {
            // Is the receiver actually running? Counters cannot tell a quiet band
            // from a dead receive chain — both are silence. A live SX1262 in RX
            // reads a noise floor around -95..-125 dBm and moves a few dB between
            // samples; a stuck or absurd value says the radio is not listening.
            if (!mesh_service::radioOk()) {
                Serial.println("[cmd] radio is not up");
                return;
            }
            Serial.printf("[cmd] sampling the noise floor on %.4f MHz...\n",
                          mesh_service::frequencyMHz());
            float lo = 999, hi = -999, sum = 0;
            constexpr int N = 20;
            for (int i = 0; i < N; ++i) {
                const float v = lora_radio::instantRssi();
                if (v < lo) lo = v;
                if (v > hi) hi = v;
                sum += v;
                Serial.printf("  %6.1f dBm\n", v);
                delay(100);
            }
            Serial.printf("[cmd] min %.1f  max %.1f  mean %.1f dBm (spread %.1f)\n",
                          lo, hi, sum / N, hi - lo);
            Serial.println("      a plausible floor that MOVES = receiver listening, "
                           "band simply quiet");
            Serial.println("      stuck or absurd = the receive chain is not running");
        } else if (arg && !strcasecmp(arg, "preset")) {
            char* want = strtok(nullptr, " \t");
            if (!want) {
                Serial.println("[cmd] mesh preset <name>. Available:");
                for (int i = 0; i < mesh::PRESET_COUNT; ++i) {
                    const mesh::ModemPreset& p = mesh::kPresets[i];
                    Serial.printf("  %-11s SF%-2u %3.0f kHz%s\n", p.name, p.sf,
                                  p.bwKhz,
                                  i == mesh_service::presetIndex() ? "  <- current" : "");
                }
                return;
            }
            const int idx = mesh::presetIndexByName(want);
            if (idx < 0) { Serial.printf("[cmd] unknown preset '%s'\n", want); return; }
            mesh_service::setPreset((uint8_t)idx);
        } else if (arg && !strcasecmp(arg, "region")) {
            char* want = strtok(nullptr, " \t");
            if (!want) {
                Serial.println("[cmd] mesh region <name>. Must match BOTH the country");
                Serial.println("      and the LoRa module's hardware band. Available:");
                for (int i = 0; i < mesh::REGION_COUNT; ++i) {
                    const mesh::Region& r = mesh::kRegions[i];
                    Serial.printf("  %-8s %8.3f-%8.3f MHz  limit %2d dBm%s\n", r.name,
                                  r.startMHz, r.endMHz, r.powerLimitDbm,
                                  i == mesh_service::regionIndex() ? "  <- current" : "");
                }
                return;
            }
            const int idx = mesh::regionIndexByName(want);
            if (idx < 0) { Serial.printf("[cmd] unknown region '%s'\n", want); return; }
            mesh_service::setRegion((uint8_t)idx);
        } else if (arg && !strcasecmp(arg, "power")) {
            char* want = strtok(nullptr, " \t");
            if (!want) {
                Serial.printf("[cmd] mesh power <dBm|max>   (now %d, limit %d)\n",
                              mesh_service::txPowerDbm(), mesh_service::txPowerLimitDbm());
                return;
            }
            mesh_service::setTxPower(!strcasecmp(want, "max") ? 0 : (int8_t)atoi(want));
        } else if (arg && !strcasecmp(arg, "slot")) {
            char* want = strtok(nullptr, " \t");
            if (!want) {
                Serial.printf("[cmd] mesh slot <1-%u|auto>   (now %u%s)\n",
                              (unsigned)mesh_service::slotCount(),
                              (unsigned)mesh_service::activeSlot(),
                              mesh_service::freqSlot() ? ", pinned" : ", from the name");
                return;
            }
            mesh_service::setFreqSlot(!strcasecmp(want, "auto") ? 0 : (uint8_t)atoi(want));
        } else if (arg && !strcasecmp(arg, "channel")) {
            char* want = strtok(nullptr, " \t");
            if (!want) {
                Serial.println("[cmd] mesh channel <name|default> [key 1-10]");
                Serial.println("      'default' follows the modem preset, which is");
                Serial.println("      what a stock Meshtastic node does.");
                return;
            }
            // "default" clears the explicit name so the preset supplies it again.
            if (!strcasecmp(want, "default")) want = (char*)"";
            char* key = strtok(nullptr, " \t");
            mesh_service::setChannel(want, key ? (uint8_t)atoi(key)
                                              : mesh_service::channelPskIndex());
        } else if (arg && !strcasecmp(arg, "on")) {
            mesh_service::setEnabled(true);
        } else if (arg && !strcasecmp(arg, "off")) {
            mesh_service::setEnabled(false);
        } else {
            printMeshReport();
        }
    } else if (!strcasecmp(cmd, "maptest")) {
        mapSelfTest();
    } else if (!strcasecmp(cmd, "farewell")) {
        farewellTest();
    } else if (!strcasecmp(cmd, "trackup")) {
        if (arg) mapTrackUp = !strcasecmp(arg, "on");
        forceDraw = true;
        Serial.printf("[trackup] %s\n", mapTrackUp ? "on" : "off");
    } else if (!strcasecmp(cmd, "heading")) {
        mapTestHeading = arg && strcasecmp(arg, "off") ? atof(arg) : -1.0f;
        forceDraw = true;
        Serial.printf("[heading] %.0f\n", mapTestHeading);
    } else if (!strcasecmp(cmd, "mapcache")) {
        if (arg) {
            if (strcasecmp(arg, "on") && strcasecmp(arg, "off")) {
                Serial.println("[cmd] mapcache [on|off]");
                return;
            }
            mapCacheOn = !strcasecmp(arg, "on");
            if (!mapCacheOn) mapCache.clear();
        }
        Serial.printf("[mapcache] %s; %u hits, %u misses, %u prerendered, %u cut short; "
                      "PSRAM free %u KB, largest block %u KB\n",
                      mapCacheOn ? "on" : "off", (unsigned)mapCacheHits,
                      (unsigned)mapCacheMisses, (unsigned)mapPrerenders,
                      (unsigned)mapPrerendersCut,
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                      (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    } else if (!strcasecmp(cmd, "panelscan")) {
        if (arg && !strcasecmp(arg, "tune")) {
            const char* drive = strtok(nullptr, " \t");
            const char* prime = strtok(nullptr, " \t");
            if (!drive || !prime || !epdc_scan_tune(atoi(drive), atoi(prime))) {
                Serial.println("[cmd] panelscan tune <draw passes> <erase prime>, with scan on");
                return;
            }
        } else if (arg) {
            if (strcasecmp(arg, "on") && strcasecmp(arg, "off")) {
                Serial.println("[cmd] panelscan [on|off|tune <draw> <prime>]");
                return;
            }
            const bool on = !strcasecmp(arg, "on");
            if (epdc_scan_enable(on)) settings::setPanelScan(on);
        }
        Serial.printf("[panelscan] %s\n", epdc_scan_enabled() ? "on" : "off");
        epdc_scan_report();
    } else if (!strcasecmp(cmd, "autosleep")) {
        if (arg) {
            if (strtok(nullptr, " \t") ||
                (strcasecmp(arg, "on") && strcasecmp(arg, "off"))) {
                Serial.println("[cmd] autosleep [on|off]");
                return;
            }
            autoSleepEnabled = !strcasecmp(arg, "on");
            // Re-enabling always grants a full idle window, even if the board
            // has already spent hours on the bench with auto-shutdown off.
            if (autoSleepEnabled) lastActivityMs = millis();
        }
        diag::log("autosleep: %s (this boot only; idle timeout %lus; CPU light sleep unchanged)",
                  autoSleepEnabled ? "ON" : "OFF", (unsigned long)(AUTO_SLEEP_MS / 1000));
    } else if (!strcasecmp(cmd, "sdtest")) {
        if (arg && !strcasecmp(arg, "status")) sd_diagnostics::status();
        else {
            char* end = nullptr;
            unsigned long seconds = arg ? strtoul(arg, &end, 10) : 180;
            if (arg && (!*arg || *end || seconds < 30 || seconds > 1800))
                Serial.println("[cmd] sdtest [30..1800|status]");
            else sd_diagnostics::start((unsigned)seconds);
        }
    } else if (!strcasecmp(cmd, "diag")) {
        if (!arg) diag::dumpToSerial();
        else if (!strcasecmp(arg, "sd")) {
            // diag sd [file] [bytes] [offset] | diag sd ls. A first token that
            // isn't a number names a /logs file; no offset means the tail.
            const char* tok = strtok(nullptr, " \t");
            const char* file = nullptr;
            bool bad = false;
            if (tok && !strcasecmp(tok, "ls")) {
                if (strtok(nullptr, " \t")) Serial.println("[cmd] diag sd ls");
                else diag::listSDLogs();
                return;
            }
            if (tok && !isdigit((unsigned char)tok[0])) {
                file = tok;
                bad = !diag::validLogName(file);
                tok = strtok(nullptr, " \t");
            }
            unsigned long bytes = 131072;
            long offset = -1;
            char* end = nullptr;
            if (tok) {
                bytes = strtoul(tok, &end, 10);
                bad |= !*tok || *end || bytes < 256 || bytes > 262144;
                tok = strtok(nullptr, " \t");
            }
            if (tok) {
                unsigned long o = strtoul(tok, &end, 10);
                bad |= !*tok || *end || o > 0x7FFFFFFFul;
                offset = (long)o;
            }
            if (bad || strtok(nullptr, " \t"))
                Serial.println("[cmd] diag sd [file] [256..262144 bytes] [offset] | diag sd ls");
            else diag::dumpSDToSerial(file, offset, (size_t)bytes);
        } else Serial.println("[cmd] diag [sd [file] [bytes] [offset] | sd ls]");
    } else if (!strcasecmp(cmd, "pm")) {
        if (arg && !strcasecmp(arg, "on")) power_mgmt::requestSleep(true);
        else if (arg && !strcasecmp(arg, "off")) power_mgmt::requestSleep(false);
        else if (arg) { Serial.println("[cmd] pm [on|off]"); return; }
        power_mgmt::report();
    } else if (!strcasecmp(cmd, "sd")) {
        // SD.cardType() is cached metadata. After a failed SD.begin() it is
        // NONE regardless of which initialization or filesystem stage failed.
        sdLock();
        uint8_t ct = SD.cardType();
        uint64_t sizeMB = SD.cardSize() / (1024ULL * 1024ULL);
        sdUnlock();
        const char* cn = ct == CARD_NONE ? "NONE" : ct == CARD_MMC ? "MMC"
                       : ct == CARD_SD ? "SD" : ct == CARD_SDHC ? "SDHC" : "UNKNOWN";
        Serial.printf("[sd] mounted=%s cardType=%s size=%lluMB\n",
                      ride_recorder::sdMounted() ? "yes" : "NO", cn, sizeMB);
        // "Not mounted" has two causes that need opposite responses, and the
        // difference is invisible on the device's own screen. Say which it is.
        if (!ride_recorder::sdMounted()) {
            if (usb_storage::hostActive())
                Serial.println("[sd] a USB host owns the card — unplug it, eject it, "
                               "or toggle the USB drive off/on ('usbdrive off' then 'on')");
            else
                Serial.println("[sd] not mounted by the firmware — cardType=NONE means "
                               "the driver cleaned up; use diag for command responses");
        }
        if (ride_recorder::sdMounted()) {
            sdLock();
            uint64_t freeMB = (SD.totalBytes() - SD.usedBytes()) /
                              (1024ULL * 1024ULL);
            sdUnlock();
            Serial.printf("[sd] %llu MB free, log=%s\n", freeMB, diag::logPath());
        }
    } else if (!strcasecmp(cmd, "usbdrive")) {
        // The serial-side escape hatch for a stuck "a USB host owns the card":
        // turning the drive off hands the card straight back to the firmware.
        // Same switch as device Settings, so it persists.
        if (!arg) {
            Serial.printf("[usbdrive] %s (host %s the card)\n",
                          settings::usbDrive() ? "on" : "off",
                          usb_storage::hostActive() ? "HAS" : "does not have");
        } else {
            bool on = !strcasecmp(arg, "on") || !strcasecmp(arg, "1");
            settings::setUsbDrive(on);
            usb_storage::setDriveEnabled(on);
            Serial.printf("[usbdrive] turned %s\n", on ? "on" : "off");
        }
    } else if (!strcasecmp(cmd, "bootloader") || !strcasecmp(cmd, "boot")) {
        rebootToBootloader();
    } else if (!strcasecmp(cmd, "autopause")) {
        if (arg) {
            int sec = !strcasecmp(arg, "off") ? 0 : atoi(arg);
            settings::setAutoPauseSec(sec);
        }
        int cur = settings::autoPauseSec();
        if (cur) Serial.printf("[autopause] after %d s stopped\n", cur);
        else     Serial.println("[autopause] off");
    } else if (!strcasecmp(cmd, "sdwait")) {   // who keeps the card busy while others wait
#ifndef OTP_SIM
        sdWaitReport();
#endif
    } else if (!strcasecmp(cmd, "reboot")) {
        Serial.println("[cmd] rebooting");
        Serial.flush(); delay(80); esp_restart();
    } else if (!strcasecmp(cmd, "timing")) {
        dbgTiming = !dbgTiming;
        diag::log("dbg timing %s", dbgTiming ? "ON" : "OFF");
    } else if (!strcasecmp(cmd, "lang") && arg) {   // DOBA: lang en|uk
        i18n::setLang(!strcasecmp(arg, "uk") ? i18n::UK : i18n::EN);
    } else if (!strcasecmp(cmd, "libopen") && arg) {   // DOBA: libopen <title>, the rest of the line
        char t[160];
        const char* rest = strtok(nullptr, "");
        snprintf(t, sizeof t, "%s%s%s", arg, rest ? " " : "", rest ? rest : "");
        if (doba_lib::openTitle(t)) screen = SCREEN_ARTICLE;
    } else if (!strcasecmp(cmd, "ls") || !strcasecmp(cmd, "cat")) {   // DOBA: ls <dir> / cat <file> on the card
        const bool list = !strcasecmp(cmd, "ls");
        sdLock();
        File f = SD.open(arg ? arg : "/", FILE_READ);
        if (!f) Serial.printf("[%s] cannot open %s\n", cmd, arg ? arg : "/");
        else if (list && f.isDirectory()) {
            for (File e = f.openNextFile(); e; e = f.openNextFile()) {
                Serial.printf("[ls] %s %lu\n", e.name(), (unsigned long)e.size());
                e.close();
            }
        } else {
            uint8_t buf[256];
            size_t total = 0;
            while (total < 64 * 1024) {
                const int n = f.read(buf, sizeof buf);
                if (n <= 0) break;
                Serial.write(buf, n);
                total += n;
            }
            Serial.printf("\n[cat] %u bytes\n", (unsigned)total);
        }
        if (f) f.close();
        sdUnlock();
    } else if (!strcasecmp(cmd, "owner")) {   // DOBA: owner <text>: the IF FOUND card ("owner" alone clears it)
        char t[160] = "";
        const char* rest = arg ? strtok(nullptr, "") : nullptr;
        if (arg) snprintf(t, sizeof t, "%s%s%s", arg, rest ? " " : "", rest ? rest : "");
        // fields split at "|": name|born|blood|medical|contact
        int fi = 0;
        char* p = t;
        for (; fi < settings::OWNER_FIELDS; ++fi) {
            char* bar = p ? strchr(p, '|') : nullptr;
            if (bar) *bar = 0;
            settings::setOwnerField(fi, p ? p : "");
            p = bar ? bar + 1 : nullptr;
        }
        Serial.printf("[cmd] owner set (%s)\n", settings::ownerField(0));
    } else if (!strcasecmp(cmd, "screen")) {
        if (!arg) { Serial.println("[cmd] screen <map|dash|menu|settings|gps|layers>"); return; }
        if      (!strcasecmp(arg, "map"))      screen = SCREEN_MAP;
        else if (!strcasecmp(arg, "dash"))     screen = SCREEN_DASH;
        else if (!strcasecmp(arg, "menu"))     screen = SCREEN_MENU;
        else if (!strcasecmp(arg, "settings")) screen = SCREEN_SETTINGS;
        else if (!strcasecmp(arg, "gps"))      screen = SCREEN_GPSDEBUG;
        else if (!strcasecmp(arg, "layers"))   screen = SCREEN_MAPLAYERS;
        else if (!strcasecmp(arg, "home"))     screen = SCREEN_HOME;      // DOBA
        else if (!strcasecmp(arg, "comms"))    screen = SCREEN_COMMS;     // DOBA
        else if (!strcasecmp(arg, "compose"))  screen = SCREEN_COMPOSE;   // DOBA
        else if (!strcasecmp(arg, "library"))  screen = SCREEN_LIBRARY;   // DOBA
        else if (!strcasecmp(arg, "setup"))    screen = SCREEN_SETUP;     // DOBA
        else if (!strcasecmp(arg, "points"))   screen = SCREEN_POINTS;    // DOBA
        else if (!strcasecmp(arg, "mapopts"))  screen = SCREEN_MAPOPTS;   // DOBA
        else if (!strcasecmp(arg, "sos"))      screen = SCREEN_SOS;       // DOBA
        else if (!strcasecmp(arg, "sun"))      screen = SCREEN_SUN;       // DOBA
        else if (!strcasecmp(arg, "more"))     screen = SCREEN_MORE;      // DOBA
        else if (!strcasecmp(arg, "battery"))  screen = SCREEN_BATTERY;   // DOBA
        else { Serial.printf("[cmd] unknown screen '%s'\n", arg); return; }
        noteActivity();
    } else if (!strcasecmp(cmd, "zoom")) {
        if (arg && !strcasecmp(arg, "in") && mapMpp > 1.0f) mapMpp /= 2.0f;
        else if (arg && !strcasecmp(arg, "out") && mapMpp < mapMaxMpp()) mapMpp *= 2.0f;
        screen = SCREEN_MAP; noteActivity();
    } else if (!strcasecmp(cmd, "greytest")) {   // DOBA: panel grey levels card
        noteActivity();
        greyTestShown = !(arg && !strcasecmp(arg, "off"));
    } else if (!strcasecmp(cmd, "mils")) {   // DOBA: mils 6000 | mils 6400
        if (arg) milspec::setMilsSystem(atoi(arg));
        Serial.printf("[mils] %d\n", milspec::milsSystem());
        screen = SCREEN_MAP; noteActivity();
    } else if (!strcasecmp(cmd, "stealth")) {   // DOBA: stealth on|off|listen|silent
        if (arg && !strcasecmp(arg, "off")) stealth::set(false);
        else if (arg && !strcasecmp(arg, "silent")) { stealth::setListen(false); stealth::set(true); }
        else if (arg && (!strcasecmp(arg, "listen") || !strcasecmp(arg, "on"))) { stealth::setListen(true); stealth::set(true); }
        Serial.printf("[stealth] state %s radio=%d\n", !stealth::active() ? "off" : stealth::listen() ? "listen" : "silent",
                      mesh_service::enabled() ? 1 : 0);
    } else if (!strcasecmp(cmd, "dem")) {    // DOBA: ground under the fix, from /doba/dem
        const RideState st = g_state.snapshot();
        float v;
        bool wet;
        if (doba_dem::ground(st.latitude, st.longitude, v, wet))
            Serial.printf("[dem] %s %.1f\n", wet ? "water" : "land", v);
        else
            Serial.println("[dem] none");
    } else if (!strcasecmp(cmd, "topo")) {   // DOBA: topo layer diagnostics
        const topo::Stats t = topo::stats();
        Serial.printf("[topo] active=%d mpp=%.2f decoded=%lu hits=%lu missing=%lu packs=%lu\n",
                      (int)mapTopoActive, mapMpp, (unsigned long)t.decoded,
                      (unsigned long)t.hits, (unsigned long)t.missing,
                      (unsigned long)t.packsOpened);
    } else if (!strcasecmp(cmd, "back")) {
        goBack(); noteActivity();
    } else {
        Serial.printf("[cmd] unknown '%s' (type 'help')\n", cmd);
    }
}

// Line-based serial console over the USB-CDC port. The one Serial reader in the
// firmware (a second reader would race it for bytes). Commands drive the UI for
// profiling and the flash/GPS iteration loop; see printConsoleHelp().
void pollSerialCommands() {
    static char buf[160];   // DOBA: room for a long article title (libopen)
    static uint8_t n = 0;
    while (Serial.available() > 0) {
        if (agnssRxRemaining > 0) {           // raw AGNSS byte-stream mode
            uint8_t b = (uint8_t)Serial.read();
            gps_service::agnssInject(&b, 1);
            if (--agnssRxRemaining == 0) {
                gps_service::agnssEnd();
                Serial.println("[cmd] AGNSS: all bytes received");
            }
            continue;
        }
        int c = Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            buf[n] = 0;
            if (n) runConsoleLine(buf);
            n = 0;
        } else if (n < sizeof(buf) - 1) {
            buf[n++] = (char)c;
        } else {
            n = 0;   // overflow — drop the line
        }
    }
}

#ifdef DOBA_TRACE
#define TRC(tag) do { static uint8_t n_ = 0; if (n_ < 3) { ++n_; Serial.printf("[trc] %lu %s\n", (unsigned long)millis(), tag); Serial.flush(); } } while (0)
#else
#define TRC(tag) ((void)0)
#endif
void task(void*) {
    uint32_t lastDraw = 0;
    Screen lastScreen = screen;
    bool lastOverlay = false;
    bool lastNavPrompt = false;

    lastActivityMs = millis();
    int lastUpdatePct = -100, lastUpdatePhase = -1;

    // Ride recovery runs HERE, not in setup(). It is unbounded card work — it
    // once ran long enough to trip the interrupt watchdog, and since a reset
    // mid-recovery tears another file, every reboot found more to do than the
    // last. Off the boot path a slow recovery costs time, not a brick.
TRC("enter");     ride_recorder::recoverRides();
    // A ride the reset cut short: put the question on screen before anything
    // else happens to the file.
    if (ride_recorder::pendingRecovery(&pendingSummary)) {
        recoveryPrompt = true;
        screen = SCREEN_SUMMARY;
    }
TRC("recovered");     applySdUpdate();          // flash a firmware.bin from the SD card, if present
TRC("sdupdate");     usb_storage::begin();     // THEN expose the SD to a host computer over USB
TRC("usb");     bool lastHostActive = false;

    for (;;) {
        // When the computer releases the SD (eject/unplug), check whether it
        // dropped a firmware.bin on the card and flash it automatically.
TRC("loop");         bool host = usb_storage::hostActive();
        if (lastHostActive && !host) {
            applySdUpdate();
            map_store::rescanCard(); doba_lib::rescan(); doba_points::rescan(); doba_dem::rescan();  // DOBA   // it may have added/removed maps too
        }
        lastHostActive = host;

        // A card that only mounted after boot had given up: setup() skipped the
        // routes, the map index and the USB drive, and nothing revisited them, so
        // the card stayed mounted-but-unused for the whole session. Run that
        // bring-up now. It happens here, in the UI task, next to the renderer
        // that consumes it.
        //
        // It used to say the map cache was "this task's alone". It never was —
        // the BLE server writes to it on every tile received — and acting on
        // that belief is why nothing locked it. map_store owns the locking now.
        if (ride_recorder::consumeLateMount()) {
            diag::log("sd: late mount — loading routes/maps, exposing USB drive");
            ride_recorder::recoverRides();
            if (ride_recorder::pendingRecovery(&pendingSummary)) {
                recoveryPrompt = true;
                screen = SCREEN_SUMMARY;
            }
            routes::begin();
            map_store::rescanCard(); doba_lib::rescan(); doba_points::rescan(); doba_dem::rescan();  // DOBA
            applySdUpdate();      // a firmware.bin may have been on the card
            usb_storage::begin(); // no-op if it already came up
        }

        // Terrain elevation from the map DEM at the current position.
        // Only the UI task touches the tile cache, so this stays race-free with
        // the map render. The recorder reads mapElevationM to integrate ascent.
        //
        // This is an SD read, and it sits near the top of the loop — ahead of
        // touch handling. While a ride is recording, the recorder task (which
        // runs at a HIGHER priority than the UI) takes sdLock every second to
        // write a FIT record, and every 15 s to flush. A DEM read here then
        // blocks behind it and the rider's tap is not processed until it
        // returns, which is why touch felt sluggish only while recording.
        //
        // So skip it entirely while the rider is interacting, and sample at
        // 2.5 s rather than 1 s otherwise: ascent is integrated with 3 m
        // hysteresis, so a slower sample costs nothing but removes most of the
        // opportunities to collide with the recorder.
        static uint32_t lastElevMs = 0;
        const bool interacting = touchIrq || touchWasDown ||
                                 millis() - lastUiInputMs < 1500;
        if (!interacting && millis() - lastElevMs > 2500 && !host) {
            lastElevMs = millis();
            RideState es = g_state.snapshot();
            double elat = 0, elon = 0;
            bool have = false;
            if (es.gpsFix) { elat = es.latitude; elon = es.longitude; have = true; }
            else if (es.phoneFixValid && millis() - es.phoneFixMs < 15000) {
                elat = es.phoneLat; elon = es.phoneLon; have = true;
            } else if (es.everHadFix) { elat = es.latitude; elon = es.longitude; have = true; }
            if (have) {
                float ev = map_store::elevationAt(elat, elon);
                bool ok = (ev == ev);   // false when NAN
                g_state.with([&](RideState& st) {
                    st.mapElevationValid = ok;
                    if (ok) st.mapElevationM = ev;
                });
            }
        }

        // A firmware update in progress takes over the screen with a progress
        // modal (redrawn only when the percentage/phase moves so the e-paper
        // isn't thrashed). Everything else is paused until it finishes/reboots.

        // Riding counts as activity even without touching the screen — a bike
        // computer must NEVER auto-off while you're moving. Any real speed keeps
        // it awake (and for the idle timeout after you stop).
        {
            RideState now = g_state.snapshot();
            if (now.gpsFix && now.speedKmh > 3.0f) lastActivityMs = millis();
            // And without a fix: an accelerometer knows the bike is moving in a
            // tunnel, under trees, or before the receiver has locked — all
            // places the idle timeout could otherwise switch the device off
            // underneath a rider who is very much still riding.
            if (now.deviceMoving) lastActivityMs = millis();

            // The page on screen, for diagnostics and the simulator.
            LiveView lv;
            lv.pageCount = (unsigned char)dash_config::pageCount();
            if (screen == SCREEN_DASH) {
                lv.page = (unsigned char)dashPage;
                lv.screen = 0;
            } else if (screen == SCREEN_MAP) {
                lv.page = (unsigned char)mapSlot();
                lv.screen = 1;
            }
            live_view::publish(lv);
        }

        // Auto-sleep after a quiet period to save battery — a bike computer
        // left on a desk otherwise burns power on GPS + BLE + CPU. Held off
        // while recording, navigating, moving, a phone is connected, or the
        // stopwatch is running (powering off would throw its time away).
        // Wake with BOOT.
        if (autoSleepEnabled && millis() - lastActivityMs > AUTO_SLEEP_MS &&
            !ride_recorder::isRecording() && !routes::navActive() &&
            // DOBA: a radio that is listening is a pager: switching off would
            // miss the message or the SOS it is on for. (OTP held this for a
            // connected phone; Doba holds it while the mesh is on.)
            !mesh_service::enabled() &&
            // A computer that has the card (copying maps or books) is a user too:
            // sleeping would unmount the card and drop off USB mid-copy.
            !usb_storage::hostActive()) {
            uint8_t* fb = epdc_framebuffer();
            shutdownDevice(fb, "auto-sleep (idle timeout)");   // does not return
        }

        // Apply a backlight level changed from the phone (BLE writes NVS but
        // not the PWM). Also covers the device's own +/- which already applied.
        static int lastBl = -1;
        if (settings::backlight() != lastBl) {
            lastBl = settings::backlight();
            applyBacklight(lastBl);
        }

        // Left-side physical buttons, each with its own debounce/hold state.
        // BOOT (GPIO0): short press starts/stops the ride, hold 1.5 s opens the
        // power dialog. Side key (expander PC12): short press cycles the
        // backlight. Interrupt-driven, with a 200 ms fallback poll; two
        // consecutive LOW reads debounce noise.
        {
            bool irq = boardBtnIrq;
            boardBtnIrq = false;

            static int bootLow = 0;
            static uint32_t bootDownAt = 0, bootPoll = 0;
            static bool bootLong = false;
            if (irq || bootLow > 0 || millis() - bootPoll > 200) {
                bootPoll = millis();
                if (digitalRead(BOARD_BOOT_BTN) == LOW) {
                    if (bootLow < 200) bootLow++;
                    if (bootLow == 1) {            // press start
                        bootDownAt = millis();
                        bootLong = false;
                    } else if (bootLow > 1 && !bootLong && !powerOverlay &&
                               millis() - bootDownAt > 1500) {
                        bootLong = true;
                        Serial.println("[btn] boot: hold -> power sheet");
                        sheetKind = SHEET_POWER;
                        powerOverlay = true;       // hold -> power dialog
                        forceDraw = true;
                    }
                } else {
                    // ONE confirmed LOW read is enough to count as a press.
                    //
                    // It used to need two consecutive lows, and the second poll
                    // only happens on the next pass of this loop — which can be
                    // several hundred ms away while a map frame is projected and
                    // painted. Release before then and the press was discarded
                    // silently, which is why a quick tap sometimes did nothing.
                    // The edge interrupt that woke us is already independent
                    // evidence, so a single debounced read is not noise.
                    // DOBA: no ride recording; BOOT goes HOME.
                    if (bootLow >= 1 && !bootLong) {
                        Serial.println("[btn] boot: short -> home");
                        noteActivity(); screen = SCREEN_HOME; forceDraw = true;
                    }
                    bootLow = 0;
                }
            }

            // DOBA: the side key is the power key. Hold 1.5 s: the power
            // sheet (switch off, restart). Short press: the front light.
            static int sideLow = 0;
            static uint32_t sidePoll = 0, sideDownAt = 0;
            static bool sideLong = false;
            if (irq || sideLow > 0 || millis() - sidePoll > 200) {
                sidePoll = millis();
                if (board_side_button_pressed()) {
                    if (sideLow < 200) sideLow++;
                    if (sideLow == 1) { sideDownAt = millis(); sideLong = false; }
                    else if (!sideLong && !powerOverlay && millis() - sideDownAt > 1500) {
                        sideLong = true;
                        Serial.println("[btn] side: hold -> power sheet");
                        noteActivity();
                        sheetKind = SHEET_POWER;
                        powerOverlay = true;
                        forceDraw = true;
                    }
                } else {
                    if (sideLow >= 1 && !sideLong) {
                        Serial.println("[btn] side: short -> light");
                        noteActivity(); cycleBacklight();
                    }
                    sideLow = 0;
                }
            }
        }

        // DOBA: an active SOS repeats; its page counts down.
        {
            static uint32_t sosPaint = 0;
TRC("dobatick");             if (doba_ui::tick(g_state.snapshot())) forceDraw = true;
            // DOBA stealth: the light goes dark the moment it starts.
            static int8_t wasStealth = -1;
            const bool st = stealth::active();
            if (st != (wasStealth == 1)) {
                if (st) applyBacklight(0);
                else if (wasStealth == 1) applyBacklight(settings::backlight());
                wasStealth = st ? 1 : 0;
                forceDraw = true;
            }
            if (screen == SCREEN_SOS && doba_ui::sosActive() && millis() - sosPaint > 30000) {
                sosPaint = millis();
                forceDraw = true;
            }
        }

        // Touch: interrupt-driven. getPoint() (an I2C read) runs when the
        // GT911 INT fired, while a touch is ongoing (to catch the release),
        // or on a slow fallback tick so input is never dropped.
TRC("touch");         if (touchOk && g_touchQ) {
            TouchEv ev;
            static uint32_t lastTapMs = 0;
            while (xQueueReceive(g_touchQ, &ev, 0) == pdTRUE) {
                touchDownAt = ev.downMs;
                if (mapDrag(ev.x0, ev.y0, ev.x1, ev.y1)) {
                    noteActivity();   // DOBA: a finger drawn across the map moves it
                    continue;
                }
                // Real releases now, so only a double read of one tap is filtered.
                if (millis() - lastTapMs < 120) continue;
                lastTapMs = millis();
                noteActivity();
                const uint32_t held = ev.upMs - ev.downMs;
                doba_ui::setPressMs(held);
                if (powerOverlay) handlePowerTap(ev.x1, ev.y1);
                else if (held >= 800 && mapLongPress(ev.x1, ev.y1)) {}   // DOBA
                else handleTap(ev.x1, ev.y1);
            }
        }

        // Capacitive Home button (GT911 key). On the two main screens it swaps
        // dash <-> map — the gesture that used to be "tap anywhere", now on a
        // dedicated key so the map body is safe to touch. Everywhere else it
        // still navigates back. Debounced so one press triggers once even while
        // the key is held.
        if (homeKeyPressed) {
            homeKeyPressed = false;
            static uint32_t lastHome = 0;
            if (millis() - lastHome > 400) {
                lastHome = millis();
                noteActivity();
                // The carousel: every page in configured order, the map
                // among them wherever the rider put it. With the default
                // config this is exactly the old dash <-> map swap.
                // DOBA: Home is home — the main screen, from anywhere, at once.
                // Back is the BACK corner of the status bar.
                if (routes::navPending()) routes::dismissNav();
                powerOverlay = false;
                if (screen == SCREEN_ARTICLE) doba_lib::leaveArticle();
                Serial.printf("[ui] home key from %d\n", (int)screen);
                screen = SCREEN_HOME;
                forceDraw = true;
            }
        }

TRC("serial");         pollSerialCommands();   // serial-driven button presses (testing/profiling)

        // A private-channel message pops up on its arrival and goes again
        // after MESH_NOTICE_MS; both ends are a repaint.
        {
            mesh_service::Notice n;
            if (mesh_service::latestNotice(n) && n.seq != noticeSeq) {
                noticeSeq = n.seq;
                notice = n;
                noticeUp = true;
                noticeUntilMs = millis() + MESH_NOTICE_MS;
                forceDraw = true;
                diag::log("ui: mesh notice from %s on %s", n.from, n.channelName);
            } else if (noticeUp && (int32_t)(millis() - noticeUntilMs) >= 0) {
                noticeUp = false;
                forceDraw = true;
            }
        }

        bool navPrompt = routes::navPending();
        if (navPrompt && !lastNavPrompt) navPromptShownAt = millis();
        static int lastDashPage = 0;
        // A page flip is a screen change in every way that matters to the
        // refresh path: full-frame content swap, wants the quality pass.
        bool screenChanged = screen != lastScreen || powerOverlay != lastOverlay
                             || navPrompt != lastNavPrompt
                             || (screen == SCREEN_DASH && dashPage != lastDashPage);
        // Was: a deferred GL16 clean scheduled after an interactive DU burst
        // settled. The driver DC-balances its own output, so there is nothing
        // left to clean up after — and the ~950 ms hitch that clean cost is gone
        // with it. Kept as a constant only because refresh() still takes the
        // argument.
        const bool wantClean = false;
        bool navPromptAppearing = navPrompt && !lastNavPrompt;

        // A modal does not tick. Its own content is fixed — "SHUT DOWN?" and two
        // buttons, or a route name and two buttons — and what is behind it is
        // scrimmed or is a static route preview, so the 1 Hz redraw was spending
        // a full-frame render plus a compaction every second to change nothing
        // the rider can read.
        //
        // That is what made the on-screen SHUT DOWN button take two goes. Touch
        // is sampled BETWEEN loop passes, so while a pass is busy rendering and
        // handing a frame to the paint task, a press-and-release that begins and
        // ends inside that window is never observed at all — the controller has
        // nothing to report by the time we next ask it. Everywhere else the loop
        // is parked on its semaphore and the touch interrupt wakes it at once,
        // which is why every other tap felt instant. Nothing is lost by holding
        // still: a real change (the modal opening or closing, a button press)
        // comes through screenChanged/forceDraw, and lastDraw is left stale so
        // the frame after the modal closes paints immediately.
TRC("predraw");         const bool modalHoldsStill = powerOverlay || navPrompt;
        if (screenChanged || forceDraw ||
            (!modalHoldsStill && tickDue(screen, lastDraw))) {
            lastDraw = millis();
            forceDraw = false;
            Screen prevScreen = lastScreen;
            const bool prevOverlay = lastOverlay, prevNav = lastNavPrompt;
            lastScreen = screen;
            lastDashPage = dashPage;
            lastOverlay = powerOverlay;
            lastNavPrompt = navPrompt;
            RideState s = g_state.snapshot();
            // Route progress for the ROUTE LEFT dashboard field. Mirrored onto
            // the snapshot rather than read inside the renderer, which is
            // host-compiled for the previews and has no routes module.
            // A sensor counts as connected only while its data is fresh. The
            // BLE link flag alone said "connected" for a power meter that had
            // stopped transmitting, so the field neither hid nor greyed and the
            // rider saw a live-looking number from a sensor that was gone.
            // Generous window: power meters are quiet when you stop pedalling.
            constexpr uint32_t kSensorStaleMs = 15000;
            const uint32_t nowMs = millis();
            if (s.powerConnected && nowMs - s.powerMs > kSensorStaleMs)
                s.powerConnected = false;
            if (s.hrConnected && nowMs - s.hrMs > kSensorStaleMs)
                s.hrConnected = false;
            if (s.cadenceConnected && nowMs - s.cadenceMs > kSensorStaleMs)
                s.cadenceConnected = false;
            s.showOffline = settings::showOffline();
            s.routeActive = routes::active();
            s.routeRemainingKm = routes::remainingKm();
            s.sdCard = ride_recorder::sdCardState();
            // Swap to a downloaded map that covers where we are, if one exists.
            // Not while a host computer owns the SD card.
            if (s.everHadFix && !usb_storage::hostActive())
                map_store::ensureForPosition(s.latitude, s.longitude);
            uint8_t* fb = epdc_framebuffer();
            memset(fb, 0xFF, epd_width() / 2 * epd_height());
            bandDrawnThisFrame = false;
            bool pickerThisFrame = false;   // set by the WORKOUT page below
            // While the "Start navigation?" prompt is up, the base screen shows
            // the whole route fitted so it can be recognized before accepting.
            if (navPrompt && !powerOverlay) {
              // Preview first (its road context projects full-screen), then the
              // status bar + accept sheet on top to mask any road spill.
              ui_render_route_preview(fb);
              // Clear the band AFTER the map, not before: the renderer has no
              // clipping, so the road and water fills paint over anything drawn
              // earlier. Clearing first (as I first wrote it) left the water
              // dither sitting on top of the clock.
              epd_fill_rect({0, 0, epd_rotated_display_width(), ui::STATUS_H},
                            0xFF, fb);
              ui::statusBar(s, fb);
              ui_render_nav_prompt(routes::activeName(),
                                   routes::maneuverCount(), fb);
            } else if (greyTestShown) {   // DOBA: `greytest` console card
              ui_render_grey_test(fb);
            } else {
            switch (screen) {
                case SCREEN_DASH: {
                    if (dash_config::page(dashPage).kind == DP_MAP)
                        dashPage = firstContentPage();
                    const DashPage& pg = dash_config::page(dashPage);
                    {
                        // Compact hero whenever a banner (turn, pause or message) owns
                        // the band, so no field hides underneath it.
                        ui_render_dashboard(s,
                                            routes::navActive() || s.ridePaused ||
                                                meshNoticeShown(),
                                            pg.layout, fb);
                    }
                    drawNavBanner(fb, 0);
                    if (pg.kind == DP_FIELDS) drawPauseBanner(s, fb);
                    drawMeshNotice(fb);
                    break;
                }
                case SCREEN_MAP: {
                    const uint32_t t0 = micros();
                    renderMapScreen(s, fb);
                    Serial.printf("[perf] map drawn in %lu ms at %.2f m/px\n", (unsigned long)((micros() - t0) / 1000), mapMpp);
                    break;
                }
                case SCREEN_SUMMARY:
                    // Modal: the ride screen stays behind the sheet, scrimmed.
                    ui_render_dashboard(s, routes::navActive(),
                                        dash_config::current(), fb);
                    ui_render_summary(pendingSummary, fb, recoveryPrompt);
                    break;
                case SCREEN_MENU: {
                    MenuInfo m;
                    m.recording = s.recording;
                    m.gpsReady = s.gpsFix && s.timeValid;
                    m.sdOk = ride_recorder::sdMounted();
                    // Card numbers, read on arrival, when a ride starts or
                    // stops, and every 30 s — not on every 1 Hz redraw. Each
                    // read is SD work under sdLock, in the pass the rider's
                    // tap is waiting behind; nothing else changes them while
                    // the Menu is up.
                    static int cachedRides = 0;
                    static uint32_t cachedFreeMB = 0, cardReadAt = 0;
                    static bool cardRead = false, cardRecording = false;
                    if (m.sdOk && (!cardRead || prevScreen != SCREEN_MENU ||
                                   cardRecording != s.recording ||
                                   millis() - cardReadAt > 30000)) {
                        cachedRides = ride_recorder::rideCount();
                        cachedFreeMB = ride_recorder::sdFreeMB();
                        cardReadAt = millis();
                        cardRecording = s.recording;
                        cardRead = true;
                    } else if (!m.sdOk) {
                        cardRead = false;   // re-read when the card comes back
                    }
                    m.rideCount = cachedRides;
                    m.sdFreeMB = cachedFreeMB;
                    m.hr = s.hrConnected;
                    m.pwr = s.powerConnected;
                    m.cad = s.cadenceConnected;
                    m.batteryPercent = s.batteryPercent;
                    m.rideDistanceM = s.distanceM;
                    m.rideElapsedS = s.elapsedS;
                    m.useMiles = s.useMiles;
                    if (routes::active()) {
                        snprintf(m.routeLine, sizeof(m.routeLine),
                                 "%s · %.1f %s left", routes::activeName(),
                                 units::dist(routes::remainingKm(), s.useMiles),
                                 s.useMiles ? "mi" : "km");
                    } else {
                        snprintf(m.routeLine, sizeof(m.routeLine),
                                 "no route loaded");
                    }
                    ui_render_menu(m, fb);
                    ui::statusBar(s, fb, "MENU");
                    break;
                }
                case SCREEN_ROUTES:
                case SCREEN_HISTORY:
                case SCREEN_DIRECTIONS:
                    renderListScreen(fb);
                    ui::statusBar(s, fb,
                                  screen == SCREEN_ROUTES   ? "ROUTES"
                                  : screen == SCREEN_HISTORY  ? "TRACKS"
                                                              : "DIRECTIONS");
                    break;
                case SCREEN_HOME:
                case SCREEN_COMMS:
                case SCREEN_COMPOSE:
                case SCREEN_LIBRARY:
                case SCREEN_SETUP: case SCREEN_SETUP_NAME: case SCREEN_POINTS:
                case SCREEN_POINT: case SCREEN_POINT_NEW: case SCREEN_MAPOPTS: case SCREEN_SOS: case SCREEN_SUN: case SCREEN_MORE: case SCREEN_BATTERY:
                case SCREEN_ARTICLE: {   // DOBA
                    const uint32_t t0 = micros(), ev0 = vf_evictions();
TRC("render");                     doba_ui::render(dobaPage(), s, fb);
TRC("rendered");                     if (screen == SCREEN_HOME) ui::statusBar(s, fb, doba_ui::title(dobaPage()));
                    else ui::statusBarBack(s, fb, doba_ui::title(dobaPage()));
                    // DOBA: what drawing the page cost (the panel's own update
                    // comes after); a font face thrown out means glyphs drawn again.
                    Serial.printf("[perf] page %d drawn in %lu ms%s\n", (int)dobaPage(),
                                  (unsigned long)((micros() - t0) / 1000),
                                  vf_evictions() != ev0 ? " (font cache full)" : "");
                    break;
                }
                case SCREEN_SETTINGS: {
                    // Mesh state comes from the SERVICE, not settings: the tap
                    // only stages a request, and the stored flag is not written
                    // until the mesh task has actually brought the radio up or
                    // put it to sleep. The task is woken immediately and a panel
                    // repaint is far slower, so this still reads as instant —
                    // and if the radio ever fails to start, the row shows what
                    // is true rather than what was asked for.
                    SettingsInfo si{settings::showOffline(),
                                    0, settings::tzMinutes(),
                                    settings::backlight(), settings::useMiles(),
                                    settings::usbDrive(),
                                    mesh_service::enabled()};
                    ui_render_settings(si, fb);
                    ui::statusBarBack(s, fb, "MORE");
                    break;
                }
                case SCREEN_MAPLAYERS:
                    ui_render_map_layers(settings::mapLayers(), fb);
                    ui::statusBarBack(s, fb, "MAP LAYERS");
                    break;
                case SCREEN_GPSDEBUG: {
                    GpsDebug d;
                    gps_service::getDebug(d);
                    GpsDebugView v;
                    v.moduleDetected = d.moduleDetected;
                    v.module = gps_service::moduleName();
                    v.chars = d.chars;
                    v.passedCksum = d.passedCksum;
                    v.failedCksum = d.failedCksum;
                    v.withFix = d.withFix;
                    v.satsInUse = d.satsInUse;
                    v.satsInView = d.satsInView;
                    v.bestSnr = d.bestSnr;
                    v.hdop = d.hdop;
                    v.locValid = d.locValid;
                    v.locAgeMs = d.locAgeMs;
                    v.lat = d.lat;
                    v.lon = d.lon;
                    v.altM = d.altM;
                    v.speedKmh = d.speedKmh;
                    v.hour = d.hour;
                    v.minute = d.minute;
                    v.second = d.second;
                    v.useMiles = settings::useMiles();
                    ui_render_gps_debug(v, fb);
                    ui::statusBarBack(s, fb, TR("GPS details"));
                    break;
                }
            }
            if (powerOverlay) {
                ui_render_power_sheet(s.recording, fb);
            }
            }  // end else (normal screens)
            // These three classified the frame so refresh() could pick a waveform.
            // The driver picks its own now (see refresh()), so they are inert — kept
            // because they document which screens are pure black/white and which
            // transitions used to need scrubbing, and because deleting them would
            // mean touching all ~15 refresh() call sites for no behaviour change.
            bool fastInPage = screenIsFast(screen, powerOverlay) && !navPrompt;
            bool listFast = screenListFast(screen) && screenListFast(prevScreen) &&
                            !powerOverlay && !navPrompt;
            // ENTERING the map is the only transition that earns a scrub — see
            // refresh(). Leaving it doesn't: what replaces the map is heavy type
            // and filled blocks, which drive over fine map lines without any
            // haze, so that flash bought nothing and just made every exit from
            // the map feel slow. Deliberately NOT gated on screenChanged, which
            // is also true when only the power sheet opened or closed: that is
            // the popup flash, and staying on SCREEN_MAP now reads as no
            // transition at all.
            // A dismissed modal also needs the scrub. Its scrim tones the whole
            // map body, and closing it does NOT change `screen` — so without
            // this the checker stayed ghosted over the map until something else
            // happened to force a clear.
            const bool modalClosed = (prevOverlay && !powerOverlay) ||
                                     (prevNav && !navPrompt);
            bool mapTransition = screen == SCREEN_MAP &&
                                 (prevScreen != SCREEN_MAP || modalClosed);

            // A greyed-out dashboard cell earns the same scrub, for the same
            // reason: the tone is a 25% dot, and whatever was on the glass
            // under it only half-erases in one pulse. The driver has already
            // recorded those pixels as white, so nothing later will touch them
            // and the cell reads as dirty rather than inactive.
            //
            // Fires when arriving on the data page with cells already toned,
            // and when one greys out IN PLACE — a sensor dropping mid-ride does
            // not change `screen`, so entry alone would miss it.
            static bool prevToned = false;
            const bool toned = screen == SCREEN_DASH && !powerOverlay &&
                               ui_render_dashboard_toned();
            const bool tonedTransition =
                toned && (prevScreen != SCREEN_DASH || modalClosed || !prevToned);
            prevToned = toned;

            // Every page flip in the Home-key carousel gets the quick scrub
            // too: the pages are visually unrelated (fields, music art,
            // workout's solid block), and DU-driving one over another leaves
            // the old page as haze under the new. Scoped to what actually
            // changed, so it stays quick; the 1 Hz in-page redraws never fire
            // it (same page → same value).
            static int prevDashPage = -1;
            const bool dashPageFlip =
                screen == SCREEN_DASH &&
                (prevScreen != SCREEN_DASH || dashPage != prevDashPage);
            prevDashPage = screen == SCREEN_DASH ? dashPage : -1;

            // Any top-band overlay (turn banner or AUTO-PAUSED) leaving the
            // glass earns a scrub: they are solid black blocks, and the
            // content that replaces them half-erases under a DU drive — the
            // band's ghost sat over the page until something else forced a
            // clear. Tracks what was actually DRAWN, so the popup turn cue
            // on the music/workout pages is covered too.
            static bool prevBand = false;
            const bool bandCleared = prevBand && !bandDrawnThisFrame;
            prevBand = bandDrawnThisFrame;

            hardClearToned = tonedTransition;
            // Only changed pixels are driven, so what stays black between two screens
            // keeps its old history and shows inside anything solid on the new one:
            // settings toggles in the start button, for one. A scrub takes every pixel
            // in the changed area through white, so on the scan path any change of
            // screen earns one, not just the transitions listed above.
            const bool scanScrub = epdc_scan_enabled() && screen != prevScreen;
            // A zoom replaces the whole map without changing screen, and the water's
            // dots that stay black show the old roads. Only the water needs it, so only
            // the water gets it: the rest of the map changes in place. Done with once a
            // frame that knows where its water is has gone out, since a touch can drop
            // the frame that carried the zoom and a partial one has no layer to ask.
            static float scrubbedMpp = 0.0f;
            const bool zoomed = screen == SCREEN_MAP && mapMpp != scrubbedMpp;
            scrubBits = zoomed && epdc_scan_enabled() ? mapFrameWater : nullptr;
            const bool scrubbing = scrubBits != nullptr;
            const bool painted = refresh(screenChanged, fastInPage, listFast, wantClean,
                                         navPromptAppearing,
                                         mapTransition || tonedTransition || dashPageFlip ||
                                             bandCleared || scanScrub);
            if (painted && scrubbing) scrubbedMpp = mapMpp;
            scrubBits = nullptr;
        }

        // The epdiy path managed panel rails by hand here — poweron before an
        // update, and a 1.5 s keep-alive so successive taps skipped the ~36 ms
        // power-up. EPD_Painter owns its rails around each paint, so there is
        // nothing to release.

        mapPrerenderStep();

        // Block until an input interrupt or a redraw request, with a fallback
        // tick that still satisfies the slowest thing the loop owes anyone (the
        // 200 ms touch-poll backstop). Was a flat 30 ms delay, i.e. ~33
        // wakeups/s to discover there was nothing to do ~30 times out of 33.
        //
        // This only became worth fixing once light sleep worked: the SoC can
        // only sleep when BOTH cores are idle, and tickless idle sizes each
        // sleep by the shortest pending timer across them, so this task's tick
        // was capping every sleep on core 1 at 30 ms.
        xSemaphoreTake(uiWake, pdMS_TO_TICKS(UI_IDLE_TICK_MS));
    }
}

}  // namespace ui_dashboard


// DOBA: the map's orientation, for doba_ui's MAP options page.
namespace doba_ui {
bool mapTrackUp() { return ::mapTrackUp; }
void setMapTrackUp(bool on) { ::mapTrackUp = on; }
}  // namespace doba_ui

namespace live_view {
namespace {
LiveView g_live;
portMUX_TYPE g_liveMux = portMUX_INITIALIZER_UNLOCKED;
}  // namespace
void publish(const LiveView& v) {
    portENTER_CRITICAL(&g_liveMux);
    g_live = v;
    portEXIT_CRITICAL(&g_liveMux);
}
void read(LiveView& v) {
    portENTER_CRITICAL(&g_liveMux);
    v = g_live;
    portEXIT_CRITICAL(&g_liveMux);
}
}  // namespace live_view
