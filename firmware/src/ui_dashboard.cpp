#include "ui_dashboard.h"
#include "doba_places.h"
#include "doba_pois.h"
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
#include "device_state.h"
#include "sd_card.h"
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
#include "i2c_bus.h"
#include "usb_storage.h"
#include <esp_random.h>

#include "lora_radio.h"
#include "mesh_service.h"
#include "settings.h"
#include "speed_source.h"
#include "aux_sensors.h"
#include "diag.h"
#include "crash_report.h"
#include "power_mgmt.h"
#include "live_view.h"

#include "ui_shared.h"
#include "doba_mem.h"
namespace uid {

TouchDrvGT911 touch;
bool touchOk = false;
int refreshCount = 0;

// Shadow of the last frame actually pushed to glass. The panel must not
// repaint when nothing changed (design: "static chrome never repaints"),
// so identical frames are dropped before any epd update.
uint8_t* shadowFb = nullptr;
size_t fbSize = 0;

Screen screen = SCREEN_HOME;   // DOBA: boot lands on the main screen

// DOBA: the grey calibration card is up (console `greytest`; any touch or
// key, or `greytest off`, takes it down).
bool greyTestShown = false;

// Serial test hooks: drive the UI over the CDC serial port to profile the map
// without physical taps. Toggle timing logs with 't'; single-char commands
// injected in the task loop mirror button presses. See pollSerialCommands().
bool dbgTiming = false;


// Turn-by-turn banner rect (top of map/dashboard); tapping it ends nav.
const EpdRect kNavBanner = {0, ui::STATUS_H, 540, 138};
// bandDrawnThisFrame feeds the dismissal scrub: a popup leaving the glass
// needs its ghost cleared.
void drawMeshNotice(uint8_t* fb);
bool bandDrawnThisFrame = false;

// The private-channel message popup (mesh_service::latestNotice). Up from
// arrival for MESH_NOTICE_MS, or until tapped; noticeSeq is the last one
// shown, so each message pops up once.
mesh_service::Notice notice;
uint32_t noticeSeq = 0;
bool noticeUp = false;
uint32_t noticeUntilMs = 0;
bool meshNoticeShown() { return noticeUp; }


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

static inline void IRAM_ATTR uiWakeFromIsr() {
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
static void IRAM_ATTR onTouchIrq() {
    touchIrq = true;
    if (g_touchTask) {
        BaseType_t hpw = pdFALSE;
        vTaskNotifyGiveFromISR(g_touchTask, &hpw);
        if (hpw) portYIELD_FROM_ISR();
    }
    uiWakeFromIsr();
}
static void IRAM_ATTR onBoardBtnIrq() { boardBtnIrq = true; uiWakeFromIsr(); }

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

}  // namespace

// DOBA: a power-off from the menu on battery cuts the battery (ship mode), and
// then only the POWER key brings Doba back; auto-sleep and power-off on USB
// stay in deep sleep, woken by BOOT. The farewell screen says which.
bool g_offByPowerKey = false;

void shutdownDevice(uint8_t* fb, const char* reason) {
    doba_lib::flush();   // the recent list and reading places, before the power goes
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
    ui_render_shutdown_ack(false, fb);
    epdc_paint();

    // Log WHY we're powering off and flush it to SD before deep sleep, so the
    // next boot's log distinguishes a user shutdown / auto-sleep from a reset
    // or a power loss (which leave no such line).
    diag::log("shutdown: %s", reason);
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
              sd_card::sdMounted(), usb_storage::hostActive(),
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



// Long-press tracking
uint32_t touchDownAt = 0;
volatile bool touchWasDown = false;   // DOBA: written by touchTask
// Where the finger went down and where it is now, while it is on the glass (the
// map follows a drag with them). Written by touchTask, read by the UI task.
volatile int16_t touchStartX = 0, touchStartY = 0, touchNowX = 0, touchNowY = 0;
void touchTask(void*) {
    int16_t x0 = 0, y0 = 0, lx = 0, ly = 0;
    uint32_t downAt = 0, lastSeen = 0;
    int empty = 0;
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
        // The GT911 driver reads its point count without waiting for a fresh
        // frame, so a read between two frames can say "no finger" while the
        // finger is still down. Polled every 15 ms that happened often: one
        // tap became two or three (the library opened a book and an article
        // at once, BACK went back twice). A finger is up only once it has
        // been missing for 3 reads in a row and 60 ms.
        if (!now && down) {
            ++empty;
            if (empty < 3 || millis() - lastSeen < 60) {
                touchWasDown = true;
                continue;
            }
        }
        if (now) { empty = 0; lastSeen = millis(); }
        if (now) {
            const int16_t px = lx, py = ly;
            lx = x[0]; ly = y[0];
            if (!down) { x0 = lx; y0 = ly; downAt = millis(); touchStartX = x0; touchStartY = y0; }
            touchNowX = lx; touchNowY = ly;
            // a finger moving: let the UI task see it (the map follows a drag)
            if (down && (abs(lx - px) + abs(ly - py)) >= 4 && uiWake) xSemaphoreGive(uiWake);
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
constexpr uint32_t AUTO_SLEEP_MS = 3 * 60 * 1000;    // 3 minutes idle (user, 18 Nov)
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
    return sc == SCREEN_MAP || sc == SCREEN_SOS || sc == SCREEN_GPSDEBUG ||
           sc == SCREEN_SUN || sc == SCREEN_HOME;
}
bool tickDue(Screen sc, uint32_t lastDraw) {
    const uint32_t since = millis() - lastDraw;
    if (sc == SCREEN_MAP) {
        // the map: when something on it changed, else every 30 s like a page
        static uint32_t lastKey = 0;
        if (since < 1000) return false;
        const uint32_t k = mapTickKey(g_state.snapshot());
        if (k != lastKey || since >= 30000) { lastKey = k; return true; }
        return false;
    }
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
    return s == SCREEN_MAP || s == SCREEN_GPSDEBUG;
}

bool inRect(const EpdRect& r, int x, int y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

void handlePowerTap(int x, int y);

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
    if (powerOverlay) {
        powerOverlay = false;
        return;
    }
    switch (screen) {
        case SCREEN_GPSDEBUG: screen = SCREEN_MORE; break;   // DOBA
        case SCREEN_HOME:
        case SCREEN_COMMS:
        case SCREEN_COMPOSE:
        case SCREEN_LIBRARY:
        case SCREEN_SETUP: case SCREEN_SETUP_NAME: case SCREEN_POINTS:
        case SCREEN_POINT: case SCREEN_POINT_NEW: case SCREEN_MAPOPTS: case SCREEN_SOS: case SCREEN_SUN: case SCREEN_MORE: case SCREEN_BATTERY:
        case SCREEN_ARTICLE:  dobaGo(doba_ui::back(dobaPage())); break;
        case SCREEN_MAP:      screen = SCREEN_HOME; break;   // DOBA: the map is a page of Doba's home
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
void ackZoomTap(bool zoomIn, float mppBefore) {
    if (screen != SCREEN_MAP) return;
    uint8_t* fb = epdc_framebuffer();
    // DOBA: the new zoom at once, from the map already drawn: the last view
    // enlarged (or shrunk) 2x about its centre, under the new zoom's chrome. Soft
    // for a moment; the sharp frame follows. Not at the end of the range.
    MapScreenData map = {};
    const DeviceState s = g_state.snapshot();
    buildMapScreenData(s, map);
    if (fabsf(map.metersPerPixel - mppBefore) > 0.01f && map.topo &&
        topo::drawZoomPreview(fb, zoomIn, map.riderX + (int)lroundf(mapPanE / map.metersPerPixel),
                              map.riderY - (int)lroundf(mapPanN / map.metersPerPixel))) {
        ui_render_map(map, s, fb, /*baseDrawn=*/true);
        if (dbgTiming) diag::log("map: zoom preview %s", zoomIn ? "in" : "out");
    }
    ui_map_draw_zoom_button(zoomIn, true, fb);
    refresh(false, true, false);
    // Stay pressed until the map has actually caught up. A single flash before
    // the redraw answers "did it hear me?" but not "is it still working?", and
    // on a cold zoom the outer tiles can take a moment to arrive. Held down, the
    // button is doing the job a spinner would.
    zoomHeld = zoomIn ? 1 : 2;
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

bool isDobaSubPage(Screen sc) {
    switch (sc) {
        case SCREEN_COMMS: case SCREEN_COMPOSE: case SCREEN_LIBRARY: case SCREEN_ARTICLE:
        case SCREEN_SETUP: case SCREEN_SETUP_NAME: case SCREEN_POINTS: case SCREEN_POINT:
        case SCREEN_POINT_NEW: case SCREEN_MAPOPTS: case SCREEN_SOS: case SCREEN_SUN: case SCREEN_MORE: case SCREEN_BATTERY:
        case SCREEN_MAP: case SCREEN_GPSDEBUG:
            return true;
        default:
            return false;
    }
}

void handleTap(int x, int y) {
    // DOBA: Doba's sub-pages carry a BACK chevron at the left of the band (the
    // Home key still goes back too; this is the one a new user finds).
    if (y < ui::STATUS_H && x < ui::BACK_W + 20 && isDobaSubPage(screen)) {
        goBack();
        return;
    }

    // A message popup is dismissed by tapping it.
    if (meshNoticeShown() && inRect(kNavBanner, x, y) && screen == SCREEN_MAP) {
        noticeUp = false;
        noteActivity();
        return;
    }

    switch (screen) {
        case SCREEN_MAP:
            if (x >= kMapZoom.zoomX &&
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
                    const float before = mapMpp;
                    if (mapMpp > 0.2f) mapMpp /= 2.0f;   // snapped to the deepest tiles here
                    Serial.printf("[map] zoom in -> %.2f m/px\n", mapMpp);
                    ackZoomTap(true, before);
                    break;
                }
                if (y >= kMapZoom.zoomOutY && y < kMapZoom.zoomOutY + kMapZoom.size) {
                    const float before = mapMpp;
                    if (mapMpp * 2.0f <= mapMaxMpp() * 1.01f) mapMpp *= 2.0f;
                    Serial.printf("[map] zoom out -> %.2f m/px\n", mapMpp);
                    ackZoomTap(false, before);
                    break;
                }
            }
            // DOBA: a tap on the cross of a dragged map sets A there (or clears
            // it); the strip then measures from A to wherever the cross goes.
            if (mapPanned() &&
                abs(x - 270) < 56 && abs(y - milspec::kCrossY) < 56) {
                milspec::measureToggleAtCross();
                Serial.printf("[map] measure %s\n", milspec::measureOn() ? "A set" : "off");
                break;
            }
            if (y < ui::STATUS_H) screen = SCREEN_HOME;   // DOBA: was the menu
            else if (y >= ui::MAP_STRIP_TOP) {
                screen = SCREEN_POINTS;   // DOBA: the strip opens POSITION (no ride dashboard)
            }
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
            if (screen == SCREEN_HOME || (screen == SCREEN_LIBRARY && !doba_lib::typing() && !doba_lib::tapIsCheap(x, y)))
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
        case SCREEN_GPSDEBUG:
            // A tap anywhere goes back to More, where it was opened from. (A tap
            // in the top 150 px, the Back corner included, ran a panel drive
            // experiment that flashed the screen dark and light for 2 s.)
            screen = SCREEN_MORE;
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

// A private-channel message, over whatever else is in the band for its ten
// seconds (except a turn that is close — see meshNoticeShown).
void drawMeshNotice(uint8_t* fb) {
    if (!meshNoticeShown()) return;
    ui_render_mesh_notice(notice.from, notice.channelName, notice.text, fb);
    bandDrawnThisFrame = true;
}





}  // namespace uid
using namespace uid;

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
    // The topo packs are opened when the map first asks for them: nothing to
    // index here, and nothing on the card to walk at boot.
    bootStep("Maps");
    bootDetailFor("Maps", "ready");
    EPDC_STEP("bootDetailFor done");
    bootStatus("Maps", true);
    EPDC_STEP("bootStatus done");
    EPDC_STEP("map loaded — begin() done");
    // Hand the panel over to the dashboard: further bootStatus() calls no-op,
    // and the task's first refresh() paints the real UI over the boot screen.
    bootScreenLive = false;
    return true;
}

// Flash a firmware.bin dropped on the SD card, then reboot. Reads straight from
// SD into the flash writer — no PSRAM staging (unlike the BLE OTA). ESP32 A/B
// OTA partitions still protect the running image if the flash fails. To update:
// copy the new firmware.bin to the SD card root and power the device on.
void applySdUpdate() {
    if (!sd_card::sdMounted()) return;

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
    Serial.println("  mem [drop]           free memory (drop: let the map and picture caches go)");
    Serial.println("  cold [unaided]       GPS cold-start test (re-seed unless 'unaided')");
    Serial.println("  gpsoff <sec>         cut GPS power for N s, then re-seed (retention test)");
    Serial.println("  gpsver               query GPS module firmware version");
    Serial.println("  gpsraw <on|off>      echo raw receiver bytes");
    Serial.println("  sd                   SD mount/USB ownership state (see diag for failures)");
    Serial.println("  sdtest [seconds|status] SD/sleep/phone verification, 30..1800s (default 180)");
    Serial.println("  crashlog [panic]     report status; panic deliberately reboots (no active ride)");
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
    } else if (!strcasecmp(cmd, "farewell")) {
        farewellTest();
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
                      sd_card::sdMounted() ? "yes" : "NO", cn, sizeMB);
        // "Not mounted" has two causes that need opposite responses, and the
        // difference is invisible on the device's own screen. Say which it is.
        if (!sd_card::sdMounted()) {
            if (usb_storage::hostActive())
                Serial.println("[sd] a USB host owns the card — unplug it, eject it, "
                               "or toggle the USB drive off/on ('usbdrive off' then 'on')");
            else
                Serial.println("[sd] not mounted by the firmware — cardType=NONE means "
                               "the driver cleaned up; use diag for command responses");
        }
        if (sd_card::sdMounted()) {
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
            if (on) doba_lib::flush();   // the card goes to the computer: nothing left unwritten
            usb_storage::setDriveEnabled(on);
            Serial.printf("[usbdrive] turned %s\n", on ? "on" : "off");
        }
    } else if (!strcasecmp(cmd, "bootloader") || !strcasecmp(cmd, "boot")) {
        rebootToBootloader();
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
        if (!arg) { Serial.println("[cmd] screen <home|map|library|comms|setup|more|points|sun|sos|gps|...>"); return; }
        powerOverlay = false;   // a jump closes the power sheet, as Back does
        if      (!strcasecmp(arg, "map"))      screen = SCREEN_MAP;
        else if (!strcasecmp(arg, "gps"))      screen = SCREEN_GPSDEBUG;
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
        if (arg && !strcasecmp(arg, "in") && mapMpp > 0.2f) mapMpp /= 2.0f;
        else if (arg && !strcasecmp(arg, "out") && mapMpp * 2.0f <= mapMaxMpp() * 1.01f) mapMpp *= 2.0f;
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
        const DeviceState st = g_state.snapshot();
        float v;
        bool wet;
        if (doba_dem::ground(st.latitude, st.longitude, v, wet))
            Serial.printf("[dem] %s %.1f\n", wet ? "water" : "land", v);
        else
            Serial.println("[dem] none");
    } else if (!strcasecmp(cmd, "pois")) {   // DOBA: pois -> the layer's state and what is round the map's middle
        double la = 0, lo = 0;
        milspec::screenToLatLon(480, 280, la, lo);   // the last map frame's middle
        static doba_pois::Poi near[5];
        const int n = doba_pois::nearest(la, lo, 0, near, 5);
        Serial.printf("[pois] layer %s, %.1f m/px (shown at <= 25), %d near %.4f,%.4f\n",
                      milspec::poisOn() ? "on" : "OFF", mapMpp, n, la, lo);
        for (int i = 0; i < n; ++i) Serial.printf("  %.0f m  kind %d  %s\n", near[i].dist, near[i].kind, near[i].name);
    } else if (!strcasecmp(cmd, "mem")) {    // DOBA: mem | mem drop (let the caches go)
        if (arg && !strcasecmp(arg, "drop")) { doba_mem::report("before"); doba_mem::dropCaches(); }
        doba_mem::report("now");
    } else if (!strcasecmp(cmd, "topo")) {   // DOBA: topo layer diagnostics
        if (arg && !strcasecmp(arg, "canvas")) {
            const char* v = strtok(nullptr, " \t");
            topo::setCanvas(!(v && !strcasecmp(v, "off")));
            mapCache.clear();
            forceDraw = true;
        } else if (arg && !strcasecmp(arg, "dither")) {
            const char* v = strtok(nullptr, " \t");
            topo::setDither(!(v && !strcasecmp(v, "off")));
            mapCache.clear();
            forceDraw = true;
        }
        const topo::Stats t = topo::stats();
        Serial.printf("[topo] active=%d mpp=%.2f decoded=%lu hits=%lu missing=%lu packs=%lu blocks=%lu blits=%lu anchors=%lu shifts=%lu\n",
                      (int)mapTopoActive, mapMpp, (unsigned long)t.decoded,
                      (unsigned long)t.hits, (unsigned long)t.missing,
                      (unsigned long)t.packsOpened, (unsigned long)t.blocks,
                      (unsigned long)t.blits, (unsigned long)t.anchors, (unsigned long)t.shifts);
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
// Everything that reads files off the card forgets what it knew and looks again:
// the card came back, or a computer wrote to it over USB. Topo packs, books,
// saved points, ground heights, place names and useful places.
void cardChanged() {
    topo::reset();
    doba_lib::rescan();
    doba_points::rescan();
    doba_dem::rescan();
    doba_places::rescan();
    doba_pois::rescan();
    mapCache.clear();
    forceDraw = true;
}

void task(void*) {
    uint32_t lastDraw = 0;
    Screen lastScreen = screen;
    bool lastOverlay = false;

    lastActivityMs = millis();


TRC("enter");     applySdUpdate();          // flash a firmware.bin from the SD card, if present
TRC("sdupdate");     usb_storage::begin();     // THEN expose the SD to a host computer over USB
TRC("usb");     bool lastHostActive = false;

    for (;;) {
        // When the computer releases the SD (eject/unplug), check whether it
        // dropped a firmware.bin on the card and flash it automatically.
TRC("loop");         bool host = usb_storage::hostActive();
        if (lastHostActive && !host) {
            applySdUpdate();
            cardChanged();   // the computer may have added or removed maps, books...
        }
        lastHostActive = host;

        // A card that only mounted after boot had given up: everything that
        // looked for its files found none, and the USB drive was skipped. Look
        // again now, here in the UI task, next to the renderer that uses them.
        if (sd_card::consumeLateMount()) {
            diag::log("sd: late mount — loading maps and books, exposing USB drive");
            cardChanged();
            applySdUpdate();      // a firmware.bin may have been on the card
            usb_storage::begin(); // no-op if it already came up
        }

        // Ground height under the position comes from the card's DEM, set by
        // gps_service with every fix (doba_dem).

        // A firmware update in progress takes over the screen with a progress
        // modal (redrawn only when the percentage/phase moves so the e-paper
        // isn't thrashed). Everything else is paused until it finishes/reboots.

        // Riding counts as activity even without touching the screen — a bike
        // computer must NEVER auto-off while you're moving. Any real speed keeps
        // it awake (and for the idle timeout after you stop).
        {
            DeviceState now = g_state.snapshot();
            if (now.gpsFix && now.speedKmh > 3.0f) lastActivityMs = millis();
            // And without a fix: an accelerometer knows the bike is moving in a
            // tunnel, under trees, or before the receiver has locked — all
            // places the idle timeout could otherwise switch the device off
            // underneath a rider who is very much still riding.
            if (now.deviceMoving) lastActivityMs = millis();

            // The page on screen, for diagnostics and the simulator.
            // `page` is the Screen itself: the sim tests tell Doba's pages apart by it.
            LiveView lv;
            if (screen == SCREEN_MAP) lv.screen = 1;
            lv.page = (unsigned char)screen;
            live_view::publish(lv);
        }

        // Auto-sleep after a quiet period to save battery. Wake with POWER.
        if (autoSleepEnabled && millis() - lastActivityMs > AUTO_SLEEP_MS &&
            // DOBA: the mesh no longer holds the device awake. Listening kept
            // CPU, screen controller, GPS and radio up: 8 %/h, flat overnight
            // (18 Nov). Until the radio can listen while the rest sleeps, idle
            // means off, mesh or not.
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
                Serial.printf("[touch] %d,%d -> %d,%d held=%lu at=%lu scr=%d\n", ev.x0, ev.y0, ev.x1, ev.y1,
                              (unsigned long)(ev.upMs - ev.downMs), (unsigned long)ev.upMs, (int)screen);
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
                doba_lib::notePress(held);
                if (powerOverlay) handlePowerTap(ev.x1, ev.y1);
                else if (held >= 800 && mapLongPress(ev.x1, ev.y1)) {}   // DOBA
                else handleTap(ev.x1, ev.y1);
            }
        }
        // DOBA: a finger dragging the map: the map goes with it.
        if (touchOk) mapLiveDrag();

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
                // DOBA: Home is home — the main screen, from anywhere, at once.
                // Back is the BACK corner of the status bar.
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

        bool screenChanged = screen != lastScreen || powerOverlay != lastOverlay;
        // Was: a deferred GL16 clean scheduled after an interactive DU burst
        // settled. The driver DC-balances its own output, so there is nothing
        // left to clean up after — and the ~950 ms hitch that clean cost is gone
        // with it. Kept as a constant only because refresh() still takes the
        // argument.
        const bool wantClean = false;

        // A modal does not tick. Its own content is fixed — "SHUT DOWN?" and two
        // buttons — and what is behind it is scrimmed, so the 1 Hz redraw was spending
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
TRC("predraw");         const bool modalHoldsStill = powerOverlay;
        if (screenChanged || forceDraw ||
            (!modalHoldsStill && tickDue(screen, lastDraw))) {
            lastDraw = millis();
            forceDraw = false;
            Screen prevScreen = lastScreen;
            const bool prevOverlay = lastOverlay;
            if (prevScreen != screen) {   // one line a page: where PSRAM goes (doba_mem.cpp)
                char why[24];
                snprintf(why, sizeof why, "screen %d", (int)screen);
                doba_mem::report(why);
            }
            lastScreen = screen;
            lastOverlay = powerOverlay;
            DeviceState s = g_state.snapshot();
            s.sdCard = sd_card::sdCardState();
            uint8_t* fb = epdc_framebuffer();
            memset(fb, 0xFF, epd_width() / 2 * epd_height());
            bandDrawnThisFrame = false;
            if (greyTestShown) {   // DOBA: `greytest` console card
              ui_render_grey_test(fb);
            } else {
            switch (screen) {
                case SCREEN_MAP: {
                    const uint32_t t0 = micros();
                    renderMapScreen(s, fb);
                    Serial.printf("[perf] map drawn in %lu ms at %.2f m/px\n", (unsigned long)((micros() - t0) / 1000), mapMpp);
                    break;
                }
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
                    v.useMiles = false;
                    ui_render_gps_debug(v, fb);
                    ui::statusBarBack(s, fb, TR("GPS details"));
                    break;
                }
            }
            if (powerOverlay) {
                ui_render_power_sheet(fb);
            }
            }  // end else (normal screens)
            // These three classified the frame so refresh() could pick a waveform.
            // The driver picks its own now (see refresh()), so they are inert — kept
            // because they document which screens are pure black/white and which
            // transitions used to need scrubbing, and because deleting them would
            // mean touching all ~15 refresh() call sites for no behaviour change.
            bool fastInPage = screenIsFast(screen, powerOverlay);
            bool listFast = false;
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
            const bool modalClosed = prevOverlay && !powerOverlay;
            bool mapTransition = screen == SCREEN_MAP &&
                                 (prevScreen != SCREEN_MAP || modalClosed);

            // Any top-band overlay (a message popup) leaving the
            // glass earns a scrub: they are solid black blocks, and the
            // content that replaces them half-erases under a DU drive — the
            // band's ghost sat over the page until something else forced a
            // clear. Tracks what was actually DRAWN, so the popup turn cue
            // on the music/workout pages is covered too.
            static bool prevBand = false;
            const bool bandCleared = prevBand && !bandDrawnThisFrame;
            prevBand = bandDrawnThisFrame;

            refresh(screenChanged, fastInPage, listFast, wantClean, false,
                    mapTransition || bandCleared);
        }

        // The epdiy path managed panel rails by hand here — poweron before an
        // update, and a 1.5 s keep-alive so successive taps skipped the ~36 ms
        // power-up. EPD_Painter owns its rails around each paint, so there is
        // nothing to release.

        const bool busy = mapPrerenderStep();
        if (!busy) doba_lib::idle();

        // Block until an input interrupt or a redraw request, with a fallback
        // tick that still satisfies the slowest thing the loop owes anyone (the
        // 200 ms touch-poll backstop). Was a flat 30 ms delay, i.e. ~33
        // wakeups/s to discover there was nothing to do ~30 times out of 33.
        //
        // This only became worth fixing once light sleep worked: the SoC can
        // only sleep when BOTH cores are idle, and tickless idle sizes each
        // sleep by the shortest pending timer across them, so this task's tick
        // was capping every sleep on core 1 at 30 ms.
        xSemaphoreTake(uiWake, busy ? 0 : pdMS_TO_TICKS(UI_IDLE_TICK_MS));
    }
}

}  // namespace ui_dashboard


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
