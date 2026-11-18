"""fixmenu.py LIB: in every twin (<book>_uk with twin.map), menu.txt rows that
open nothing are replaced by the twin title of the source menu's row on the
same line (twin.map pairs source and twin titles). QA 18 Nov."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dobafold import key

def keys_of(book):
    idx = open(os.path.join(book, "index.dti"), "rb").read()
    n = struct.unpack_from("<I", idx, 8)[0]
    return {idx[32 + i * 128:32 + i * 128 + 56].rstrip(b"\0"): idx[32 + i * 128 + 56:32 + i * 128 + 112].rstrip(b"\0").decode("utf-8", "replace") for i in range(n)}

lib = sys.argv[1]
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
    pair = {}
    for i in range(n):
        st = m[12 + i * 132 + 20:12 + i * 132 + 76].rstrip(b"\0").decode("utf-8", "replace")
        ut = m[12 + i * 132 + 76:12 + i * 132 + 132].rstrip(b"\0").decode("utf-8", "replace")
        pair[key(st).rstrip(b"\0")] = ut
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
