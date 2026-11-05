"""dempack round trip and size on a synthetic landscape."""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import dempack  # noqa: E402


def landscape(n):
    k = np.linspace(0, 1, n)
    x, y = np.meshgrid(k, k)
    return (40 + 300 * np.exp(-((x - 0.4) ** 2 + (y - 0.6) ** 2) * 12)
            + 20 * np.sin(x * 40) * np.cos(y * 30)).astype(np.float32)


def test_round_trip_every_block():
    h = landscape(1200)
    data = dempack.encode(h, 52, 6)
    rng = np.random.default_rng(1)
    for i, j in [(0, 0), (1199, 1199), (99, 100), (600, 1150)] + [tuple(p) for p in rng.integers(0, 1200, (40, 2))]:
        assert dempack.decode_sample(data, i, j) == int(np.rint(h[i, j]))


def test_a_degree_is_about_a_megabyte_or_less():
    data = dempack.encode(landscape(1200), 52, 6)
    assert len(data) < 1.6e6, len(data)


def test_names():
    assert dempack.name_for(52, 6) == "N52E006.dem"
    assert dempack.name_for(-3, -70) == "S03W070.dem"
