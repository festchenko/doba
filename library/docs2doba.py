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
import re
import struct
import sys
import zipfile
import zlib

from dobafold import KEY_BYTES, fold
from zim2doba import HDR, LINK_A, LINK_B, LINK_C, REC, to_text, utf8_cut

MAX_ARTICLE = 1_400_000          # bytes of text; the device holds 1.5 MB inflated
KINDS = set("THSPB")
ALIASES = []                     # (article title, [other titles]) gathered by the readers


# --- text helpers ----------------------------------------------------------------

def clean(s):
    s = s.replace("­", "").replace("\x00", "")
    for c in (LINK_A, LINK_B, LINK_C):
        s = s.replace(c, "")
    return " ".join(s.split())


def lines_to_text(title, lines):
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
    raw = open(path, "rb").read().decode("utf-8", "replace")
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
    return re.sub("\x01([^\x02]*)\x02[^\x03]*\x03", r"\1", text)


def read_html(path, title):
    raw = open(path, "rb").read().decode("utf-8", "replace")
    m = re.search(r"<title[^>]*>(.*?)</title>", raw, re.S | re.I)
    if m and clean(html.unescape(m.group(1))):
        title = clean(html.unescape(m.group(1)))
    return [(title, strip_links(to_text(title, raw)))]


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
    body = []
    for n in order:
        if n not in names:
            continue
        t = strip_links(to_text("", z.read(n).decode("utf-8", "replace")))
        body += [ln for ln in t.split("\n")[1:] if ln]
    text = "\n".join(["T" + title] + body)
    return split_long(title, text)


def read_pdf(path, title):
    import pymupdf
    doc = pymupdf.open(path)
    meta_title = clean((doc.metadata or {}).get("title") or "")
    if meta_title and len(meta_title) > 3 and not meta_title.lower().startswith(("microsoft", "untitled")):
        title = meta_title
    # Body size: the commonest span size; bigger, bold-ish lines are headings.
    sizes = {}
    pages = []
    for page in doc:
        d = page.get_text("dict", flags=pymupdf.TEXTFLAGS_TEXT)
        blocks = []
        for b in d["blocks"]:
            if b.get("type") != 0:
                continue
            spans = [s for l in b["lines"] for s in l["spans"] if s["text"].strip()]
            if not spans:
                continue
            text = " ".join(" ".join(s["text"] for s in l["spans"]) for l in b["lines"])
            size = max(s["size"] for s in spans)
            for s in spans:
                sizes[round(s["size"])] = sizes.get(round(s["size"]), 0) + len(s["text"])
            blocks.append((size, text, b["bbox"]))
        pages.append((page.rect.height, blocks))
    body = max(sizes, key=sizes.get) if sizes else 10

    def page_lines(pno):
        h, blocks = pages[pno]
        out = []
        for size, text, bbox in blocks:
            t = clean(text)
            if not t:
                continue
            # Running heads/feet and bare page numbers.
            if (bbox[1] < h * 0.06 or bbox[3] > h * 0.95) and len(t) < 80:
                continue
            if re.fullmatch(r"[\divxlcIVXLC\-–. ]{1,12}", t):
                continue
            if size >= body * 1.35 and len(t) < 120:
                out.append(("H", t))
            elif size >= body * 1.12 and len(t) < 160:
                out.append(("S", t))
            elif re.match(r"^([•▪●◦\-–]|\(?[a-z0-9]\)|\d+\.)\s", t) and len(t) > 4:
                out.append(("B", re.sub(r"^([•▪●◦\-–])\s*", "", t)))
            else:
                out.append(("P", t))
        return out

    toc = [(lvl, clean(t), p - 1) for lvl, t, p in doc.get_toc(simple=True) if clean(t) and p >= 1]
    # Chapters: the bookmarks at the top one or two levels, if there are enough.
    marks = []
    if toc:
        top = min(l for l, _, _ in toc)
        marks = [m for m in toc if m[0] <= top + 1]
        marks = sorted({(p, t, l) for l, t, p in marks})
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
        at = f"{title} — {t}"
        while fold(at) in seen:
            at += " ·"
        seen.add(fold(at))
        if not lines:
            continue
        parts = split_long(at, lines_to_text(at, lines))
        # Found by the chapter's own words too: "Chapter 2 Shelter", "Shelter".
        bare = re.sub(r"^(chapter|section|part|appendix|annex|глава|раздел|розділ)\s+[\w.\-]+[.:]?\s*", "",
                      t, flags=re.I).strip() or t
        bare = re.sub(r"^[\dIVXLC]+(\.\d+)*[.:)]?\s+", "", bare).strip() or bare
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
    records, pos = [], 0
    where = {}
    with open(os.path.join(out, "a0000.dta"), "wb") as f:
        for title, text in articles:
            z = zlib.compress(text.encode("utf-8"), 9)
            f.write(z)
            key = fold(title)
            if key:
                records.append((key, utf8_cut(title, KEY_BYTES), 0, 0, pos, len(z)))
            where[title] = (pos, len(z))
            pos += len(z)
    have = {r[0] for r in records}
    for title, others in aliases:
        w = where.get(title)
        for o in others:
            k = fold(o)
            if w and k and k not in have:
                records.append((k, utf8_cut(o, KEY_BYTES), 0, 1, w[0], w[1]))
                have.add(k)
    records.sort(key=lambda r: (r[0].ljust(KEY_BYTES, b"\0"), r[1]))
    with open(os.path.join(out, "index.dti"), "wb") as f:
        f.write(HDR.pack(b"DOBAIX1\0", len(records), REC.size, 1, b"\0" * 12))
        for key, t, ch, flags, off, ln in records:
            f.write(REC.pack(key, t, ch, flags, off, ln, 0))
    with open(os.path.join(out, "info.txt"), "w", encoding="utf-8") as f:
        f.write(f"name={name}\nlang=\narticles={len(records)}\nredirects=0\nsource={source}\n")
    return len(records), pos


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", help="a folder (searched recursively) or one file")
    ap.add_argument("out")
    ap.add_argument("--name", required=True, help="shown on the device")
    ap.add_argument("--source", default="Own files", help="shown under every article")
    a = ap.parse_args()
    files = [a.src] if os.path.isfile(a.src) else sorted(
        os.path.join(d, f) for d, _, fs in os.walk(a.src) for f in fs if not f.startswith("."))
    articles, used, renamed = [], set(), {}
    for path in files:
        ext = os.path.splitext(path)[1].lower()
        reader = READERS.get(ext)
        if not reader:
            continue
        title = os.path.splitext(os.path.basename(path))[0].replace("_", " ")
        try:
            got = reader(path, title)
        except Exception as e:                     # one bad file never stops the book
            print(f"!! {path}: {e}", file=sys.stderr)
            continue
        n = 0
        for t, text in got:
            if len(text) < len(t) + 20:
                continue
            t0 = t
            while fold(t) in used:
                t += " ·"
            used.add(fold(t))
            renamed[t0] = t
            articles.append((t, text))
            n += 1
        print(f"{os.path.basename(path)}: {n} article(s)")
    if not articles:
        sys.exit("nothing readable found")
    aliases = [(renamed.get(t, t), o) for t, o in ALIASES]
    n, size = write_book(a.out, a.name, " ".join(a.source.split())[:60], articles, aliases)
    print(f"done: {n} articles, {size / 1e6:.1f} MB -> {a.out}")


if __name__ == "__main__":
    main()
