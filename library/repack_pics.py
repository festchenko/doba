#!/usr/bin/env python3
"""Pack a book's loose pictures (img/**/<name>.dgi) into img/NN.dgp + index.dgx.

Books converted before pictures were packed keep one file per picture; on a
FAT32 card (32 KB clusters) and on exFAT (128 KB) that costs several times the
pictures themselves. The device reads packs first, loose files second, so a
book works the same before and after.

    python3 repack_pics.py <book dir> [--delete]
"""
import argparse
import os
import sys

import dobapic


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("book")
    ap.add_argument("--delete", action="store_true", help="remove the loose files once packed")
    a = ap.parse_args()
    img = os.path.join(a.book, "img")
    if not os.path.isdir(img):
        print(f"{a.book}: no pictures")
        return 0
    if os.path.exists(os.path.join(img, "index.dgx")):
        print(f"{a.book}: already packed")
        return 0
    loose = []
    for d, _, fs in os.walk(img):
        for f in fs:
            if f.endswith(".dgi") and not f.startswith("._"):
                loose.append(os.path.join(d, f))
    loose.sort()
    p = dobapic.Packer(a.book)
    for i, path in enumerate(loose):
        with open(path, "rb") as f:
            blob = f.read()
        if blob[:4] == b"DGI1":
            p.add(os.path.basename(path)[:-4], blob)
        if i % 20000 == 0:
            print(f"  {i}/{len(loose)}", flush=True)
    n = p.close()
    print(f"{a.book}: {n} pictures packed")
    if a.delete:
        for path in loose:
            os.remove(path)
            dot = os.path.join(os.path.dirname(path), "._" + os.path.basename(path))
            if os.path.exists(dot):
                os.remove(dot)
        for d, ds, fs in sorted(os.walk(img, topdown=False)):
            if d != img and not [x for x in os.listdir(d) if not x.startswith("._")]:
                for x in os.listdir(d):
                    os.remove(os.path.join(d, x))
                os.rmdir(d)
        print(f"  loose files removed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
