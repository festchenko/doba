#!/usr/bin/env python3
"""Everything Doba needs, onto its card, in one command — and a record of it.

    python3 tools/doba_card.py write  DATA [/Volumes/DOBA] [--jobs 3] [--only lib,map,dem]
    python3 tools/doba_card.py verify DATA [/Volumes/DOBA]
    python3 tools/doba_card.py status [/Volumes/DOBA]

DATA is the build folder (doba-build/data): lib/<book>/, tiles/<region>/ (a
region counts once logs/maps-<region>.status says "done", when that folder of
status files is there), dem-pack/, places.dgz, pois.dgp.

write copies what is new or changed only (size and time), each file to a
temporary name first and renamed when complete, so a copy cut short never
leaves a half file under the real name. Books still being built (*.new,
*.old, _old, .cache) stay behind. Map regions are merged by maps/mergepacks.py
(borders composited). At the end it writes /doba/manifest.json on the card:
what is there, from which build, when — and verify checks the card against
the build: every file present with its size, and the small files that say
what a book is (index, info, menu, twin map, sections) byte for byte.
"""
import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SKIP_DIRS = {"_old", ".cache"}
SMALL = ("index.dti", "info.txt", "menu.txt", "twin.map", "picks.txt", "sections.txt")


def skipped(name):
    return name.startswith(".") or name.startswith("._") or name.endswith((".new", ".old", ".part")) \
        or name in SKIP_DIRS


def walk(src):
    """Relative paths of the files under src that belong on the card."""
    for d, dirs, files in os.walk(src):
        dirs[:] = sorted(x for x in dirs if not skipped(x))
        for f in sorted(files):
            if not skipped(f):
                p = os.path.join(d, f)
                yield os.path.relpath(p, src), p


def same(a, b):
    try:
        sa, sb = os.stat(a), os.stat(b)
    except FileNotFoundError:
        return False
    return sa.st_size == sb.st_size and int(sa.st_mtime) <= int(sb.st_mtime) + 2


def copy_tree(src, dst, what):
    n = changed = bytes_ = 0
    t0 = time.time()
    for rel, p in walk(src):
        n += 1
        q = os.path.join(dst, rel)
        if same(p, q):
            continue
        os.makedirs(os.path.dirname(q), exist_ok=True)
        tmp = q + ".part"
        shutil.copyfile(p, tmp)
        st = os.stat(p)
        os.utime(tmp, (st.st_atime, st.st_mtime))
        os.replace(tmp, q)
        changed += 1
        bytes_ += st.st_size
        if changed % 200 == 0:
            print(f"   {what}: {changed} files, {bytes_ / 1e9:.1f} GB, {bytes_ / 1e6 / max(time.time() - t0, 1):.1f} MB/s",
                  flush=True)
    print(f"== {what}: {n} files, {changed} copied ({bytes_ / 1e9:.2f} GB) in {time.time() - t0:.0f} s", flush=True)


def regions(data):
    logs = os.path.join(os.path.dirname(data), "logs")
    out = []
    tiles = os.path.join(data, "tiles")
    for r in sorted(os.listdir(tiles)) if os.path.isdir(tiles) else []:
        if skipped(r) or r.startswith("_") or r.endswith(".v3") or r == "groenlo":
            continue
        st = os.path.join(logs, f"maps-{r}.status")
        if os.path.isdir(logs) and os.path.exists(st) and not open(st).read().startswith("done"):
            continue
        out.append(os.path.join(tiles, r))
    return out


def sha1(p):
    h = hashlib.sha1()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def manifest(data, dst):
    m = {"written": datetime.datetime.now().isoformat(timespec="seconds"), "build": data, "books": {}, "regions": []}
    lib = os.path.join(dst, "doba", "lib")
    for b in sorted(os.listdir(lib)) if os.path.isdir(lib) else []:
        bd = os.path.join(lib, b)
        if not os.path.isdir(bd) or skipped(b):
            continue
        info = {}
        if os.path.exists(os.path.join(bd, "info.txt")):
            for line in open(os.path.join(bd, "info.txt"), encoding="utf-8", errors="replace"):
                if "=" in line:
                    k, v = line.rstrip("\n").split("=", 1)
                    info[k] = v
        size = sum(os.path.getsize(p) for _, p in walk(bd))
        m["books"][b] = {"name": info.get("name", b), "articles": info.get("articles"), "bytes": size,
                         "index_sha1": sha1(os.path.join(bd, "index.dti")) if os.path.exists(os.path.join(bd, "index.dti")) else None}
    m["regions"] = [os.path.basename(r) for r in regions(data)]
    return m


def write(a):
    dst = a.dest
    if not os.path.isdir(dst):
        sys.exit(f"{dst} is not mounted")
    only = set(a.only.split(",")) if a.only else {"lib", "map", "dem"}
    for sub in ("lib", "map", "dem"):
        os.makedirs(os.path.join(dst, "doba", sub), exist_ok=True)
    if "lib" in only:
        subprocess.run([sys.executable, os.path.join(ROOT, "library", "sections.py"), os.path.join(a.data, "lib")], check=False)
        copy_tree(os.path.join(a.data, "lib"), os.path.join(dst, "doba", "lib"), "books")
    if "map" in only:
        rs = regions(a.data)
        print(f"== maps: {len(rs)} regions", flush=True)
        if rs:
            subprocess.run([sys.executable, "-u", os.path.join(ROOT, "maps", "mergepacks.py"), "--jobs", str(a.jobs),
                            "--out", os.path.join(dst, "doba", "map")] + rs, check=True)
        for f in ("places.dgz", "pois.dgp"):
            p = os.path.join(a.data, f)
            if os.path.exists(p) and not same(p, os.path.join(dst, "doba", "map", f)):
                shutil.copyfile(p, os.path.join(dst, "doba", "map", f + ".part"))
                os.replace(os.path.join(dst, "doba", "map", f + ".part"), os.path.join(dst, "doba", "map", f))
    if "dem" in only and os.path.isdir(os.path.join(a.data, "dem-pack")):
        copy_tree(os.path.join(a.data, "dem-pack"), os.path.join(dst, "doba", "dem"), "heights")
    for d, _, fs in os.walk(dst):          # macOS AppleDouble files
        for f in fs:
            if f.startswith("._"):
                try:
                    os.remove(os.path.join(d, f))
                except OSError:
                    pass
    m = manifest(a.data, dst)
    with open(os.path.join(dst, "doba", "manifest.json"), "w") as f:
        json.dump(m, f, ensure_ascii=False, indent=1)
    print(f"== manifest: {len(m['books'])} books, {len(m['regions'])} regions", flush=True)
    return verify(a)


def verify(a):
    bad = 0
    for what, src, sub in (("books", "lib", "lib"), ("heights", "dem-pack", "dem")):
        s = os.path.join(a.data, src)
        if not os.path.isdir(s):
            continue
        for rel, p in walk(s):
            q = os.path.join(a.dest, "doba", sub, rel)
            if not os.path.exists(q):
                print(f"MISSING {sub}/{rel}")
                bad += 1
            elif os.path.getsize(q) != os.path.getsize(p):
                print(f"SIZE    {sub}/{rel}: {os.path.getsize(q)} on the card, {os.path.getsize(p)} built")
                bad += 1
            elif os.path.basename(rel) in SMALL and sha1(p) != sha1(q):
                print(f"DIFFERS {sub}/{rel}")
                bad += 1
    print("== verify:", "the card matches the build" if not bad else f"{bad} problem(s)", flush=True)
    return 0 if not bad else 1


def status(a):
    p = os.path.join(a.dest, "doba", "manifest.json")
    if not os.path.exists(p):
        sys.exit("no manifest on the card (written by doba_card.py write)")
    m = json.load(open(p))
    print(f"written {m['written']} from {m['build']}")
    for b, x in m["books"].items():
        print(f"  {b:14s} {x['bytes'] / 1e9:7.2f} GB  {x['name']}")
    print(f"  maps: {len(m['regions'])} regions: {' '.join(m['regions'])}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["write", "verify", "status"])
    ap.add_argument("data", nargs="?")
    ap.add_argument("dest", nargs="?", default="/Volumes/DOBA")
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--only", default="", help="comma list of lib,map,dem")
    a = ap.parse_args()
    if a.cmd == "status":
        if a.data and not a.dest:
            a.dest = a.data
        elif a.data:
            a.dest = a.data if os.path.isdir(os.path.join(a.data, "doba")) else a.dest
        return status(a)
    if not a.data:
        ap.error("DATA is needed")
    return write(a) if a.cmd == "write" else verify(a)


if __name__ == "__main__":
    sys.exit(main())
