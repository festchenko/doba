#!/usr/bin/env python3
"""A Ukrainian twin of an English Doba book, machine-translated offline with
Argos Translate (en->uk).

    translate_book.py SRC_BOOK OUT_BOOK [--model X.argosmodel]

The twin is a book of its own (index.dti by its Ukrainian titles, so search
and A-Z work in Ukrainian) plus twin.map, which pairs each source article with
its translation: the reader flips between the two at the same place. Titles,
headings and text are translated; the pictures are the source's (copied).
Already translated articles are
kept in OUT_BOOK/.cache, so an interrupted run carries on where it stopped.
"""
import argparse
import json
import os
import re
import struct
import sys
import time
import zlib

REC = struct.Struct("<56s56sHHIII")
HDR = struct.Struct("<8sIII12s")
TEXT_KINDS = set("THSPBQ")
LINK_RE = re.compile("\x01([^\x01\x02\x03]*)\x02([^\x01\x02\x03]*)\x03")
SENT_RE = re.compile(r"(?<=[.!?])\s+(?=[A-Z0-9\"'(\[])")


def split_sentences(text):
    out = []
    for s in SENT_RE.split(text):
        while len(s) > 400:                 # run-ons: cut at a comma or space
            cut = max(s.rfind(", ", 0, 400), s.rfind(" ", 0, 400))
            cut = cut if cut > 100 else 400
            out.append(s[:cut + 1])
            s = s[cut + 1:]
        if s.strip():
            out.append(s)
    return out


def setup(model):
    import argostranslate.package as pkg
    import argostranslate.sbd as sbd
    for cls in (sbd.StanzaSentencizer, sbd.MiniSBDSentencizer, sbd.SpacySentencizerSmall):
        cls.split_sentences = lambda self, t: split_sentences(t)
    if not any(p.from_code == "en" and p.to_code == "uk" for p in pkg.get_installed_packages()):
        pkg.install_from_path(model)
    import argostranslate.translate as tr
    langs = {l.code: l for l in tr.get_installed_languages()}
    return langs["en"].get_translation(langs["uk"])


def plain(t):
    return LINK_RE.sub(lambda m: m.group(1), t).replace("\x01", "").replace("\x02", "").replace("\x03", "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--model", default="")
    ap.add_argument("--name", default="")
    ap.add_argument("--fake", action="store_true", help='no Argos: "translate" by marking (tests)')
    ap.add_argument("--limit", type=int, default=0, help="translate the first N articles only (tests)")
    a = ap.parse_args()
    if a.fake:
        class Mark:
            def translate(self, s):
                return "Укр " + s
        t = Mark()
    else:
        t = setup(a.model)
    memo = {}

    def tr(s):
        s = s.strip()
        if not s or not re.search(r"[A-Za-z]{2}", s):
            return s
        if s not in memo:
            try:
                memo[s] = t.translate(s)
            except Exception as e:
                print("  !! " + str(e)[:80], file=sys.stderr)
                memo[s] = s
        return memo[s]

    os.makedirs(os.path.join(a.out, ".cache"), exist_ok=True)
    idx = open(os.path.join(a.src, "index.dti"), "rb").read()
    magic, n, recsize, chunks, _ = HDR.unpack_from(idx, 0)
    recs = [list(REC.unpack_from(idx, HDR.size + i * recsize)) for i in range(n)]
    dta = [open(os.path.join(a.src, "a%04d.dta" % c), "rb").read() for c in range(chunks)]

    # Articles in their stored order; each translated once.
    arts = sorted({(r[2], r[4], r[5]) for r in recs})
    newpos = {}
    out_f = open(os.path.join(a.out, "a0000.dta"), "wb")
    pos = 0
    t0 = time.time()
    chars = 0
    for i, (ch, off, ln) in enumerate(arts):
        cache = os.path.join(a.out, ".cache", f"{ch}_{off}.z")
        if os.path.exists(cache):
            z = open(cache, "rb").read()
        else:
            text = zlib.decompress(dta[ch][off:off + ln]).decode("utf-8", "replace")
            if a.limit and i >= a.limit:
                text = text[:300]
            lines = []
            for line in text.split("\n"):
                k, body = line[:1], line[1:]
                if k in TEXT_KINDS and body:
                    body = plain(body)
                    chars += len(body)
                    body = tr(body)
                lines.append(k + body)
            z = zlib.compress("\n".join(lines).encode("utf-8"), 9)
            open(cache, "wb").write(z)
        out_f.write(z)
        newpos[(ch, off, ln)] = (pos, len(z))
        pos += len(z)
        if i % 10 == 0:
            el = time.time() - t0
            print(f"  {i + 1}/{len(arts)} articles, {chars / max(el, 1):.0f} chars/s", flush=True)
    out_f.close()

    # index.dti: the twin's own index, by its Ukrainian titles (search, A-Z, menu).
    # twin.map: source article <-> twin article, for the reader's EN/UA switch:
    # "DOBATW1\0", u32 count, then per article u16 source chunk, u16 0,
    # u32 source offset, u32 source length, u32 twin offset, u32 twin length,
    # source title[56], twin title[56] (132 bytes).
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from dobafold import display, key
    own, pairs = [], {}
    for k, title, ch, flags, off, ln, extra in recs:
        p, l = newpos[(ch, off, ln)]
        uk = tr(title.rstrip(b"\0").decode("utf-8", "replace"))
        if key(uk):
            own.append((key(uk), display(uk), 0, flags, p, l))
        if flags == 0 or (ch, off, ln) not in pairs:
            pairs[(ch, off, ln)] = (title.rstrip(b"\0"), display(uk), p, l)
    own.sort(key=lambda r: (r[0].ljust(56, b"\0"), r[3], r[1]))
    with open(os.path.join(a.out, "index.dti"), "wb") as f:
        f.write(HDR.pack(magic, len(own), recsize, 1, b"\0" * 12))
        for r in own:
            f.write(REC.pack(*r, 0))
    with open(os.path.join(a.out, "twin.map"), "wb") as f:
        f.write(b"DOBATW1\0" + struct.pack("<I", len(pairs)))
        for (ch, off, ln), (st, ut, p, l) in sorted(pairs.items()):
            f.write(struct.pack("<HHIIII56s56s", ch, 0, off, ln, p, l, st, ut))
    old = os.path.join(a.out, "twin.dti")
    if os.path.exists(old):
        os.remove(old)

    info = {}
    for line in open(os.path.join(a.src, "info.txt"), encoding="utf-8"):
        if "=" in line:
            k, v = line.rstrip("\n").split("=", 1)
            info[k] = v
    info["name"] = a.name or tr(info.get("name", ""))
    if info.get("about"):
        info["about"] = tr(info["about"])
    info["lang"] = "uk"
    info["twin_of"] = os.path.basename(os.path.normpath(a.src))
    info["source"] = (info.get("source", "") + " · машинний переклад")[:60]
    with open(os.path.join(a.out, "info.txt"), "w", encoding="utf-8") as f:
        for k, v in info.items():
            f.write(f"{k}={v}\n")

    # The book's first page: the same entries, their shown words translated.
    menu = os.path.join(a.src, "menu.txt")
    if os.path.exists(menu):
        out = []
        for line in open(menu, encoding="utf-8").read().split("\n"):
            if line.startswith("#") or "|" not in line:
                out.append(line)
                continue
            shown, rest = line.split("|", 1)
            out.append(tr(shown) + "|" + rest)
        open(os.path.join(a.out, "menu.txt"), "w", encoding="utf-8").write("\n".join(out))
    # The pictures are the source's: the same names, so the same files.
    src_img, out_img = os.path.join(a.src, "img"), os.path.join(a.out, "img")
    if os.path.isdir(src_img) and not os.path.exists(out_img):
        import shutil
        shutil.copytree(src_img, out_img)
    json.dump({"done": True}, open(os.path.join(a.out, ".cache", "done.json"), "w"))
    print(f"done: {len(arts)} articles -> {a.out}", flush=True)


if __name__ == "__main__":
    main()
