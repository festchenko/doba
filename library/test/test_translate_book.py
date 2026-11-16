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


def test_the_links_of_an_article_survive_its_translation(tmp_path):
    """The translation dropped every link (the UA side of Field medicine had
    none, 16 Nov). Each article with links keeps them: in its text, or in the
    "Див. також" line at its end — and each points at a title of the twin."""
    pytest.importorskip("libzim")
    import zlib
    zim = str(tmp_path / "t.zim")
    subprocess.run([sys.executable, os.path.join(HERE, "make_test_zim.py"), zim], check=True, capture_output=True)
    src = str(tmp_path / "field")
    subprocess.run([sys.executable, os.path.join(LIB, "zim2doba.py"), zim, src, "--name", "Field"],
                   check=True, capture_output=True)
    out = str(tmp_path / "field_uk")
    subprocess.run([sys.executable, os.path.join(LIB, "translate_book.py"), src, out, "--fake"],
                   check=True, capture_output=True)
    m = open(os.path.join(out, "twin.map"), "rb").read()
    n = struct.unpack_from("<I", m, 8)[0]
    src_dta = open(os.path.join(src, "a0000.dta"), "rb").read()
    out_dta = open(os.path.join(out, "a0000.dta"), "rb").read()
    twin_titles = {m[12 + i * 132 + 76:12 + i * 132 + 132].rstrip(b"\0").decode() for i in range(n)}
    with_links = 0
    for i in range(n):
        ch, _, off, ln, p, l = struct.unpack_from("<HHIIII", m, 12 + i * 132)
        s = zlib.decompress(src_dta[off:off + ln]).decode()
        if "\x01" not in s:
            continue
        with_links += 1
        t = zlib.decompress(out_dta[p:p + l]).decode()
        assert "\x01" in t, t[:300]
        for target in [x.split("\x03")[0] for x in t.split("\x02")[1:]]:
            # a twin title, not the English one (a target with no words to
            # translate, such as a number, stays as it is)
            assert target.startswith("Укр ") or not __import__("re").search("[A-Za-z]{2}", target), target
    assert with_links > 0
