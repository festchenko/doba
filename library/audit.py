#!/usr/bin/env python3
"""Check every book on the card the way the device reads it, and list what is
wrong, book by book, with examples:

    python3 audit.py DOBA_BUILD/lib [book ...] > report.txt

Per book:
  records, articles, redirects, hidden
  clashes   titles whose keys are the same but pages differ: the device opens
            only the first (old 55-byte keys cut long titles)
  cut       keys cut at 55 bytes the old way (long titles that may clash)
  stubs     visible pages with almost no text (sampled)
  links     links in a sample of pages that lead nowhere
  menu      menu.txt / picks.txt rows the device cannot open
  info      info.txt without a description or unit
"""
import os
import random
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from dobafold import KEY_MAX, fold, key  # noqa: E402

REC, HDR = 128, 32
LINK = re.compile("\x01([^\x01\x02\x03]*)\x02([^\x01\x02\x03]*)\x03")


class Book:
    def __init__(self, d):
        self.d = d
        self.f = open(os.path.join(d, "index.dti"), "rb")
        self.n = struct.unpack("<8sIII12s", self.f.read(HDR))[1]

    def rec(self, i):
        self.f.seek(HDR + i * REC)
        r = self.f.read(REC)
        ch, flags, off, ln, _ = struct.unpack("<HHIII", r[112:128])
        return r[:56], r[56:112].rstrip(b"\0").decode("utf-8", "replace"), ch, flags, off, ln

    def lookup(self, k):
        want = k.ljust(56, b"\0")
        lo, hi = 0, self.n
        while lo < hi:
            mid = (lo + hi) // 2
            self.f.seek(HDR + mid * REC)
            if self.f.read(56) < want:
                lo = mid + 1
            else:
                hi = mid
        if lo < self.n:
            r = self.rec(lo)
            if r[0] == want:
                return r
        return None

    def find(self, title):
        """As the device: the new key, then the old 55-byte cut."""
        r = self.lookup(key(title))
        if r is None:
            full = fold(title, 10 ** 6)
            if len(full) > KEY_MAX:
                r = self.lookup(fold(title))
        return r

    def text(self, r):
        with open(os.path.join(self.d, "a%04d.dta" % r[2]), "rb") as d:
            d.seek(r[4])
            return zlib.decompress(d.read(r[5])).decode("utf-8", "replace")


def plain(t):
    t = LINK.sub(lambda m: m.group(1), t)
    return re.sub(r"(^|\n)[A-Z]", " ", t)


def audit(lib, name, sample=1500, link_pages=300):
    d = os.path.join(lib, name)
    b = Book(d)
    out = []
    arts = reds = hidden = cut = 0
    clash_groups, clash_recs, clash_ex = 0, 0, []
    prev, group = None, []
    visible = []

    def close_group():
        nonlocal clash_groups, clash_recs
        pages = {(r[2], r[4]) for r in group if not r[3] & 1}
        if len(pages) > 1:
            clash_groups += 1
            clash_recs += len(pages)
            if len(clash_ex) < 6:
                clash_ex.append(" | ".join(sorted({r[1] for r in group})[:3]))

    seq = open(os.path.join(d, "index.dti"), "rb", buffering=1 << 20)
    seq.seek(HDR)
    for i in range(b.n):
        raw = seq.read(REC)
        ch, flags, off, ln, _ = struct.unpack("<HHIII", raw[112:128])
        k = raw[:56]
        r = (k, raw[56:112].rstrip(b"\0").decode("utf-8", "replace"), ch, flags, off, ln)
        if flags & 1:
            reds += 1
        else:
            arts += 1
        if flags & 2:
            hidden += 1
        elif not flags & 1 and len(visible) < 2_000_000:
            visible.append(i)
        kk = k.rstrip(b"\0")
        if b"\x01" not in kk and len(kk) >= KEY_MAX - 3:
            cut += 1
        if k == prev:
            group.append(r)
        else:
            if len(group) > 1:
                close_group()
            group, prev = [r], k
    if len(group) > 1:
        close_group()

    out.append(f"== {name}: {b.n} records, {arts} articles, {reds} redirects, {hidden} hidden")
    info = {}
    p = os.path.join(d, "info.txt")
    if os.path.exists(p):
        for line in open(p, encoding="utf-8"):
            if "=" in line:
                a, v = line.rstrip("\n").split("=", 1)
                info[a] = v
    if not info.get("about") or not info.get("unit"):
        out.append(f"   info: missing {'about ' if not info.get('about') else ''}{'unit' if not info.get('unit') else ''}")
    if clash_groups:
        out.append(f"   CLASH: {clash_groups} keys shared by {clash_recs} different pages (device opens only the first)")
        for e in clash_ex:
            out.append(f"      e.g. {e}")
    if cut:
        out.append(f"   cut: {cut} keys cut at 55 bytes the old way")

    rnd = random.Random(1)
    pick = rnd.sample(visible, min(sample, len(visible)))
    stubs, stub_ex = 0, []
    for i in pick:
        r = b.rec(i)
        try:
            body = plain(b.text(r))
        except Exception:
            stubs += 1
            continue
        body = " ".join(body.split())
        if len(body) - len(r[1]) < 120:
            stubs += 1
            if len(stub_ex) < 5:
                stub_ex.append(f"{r[1]}: {body[:90]}")
    if pick:
        pct = 100.0 * stubs / len(pick)
        out.append(f"   stubs: {pct:.1f}% of {len(pick)} sampled visible pages have under 120 characters")
        if pct >= 2:
            for e in stub_ex:
                out.append(f"      e.g. {e}")

    links = dead = 0
    dead_ex = []
    for i in pick[:link_pages]:
        r = b.rec(i)
        try:
            t = b.text(r)
        except Exception:
            continue
        for m in LINK.finditer(t):
            tgt = m.group(2)
            if not tgt or tgt.startswith("#"):
                continue
            links += 1
            if b.find(tgt) is None:
                dead += 1
                if len(dead_ex) < 5:
                    dead_ex.append(f"{r[1]} -> {tgt[:70]}")
    if links:
        pct = 100.0 * dead / links
        out.append(f"   links: {pct:.1f}% of {links} links in {min(link_pages, len(pick))} pages lead nowhere")
        if pct >= 2:
            for e in dead_ex:
                out.append(f"      e.g. {e}")

    for fn in ("menu.txt", "picks.txt"):
        p = os.path.join(d, fn)
        if not os.path.exists(p):
            continue
        rows = bad = 0
        bad_ex = []
        for line in open(p, encoding="utf-8"):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            t = line.split("|", 1)[0].strip()
            rows += 1
            if b.find(t) is None:
                bad += 1
                bad_ex.append(t)
        out.append(f"   {fn}: {rows} rows" + (f", {bad} do not open: " + "; ".join(bad_ex[:8]) if bad else ""))
    if not os.path.exists(os.path.join(d, "menu.txt")):
        out.append("   menu.txt: none (the book opens on its A-Z / list)")
    return "\n".join(out)


def main():
    lib = sys.argv[1]
    books = sys.argv[2:] or sorted(
        x for x in os.listdir(lib)
        if os.path.isfile(os.path.join(lib, x, "index.dti")) and not x.endswith((".new", ".old", ".part")))
    for name in books:
        try:
            print(audit(lib, name), flush=True)
        except Exception as e:
            print(f"== {name}: audit failed: {e}", flush=True)


if __name__ == "__main__":
    main()
