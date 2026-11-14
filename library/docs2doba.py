#!/usr/bin/env python3
"""A folder of documents into a Doba book: field manuals, your own files.

PDF, EPUB, HTML, Markdown and plain text, every file under the folder. A PDF
with bookmarks is split into one article per chapter (and section), plus a
contents article linking to them, so "shelter" finds the chapter, not just
the manual. Text only: pictures and tables are not carried.

    pip3 install pymupdf
    python3 docs2doba.py ~/Manuals out/manuals --name "Field manuals" \\
        --source "US Army / FEMA · public domain"
    python3 docs2doba.py ~/MyDocs out/mine --name "My files" --source "Own files"

Then copy out/<book> to /doba/lib/<book> on the card
(tools/prepare-sd-mac.sh copy-lib out).
"""
import argparse
import html
import os
import urllib.parse
import io
import re
import struct
import sys
import zipfile
import zlib

import dobapic
from dobafold import KEY_BYTES, display, fold, key
import zim2doba
from zim2doba import HDR, LINK_A, LINK_B, LINK_C, REC, to_text

MAX_ARTICLE = 1_400_000          # bytes of text; the device holds 1.5 MB inflated
KINDS = set("THSPBI")
TITLES_FROM_FILENAMES = False    # --titles-from-filenames: the file name is the title
ALIASES = []                     # (article title, [other titles]) gathered by the readers
IMAGES = []                      # (name, .dgi bytes): pictures for <out>/img/
IMAGE_NAMES = {}                 # .dgi bytes -> name (the same picture is stored once)
WITH_IMAGES = True               # --no-images turns them off
IMG_W, IMG_MAX_H = dobapic.IMG_W, dobapic.IMG_MAX_H


# --- pictures --------------------------------------------------------------------

def to_dgi(img):
    """A PIL image -> 'name w h' of a picture stored once for this book."""
    blob, w, h = dobapic.encode(img)
    name = IMAGE_NAMES.get(blob)
    if not name:
        name = f"{len(IMAGES):05d}"
        IMAGE_NAMES[blob] = name
        IMAGES.append((name, blob))
    return f"{name} {w} {h}"


def _merge(rects, pad):
    """Union rectangles that touch (within pad) until none do."""
    rects = [r for r in rects]
    changed = True
    while changed:
        changed = False
        out = []
        for r in rects:
            for i, o in enumerate(out):
                if (r.x0 - pad < o.x1 and o.x0 - pad < r.x1 and r.y0 - pad < o.y1 and o.y0 - pad < r.y1):
                    out[i] = o | r
                    changed = True
                    break
            else:
                out.append(r)
        rects = out
    return rects


def page_figures(page, logos):
    """Figure areas on a PDF page: placed pictures plus clusters of vector
    drawing, without page frames, rules and repeated logos."""
    import pymupdf
    pr = page.rect
    top, bottom = pr.y0 + pr.height * 0.06, pr.y1 - pr.height * 0.05
    found = []
    for info in page.get_image_info(xrefs=True):
        r = pymupdf.Rect(info["bbox"]) & pr
        if r.is_empty or r.width < 40 or r.height < 30 or info.get("xref") in logos:
            continue
        found.append(r)
    lines = []
    try:
        drawings = page.get_drawings()
    except Exception:
        drawings = []
    for d in drawings:
        r = pymupdf.Rect(d["rect"]) & pr
        if r.is_empty and not (r.width or r.height):
            continue
        if r.width > pr.width * 0.6 and r.height > pr.height * 0.5:
            continue                                    # a frame around the page
        if r.height < 3 and r.width > pr.width * 0.45:
            continue                                    # a full-width rule
        if r.y1 < top or r.y0 > bottom:
            continue                                    # running head / foot
        lines.append(r)
    texts = [pymupdf.Rect(b["bbox"]) for b in page.get_text("dict", flags=0)["blocks"] if b.get("type") == 0]
    for r in _merge(lines, 6):
        if r.width < 60 or r.height < 40:
            continue
        # A frame round one paragraph (WARNING, NOTE) stays text.
        inner = [t for t in texts if (t & r).get_area() > t.get_area() * 0.5]
        if len(inner) <= 2 and sum(t.get_area() for t in inner) > r.get_area() * 0.2 \
                and not any((f & r).get_area() > 0 for f in found):
            continue
        found.append(r)
    figs = [r for r in _merge(found, 4) if r.get_area() < pr.get_area() * 0.85]
    return figs


def render_figure(page, r):
    import pymupdf
    from PIL import Image
    zoom = min(IMG_W / r.width, 2.5)
    if r.height * zoom > IMG_MAX_H:
        zoom = IMG_MAX_H / r.height
    pix = page.get_pixmap(matrix=pymupdf.Matrix(zoom, zoom), clip=r, colorspace=pymupdf.csGRAY, alpha=False)
    return Image.frombytes("L", (pix.width, pix.height), pix.samples)


# --- text helpers ----------------------------------------------------------------

def clean(s):
    s = s.replace("­", "").replace("\x00", "")
    for c in (LINK_A, LINK_B, LINK_C):
        s = s.replace(c, "")
    return " ".join(s.split())


def merge_broken(lines):
    """A paragraph cut by a page turn (no full stop, the next starts in lower
    case) is one paragraph again; pictures that fell in between follow it."""
    out = []
    i = 0
    while i < len(lines):
        kind, t = lines[i]
        if kind == "P":
            j = i + 1
            pics = []
            while True:
                k = j
                while k < len(lines) and lines[k][0] == "I":
                    k += 1
                if (k < len(lines) and lines[k][0] == "P" and t and t.rstrip()[-1:] not in ".!?:;)\"”"
                        and lines[k][1][:1].islower()):
                    pics += lines[j:k]
                    t = t[:-1] + lines[k][1] if t.endswith("-") and t[-2:-1].isalpha() else t + " " + lines[k][1]
                    j = k + 1
                else:
                    break
            out.append(("P", t))
            out += pics
            i = j
            continue
        out.append((kind, t))
        i += 1
    return out


# A printed table of contents, list of figures or tables: "Burns ........ 45".
LEADERS = re.compile(r"(\.\s?){8,}|…{3,}")
# Front-matter lists as chapters: the book's own contents article replaces them.
FRONT_LISTS = re.compile(r"(?i)^((table of )?contents|(list of )?(figures|tables|illustrations)"
                         r"|figures and tables|tables and figures)$")


def lines_to_text(title, lines):
    lines = merge_broken([(k, clean(t)) for k, t in lines if not (k != "I" and LEADERS.search(t))])
    lines = [(k, t) for k, t in lines if not (k != "I" and LEADERS.search(t))]
    out = ["T" + clean(title)]
    for kind, t in lines:
        t = clean(t)
        if t and kind in KINDS:
            out.append(kind + t)
    return "\n".join(out)


def split_long(title, text):
    """Articles over the device's limit become 'Title (2)', 'Title (3)'..."""
    data = text.encode("utf-8")
    if len(data) <= MAX_ARTICLE:
        return [(title, text)]
    parts, cur, size, n = [], [], 0, 1
    for line in text.split("\n")[1:]:
        b = len(line.encode("utf-8")) + 1
        if cur and size + b > MAX_ARTICLE - 200:
            parts.append(cur)
            cur, size = [], 0
        cur.append(line)
        size += b
    parts.append(cur)
    out = []
    for i, p in enumerate(parts):
        t = title if i == 0 else f"{title} ({i + 1})"
        out.append((t, "\n".join(["T" + t] + p)))
    return out


# --- readers: each returns [(title, text)] ---------------------------------------

def read_txt(path, title):
    raw = read_text(path)
    md = path.lower().endswith((".md", ".markdown"))
    lines, para = [], []

    def flush():
        if para:
            lines.append(("P", " ".join(para)))
            para.clear()
    for ln in raw.splitlines():
        s = ln.strip()
        if not s:
            flush()
            continue
        if md and s.startswith("#"):
            flush()
            level = len(s) - len(s.lstrip("#"))
            lines.append(("H" if level <= 2 else "S", s.lstrip("#").strip()))
        elif md and re.match(r"^([-*+]|\d+[.)])\s+", s):
            flush()
            lines.append(("B", re.sub(r"^([-*+]|\d+[.)])\s+", "", s)))
        else:
            if md:
                s = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", s)               # images
                s = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", s)           # links -> text
                s = re.sub(r"[*_`]{1,3}([^*_`]+)[*_`]{1,3}", r"\1", s)   # emphasis
            para.append(s)
    flush()
    return [(title, lines_to_text(title, lines))]


def strip_links(text):
    """Links out of a document's text, but jumps to its own headings ("#...") stay."""
    return re.sub("\x01([^\x02]*)\x02(?!#)[^\x03]*\x03", r"\1", text)


def read_text(path):
    """A text file as str; gzip-compressed copies (as web archives store them) too."""
    data = open(path, "rb").read()
    if data[:2] == b"\x1f\x8b":
        import gzip
        data = gzip.decompress(data)
    return data.decode("utf-8", "replace")


def read_html(path, title):
    raw = read_text(path)
    m = re.search(r"<title[^>]*>(.*?)</title>", raw, re.S | re.I)
    if m and clean(html.unescape(m.group(1))) and not TITLES_FROM_FILENAMES:
        title = clean(html.unescape(m.group(1)))
    # A web page: keep its main content, not the site's menus and footers.
    mm = re.search(r"<main\b.*?</main>", raw, re.S | re.I)
    if mm:
        raw = mm.group(0)
    raw = re.sub(r"<(header|aside|form)\b.*?</\1>", " ", raw, flags=re.S | re.I)

    def picture(src, attrs):
        """A local <img> beside the page (a saved web page's own files)."""
        if not WITH_IMAGES or not src or src.startswith(("data:", "http:", "https:")):
            return None
        f = os.path.join(os.path.dirname(path), urllib.parse.unquote(src.split("#")[0].split("?")[0]))
        if not os.path.isfile(f) or f.lower().endswith(".svg"):
            return None
        try:
            from PIL import Image
            img = Image.open(f)
            img.load()
            if img.width < 90 or img.height < 60:
                return None
            return to_dgi(img)
        except Exception:
            return None
    zim2doba.MAX_PICS = 60
    return [(title, strip_links(to_text(title, raw, picture)))]


def epub_toc(z, names):
    """{chapter file: its name in the table of contents}, from EPUB 3 nav or
    EPUB 2 toc.ncx; the first label wins for each file."""
    out = {}
    def add(base, href, label):
        f = os.path.normpath(os.path.join(base, href.split("#")[0])).replace("\\", "/")
        label = clean(html.unescape(re.sub(r"<[^>]+>", " ", label)))
        if f and label and f not in out:
            out[f] = label[:120]
    for n in names:
        if n.endswith(".ncx"):
            x = z.read(n).decode("utf-8", "replace")
            for lab, src in re.findall(r"<navLabel>\s*<text>(.*?)</text>\s*</navLabel>\s*<content[^>]*src=\"([^\"]+)\"", x, re.S):
                add(os.path.dirname(n), src, lab)
    for n in names:
        if n.endswith((".xhtml", ".html")) and "nav" in os.path.basename(n).lower():
            x = z.read(n).decode("utf-8", "replace")
            for src, lab in re.findall(r'<a[^>]*href="([^"]+)"[^>]*>(.*?)</a>', x, re.S):
                add(os.path.dirname(n), src, lab)
    return out


def read_epub(path, title):
    z = zipfile.ZipFile(path)
    names = z.namelist()
    opf = next((n for n in names if n.endswith(".opf")), None)
    order = []
    if opf:
        o = z.read(opf).decode("utf-8", "replace")
        m = re.search(r"<dc:title[^>]*>(.*?)</dc:title>", o, re.S)
        if m:
            title = clean(html.unescape(m.group(1))) or title
        items = dict(re.findall(r'<item[^>]*id="([^"]+)"[^>]*href="([^"]+)"', o))
        items.update({k: v for v, k in re.findall(r'<item[^>]*href="([^"]+)"[^>]*id="([^"]+)"', o)})
        base = os.path.dirname(opf)
        for idref in re.findall(r'<itemref[^>]*idref="([^"]+)"', o):
            href = items.get(idref)
            if href:
                order.append(os.path.normpath(os.path.join(base, href)).replace("\\", "/"))
    if not order:
        order = sorted(n for n in names if n.endswith((".xhtml", ".html", ".htm")))
    labels = epub_toc(z, names)
    zim2doba.MAX_PICS = 400      # a book's own pictures: all of them, not an encyclopedia's ration
    body = []
    for n in order:
        if n not in names:
            continue
        def picture(src, attrs, chapter=n):
            """An <img> of this chapter -> 'name w h' (stored in the book's img/)."""
            if not WITH_IMAGES or not src or src.startswith("data:"):
                return None
            f = os.path.normpath(os.path.join(os.path.dirname(chapter), urllib.parse.unquote(src.split("#")[0]))).replace("\\", "/")
            if f not in names:   # some books keep the %20 in the file's own name
                f = os.path.normpath(os.path.join(os.path.dirname(chapter), src.split("#")[0])).replace("\\", "/")
            if f not in names or f.lower().endswith(".svg"):
                return None
            try:
                from PIL import Image
                img = Image.open(io.BytesIO(z.read(f)))
                img.load()
                if img.width < 90 or img.height < 60:      # ornaments, bullets, rules
                    return None
                return to_dgi(img)
            except Exception:
                return None
        t = strip_links(to_text("", z.read(n).decode("utf-8", "replace"), picture))
        lines = [ln for ln in t.split("\n")[1:] if ln]
        # A chapter file without a heading of its own still gets one, from the
        # book's table of contents: the reader's CONTENTS is built from them.
        lab = labels.get(n)
        if lab and not any(ln[:1] == "H" for ln in lines[:3]):
            lines.insert(0, "H" + lab)
        body += lines
    text = "\n".join(["T" + title] + body)
    return split_long(title, text)


def bookmark_name(t):
    """A PDF bookmark as a chapter name, or "" for a broken one."""
    t = re.sub(r"^\d{1,4}B\s?(?=[A-Z])", "", clean(t))   # Word's bookmark ids: "23BFigures"
    t = t.strip(" .·•-–—_")
    letters = sum(c.isalpha() for c in t)
    if letters < 3 or (letters < 5 and t.islower()):
        return ""
    return t[:1].upper() + t[1:]


def short_title(title):
    """What a chapter's title starts with: the publication number if there is
    one ("ATP 4-02.11"), else the title cut to a few words. Keys are 55 bytes,
    so the chapter's own name has to fit after it."""
    m = re.match(r"^((?:FM|ATP|TC|TM|ADP|GTA|AIB|MCRP|AFH|NAVEDTRA|OSHA)\s*[\d][\w.\-]*)", title)
    if m:
        return m.group(1)
    t = re.sub(r"(\s*\([^)]*\))+\s*$", "", title).strip()
    head = re.split(r"[:?—–]|\s{2,}", t)[0].strip()
    if 3 <= len(head) <= 28:
        return head
    words = t[:28].rsplit(" ", 1)[0].split()
    while len(words) > 1 and words[-1].lower().strip(",.;:") in STOP_WORDS:
        words.pop()
    return " ".join(words) or t[:28]


STOP_WORDS = {"and", "or", "of", "the", "a", "an", "to", "for", "from", "in", "on", "with", "by", "at", "&"}


def pretty_title(t):
    """A title as a file or a database left it: "the-ten-bushcraft-books",
    "checklist_1_72_hr._kit" -> "The ten bushcraft books", "Checklist 1 72 hr. kit"."""
    t = t.replace("_", " ")
    if " " not in t.strip() or t.count("-") >= 2 and t.count(" ") < t.count("-"):
        t = re.sub(r"(?<=\w)-(?=\w)", " ", t)
    t = " ".join(t.split()).strip(" -.,;")
    t = re.sub(r"\s+\(\d+\)$", "", t)
    return t[:1].upper() + t[1:] if t else t


SHIFTED = False     # the PDF's Cyrillic sits 0x17A above its cp1251 codes ("ɉɭɥɢ" = "Пули")


def unshift(t):
    """Undo a broken font map that put each cp1251 byte at U+017A + byte."""
    out = []
    for c in t:
        o = ord(c) - 0x17A
        if 0x80 <= o <= 0xFF:
            try:
                c = bytes([o]).decode("cp1251")
            except UnicodeDecodeError:
                pass
        out.append(c)
    return "".join(out)


def page_blocks(page):
    out = _page_blocks(page)
    if SHIFTED:
        out = [(sz, unshift(t), bb, st) for sz, t, bb, st in out]
    return out


def _page_blocks(page):
    """(size, text, bbox, [(size, chars)]) for each paragraph of a PDF page, in
    reading order. A two-column page whose lines run across the gutter (old
    scans with an OCR layer) is rebuilt column by column from its words."""
    import pymupdf
    words = page.get_text("words")
    gut = find_gutter(words, page.rect)
    if gut is not None:
        spans = [(pymupdf.Rect(sp["bbox"]), sp["size"])
                 for b in page.get_text("dict", flags=pymupdf.TEXTFLAGS_TEXT)["blocks"] if b.get("type") == 0
                 for l in b["lines"] for sp in l["spans"]]
        return blocks_from_words(words, gut, page.rect, spans)
    out = []
    d = page.get_text("rawdict", flags=pymupdf.TEXTFLAGS_TEXT)
    for b in d["blocks"]:
        if b.get("type") != 0:
            continue
        for l in b["lines"]:
            for sp in l["spans"]:
                sp["text"] = "".join(c["c"] for c in sp.get("chars", []))
        spans = [sp for l in b["lines"] for sp in l["spans"] if sp["text"].strip()]
        if not spans:
            continue
        out.append((max(sp["size"] for sp in spans), block_text(b), b["bbox"],
                    [(round(sp["size"]), len(sp["text"])) for sp in spans]))
    return out


def find_gutter(words, pr):
    """x of an empty vertical band near the middle with text on both sides
    of it on most lines, or None."""
    body = [w for w in words if pr.height * 0.07 < w[1] < pr.height * 0.93]
    if len(body) < 80:
        return None
    lo, hi = int(pr.width * 0.38), int(pr.width * 0.62)
    cover = [0] * (int(pr.width) + 2)
    for w in body:
        for x in range(max(0, int(w[0])), min(len(cover), int(w[2]) + 1)):
            cover[x] += 1
    best, run, start = None, 0, 0
    for x in range(lo, hi):
        if cover[x] <= len(body) * 0.01:
            if run == 0:
                start = x
            run += 1
            if run >= 8 and (best is None or run > best[1]):
                best = (start, run)
        else:
            run = 0
    if not best:
        return None
    g = best[0] + best[1] / 2
    left = sum(1 for w in body if w[2] < g)
    right = sum(1 for w in body if w[0] > g)
    if min(left, right) < len(body) * 0.25:
        return None
    # Only worth it when the text layer mixes the columns: OCR paragraphs
    # (blocks) holding words from both sides.
    crossing = {w[5] for w in body if w[2] < g} & {w[5] for w in body if w[0] > g}
    return g if crossing else None


def blocks_from_words(words, gut, pr, spans=()):
    """Columns read in bands: a full-width line (a heading, a wide figure
    caption) ends the band above it; each band is left column, then right."""
    cross = sorted((w for w in words if w[0] < gut < w[2]), key=lambda w: w[1])
    cuts = []
    for w in cross:
        if cuts and w[1] <= cuts[-1][1] + 2:
            cuts[-1] = (cuts[-1][0], max(cuts[-1][1], w[3]))
        else:
            cuts.append((w[1], w[3]))
    out = []
    top = -1e9
    for y0, y1 in cuts + [(1e9, 1e9)]:
        band = [w for w in words if top <= (w[1] + w[3]) / 2 < y0 and not (w[0] < gut < w[2])]
        out += _columns(band, gut, spans)
        full = [w for w in words if y0 - 2 <= (w[1] + w[3]) / 2 <= y1 + 2]
        if full and y0 < 1e9:
            out += _columns(full, 1e9, spans)          # one "column": the whole line
        top = y1 + 2
    return out


def _columns(words, gut, spans):
    out = []
    for side in (0, 1):
        ws = [w for w in words if (w[2] < gut if side == 0 else w[0] > gut)]
        ws.sort(key=lambda w: (round(w[1]), w[0]))
        lines = []
        for w in ws:
            h = w[3] - w[1]
            if lines and abs(w[1] - lines[-1][0]) < h * 0.5:
                lines[-1][1].append(w)
            else:
                lines.append([w[1], [w]])
        para, prev = [], None
        def flush():
            if not para:
                return
            x0 = min(w[0] for l in para for w in l); y0 = min(w[1] for l in para for w in l)
            x1 = max(w[2] for l in para for w in l); y1 = max(w[3] for l in para for w in l)
            def wsize(w):
                cx, cy = (w[0] + w[2]) / 2, (w[1] + w[3]) / 2
                for r, sz in spans:
                    if r.x0 <= cx <= r.x1 and r.y0 <= cy <= r.y1:
                        return sz
                return (w[3] - w[1]) / 1.2
            hs = sorted(wsize(w) for l in para for w in l)
            size = hs[len(hs) // 2]
            text = ""
            for l in para:
                t = " ".join(w[4] for w in sorted(l, key=lambda w: w[0]))
                if text.endswith("-") and t[:1].islower() and len(text) > 1 and text[-2].isalpha():
                    text = text[:-1] + t
                else:
                    text = (text + " " + t) if text else t
            out.append((size, text, (x0, y0, x1, y1), [(round(size), len(text))]))
        for y, l in lines:
            h = max(w[3] - w[1] for w in l)
            if prev is not None and (y - prev[0] > h * 1.9 or abs(h - prev[1]) > h * 0.3):
                flush()
                para = []
            para.append(l)
            prev = (y, h)
        flush()
    return out


def line_text(line):
    """A PDF line's spans joined as printed: a space only where there is a gap
    (letter-spaced headings come one letter a span)."""
    out, prev = "", None
    for sp in line["spans"]:
        t = sp["text"]
        if not t:
            continue
        if prev is not None:
            gap = sp["bbox"][0] - prev["bbox"][2]
            if gap > sp["size"] * 0.25 and not out.endswith(" ") and not t.startswith(" "):
                out += " "
        out += t
        prev = sp
    # L e t t e r - s p a c e d words written with real spaces: the gaps
    # between glyphs say where the words end.
    if re.fullmatch(r"(?:\S {1,2}){3,}\S", out.strip()):
        glyphs = [c for sp in line["spans"] for c in sp.get("chars", []) if c["c"].strip()]
        if len(glyphs) >= 4:
            gaps = sorted(b["bbox"][0] - a["bbox"][2] for a, b in zip(glyphs, glyphs[1:]))
            m = gaps[len(gaps) // 2]
            out = glyphs[0]["c"]
            for a, b in zip(glyphs, glyphs[1:]):
                if b["bbox"][0] - a["bbox"][2] > m * 1.8 + 0.8:
                    out += " "
                out += b["c"]
        else:
            out = out.replace(" ", "")
    return out


def block_text(b):
    """Lines of a block as one paragraph; words hyphenated across lines rejoined."""
    out = ""
    for l in b["lines"]:
        t = line_text(l).strip()
        if not t:
            continue
        if out.endswith("-") and t[:1].islower() and len(out) > 1 and out[-2].isalpha():
            out = out[:-1] + t
        else:
            out = (out + " " + t) if out else t
    return out


def heading_marks(pages, body):
    """Chapters from the print itself when the bookmarks don't give them: the
    largest heading size that starts between 8 and 80 sections."""
    by_size = {}
    for pno, (h, blocks) in enumerate(pages):
        for size, text, bbox in blocks:
            t = clean(text)
            if size < body * 1.35 or not t or len(t) > 90 or bbox[1] < h * 0.04:
                continue
            if re.search(r"(?i)intentionally left blank|^contents$|^\W*$|^[\divxlc\-–. ]+$", t):
                continue
            seen = by_size.setdefault(round(size), {})
            seen.setdefault(t.lower(), (pno, t))
    for size in sorted(by_size, reverse=True):
        heads = sorted(by_size[size].values())
        if 8 <= len(heads) <= 80:
            return [(p, t, 1) for p, t in heads]
    return []


def unreadable(text):
    """True for a text layer in a broken font encoding ("serviÖe2memÿer"):
    most words are not runs of letters of one script."""
    words = [w.strip(".,;:()[]\"'!?—–-") for w in text.split()]
    words = [w for w in words if len(w) >= 3]
    if len(words) < 200:
        return False
    def ok(w):
        return w.isalpha() and (all(ord(c) < 0x250 for c in w) or all(0x400 <= ord(c) < 0x530 for c in w)) \
            and not re.search(r"[àâäéèêëîïôöùûüÿçÖÿ]", w[1:-1] if len(w) > 3 else "")
    good = sum(1 for w in words if ok(w))
    return good < len(words) * 0.6


def read_pdf(path, title):
    import pymupdf
    doc = pymupdf.open(path)
    sample = " ".join(doc[i].get_text() for i in range(min(len(doc), 40)))
    global SHIFTED
    SHIFTED = False
    if unreadable(sample) and not unreadable(unshift(sample)):
        SHIFTED = True
        print(f"   {os.path.basename(path)}: shifted cp1251 font map, decoded", file=sys.stderr)
    elif unreadable(sample):
        print(f"!! {os.path.basename(path)}: its text layer is in a broken encoding; skipped "
              "(find another copy, or OCR it)", file=sys.stderr)
        return []
    meta_title = clean((doc.metadata or {}).get("title") or "")
    if TITLES_FROM_FILENAMES:
        meta_title = ""
    if meta_title and len(meta_title) > 3 and not meta_title.lower().startswith(("microsoft", "untitled")):
        title = meta_title
    # Body size: the commonest span size; bigger, bold-ish lines are headings.
    sizes = {}
    pages = []
    # A picture on many pages is a logo or a page ornament, not a figure.
    seen_on = {}
    if WITH_IMAGES:
        for page in doc:
            for x in {i[0] for i in page.get_images()}:
                seen_on[x] = seen_on.get(x, 0) + 1
    logos = {x for x, n in seen_on.items() if n > 3}
    for page in doc:
        figs = page_figures(page, logos) if WITH_IMAGES else []
        blocks = []
        for size, text, bbox, stats in page_blocks(page):
            for k, n in stats:
                sizes[k] = sizes.get(k, 0) + n
            b = {"bbox": bbox}
            br = pymupdf.Rect(b["bbox"])
            inside = False
            for i, f in enumerate(figs):
                ov = (br & f).get_area() if not (br & f).is_empty else 0
                if br.get_area() and ov / br.get_area() > 0.5 and len(text) < 300:
                    figs[i] = f | br                    # labels belong to the picture
                    inside = True
                    break
            if not inside:
                blocks.append((size, text, b["bbox"]))
        # Pictures go into the text flow before the first block below their top.
        for f in sorted(figs, key=lambda r: r.y0):
            try:
                ref = to_dgi(render_figure(page, f))
            except Exception as e:
                print(f"   picture skipped on page {page.number + 1}: {e}", file=sys.stderr)
                continue
            # (in its own column: the block must also lie under it)
            at = next((i for i, bl in enumerate(blocks) if bl[0] != -1 and bl[2][1] >= f.y0
                       and bl[2][0] < f.x1 and bl[2][2] > f.x0), None)
            if at is None:
                at = next((i for i, bl in enumerate(blocks) if bl[0] != -1 and bl[2][1] >= f.y0), len(blocks))
            blocks.insert(at, (-1, ref, tuple(f)))
        pages.append((page.rect.height, blocks))
    body = max(sizes, key=sizes.get) if sizes else 10
    # Running heads and feet: the same short line (page numbers aside) on many pages.
    def norm(t):
        return re.sub(r"[\d\-–.]+", "#", clean(t).lower())
    rep = {}
    for _, blocks in pages:
        for size, text, bbox in blocks:
            if size != -1 and len(text) < 90:
                k = norm(text)
                rep[k] = rep.get(k, 0) + 1
    running = {k for k, n in rep.items() if n >= max(4, len(pages) // 8)} if len(pages) >= 12 else set()

    def page_lines(pno):
        h, blocks = pages[pno]
        out = []
        for size, text, bbox in blocks:
            if size == -1:
                out.append(("I", text))
                continue
            t = clean(text)
            if not t or (len(t) < 90 and norm(t) in running):
                continue
            # A page foot or head with its page number ("2-1", "Ladders 2-1").
            if (bbox[1] > h * 0.88 or bbox[3] < h * 0.09) and len(t) < 80 and \
                    re.search(r"(^|\s)(\d+-\d+|[A-Z]-\d+|\d+)(\s|$)", t):
                continue
            # Running heads/feet and bare page numbers.
            if (bbox[1] < h * 0.06 or bbox[3] > h * 0.95) and len(t) < 80:
                continue
            if re.fullmatch(r"[\divxlcIVXLC\-–. ]{1,12}", t):
                continue
            t = re.sub(r"^\d{1,4}B\s?(?=[A-Z]{2})", "", t)   # bookmark ids printed by some PDFs ("208BOVERVIEW")
            if size >= body * 1.35 and len(t) < 120:
                out.append(("H", t))
            elif size >= body * 1.12 and len(t) < 160:
                out.append(("S", t))
            elif re.match(r"^([•▪●◦\-–]|\(?[a-z0-9]\)|\d+\.)\s", t) and len(t) > 4:
                out.append(("B", re.sub(r"^([•▪●◦\-–])\s*", "", t)))
            else:
                out.append(("P", t))
        return out

    toc = [(lvl, bookmark_name(unshift(t) if SHIFTED else t), p - 1) for lvl, t, p in doc.get_toc(simple=True) if clean(t) and p >= 1]
    # Broken bookmarks ("eli", "Tra", "d a") say nothing about the chapters.
    junk = [m for m in toc if not m[1]]
    if toc and len(junk) > len(toc) * 0.3:
        toc = []
    toc = [m for m in toc if m[1]]
    # Bookmarks for figures and tables ("Figure_Burn1d", "Table 2-1") are not chapters.
    toc = [m for m in toc if not re.match(r"(?i)^(fig(ure)?|table|tab)\b|^\S*_\S*$", m[1])]
    # Chapters: the bookmarks at the top one or two levels, if there are enough.
    marks = []
    if toc:
        top = min(l for l, _, _ in toc)
        # Chapters: the top level; its sections only when there are too few chapters.
        marks = [m for m in toc if m[0] <= top]
        if len(marks) < 3:
            marks = [m for m in toc if m[0] <= top + 1]
        marks = sorted({(p, t, l) for l, t, p in marks})
    if len(marks) < 3:
        marks = heading_marks(pages, body)
    if len(marks) < 3:
        lines = [ln for p in range(len(pages)) for ln in page_lines(p)]
        return split_long(title, lines_to_text(title, lines))

    arts = []
    contents = []
    aliases = []
    starts = [p for p, _, _ in marks] + [len(pages)]
    seen = set()
    for i, (p, t, lvl) in enumerate(marks):
        end = max(starts[i + 1], p + 1) if starts[i + 1] > p else p + 1
        lines = [ln for q in range(p, min(end, len(pages))) for ln in page_lines(q)]
        at = f"{short_title(title)} — {t}"
        # Unique by the whole title: a long one's key is cut at 55 bytes, so a
        # suffix there would never change it.
        n = 2
        base = at
        while fold(at, 10**6) in seen:
            at = f"{base} ({n})"
            n += 1
        seen.add(fold(at, 10**6))
        if not lines or FRONT_LISTS.match(t):
            continue
        parts = split_long(at, lines_to_text(at, lines))
        # Found by the chapter's own words too: "Chapter 2 Shelter", "Shelter".
        bare = re.sub(r"^(chapter|section|part|appendix|annex|глава|раздел|розділ)\s+[\w.\-]+[.:]?\s*", "",
                      t, flags=re.I).strip() or t
        bare = re.sub(r"^[\dIVXLC]+(\.\d+)*[.:)]?\s+", "", bare).strip() or bare
        bare = re.sub(r"^[·•\-–—:]+\s*", "", bare).strip() or bare
        aliases.append((parts[0][0], [t, bare]))
        arts += parts
        contents.append(("B" if lvl > marks[0][2] else "P", LINK_A + t + LINK_B + at + LINK_C))
    # The contents article keeps its links (that's its point).
    toc_text = "\n".join(["T" + title] + [k + v for k, v in contents])
    ALIASES.extend(aliases)
    return [(title, toc_text)] + arts


READERS = {".pdf": read_pdf, ".epub": read_epub, ".html": read_html, ".htm": read_html,
           ".xhtml": read_html, ".txt": read_txt, ".md": read_txt, ".markdown": read_txt}


# --- book writer -------------------------------------------------------------------

def write_book(out, name, source, articles, aliases=()):
    os.makedirs(out, exist_ok=True)
    if IMAGES:
        os.makedirs(os.path.join(out, "img"), exist_ok=True)
        for nm, blob in IMAGES:
            with open(os.path.join(out, "img", nm + ".dgi"), "wb") as f:
                f.write(blob)
    records, pos = [], 0
    where = {}
    with open(os.path.join(out, "a0000.dta"), "wb") as f:
        for title, text in articles:
            z = zlib.compress(text.encode("utf-8"), 9)
            f.write(z)
            k = key(title)
            if k:
                records.append((k, display(title), 0, 0, pos, len(z)))
            where[title] = (pos, len(z))
            pos += len(z)
    have = {r[0] for r in records}
    for title, others in aliases:
        w = where.get(title)
        for o in others:
            k = key(o)
            if w and k and k not in have:
                records.append((k, display(o), 0, 1, w[0], w[1]))
                have.add(k)
    records.sort(key=lambda r: (r[0].ljust(KEY_BYTES, b"\0"), r[1]))
    with open(os.path.join(out, "index.dti"), "wb") as f:
        f.write(HDR.pack(b"DOBAIX1\0", len(records), REC.size, 1, b"\0" * 12))
        for k, t, ch, flags, off, ln in records:
            f.write(REC.pack(k, t, ch, flags, off, ln, 0))
    with open(os.path.join(out, "info.txt"), "w", encoding="utf-8") as f:
        f.write(f"name={name}\nlang=\narticles={len(records)}\nredirects=0\nsource={source}\n")
    return len(records), pos


def read_manifest(src):
    """nautilus2dir.py's _docs.tsv: file stem -> (title, group, description, order)."""
    p = os.path.join(src, "_docs.tsv") if os.path.isdir(src) else ""
    out = {}
    if p and os.path.isfile(p):
        for line in open(p, encoding="utf-8"):
            f = line.rstrip("\n").split("\t")
            if len(f) >= 5:
                out[f[0]] = (f[1], f[2], f[3], int(f[4]))
    return out


def write_menu(out, menu, name):
    """<book>/menu.txt: the book's first page on the device, its documents by
    group ("# GROUP" lines, then "Title|what it is")."""
    groups = {}
    for _, g, t, sub in sorted(menu, key=lambda m: m[0]):
        groups.setdefault(g, []).append((t, sub))
    with open(os.path.join(out, "menu.txt"), "w", encoding="utf-8") as f:
        for g, rows in groups.items():
            f.write(f"# {(g or 'Documents').upper()}\n")
            for t, sub in rows:
                f.write(f"{t}|{' '.join(sub.split())}\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", help="a folder (searched recursively) or one file")
    ap.add_argument("out")
    ap.add_argument("--name", required=True, help="shown on the device")
    ap.add_argument("--source", default="Own files", help="shown under every article")
    ap.add_argument("--titles-from-filenames", action="store_true",
                    help="use each file's name as its title (fetch_gov.py names them)")
    ap.add_argument("--no-images", action="store_true", help="text only")
    a = ap.parse_args()
    global TITLES_FROM_FILENAMES, WITH_IMAGES
    WITH_IMAGES = not a.no_images
    TITLES_FROM_FILENAMES = a.titles_from_filenames
    files = [a.src] if os.path.isfile(a.src) else sorted(
        os.path.join(d, f) for d, _, fs in os.walk(a.src) for f in fs if not f.startswith("."))
    manifest = read_manifest(a.src)
    articles, used, renamed = [], set(), {}
    menu = []                                    # (group, title, subtitle) per document
    for path in files:
        ext = os.path.splitext(path)[1].lower()
        reader = READERS.get(ext)
        if not reader:
            continue
        stem = os.path.splitext(os.path.basename(path))[0]
        group, desc, order = "", "", len(menu)
        if stem in manifest:
            title, group, desc, order = manifest[stem]
        else:
            title = pretty_title(stem) if TITLES_FROM_FILENAMES else stem.replace("_", " ")
        try:
            got = reader(path, title)
        except Exception as e:                     # one bad file never stops the book
            print(f"!! {path}: {e}", file=sys.stderr)
            continue
        n = 0
        first = None
        for t, text in got:
            if len(text) < len(t) + 20:
                continue
            t0 = t
            k, base = 2, t
            while fold(t, 10**6) in used:
                t = f"{base} ({k})"
                k += 1
            used.add(fold(t, 10**6))
            renamed[t0] = t
            articles.append((t, text))
            first = first or t
            n += 1
        print(f"{os.path.basename(path)}: {n} article(s)")
        if first:
            chapters = f"{n - 1} chapters" if n > 2 else ""
            menu.append((order, group, first, desc or chapters))
    if not articles:
        sys.exit("nothing readable found")
    aliases = [(renamed.get(t, t), o) for t, o in ALIASES]
    n, size = write_book(a.out, a.name, " ".join(a.source.split())[:60], articles, aliases)
    write_menu(a.out, menu, a.name)
    print(f"done: {n} articles, {len(IMAGES)} pictures, {size / 1e6:.1f} MB -> {a.out}")


if __name__ == "__main__":
    main()
