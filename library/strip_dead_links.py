"""strip_dead_links.py LIB BOOK: links whose page is not in the book become
plain words (iFixit: 63 % of its links went to authors' profiles, QA 18 Nov).
Places (geo:), phones, mail, web and #headings are kept: the reader handles
them. Writes a%04d.dta.new and index.dti.new beside the book's files, then
moves the old ones to *.pre (kept; delete them by hand once the book reads
well on the device) and the new ones into place."""
import os, re, struct, sys, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dobafold import key, fold

HDR, REC, KEY_MAX = 32, 128, 55
LINK = re.compile(r"\x01([^\x01\x02\x03]*)\x02([^\x01\x02\x03]*)\x03")
KEEP = ("geo:", "tel:", "mailto:", "http", "#")

lib, name = sys.argv[1], sys.argv[2]
d = os.path.join(lib, name)
idx = bytearray(open(os.path.join(d, "index.dti"), "rb").read())
n = struct.unpack_from("<I", idx, 8)[0]
keys = set()
for i in range(n):
    keys.add(bytes(idx[HDR + i * REC:HDR + i * REC + 56]).rstrip(b"\0"))


def exists(t):
    if key(t).rstrip(b"\0") in keys:
        return True
    full = fold(t, 10 ** 6)
    return len(full) > KEY_MAX and fold(t).rstrip(b"\0") in keys


moved, dead, links, outs, src = {}, 0, 0, {}, {}
for i in range(n):
    o = HDR + i * REC
    ch, fl, off, ln, _ = struct.unpack_from("<HHIII", idx, o + 112)
    if (ch, off) not in moved:
        if ch not in src:
            src[ch] = open(os.path.join(d, "a%04d.dta" % ch), "rb")
            outs[ch] = open(os.path.join(d, "a%04d.dta.new" % ch), "wb")
        src[ch].seek(off)
        raw = src[ch].read(ln)
        try:
            t = zlib.decompress(raw).decode("utf-8")
        except Exception:
            t = None
        if t is not None and "\x01" in t:
            def fix(m):
                global dead, links
                links += 1
                if m.group(2).startswith(KEEP) or exists(m.group(2)):
                    return m.group(0)
                dead += 1
                return m.group(1)
            t2 = LINK.sub(fix, t)
            if t2 != t:
                raw = zlib.compress(t2.encode("utf-8"), 9)
        moved[(ch, off)] = (outs[ch].tell(), len(raw))
        outs[ch].write(raw)
    noff, nln = moved[(ch, off)]
    struct.pack_into("<HHIII", idx, o + 112, ch, fl, noff, nln, 0)
    if i % 20000 == 0:
        print(f"  {i}/{n} records, {dead}/{links} links unlinked", flush=True)
for f in list(outs.values()) + list(src.values()):
    f.close()
open(os.path.join(d, "index.dti.new"), "wb").write(idx)
for ch in outs:
    p = os.path.join(d, "a%04d.dta" % ch)
    os.replace(p, p + ".pre")
    os.replace(p + ".new", p)
p = os.path.join(d, "index.dti")
os.replace(p, p + ".pre")
os.replace(p + ".new", p)
print(f"{name}: {dead} of {links} links led nowhere and are plain words now")
