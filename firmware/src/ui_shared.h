#pragma once

// What ui_dashboard.cpp (the UI task, the panel, the console) and ui_map.cpp
// (the map screen) share. Internal to those two files.

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

namespace uid {

enum Screen { SCREEN_MAP, SCREEN_GPSDEBUG,
              // DOBA: the launcher, comms and library (doba_ui.h).
              SCREEN_HOME, SCREEN_COMMS, SCREEN_COMPOSE, SCREEN_LIBRARY,
              SCREEN_ARTICLE, SCREEN_SETUP, SCREEN_SETUP_NAME, SCREEN_POINTS,
              SCREEN_POINT, SCREEN_POINT_NEW, SCREEN_MAPOPTS, SCREEN_SOS, SCREEN_SUN, SCREEN_MORE, SCREEN_BATTERY };

extern Screen screen;

// ---- ui_dashboard.cpp: the panel, input, the task --------------------------------
extern uint8_t* shadowFb;            // what is on the glass
extern size_t fbSize;
extern bool dbgTiming;               // console `timing`: log every frame
extern bool powerOverlay;            // the power sheet is up
extern bool g_offByPowerKey;
extern volatile bool touchIrq, touchWasDown;
extern volatile int16_t touchStartX, touchStartY, touchNowX, touchNowY;
bool refresh(bool screenChanged, bool fastInPage, bool listFast,
             bool forceClean, bool gc16, bool fullFlash);
extern bool forceDraw;
bool meshNoticeShown();
void drawMeshNotice(uint8_t* fb);
void ackBusy();

// ---- ui_map.cpp: the map screen ----------------------------------------------------
extern float mapMpp;                 // zoom, ground metres a pixel
extern bool mapTopoActive;           // the last frame had a topo pack under it
extern int zoomHeld;                 // the zoom button showing pressed (0, 1 in, 2 out)
extern float mapPanE, mapPanN;       // dragged off the position, metres east/north
extern float mapTestHeading;         // console `heading`
extern MapBaseCache mapCache;
extern bool mapCacheOn;
extern uint32_t mapCacheHits, mapCacheMisses, mapPrerenders, mapPrerendersCut;
float mapMaxMpp();
bool mapPanned();
void buildMapScreenData(const DeviceState& s, MapScreenData& map, bool finalFrame = false);
void renderMapScreen(const DeviceState& s, uint8_t* fb);
bool mapPrerenderStep();             // idle work; true when it did some
bool mapDrag(int x0, int y0, int x1, int y1);
bool mapLongPress(int x, int y);
void renderFarewell(uint8_t* fb);
void farewellTest();
bool mapLiveDrag();                  // a frame of the map under a moving finger; true if painted

}  // namespace uid
