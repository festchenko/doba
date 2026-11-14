#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// The SD card (SPI) is shared by the ride recorder (writes FIT 1 Hz), the UI
// task (reads map tiles on demand while rendering), the BLE server (saves
// downloaded maps/firmware), diag logging, and route storage. Concurrent SPI
// transactions from different tasks corrupt the card. Every unit of SD work
// (a file open->io->close, or a directory scan) must be wrapped in
// sdLock()/sdUnlock(). Created in setup() before any task starts; the guards
// no-op until then. Recursive so a locked helper can call another safely.

// NOT only the SD card: the SX1262 LoRa module hangs off the same MISO/MOSI/SCK
// with its own chip select, so lora_radio.cpp takes this lock for every radio
// transfer too. That is also why it never holds it across a transmission — a
// LongFast packet is seconds of air time, and the recorder needs the bus at 1 Hz.

// These also hold light sleep off for the duration. The SD path runs through
// Arduino's SPIClass, which — unlike ESP-IDF's spi_master — takes no PM lock, so
// a light sleep during a command/retry can interrupt the protocol. Keep the
// whole operation protected. Since every SD access is already wrapped
// here, this is the one place that needs to know. No-op without PM.
// See power_mgmt::busyAcquire().

#include "power_mgmt.h"

extern SemaphoreHandle_t g_sdMutex;

// Mutex FIRST, then the PM lock. The other order held light sleep off for the
// whole time a task merely WAITED for the bus — during a 2 MB OTA write or a
// map save, every queued SD user was pinning the CPU awake before doing any
// work. The PM lock only needs to cover actual bus transactions, and those
// only happen while the mutex is held.
#ifndef OTP_SIM
// Who made whom wait for the bus (sd_bus.cpp): a wait behind another task is
// counted per holder, and a long one by the UI task is logged with the
// holder's name — the evidence for what keeps a tap waiting.
void sdNoteWait(uint32_t waitedMs, TaskHandle_t holder);
void sdWaitReport();
#endif

inline void sdLock()   {
    if (g_sdMutex) {
#ifndef OTP_SIM
        TaskHandle_t holder = xSemaphoreGetMutexHolder(g_sdMutex);
        if (holder && holder != xTaskGetCurrentTaskHandle()) {
            const TickType_t t0 = xTaskGetTickCount();
            xSemaphoreTakeRecursive(g_sdMutex, portMAX_DELAY);
            sdNoteWait((uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS), holder);
        } else
#endif
        xSemaphoreTakeRecursive(g_sdMutex, portMAX_DELAY);
    }
    power_mgmt::busyAcquire();
}
inline void sdUnlock() {
    power_mgmt::busyRelease();
    if (g_sdMutex) xSemaphoreGiveRecursive(g_sdMutex);
}
