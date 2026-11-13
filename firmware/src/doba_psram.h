// DOBA: a fixed array that lives in PSRAM instead of internal DRAM.
//
// A plain `static T arr[N]` lands in .bss, which on this build is INTERNAL
// RAM — and internal RAM is what every FreeRTOS task stack is carved from.
// The Doba additions (library tables, places, points, map overlays) put about
// 30 KB of such arrays there, and on the real board that left too little for
// the last task setup() creates: the UI task's 12 KB stack. xTaskCreate failed
// silently, and the panel sat on the boot screen for ever while GPS, mesh and
// the console logger ran on behind it. The simulator has no such limit, so it
// never showed.
//
// PsArr<T, N> allocates zeroed (exactly what .bss gave) from PSRAM on first
// touch and then behaves as a T*: arr[i], arr + k, f.read(arr, n) all work.
// Only for plain data — T is never constructed, only zero-filled.
#pragma once
#include <stddef.h>
#include <stdlib.h>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

template <typename T, size_t N>
class PsArr {
public:
    operator T*() { return get(); }
    operator const T*() const { return const_cast<PsArr*>(this)->get(); }
    T* data() { return get(); }
    static constexpr size_t size() { return N; }

private:
    T* get() {
        if (!p_) {
#ifdef ARDUINO
            p_ = (T*)heap_caps_calloc(N, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!p_) p_ = (T*)calloc(N, sizeof(T));
#else
            p_ = (T*)calloc(N, sizeof(T));
#endif
        }
        return p_;
    }
    T* p_ = nullptr;
};
