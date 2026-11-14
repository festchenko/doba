// `otp-sim card-tool`: host-side helpers that build SD card contents with the
// firmware's own code, for sim/client/otpsim/cards.py.
//
//   otp-sim card-tool h3 S W N E
//       the res-6 H3 cells (map tiles) covering the box, one per line:
//       "<id> <s> <w> <n> <e>" (the cell's own bounding box)
#include <FS.h>
#include <SD.h>

#include "h3shim.h"
#include "sim.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int h3Cells(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "card-tool h3 S W N E\n"); return 1; }
    const double s = atof(argv[0]), w = atof(argv[1]), n = atof(argv[2]), e = atof(argv[3]);
    std::vector<uint64_t> cells(200000);
    const int k = h3_covering_cells(s, w, n, e, cells.data(), (int)cells.size());
    for (int i = 0; i < k; ++i) {
        char id[24];
        double cs, cw, cn, ce;
        h3_cell_id(cells[i], id, sizeof id);
        h3_cell_bbox(cells[i], &cs, &cw, &cn, &ce);
        printf("%s %.7f %.7f %.7f %.7f\n", id, cs, cw, cn, ce);
    }
    return 0;
}

}  // namespace

int sim::cardTool(int argc, char** argv) {
    const std::string what = argc > 0 ? argv[0] : "";
    if (what == "h3") return h3Cells(argc - 1, argv + 1);
    fprintf(stderr, "otp-sim card-tool h3 S W N E\n");
    return 1;
}
