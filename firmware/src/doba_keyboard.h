#pragma once

// DOBA: on-screen keyboard for the touch e-ink panel. Latin, Russian and
// Ukrainian letters, digits and symbols; text is UTF-8. Drawn in the bottom
// part of the portrait screen; one tap = one key.

#include <cstddef>
#include <cstdint>

namespace kbd {

enum Layout : uint8_t { EN = 0, RU, UA, SYM, LAYOUT_COUNT };

enum Action : uint8_t {
    NONE = 0,   // missed every key
    TYPED,      // a character went in (or space)
    DELETED,
    ENTER,      // the action key (SEND / SEARCH)
    CHANGED,    // shift / layout / symbols switched
};

struct Keyboard {
    char text[256] = {};     // UTF-8, NUL-terminated
    uint8_t layout = EN;
    uint8_t lastLetters = EN;   // where SYM goes back to
    bool shift = false;
    int top = 560;           // y of the first key row (portrait px)
    const char* enterLabel = "SEND";

    void clear() { text[0] = 0; shift = false; }
    void render(uint8_t* fb) const;
    Action tap(int x, int y);
    int height() const;      // px from `top` to the bottom of the panel
};

// Removes the last UTF-8 character of s.
void popUtf8(char* s);

}  // namespace kbd
