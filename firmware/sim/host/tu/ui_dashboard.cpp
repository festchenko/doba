// src/ui_dashboard.cpp as it is, plus read-only hooks for the control channel
// that need its file-local state. build.sh compiles this TU in place of
// src/ui_dashboard.cpp (and rebuilds it whenever that file changes).
//
// Hooks here only READ: the UI task owns this state; the control thread
// takes a snapshot that may be one frame stale, as a rider glancing at the
// panel would.
#include "../../../src/ui_dashboard.cpp"

#include <sstream>

#include "json.h"

