// DOBA: where the battery goes, and how long it will last.
//
// The fuel gauge (BQ27220) measures one thing: the current out of the cell,
// the whole board together. It cannot say how much of it is the GPS. So the
// split is an ESTIMATE: what each part is doing right now (GPS on, radio on,
// light level, how often the screen repaints) times what that part typically
// draws, with the processor and the rest taking whatever is left of the
// measured total. The forecast itself uses the measurement only.
#pragma once
#include <stdint.h>

namespace doba_batt {

// From the battery task, every 30 s: the gauge's reading.
void sample(uint16_t mv, int16_t ma, uint8_t soc, uint16_t remainingMah, bool charging);
// From the UI: one panel repaint happened.
void notePaint();

struct Part {
    const char* name;   // English, through TR() when drawn
    float ma;           // estimated draw now
};
constexpr int MAX_PARTS = 6;

struct Report {
    bool have;          // at least one reading
    bool charging;      // on USB / charging: no forecast
    uint8_t soc;
    uint16_t mv, remainingMah;
    float nowMa;        // last reading, discharge positive
    float avgMa;        // smoothed discharge (about the last 10 minutes)
    float hoursLeft;    // remainingMah / avgMa; < 0 when unknown
    int parts;
    Part part[MAX_PARTS];   // largest first
};
void report(Report& r);

}  // namespace doba_batt
