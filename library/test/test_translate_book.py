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


def test_every_link_in_a_twin_leads_to_a_page_of_the_twin(tmp_path):
    """A link names its target as the source page did, which is not always the
    target's own title; translated as it stood, it came out differently from
    the title in the twin's index, and the link led nowhere (QA 18 Nov:
    water_uk 63 %, disaster_uk 49 % dead). With a translator that gives every
    different string a different word, the twin may have no more dead links
    than its source."""
    pytest.importorskip("libzim")
    import re
    import zlib
    sys.path.insert(0, LIB)
    from dobafold import key
    zim = str(tmp_path / "t.zim")
    subprocess.run([sys.executable, os.path.join(HERE, "make_test_zim.py"), zim], check=True, capture_output=True)
    src = str(tmp_path / "field")
    subprocess.run([sys.executable, os.path.join(LIB, "zim2doba.py"), zim, src, "--name", "Field"],
                   check=True, capture_output=True)
    out = str(tmp_path / "field_uk")
    subprocess.run([sys.executable, os.path.join(LIB, "translate_book.py"), src, out, "--fake-strict"],
                   check=True, capture_output=True)

    def dead_links(book):
        idx = open(os.path.join(book, "index.dti"), "rb").read()
        n = struct.unpack_from("<I", idx, 8)[0]
        keys = {idx[32 + i * 128:32 + i * 128 + 56].rstrip(b"\0") for i in range(n)}
        data = open(os.path.join(book, "a0000.dta"), "rb").read()
        links = dead = 0
        for i in range(n):
            off, ln = struct.unpack_from("<II", idx, 32 + i * 128 + 116)
            for m in re.finditer("\x01([^\x01\x02\x03]*)\x02([^\x01\x02\x03]*)\x03",
                                 zlib.decompress(data[off:off + ln]).decode()):
                if m.group(2) and not m.group(2).startswith("#"):
                    links += 1
                    dead += key(m.group(2)).rstrip(b"\0") not in keys
        return links, dead

    links, dead = dead_links(out)
    assert links > 0
    assert dead <= dead_links(src)[1], "%d of %d links lead nowhere" % (dead, links)


def test_a_long_title_is_translated_whole(tmp_path):
    """The index keeps a title cut at 56 bytes with '…'; the twin's title was the
    translation of that stub ('Ноти … Res ...'), and a menu row naming the whole
    title opened nothing (QA 18 Nov). The twin is keyed by the translation of
    the whole title, from the article's own first line."""
    import zlib
    sys.path.insert(0, LIB)
    from dobafold import key, display
    title = "Ropes for Mountaineering Camping Climbing Utility Rescue Etc"
    src = tmp_path / "knots"
    src.mkdir()
    z = zlib.compress(("T" + title + "\nPTie it well.").encode(), 9)
    (src / "a0000.dta").write_bytes(z)
    rec = struct.pack("<56s56sHHIII", key(title), display(title), 0, 0, 0, len(z), 0)
    (src / "index.dti").write_bytes(struct.pack("<8sIII12s", b"DOBAIDX1", 1, 128, 1, b"\0" * 12) + rec)
    (src / "info.txt").write_text("name=Knots\n", encoding="utf-8")
    assert display(title).decode().endswith("…")
    out = str(tmp_path / "knots_uk")
    subprocess.run([sys.executable, os.path.join(LIB, "translate_book.py"), str(src), out, "--fake"],
                   check=True, capture_output=True)
    idx = open(os.path.join(out, "index.dti"), "rb").read()
    assert idx[32:88].rstrip(b"\0") == key("Укр " + title).rstrip(b"\0")


def test_knots_are_knots_not_music():
    """Argos turned "knots" into "ноти" (musical notes) all through knots_uk."""
    sys.path.insert(0, LIB)
    from translate_book import glossary
    assert glossary("Knots for Mountaineering", "Ноти для гірського") == "Вузли для гірського"
    assert glossary("Tie the knot", "Зв'яжіть ноту") == "Зв'яжіть вузол"
    assert glossary("Sheet music", "Ноти") == "Ноти"   # only where the English has the word
