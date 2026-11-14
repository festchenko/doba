#pragma once
// sim: the FatFs drive table. The card is drive 0.
#include <stdint.h>
#include "esp_err.h"
#ifndef BYTE_DEFINED_SIM
#define BYTE_DEFINED_SIM
typedef uint8_t BYTE;
#endif
inline esp_err_t ff_diskio_get_drive(BYTE* out_pdrv) { *out_pdrv = 0; return ESP_OK; }
