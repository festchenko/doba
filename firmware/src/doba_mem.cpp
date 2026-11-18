#include "doba_mem.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "doba_library.h"
#include "doba_ui.h"
#include "topo_layer.h"

namespace doba_mem {

void report(const char* why) {
    Serial.printf("[mem] %s: internal %u free (largest %u), psram %u free (largest %u, lowest %u)\n", why,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}

void dropCaches() {
    topo::trim();
    doba_lib::dropCaches();
    doba_ui::dropHomeCache();
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

void* grow(void* p, size_t n) {
    void* q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
    if (q || !n) return q;
    Serial.printf("[mem] no room to grow to %u bytes: letting the caches go\n", (unsigned)n);
    dropCaches();
    q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
    report(q ? "after the caches went" : "STILL NO ROOM");
    return q;
}

}  // namespace doba_mem
