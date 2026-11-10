#!/usr/bin/env python3
"""A book's first page on the device: <book>/menu.txt, from menus/<book>.src.

The device opens a book on its menu, the way Kiwix opens a ZIM on its main
page: a few groups of the pages worth starting from, each with a line on what
it is. A ZIM's own main page is made for a browser (pictures, carousels,
"on this day"), so the menu is picked here instead and checked against the
book's index: a title the book does not have is left out (and reported).

menus/<book>.src:
    about=One line under the book's name
    unit=questions                    # "9.5M articles", "71k questions"
    # GROUP NAME
    Title | Other title :: a line on what it is     (first title the book has)
    @tag water 6                      # Q&A: the tag's 6 highest-voted questions
    @sections                         # the topics' (sections.src) titles from this book

A book docs2doba made already has its menu.txt (its documents); with no .src
only its info.txt gets the unit and the count.

    python3 bookmenu.py /path/to/lib            # every book with a .src or a menu
    python3 bookmenu.py /path/to/lib wiki_en
"""
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from dobafold import fold, key  # noqa: E402

REC, HDR = 128, 32


class Index:
    def __init__(self, book):
        self.book = book
        self.f = open(os.path.join(book, "index.dti"), "rb")
        self.n = struct.unpack("<8sIII12s", self.f.read(HDR))[1]

    def rec(self, i):
        self.f.seek(HDR + i * REC)
        r = self.f.read(REC)
        t = r[56:112].rstrip(b"\0").decode("utf-8", "replace")
        ch, flags, off, ln, _ = struct.unpack("<HHIII", r[112:128])
        return r[:56], t, ch, flags, off, ln

    def find(self, title):
        """The title as the book has it (same fold), or None."""
        want = key(title).ljust(56, b"\0")
        lo, hi = 0, self.n
        while lo < hi:
            mid = (lo + hi) // 2
            self.f.seek(HDR + mid * REC)
            if self.f.read(56) < want:
                lo = mid + 1
            else:
                hi = mid
        best = None
        while lo < self.n:
            k, t, *_ = self.rec(lo)
            if k != want:
                break
            if t == title:
                return t
            best = best or t
            lo += 1
        return best

    def text(self, title):
        want = key(title).ljust(56, b"\0")
        lo, hi = 0, self.n
        while lo < hi:
            mid = (lo + hi) // 2
            self.f.seek(HDR + mid * REC)
            if self.f.read(56) < want:
                lo = mid + 1
            else:
                hi = mid
        if lo >= self.n:
            return ""
        k, t, ch, flags, off, ln = self.rec(lo)
        if k != want:
            return ""
        with open(os.path.join(self.book, "a%04d.dta" % ch), "rb") as d:
            d.seek(off)
            return zlib.decompress(d.read(ln)).decode("utf-8", "replace")


VERSIONS = ("Версії роботи", "Versions of ", "Версии ")
LINK_RE = re.compile("\x01([^\x01\x02\x03]*)\x02([^\x01\x02\x03]*)\x03")


def best_version(ix, title):
    """A Wikisource "versions of this work" page lists editions and holds no
    text: the menu goes to the edition with the most text instead."""
    t = ix.text(title)
    if not t or not any(v in t[:300] for v in VERSIONS):
        return title
    best, size = title, 0
    for m in list(LINK_RE.finditer(t))[:40]:
        tgt = m.group(2)
        if not tgt or tgt.startswith("#") or ":" in tgt.split("/")[0][:12]:
            continue
        x = ix.text(tgt)
        n = len(x) + 400 * x.count("\x01")      # an edition's contents page counts its chapters
        if n > size:
            best, size = tgt, n
    return best


def tag_questions(ix, tag, n):
    """A Stack Exchange tag page lists its questions highest-voted first."""
    text = ix.text(f"questions/tagged/{tag}_page=1")
    out = []
    for line in text.split("\n"):
        if line.startswith("S") and len(out) < n:
            q = line[1:].strip()
            if ix.find(q):
                out.append(q)          # the whole question; the device finds it by its first 55 bytes
    return out


def sections_rows(lib, book):
    """(group, title, line) for the topics' guides this book has."""
    rows = []
    group = None
    for line in open(os.path.join(HERE, "sections.src"), encoding="utf-8"):
        if line.startswith("section "):
            group = line.split("|")[1].strip()
            continue
        m = re.match(r"\s+[*-]\s+(.+?)\s*\|\s*(.*?)\s*\|\s*(.+)$", line)
        if not m or not group:
            continue
        for ref in m.group(3).split(";"):
            b, _, t = ref.strip().partition(":")
            if b == book and not t.endswith("*"):
                rows.append((group, t.strip(), m.group(2)))
                break
    return rows


def build(lib, book):
    path = os.path.join(lib, book)
    src = os.path.join(HERE, "menus", book + ".src")
    info_p = os.path.join(path, "info.txt")
    info = {}
    for line in open(info_p, encoding="utf-8"):
        if "=" in line:
            k, v = line.rstrip("\n").split("=", 1)
            info[k] = v
    menu_p = os.path.join(path, "menu.txt")
    missing = []
    rows = 0
    spec = [l.strip() for l in open(src, encoding="utf-8")] if os.path.isfile(src) else []
    for l in spec:
        if re.match(r"^(about|unit)=", l):
            k, v = l.split("=", 1)
            info[k] = v
    if any(l and not l.startswith("//") and not re.match(r"^(about|unit)=", l) for l in spec):
        ix = Index(path)
        out, seen = [], set()

        def row(t, sub=""):
            if t and t not in seen:
                seen.add(t)
                out.append(f"{t}|{sub}" if sub else t)

        for line in open(src, encoding="utf-8"):
            line = line.strip()
            if not line or line.startswith("//"):
                continue
            if re.match(r"^(about|unit)=", line):
                k, v = line.split("=", 1)
                info[k] = v
            elif line.startswith("#"):
                out.append("# " + line.lstrip("# ").strip())
            elif line.startswith("@tag"):
                _, tag, n, *label = line.split()
                qs = tag_questions(ix, tag, int(n))
                if qs:
                    out.append("# " + (" ".join(label) or tag.replace("-", " ")).upper())
                    for q in qs:
                        row(q)
                else:
                    missing.append(line)
            elif line.startswith("@sections"):
                group = None
                for g, t, sub in sections_rows(lib, book):
                    have = ix.find(t)
                    if not have:
                        missing.append(t)
                        continue
                    if g != group:
                        out.append("# " + g)
                        group = g
                    row(have, sub)
            else:
                names, _, sub = line.partition("::")
                name = next((x.strip() for x in names.split("|") if ix.find(x.strip())), None)
                hit = None
                if name:
                    have = ix.find(name)
                    # the index's spelling when it is the same title; else (a
                    # long title shortened for lists) the name as written
                    hit = have if fold(have, 10 ** 6) == fold(name, 10 ** 6) else name
                    hit = best_version(ix, hit)
                if hit:
                    row(hit, sub.strip())
                else:
                    missing.append(names.strip())
        # drop groups left empty
        tidy = []
        for i, l in enumerate(out):
            if l.startswith("# ") and (i + 1 == len(out) or out[i + 1].startswith("# ")):
                continue
            tidy.append(l)
        with open(menu_p, "w", encoding="utf-8") as f:
            f.write("\n".join(tidy) + "\n")
        rows = sum(1 for l in tidy if not l.startswith("#"))
    elif os.path.isfile(menu_p):                  # docs2doba's: its documents
        rows = sum(1 for l in open(menu_p, encoding="utf-8") if l.strip() and not l.startswith("#"))
        info.setdefault("unit", "documents")
        info["shown"] = str(rows)
    with open(info_p, "w", encoding="utf-8") as f:
        for k, v in info.items():
            f.write(f"{k}={v}\n")
    print(f"{book}: {rows} rows" + (f", {len(missing)} missing" if missing else ""))
    for m in missing:
        print("   missing:", m)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    lib = sys.argv[1]
    books = sys.argv[2:] or sorted(
        b for b in os.listdir(lib)
        if os.path.isfile(os.path.join(lib, b, "index.dti")) and not b.endswith((".new", ".old", ".part"))
        and (os.path.isfile(os.path.join(HERE, "menus", b + ".src")) or os.path.isfile(os.path.join(lib, b, "menu.txt"))))
    for b in books:
        build(lib, b)


if __name__ == "__main__":
    main()
