// Big buffers in PSRAM that give way to each other.
//
// The map keeps a lot of PSRAM as caches (the topo canvas, decoded tiles, index
// pages: about 3 MB) and keeps it after you leave the map. The library and the
// home screen then asked for theirs, did not get it, and quietly drew nothing:
// a gap where a picture should be, no topo lines behind the tiles (17 Nov).
// big() asks once; when that fails it lets the caches go and asks again.
#pragma once

#include <stddef.h>

namespace doba_mem {

// PSRAM for a buffer the caller frees with heap_caps_free(). nullptr only when
// even with every cache let go there is no room.
void* big(size_t n);

// Lets every cache go now (the map's canvas, tiles and index pages, the
// library's pictures). Each fills again as it is used.
void dropCaches();

// One console line: free and largest block, internal and PSRAM.
void report(const char* why);

}  // namespace doba_mem
