#!/usr/bin/env python3
"""Round-trip test for the .dtp tile pack format."""
import os
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from tilepack import (HEADER, ENTRY, PackLibrary, PackReader, PackWriter,  # noqa
                      pack_4bpp, unpack_4bpp, pack_path)


def main():
    rng = np.random.default_rng(1)
    tiles = {}
    with tempfile.TemporaryDirectory() as d:
        p = pack_path(d, 8, 132, 84)
        w = PackWriter(p, 8, 132, 84)
        for z in (9, 12, 16):
            s = 2 ** (z - 8)
            for k in range(20):
                x = 132 * s + int(rng.integers(s))
                y = 84 * s + int(rng.integers(s))
                if k % 5 == 0:
                    t = np.full((256, 256), int(rng.integers(16)), np.uint8)
                else:
                    t = rng.integers(0, 16, (256, 256), dtype=np.uint8)
                tiles[(z, x, y)] = t
        for (z, x, y), t in tiles.items():
            w.add(z, x, y, t)
        w.close()

        r = PackReader(p)
        assert r.count == len(tiles), (r.count, len(tiles))
        assert (r.cell_z, r.cell_x, r.cell_y) == (8, 132, 84)
        for key, t in tiles.items():
            got = r.get(*key)
            assert got is not None and np.array_equal(got, t), key
        assert r.get(16, 0, 0) is None
        solid = sum(1 for e in r.entries if e[1] & 1)
        lib = PackLibrary(d)
        k0 = next(iter(tiles))
        assert np.array_equal(lib.get(*k0), tiles[k0])
        size = os.path.getsize(p)
        assert size == HEADER.size + ENTRY.size * r.count + sum(e[6] for e in r.entries)

    t = np.arange(256 * 256, dtype=np.uint32).reshape(256, 256) % 16
    assert np.array_equal(unpack_4bpp(pack_4bpp(t.astype(np.uint8))), t)
    raw = pack_4bpp(np.array([[3, 12] * 128] * 256, np.uint8))
    assert raw[0] == 0x3C  # high nibble = left pixel
    print(f"ok: {len(tiles)} tiles, {solid} solid, layout verified")


if __name__ == "__main__":
    main()
