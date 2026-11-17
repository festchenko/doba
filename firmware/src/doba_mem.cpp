#include "doba_mem.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "doba_library.h"
#include "topo_layer.h"

namespace doba_mem {

void report(const char* why) {
    Serial.printf("[mem] %s: internal %u free (largest %u), psram %u free (largest %u)\n", why,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

void dropCaches() {
    topo::trim();
    doba_lib::dropCaches();
}

void* big(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p || !n) return p;
    Serial.printf("[mem] no room for %u bytes: letting the caches go\n", (unsigned)n);
    dropCaches();
    p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    report(p ? "after the caches went" : "STILL NO ROOM");
    return p;
}

}  // namespace doba_mem
