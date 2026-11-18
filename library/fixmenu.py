"""fixmenu.py LIB: in every twin (<book>_uk with twin.map), menu.txt rows that
open nothing are replaced by the twin title of the source menu's row on the
same line (twin.map pairs source and twin titles). QA 18 Nov."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dobafold import key

def full_titles(book):
    """(chunk, offset) -> the article's own title line: index and twin.map keep
    titles cut at 56 bytes ('Knots для гірського кемпінгу Clim…'), a menu row
    needs the whole title to open."""
    import zlib
    idx = open(os.path.join(book, "index.dti"), "rb").read()
    n = struct.unpack_from("<I", idx, 8)[0]
    out, dta = {}, {}
    for i in range(n):
        k, t, ch, fl, off, ln, _ = struct.unpack_from("<56s56sHHIII", idx, 32 + i * 128)
        if b"\x01" not in k:
            continue
        if ch not in dta:
            dta[ch] = open(os.path.join(book, "a%04d.dta" % ch), "rb").read()
        try:
            first = zlib.decompress(dta[ch][off:off + ln]).decode("utf-8", "replace").split("\n", 1)[0]
        except zlib.error:
            continue
        if first[:1] == "T":
            out[(ch, off)] = first[1:].strip()
    return out


def keys_of(book):
    idx = open(os.path.join(book, "index.dti"), "rb").read()
    n = struct.unpack_from("<I", idx, 8)[0]
    return {idx[32 + i * 128:32 + i * 128 + 56].rstrip(b"\0"): idx[32 + i * 128 + 56:32 + i * 128 + 112].rstrip(b"\0").decode("utf-8", "replace") for i in range(n)}

lib = sys.argv[1]
# A long title's key ends in a hash of the whole title, and the twin's key was
# made from the translation of the source index's (cut) title. With MODEL the
# same translation is made again, so the row names that exact string.
tr = None
if len(sys.argv) > 2:
    from translate_book import setup
    tr = setup(sys.argv[2])
for b in sorted(os.listdir(lib)):
    tw = os.path.join(lib, b, "twin.map")
    if not b.endswith("_uk") or not os.path.exists(tw):
        continue
    src = os.path.join(lib, b[:-3])
    um, sm = os.path.join(lib, b, "menu.txt"), os.path.join(src, "menu.txt")
    if not (os.path.exists(um) and os.path.exists(sm)):
        continue
    m = open(tw, "rb").read()
    n = struct.unpack_from("<I", m, 8)[0]
    pair, full = {}, full_titles(os.path.join(lib, b))
    for i in range(n):
        st = m[12 + i * 132 + 20:12 + i * 132 + 76].rstrip(b"\0").decode("utf-8", "replace")
        ut = m[12 + i * 132 + 76:12 + i * 132 + 132].rstrip(b"\0").decode("utf-8", "replace")
        ch, _, so, sn, to, tn = struct.unpack_from("<HHIIII", m, 12 + i * 132)
        pair[key(st).rstrip(b"\0")] = full.get((ch, to), ut)
    skeys, ukeys = keys_of(src), keys_of(os.path.join(lib, b))
    ul = open(um, encoding="utf-8").read().split("\n")
    sl = open(sm, encoding="utf-8").read().split("\n")
    fixed = 0
    for i, line in enumerate(ul):
        if not line.strip() or line.startswith("#") or i >= len(sl):
            continue
        t, _, rest = line.partition("|")
        if key(t.strip()).rstrip(b"\0") in ukeys:
            continue
        s = sl[i].partition("|")[0].strip()
        k = key(s).rstrip(b"\0")
        if k in skeys:
            k = key(skeys[k]).rstrip(b"\0")
        new = pair[k] if k in pair and key(pair[k]).rstrip(b"\0") in ukeys else None
        if not new and tr:
            for src_title in [v for kk, v in skeys.items() if key(v).rstrip(b"\0") == k]:
                cand = tr.translate(src_title)
                if key(cand).rstrip(b"\0") in ukeys:
                    new = cand
                    break
        if not new:   # lines out of step: the closest title of the twin, if close
            import difflib
            c = difflib.get_close_matches(t.strip(), list(ukeys.values()), n=1, cutoff=0.6)
            new = c[0] if c else None
        if new:
            ul[i] = new + ("|" + rest if rest or "|" in line else "")
            fixed += 1
            print(f"{b}: {t.strip()[:50]} -> {new[:50]}")
        else:
            print(f"{b}: no match for {t.strip()[:60]}")
    if fixed:
        open(um, "w", encoding="utf-8").write("\n".join(ul))
    print(f"{b}: {fixed} menu rows fixed")
