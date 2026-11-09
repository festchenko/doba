#!/usr/bin/env python3
"""Picks for a big book: a list of titles (alternatives after '|') checked
against the book's index; the first title it has goes to <book>/picks.txt,
which the device shows first on the book's page."""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dobafold import fold  # noqa: E402

REC, HDR = 128, 32


def lookup(f, count, title):
    want = fold(title)[:55].ljust(56, b"\0")
    lo, hi = 0, count
    while lo < hi:
        mid = (lo + hi) // 2
        f.seek(HDR + mid * REC)
        if f.read(56) < want:
            lo = mid + 1
        else:
            hi = mid
    while lo < count:
        f.seek(HDR + lo * REC)
        r = f.read(REC)
        if r[:56] != want:
            return None
        t = r[56:112].rstrip(b"\0").decode("utf-8", "replace")
        if fold(t)[:55].ljust(56, b"\0") == want:
            return t
        lo += 1
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("book")
    ap.add_argument("src")
    a = ap.parse_args()
    path = os.path.join(a.book, "index.dti")
    with open(path, "rb") as f:
        count = struct.unpack("<8sIII12s", f.read(HDR))[1]
        out, missing = [], []
        for line in open(a.src, encoding="utf-8"):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            hit = next((t for t in (x.strip() for x in line.split("|")) if lookup(f, count, t)), None)
            (out.append(hit) if hit else missing.append(line))
    with open(os.path.join(a.book, "picks.txt"), "w", encoding="utf-8") as g:
        g.write("".join(t + "\n" for t in dict.fromkeys(out)))
    print(f"{len(out)} picks, {len(missing)} missing")
    for m in missing:
        print("  missing:", m)


if __name__ == "__main__":
    main()
