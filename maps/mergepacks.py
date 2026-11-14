#!/usr/bin/env python3
"""Merge the map packs of several regions into one tree for the card.

Each region is rendered on its own: its own base.dtp (zoom 0-8) and, along a
border, its own pack for a z8 cell the neighbour also has. Copied one after
the other onto the card, the second would overwrite the first. Here a file
that only one region has is copied as is (hard-linked when possible); a file
two or more regions share is rebuilt tile by tile, each tile composited by
the darker pixel — the map is ink on white, so both sides of the border show.

    python3 mergepacks.py --out data/tiles/_card data/tiles/netherlands data/tiles/germany ...
"""
import argparse
import os
import shutil
import sys

import numpy as np

from tilepack import PackReader, PackWriter


def files_of(root):
    out = {}
    for d, _, fs in os.walk(root):
        for f in fs:
            if f.endswith(".dtp") and not f.startswith("._"):
                p = os.path.join(d, f)
                out[os.path.relpath(p, root)] = p
    return out


def merge(paths, dst):
    readers = [PackReader(p) for p in paths]
    r0 = readers[0]
    w = PackWriter(dst, r0.cell_z, r0.cell_x, r0.cell_y, relative=bool(r0.flags & 1))
    keys = sorted({k for r in readers for k in r.keys})
    for z, x, y in keys:
        # readers keep relative keys for detail packs: back to absolute for get()
        if r0.flags & 1:
            k = 2 ** (z - r0.cell_z)
            ax, ay = x + r0.cell_x * k, y + r0.cell_y * k
        else:
            ax, ay = x, y
        tile = None
        for r in readers:
            t = r.get(z, ax, ay)
            if t is not None:
                tile = t if tile is None else np.minimum(tile, t)
        w.add(z, ax, ay, tile)
    for r in readers:
        r.close()
    w.close()


def _merge_job(args):
    rel, ps, dst = args
    merge(ps, dst)
    return rel


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=1, help="border packs merged in parallel")
    ap.add_argument("regions", nargs="+")
    a = ap.parse_args()
    by_rel = {}
    for root in a.regions:
        for rel, p in files_of(root).items():
            by_rel.setdefault(rel, []).append(p)
    merged = linked = kept = 0
    todo = []
    # Single-region packs first: plain copies, most of the bytes, quick. The
    # border packs (rebuilt tile by tile, CPU-bound) after, several at a time.
    for rel, ps in sorted(by_rel.items()):
        dst = os.path.join(a.out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        newest = max(os.path.getmtime(p) for p in ps)
        if os.path.exists(dst) and os.path.getmtime(dst) >= newest and \
                (len(ps) > 1 or os.path.getsize(dst) == os.path.getsize(ps[0])):
            kept += 1
            continue                      # done on an earlier run (a copy cut short has the wrong size)
        if len(ps) == 1:
            if os.path.exists(dst):
                os.remove(dst)
            try:
                os.link(ps[0], dst)
            except OSError:
                shutil.copy2(ps[0], dst)
            linked += 1
        else:
            todo.append((rel, ps, dst))
    print(f"{linked} copied, {kept} unchanged; {len(todo)} border packs to merge", flush=True)
    if a.jobs > 1 and len(todo) > 1:
        from multiprocessing import Pool
        with Pool(a.jobs) as pool:
            for rel in pool.imap_unordered(_merge_job, todo):
                merged += 1
                print(f"  merged {rel} ({merged}/{len(todo)})", flush=True)
    else:
        for job in todo:
            _merge_job(job)
            merged += 1
            print(f"  merged {job[0]} ({merged}/{len(todo)})", flush=True)
    print(f"{linked} copied, {merged} merged, {kept} unchanged -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
