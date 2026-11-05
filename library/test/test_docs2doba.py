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
