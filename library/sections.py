#!/usr/bin/env python3
"""Build /doba/lib/sections.txt: the library's icon sections, from sections.src.

sections.src is the curated part: for every section the books it searches and
the guides worth opening first, each with candidate articles in several books.
This script looks every candidate up in the books actually built (their
index.dti) and keeps the first one that exists, so the same sections.src works
for a card with Wikipedia only, or with WikiMed, the gov shelf and manuals too.

    python3 sections.py LIB_DIR [--src sections.src] [--out LIB_DIR/sections.txt]

sections.src lines (indent is cosmetic, '#' starts a comment):
    section ID | NAME | icon | book,book,...
      * Shown label | one-line description | book:Title ; book:Title ...   (START HERE card)
      - Shown label | one-line description | book:Title ; ...              (list row)
    emergency LABEL | icon | book:Title ; book:Title ...

A title ending in '*' matches the first article whose title starts with it.
"""
import argparse
import os
import struct
import sys

from dobafold import KEY_BYTES, fold

HDR = struct.Struct("<8sIII12s")
REC = struct.Struct("<56s56sHHIII")


def load_titles(book_dir):
    """folded key -> display title, for one built book (aliases included)."""
    path = os.path.join(book_dir, "index.dti")
    out = {}
    with open(path, "rb") as f:
        magic, n, size, _chunks, _ = HDR.unpack(f.read(HDR.size))
        if not magic.startswith(b"DOBAIX"):
            raise SystemExit(f"{path}: not a Doba index")
        for _ in range(n):
            rec = f.read(size)
            key, title = REC.unpack(rec[:REC.size])[:2]
            k = key.rstrip(b"\0")
            out.setdefault(k, title.rstrip(b"\0").decode("utf-8", "ignore"))
    return out


class Books:
    def __init__(self, lib):
        self.lib = lib
        self.cache = {}
        self.sorted = {}

    def has(self, book):
        return os.path.exists(os.path.join(self.lib, book, "index.dti"))

    def titles(self, book):
        if book not in self.cache:
            self.cache[book] = load_titles(os.path.join(self.lib, book)) if self.has(book) else {}
            self.sorted[book] = sorted(self.cache[book])
        return self.cache[book]

    def find(self, book, title):
        """The display title of `title` in `book`, or None."""
        t = self.titles(book)
        if title.endswith("*"):
            pre = fold(title[:-1])
            for k in self.sorted[book]:
                if k.startswith(pre):
                    return t[k]
            return None
        return t.get(fold(title))

    def first(self, cands):
        for c in cands:
            book, _, title = c.partition(":")
            book, title = book.strip(), title.strip()
            if book and title:
                hit = self.find(book, title)
                if hit:
                    return book, hit
        return None


def clean(s):
    return s.replace("|", "/").strip()


def build(lib, src_lines, warn=print):
    books = Books(lib)
    out, n_sec, n_ent, n_x, missed = [], 0, 0, 0, 0
    for raw in src_lines:
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("section "):
            sid, name, icon, blist = [p.strip() for p in line[8:].split("|")]
            present = [b for b in blist.split(",") if books.has(b.strip())]
            out.append(f"S|{sid}|{clean(name)}|{icon}|{','.join(present)}")
            n_sec += 1
        elif line[0] in "*-":
            label, desc, cands = [p.strip() for p in line[1:].split("|", 2)]
            hit = books.first(cands.split(";"))
            if not hit:
                warn(f"  - {label}: none of {cands}")
                missed += 1
                continue
            sid = out_last_section(out)
            out.append(f"E|{sid}|{'*' if line[0] == '*' else '-'}|{clean(label)}|{hit[0]}|{clean(hit[1])}|{clean(desc)}")
            n_ent += 1
        elif line.startswith("emergency "):
            label, icon, cands = [p.strip() for p in line[10:].split("|", 2)]
            hit = books.first(cands.split(";"))
            if not hit:
                warn(f"  ! {label}: none of {cands}")
                missed += 1
                continue
            out.append(f"X|{clean(label)}|{icon}|{hit[0]}|{clean(hit[1])}")
            n_x += 1
        else:
            raise SystemExit(f"sections.src: can't read: {raw.rstrip()}")
    return out, (n_sec, n_ent, n_x, missed)


def out_last_section(out):
    for l in reversed(out):
        if l.startswith("S|"):
            return l.split("|")[1]
    raise SystemExit("sections.src: a guide before any section")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("lib", help="the card's /doba/lib (one folder per book)")
    ap.add_argument("--src", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "sections.src"))
    ap.add_argument("--out")
    a = ap.parse_args()
    with open(a.src, encoding="utf-8") as f:
        lines, (s, e, x, m) = build(a.lib, f.readlines())
    # Emergency buttons first: the device reads them in any order, people read top-down.
    lines = [l for l in lines if l.startswith("X|")] + [l for l in lines if not l.startswith("X|")]
    out = a.out or os.path.join(a.lib, "sections.txt")
    with open(out + ".part", "w", encoding="utf-8") as f:
        f.write("# Doba library sections — made by library/sections.py, edit sections.src\n")
        f.write("\n".join(lines) + "\n")
    os.replace(out + ".part", out)
    print(f"{out}: {s} sections, {e} guides, {x} emergency buttons ({m} not on this card)")


if __name__ == "__main__":
    sys.exit(main())
