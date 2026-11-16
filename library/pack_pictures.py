#!/usr/bin/env python3
"""Packs a book's loose pictures (img/*.dgi, img/ab/*.dgi) into img/NN.dgp +
img/index.dgx, the way the big books keep theirs (dobapic.Packer).

A book of documents kept each picture as its own file: ~5000 in one folder for
school_uk and my_books. The card's file system finds a file by name by reading
its folder from the top, so every picture cost Doba about 4 s (16 Nov). A pack
is found by a binary search of its index: a few reads.

The loose files are moved out of the book (to --keep, default
<book>/../_loose/<book>/), not deleted: the card writer copies the book folder,
and the device reads the pack first anyway.

    pack_pictures.py BOOK_DIR [BOOK_DIR ...] [--keep DIR]
"""
import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dobapic import Packer  # noqa: E402


def loose(img):
    """(name, path) of every loose picture under img/ (one level of shards)."""
    out = []
    for e in sorted(os.listdir(img)):
        if e.startswith("._"):
            continue
        p = os.path.join(img, e)
        if e.endswith(".dgi") and os.path.isfile(p):
            out.append((e[:-4], p))
        elif os.path.isdir(p) and len(e) == 2:
            for f in sorted(os.listdir(p)):
                if f.endswith(".dgi") and not f.startswith("._"):
                    out.append((f[:-4], os.path.join(p, f)))
    return out


def finish(work, img):
    """Moves a pack from the work folder into img/ (macOS moves a file's ._
    twin along with it: those are skipped, not moved twice)."""
    for f in os.listdir(work):
        if f.startswith("._"):
            continue
        src = os.path.join(work, f)
        if os.path.exists(src):
            shutil.move(src, os.path.join(img, f))
    shutil.rmtree(work, ignore_errors=True)


def pack_book(book, keep):
    img = os.path.join(book, "img")
    if not os.path.isdir(img):
        return 0
    work = os.path.join(book, "img.packing")
    if os.path.isdir(work) and os.path.exists(os.path.join(work, "index.dgx")):
        finish(work, img)            # a run cut short after the loose files moved
        print(f"{book}: pack finished", flush=True)
    pics = loose(img)
    if not pics:
        return 0
    if os.path.exists(os.path.join(img, "index.dgx")):
        print(f"!! {book}: has a pack and loose pictures; leaving it", file=sys.stderr)
        return 0
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    p = Packer(book)                 # writes into book/img: point it at the work dir
    p.dir = work
    p.pack, p.f, p.pos = -1, None, 0
    p._next()
    skipped = 0
    for name, path in pics:
        if len(name) > 10 or not name.isascii():
            skipped += 1
            continue
        with open(path, "rb") as f:
            p.add(name, f.read())
    n = p.close()
    if skipped:
        print(f"!! {book}: {skipped} pictures with names a pack cannot hold stay loose", file=sys.stderr)
    # the loose files out of the book, the packs in
    dst = os.path.join(keep or os.path.join(os.path.dirname(os.path.abspath(book)), "_loose"),
                       os.path.basename(os.path.abspath(book)))
    os.makedirs(dst, exist_ok=True)
    for name, path in pics:
        if len(name) <= 10 and name.isascii():
            rel = os.path.relpath(path, img)
            os.makedirs(os.path.dirname(os.path.join(dst, rel)), exist_ok=True)
            shutil.move(path, os.path.join(dst, rel))
    for e in os.listdir(img):        # emptied shards, and macOS ._ twins of the moved files
        q = os.path.join(img, e)
        if os.path.isdir(q) and not os.listdir(q):
            os.rmdir(q)
        elif e.startswith("._") and os.path.exists(q):
            os.remove(q)
    finish(work, img)
    print(f"{book}: {n} pictures packed", flush=True)
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("books", nargs="+")
    ap.add_argument("--keep", default="")
    a = ap.parse_args()
    for b in a.books:
        pack_book(b, a.keep)
    return 0


if __name__ == "__main__":
    sys.exit(main())
