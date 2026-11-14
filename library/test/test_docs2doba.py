"""docs2doba helpers: chapter prefixes, gzip copies, long-title chapters."""
import gzip
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import docs2doba  # noqa: E402


def test_a_chapter_starts_with_the_publication_number_or_a_short_title():
    assert docs2doba.short_title("ATP 4-02.11 Casualty Response, TCCC and First Aid (US Army)") == "ATP 4-02.11"
    assert docs2doba.short_title("Are You Ready  Guide to Citizen Preparedness (FEMA)") == "Are You Ready"
    assert docs2doba.short_title("The Grid-Down Field Guide") == "The Grid-Down Field Guide"


def test_a_gzipped_web_archive_copy_reads_as_text(tmp_path):
    p = tmp_path / "page.html"
    p.write_bytes(gzip.compress("<main><h1>Floods</h1><p>Turn around, don't drown.</p></main>".encode()))
    assert "Turn around" in docs2doba.read_text(str(p))


def test_chapters_of_a_long_titled_pdf_with_the_same_names_all_get_through(tmp_path):
    """Regression: a suffix after the 55-byte key never made it unique -> hang."""
    pymupdf = pytest.importorskip("pymupdf")
    doc = pymupdf.open()
    toc = []
    for i in range(4):
        p = doc.new_page()
        p.insert_text((72, 90), "Overview", fontsize=18)
        p.insert_text((72, 130), f"Body text of part {i}, long enough to keep.", fontsize=10)
        toc.append([1, "Overview", i + 1])
    doc.set_toc(toc)
    long = "A Very Long Publication Title That Goes On And On Past The Key Limit (Agency)"
    path = tmp_path / (long + ".pdf")
    doc.save(str(path))
    arts = docs2doba.read_pdf(str(path), long)
    assert len(arts) >= 4


def test_printed_contents_lists_and_bookmark_ids_do_not_reach_the_device(tmp_path):
    """US manuals: "23BFigures" chapters full of "Burns ........ 45" lines."""
    pymupdf = pytest.importorskip("pymupdf")
    doc = pymupdf.open()
    toc = []
    pages = [("23BFigures", "Figure 1-1. Check the pulse .................... 12"),
             ("208BChapter 1 Bleeding", "Press hard on the wound. See also Shock ........... 30"),
             ("209BChapter 2 Burns", "Cool the burn with water for twenty minutes."),
             ("210BChapter 3 Shock", "Lay the casualty flat and keep them warm.")]
    for i, (name, body) in enumerate(pages):
        p = doc.new_page()
        p.insert_text((72, 90), name[4:] if "B" in name[:4] else name, fontsize=18)
        p.insert_text((72, 130), body, fontsize=10)
        p.insert_text((72, 150), "More plain body text so the page reads as prose.", fontsize=10)
        toc.append([1, name, i + 1])
    doc.set_toc(toc)
    path = tmp_path / "ATP 9-99 Test.pdf"
    doc.save(str(path))
    arts = docs2doba.read_pdf(str(path), "ATP 9-99 Test")
    titles = [t for t, _ in arts]
    text = "\n".join(x for _, x in arts)
    assert not any("Figures" in t for t in titles), titles
    assert any(t.endswith("— Chapter 1 Bleeding") for t in titles), titles
    assert "........" not in text
    assert "Press hard" not in text           # a leader line goes whole
    assert "Cool the burn" in text


def test_broken_cp1251_font_maps_are_decoded():
    """Two kinds of PDF text layers seen in the wild: cp1251 bytes shown as
    Latin-1 (MON textbooks) and cp1251 bytes at U+017A + byte."""
    assert docs2doba.unshift("êіëüêіñòü", 0xFF) == "кількість"
    assert docs2doba.unshift("ɉɭɥɢ", 0x17A) == "Пули"
    words = " ".join(["êіëüêіñòü ðåãóëÿòîðíèõ ãåíіâ"] * 80)
    assert docs2doba.unreadable(words) and not docs2doba.unreadable(docs2doba.unshift(words, 0xFF))
