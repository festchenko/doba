#!/usr/bin/env python3
"""Give a converted book the new keys without converting it again.

Books converted before dobafold.key() cut every long title's key at 55 bytes,
so Wikisource chapters, dictionary entries, long iFixit guide names... share
one key and the device opens only the first. Each article's text starts with
its whole title (the "T" line), so its key and its list title can be made
again from that: a long title gets the hashed key and the short display.
Redirects keep their keys (their whole names are not stored); the device
looks a long name up by the new key first, then by the old cut.

    python3 rekey.py /path/to/lib/BOOK [BOOK ...]
"""
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from dobafold import KEY_BYTES, KEY_MAX, display, fold, key  # noqa: E402

REC = struct.Struct("<56s56sHHIII")
HDR = 32


def sort_key(r):
    return (r[0].ljust(KEY_BYTES, b"\0"), r[3] & 1, r[1])


def run(book):
    """Streams: Wikipedia's index is 20 million records, so only the long
    keys are held; the rest is merged back in order."""
    p = os.path.join(book, "index.dti")
    todo = []
    with open(p, "rb", buffering=1 << 20) as f:
        head = f.read(HDR)
        n = struct.unpack("<8sIII12s", head)[1]
        for i in range(n):
            r = REC.unpack(f.read(REC.size))
            if not r[3] & 1 and b"\x01" not in r[0] and len(r[0].rstrip(b"\0")) >= KEY_MAX - 3:
                todo.append((r[2], r[4], r[5], i))
    todo.sort()
    changed = {}
    files = {}
    for ch, off, ln, i in todo:
        d = files.get(ch)
        if d is None:
            d = files[ch] = open(os.path.join(book, "a%04d.dta" % ch), "rb")
        d.seek(off)
        try:
            text = zlib.decompress(d.read(ln)).decode("utf-8", "replace")
        except zlib.error:
            continue
        if not text.startswith("T"):
            continue
        title = text[1:].split("\n", 1)[0].strip()
        if len(fold(title, 10 ** 6)) <= KEY_MAX:
            continue
        nk = key(title)
        if nk:
            changed[i] = (nk.ljust(KEY_BYTES, b"\0"), display(title).ljust(KEY_BYTES, b"\0"))
    for d in files.values():
        d.close()
    moved = []
    tmp = p + ".tmp"
    with open(p, "rb", buffering=1 << 20) as f, open(tmp, "wb", buffering=1 << 20) as g:
        f.read(HDR)
        g.write(head)
        # the records whose keys changed, with their new keys, in order
        f2 = open(p, "rb")
        for i in sorted(changed):
            f2.seek(HDR + i * REC.size)
            r = list(REC.unpack(f2.read(REC.size)))
            r[0], r[1] = changed[i]
            moved.append(tuple(r))
        f2.close()
        moved.sort(key=sort_key)
        m = 0
        for i in range(n):
            raw = f.read(REC.size)
            if i in changed:
                continue
            r = REC.unpack(raw)
            sk = sort_key(r)
            while m < len(moved) and sort_key(moved[m]) <= sk:
                g.write(REC.pack(*moved[m]))
                m += 1
            g.write(raw)
        for r in moved[m:]:
            g.write(REC.pack(*r))
    os.replace(tmp, p)
    print(f"{os.path.basename(book.rstrip('/'))}: {len(changed)} of {len(todo)} long keys made whole ({n} records)",
          flush=True)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for b in sys.argv[1:]:
        run(b)
