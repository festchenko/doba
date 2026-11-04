#pragma once
// DOBA: host preview stand-in (settings read back as defaults).
#include <cstddef>
#include <cstdint>
class Preferences {
public:
    bool begin(const char*, bool = false, const char* = nullptr) { return true; }
    void end() {}
    int32_t getInt(const char*, int32_t d = 0) { return d; }
    size_t putInt(const char*, int32_t) { return 4; }
};
