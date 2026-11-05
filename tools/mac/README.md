# Building on a Mac, all on one external SSD

Copy this folder's files and `doba-src.tgz` (a tarball of this repo) to the
SSD's build folder, e.g. `/Volumes/<SSD>/doba-build/`, then double-click:

- `doba_maps.command` — maps + heights for a region (default Groenlo)
- `doba_library.command` — download the Kiwix ZIMs and convert them
- `doba_runner.command` — runs any `jobs/*.job` dropped on the SSD, in the
  background (`logs/<job>.log`); close its Terminal window to stop it

`doba_env.sh` installs its own Python 3.12 and Java 21 under `~/.doba-tools`
(no Homebrew), keeps the Mac awake while jobs run, and puts all data under
`data/` on the SSD.

## The simulator

`doba_sim.command` (with `doba-fw-src.tgz`, a tarball of `firmware/` minus
build output, next to it): builds the real firmware as a Mac program
(`sim/build.sh`, Apple clang), then opens `http://127.0.0.1:8090` —
`sim/simview.py`, the panel live in a browser. Click to tap; HOME / BOOT;
GPS places (Groenlo, the North Sea, Kyiv, Odesa…), a 5 km/h walk, a mesh
neighbour saying hi, and the device console (`screen points`, `dem`, …).
The sim's card points at this SSD's `data/`: `tiles/<region>` as the map,
`dem-pack` as heights and depths, `lib` as the library.
