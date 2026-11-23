#!/usr/bin/env python3
"""Let search find a carefully translated article by the title it shows.

inject_translation.py puts the careful title on the article's T line but keeps
the twin's machine-translated index title (menus, links and twin.map use it).
Search goes by the index, so "Де немає лікаря" found nothing while the
article was titled so. This adds one more index record per such article: the
careful title as key and list title, pointing at the same text. The old
record stays, so nothing that used it breaks.

An article counts as carefully translated when its second line is the note
inject_translation.py writes.

    python3 title_aliases.py /path/to/lib/BOOK [BOOK ...]
"""
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from dobafold import KEY_BYTES, display, key  # noqa: E402

REC = struct.Struct("<56s56sHHIII")
HDR = 32
NOTE = "CПереклад: ШІ Claude, не перевірено медиком."


def sort_key(r):
    return (r[0].ljust(KEY_BYTES, b"\0"), r[3] & 1, r[1])


def run(book):
    p = os.path.join(book, "index.dti")
    data = open(p, "rb").read()
    head = bytearray(data[:HDR])
    n = struct.unpack_from("<I", head, 8)[0]
    recs = [REC.unpack_from(data, HDR + i * REC.size) for i in range(n)]
    keys = {r[0].rstrip(b"\0") for r in recs}
    files, seen, add = {}, set(), []
    for r in recs:
        if r[3] & 1 or (r[2], r[4]) in seen:
            continue
        f = files.get(r[2])
        if f is None:
            f = files[r[2]] = open(os.path.join(book, "a%04d.dta" % r[2]), "rb")
        f.seek(r[4])
        try:
            text = zlib.decompress(f.read(r[5])).decode("utf-8", "replace")
        except zlib.error:
            continue
        lines = text.split("\n", 2)
        if len(lines) < 2 or lines[1] != NOTE or not lines[0].startswith("T"):
            continue
        seen.add((r[2], r[4]))
        title = lines[0][1:].strip()
        k = key(title)
        if not k or k in keys:
            continue
        keys.add(k)
        add.append((k.ljust(KEY_BYTES, b"\0"), display(title).ljust(KEY_BYTES, b"\0"), r[2], r[3], r[4], r[5], 0))
    for f in files.values():
        f.close()
    if not add:
        print(f"{book}: nothing to add")
        return
    recs = sorted(recs + add, key=sort_key)
    struct.pack_into("<I", head, 8, len(recs))
    tmp = p + ".tmp"
    with open(tmp, "wb") as g:
        g.write(head)
        for r in recs:
            g.write(REC.pack(*r))
    os.replace(tmp, p)
    print(f"{book}: {len(add)} titles added, {len(recs)} records")


if __name__ == "__main__":
    for b in sys.argv[1:]:
        run(b)
