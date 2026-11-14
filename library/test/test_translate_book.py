"""translate_book.py: a twin book with its own index and a twin.map that pairs
every source article with its translation."""
import os
import struct
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(HERE, "..")


def test_a_twin_book_pairs_every_article(tmp_path):
    pytest.importorskip("libzim")
    zim = str(tmp_path / "t.zim")
    subprocess.run([sys.executable, os.path.join(HERE, "make_test_zim.py"), zim], check=True, capture_output=True)
    src = str(tmp_path / "field")
    subprocess.run([sys.executable, os.path.join(LIB, "zim2doba.py"), zim, src, "--name", "Field"],
                   check=True, capture_output=True)
    out = str(tmp_path / "field_uk")
    subprocess.run([sys.executable, os.path.join(LIB, "translate_book.py"), src, out, "--fake"],
                   check=True, capture_output=True)
    m = open(os.path.join(out, "twin.map"), "rb").read()
    assert m[:8] == b"DOBATW1\0"
    n = struct.unpack_from("<I", m, 8)[0]
    assert n > 0 and len(m) == 12 + n * 132
    src_count = struct.unpack_from("<I", open(os.path.join(src, "index.dti"), "rb").read(), 8)[0]
    twin_count = struct.unpack_from("<I", open(os.path.join(out, "index.dti"), "rb").read(), 8)[0]
    assert twin_count == src_count
    info = open(os.path.join(out, "info.txt"), encoding="utf-8").read()
    assert "twin_of=field" in info and "lang=uk" in info
