#include "sd_card.h"
#include "diskio_impl.h"   // ff_diskio_get_drive

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <driver/gpio.h>

#include "ff.h"

#include "config.h"
#include "crash_report.h"
#include "diag.h"
#include "ride_state.h"
#include "sd_bus.h"
#include "usb_storage.h"

namespace sd_card {

namespace {

bool sdOk = false;
// Whether the last mount attempt found a card at all: it answered CMD0 but
// SD.begin() still failed — a card the FAT driver cannot read (unformatted, or
// a file system FatFs is not built for), which the UI tells from an empty slot.
bool cardAnswered = false;
// Set when a mount succeeds AFTER boot gave up; see consumeLateMount().
bool lateMount = false;

// Arduino SD/SPI recovery, under sdLock. The card stays powered across an
// ESP reset on this board. CMD0 has a valid CRC even if the previous session
// enabled CRC checking. This is NOT IDF's CMD52 issue (#14000): Arduino SD
// does not issue CMD52. Capture real wire responses instead of cardType(),
// which is always NONE after SD.begin() has cleaned up a failed mount.
struct SdProbe {
    uint8_t r1 = 0xff;
    uint8_t cmd8 = 0xff;
    uint32_t echo = 0;
    bool ready = false;
    uint32_t waitMs = 0;
};

SdProbe sdSpiForceIdle() {
    SdProbe probe;
    SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    digitalWrite(BOARD_LORA_CS, HIGH);
    digitalWrite(BOARD_SD_CS, HIGH);
    for (int i = 0; i < 10; ++i) SPI.transfer(0xFF);
    digitalWrite(BOARD_SD_CS, LOW);
    uint32_t start = millis();
    // Let any previous write finish before issuing reset. Yield between polls;
    // the caller's PM guard stays held across each yield.
    do {
        if (SPI.transfer(0xFF) == 0xFF) { probe.ready = true; break; }
        delay(1);
    } while (millis() - start < 500);
    probe.waitMs = millis() - start;
    auto command = [](const uint8_t* packet) {
        for (int i = 0; i < 6; ++i) SPI.transfer(packet[i]);
        uint8_t r1 = 0xff;
        for (int i = 0; i < 10; ++i) {
            r1 = SPI.transfer(0xff);
            if (!(r1 & 0x80)) break;
        }
        return r1;
    };
    static const uint8_t cmd0[] = {0x40, 0, 0, 0, 0, 0x95};
    probe.r1 = command(cmd0);
    digitalWrite(BOARD_SD_CS, HIGH);
    SPI.transfer(0xFF);
    if (probe.r1 == 1) {
        digitalWrite(BOARD_SD_CS, LOW);
        SPI.transfer(0xFF);
        static const uint8_t cmd8[] = {0x48, 0, 0, 1, 0xaa, 0x87};
        probe.cmd8 = command(cmd8);
        if (probe.cmd8 == 1)
            for (int i = 0; i < 4; ++i)
                probe.echo = (probe.echo << 8) | SPI.transfer(0xff);
        digitalWrite(BOARD_SD_CS, HIGH);
        SPI.transfer(0xFF);
    }
    SPI.endTransaction();
    return probe;
}

uint32_t g_mountGen = 0;
uint8_t g_sdDrive = 0xFF;   // the card's FatFs drive number (sdDrive())
bool mountLocked(bool logFailures) {
    // settleMs is waited BEFORE the attempt. The old schedule (three tries, 50 ms
    // apart) gave the card ~100 ms to come up and then declared it absent, which
    // on this board is simply too early: measured on a cold boot with a known-good
    // 32 GB SDHC, every probe inside the first ~1.5 s failed and the mount only
    // took at ~2 s (investigations/sd-boot-mount-2026-08-05.log). Boot therefore
    // ALWAYS missed the card and left the device in its "no SD" state until the
    // 30 s background retry rescued it — by which point map loading had
    // already been skipped for the session.
    //
    // The cost is paid only on failure: a card that answers immediately still
    // mounts on the first attempt with zero added delay, and a genuinely absent
    // card costs ~2.3 s of settle on top of SD.begin()'s own timeouts. Boot is
    // never blocked on the result.
    static const struct { uint32_t settleMs; uint32_t freq; } kAttempts[] = {
        // DOBA: the library and the maps READ a lot (a book page is a 64 KB
        // chunk, a map screen a dozen tiles). At 4 MHz the bus moves ~0.4 MB/s
        // and every page turn waits on it. SD cards in SPI mode are specified
        // to 25 MHz; 20 first, then the old safe clocks if the card or the
        // wiring will not have it.
        {0,    20000000},
        {100,  10000000},
        {400,  4000000},
        {800,  4000000},
        {1000, 1000000},   // last-ditch, slow clock for a marginal card
    };
    unsigned attempt = 0;
    bool answered = false;   // published once at the end: read lock-free
    for (auto& a : kAttempts) {
        ++attempt;
        uint32_t f = a.freq;
        if (a.settleMs) delay(a.settleMs);   // let the card/bus settle first
        uint32_t start = millis();
        diag::storageStage(2, attempt);
        SdProbe probe = sdSpiForceIdle();
        if (probe.r1 == 1) answered = true;
        if (logFailures)
            diag::log("sd probe: try=%u uptime=%lu ready=%d wait=%lu cmd0=0x%02x "
                      "cmd8=0x%02x echo=0x%08lx CS=%d/%d MISO=%d",
                      attempt, (unsigned long)start, probe.ready,
                      (unsigned long)probe.waitMs, probe.r1, probe.cmd8,
                      (unsigned long)probe.echo, digitalRead(BOARD_SD_CS),
                      digitalRead(BOARD_LORA_CS), digitalRead(BOARD_SPI_MISO));
        // max_files: the library default is 5 open files for the WHOLE
        // firmware; ten leaves room for map tiles, a book with its index files
        // and a diag flush at once.
        diag::storageStage(3, attempt, probe.r1);
        // The FatFs drive SD.begin() is about to take: the first free one
        // (sdcard_init() asks ff_diskio_get_drive() too). USB storage reads
        // and writes sectors through it; SDFS keeps its own copy private.
        BYTE drive = 0xFF;
        if (ff_diskio_get_drive(&drive) != ESP_OK) drive = 0xFF;
        bool mounted = SD.begin(BOARD_SD_CS, SPI, f, "/sd", 10);
        if (mounted) g_sdDrive = drive;
        diag::storageStage(mounted ? 4 : 5, attempt, probe.r1);
        diag::drainDriverLogs();
        if (mounted) {
            diag::log("sd mount: OK try=%u clock=%luHz duration=%lums type=%u size=%lluMB",
                      attempt, (unsigned long)f, (unsigned long)(millis() - start),
                      SD.cardType(), SD.cardSize() / (1024ULL * 1024ULL));
            cardAnswered = true;
            ++g_mountGen;
            return true;
        }
        if (logFailures) {
            // Deliberately NOT logging cardType()/cardSize() here. A previous
            // version did, to tell "card not talking" from "bad filesystem", but
            // that reading can never happen: SD.begin() unmounts, uninits and
            // sets _pdrv = 0xFF itself before returning false, and both accessors
            // early-return on that sentinel. They report NONE/0 unconditionally,
            // so the line only ever looked like a dead card. (Arduino-esp32
            // SD.cpp: begin() 38-42, cardType() 62, cardSize() 70.)
            diag::log("sd mount: FAILED try=%u clock=%luHz duration=%lums settle=%lu "
                      "(CMD0 idle=%d; later SD init/FAT failure still possible)",
                      attempt, (unsigned long)f, (unsigned long)(millis() - start),
                      (unsigned long)a.settleMs, probe.r1 == 1);
        }
        SD.end();
        diag::drainDriverLogs();
    }
    cardAnswered = answered;
    return false;
}

}  // namespace

uint32_t mountGeneration() { return g_mountGen; }
uint8_t sdDrive() { return g_sdDrive; }

bool begin() {
    sdLock();
    // SD and LoRa share the SPI bus; a floating LoRa CS corrupts SD traffic.
    pinMode(BOARD_LORA_CS, OUTPUT);
    digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_SD_CS, OUTPUT);
    digitalWrite(BOARD_SD_CS, HIGH);
    // Keep the chip selects driven in automatic light sleep. Do not latch
    // them with gpio_hold here: normal SPI traffic must still toggle CS.
    gpio_sleep_sel_dis((gpio_num_t)BOARD_SD_CS);
    gpio_sleep_sel_dis((gpio_num_t)BOARD_LORA_CS);
    SPI.begin(BOARD_SPI_SCLK, BOARD_SPI_MISO, BOARD_SPI_MOSI);
    sdOk = mountLocked(true);
    sdUnlock();
    if (!sdOk) {
        diag::log("sd: boot mount failed; diagnostics in RAM/flash; use 'diag'");
        diag::checkpoint("boot SD mount failed");
        return false;
    }
    diag::log("sd: ready, %lu MB free", (unsigned long)sdFreeMB());
    return true;
}

// A computer took the card over USB: unmount Doba's own FatFs volume so that
// nothing in the firmware can read stale tables or write while the host's
// filesystem driver writes too (two writers on one FAT is how cards get
// corrupted). The SD driver underneath stays up: USB storage reads and writes
// raw sectors through it. Every SD.open() fails cleanly until remount(), and
// the mount generation moves on so caches drop their open files.
bool lendToHost() {
    if (g_sdDrive == 0xFF) return false;
    sdLock();
    char drv[3] = {(char)('0' + g_sdDrive), ':', 0};
    const FRESULT r = f_mount(nullptr, drv, 0);
    ++g_mountGen;
    sdUnlock();
    diag::log("sd: lent to the USB host (FatFs volume %s unmounted: %d)", drv, (int)r);
    return r == FR_OK;
}

bool remount(const char* why) {
    sdLock();
    SD.end();
    sdOk = mountLocked(true);
    sdUnlock();
    diag::log("sd: remount (%s) -> %s", why, sdOk ? "ok" : "FAILED");
    return sdOk;
}

void retryMountIfNeeded() {
    // Nothing to do when it's mounted, and nothing we're allowed to do while a
    // host computer owns the card over USB.
    if (sdOk || usb_storage::hostActive()) return;

    static uint32_t lastTry = 0;
    static uint16_t tries = 0;
    uint32_t now = millis();
    if (lastTry != 0 && now - lastTry < 30000) return;
    lastTry = now;

    sdLock();
    bool ok = mountLocked(tries < 3);   // stop logging once it's clearly absent
    sdUnlock();

    if (ok) {
        sdOk = true;
        // Deliver crash evidence BEFORE flagging the late mount: the UI task
        // reacts to that flag by exposing the card over USB.
        crash_report::onSdMounted("late mount");
        lateMount = true;
        diag::log("sd: card back after %u retr%s", (unsigned)tries + 1,
                  tries ? "ies" : "y");
        tries = 0;
    } else {
        if (tries == 3) diag::log("sd: still absent — retrying quietly from here");
        if (tries < 0xFFFF) tries++;
    }
}

bool consumeLateMount() {
    if (!lateMount) return false;
    lateMount = false;
    return true;
}

// The card is "unavailable" to the firmware while a host computer owns it.
bool sdMounted() { return sdOk && !usb_storage::hostActive(); }

// A card lent to a USB host is still a card: only a failed mount counts here.
uint8_t sdCardState() {
    if (sdOk) return SD_CARD_OK;
    return cardAnswered ? SD_CARD_UNREADABLE : SD_CARD_NONE;
}

uint32_t sdFreeMB() {
    if (!sdOk) return 0;
    sdLock();
    uint32_t mb = (uint32_t)((SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL));
    sdUnlock();
    return mb;
}

}  // namespace sd_card
