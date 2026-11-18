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


# Words the translator gets wrong in these books, fixed after it: "knots" came
# out as "ноти" (musical notes) all through knots_uk, disaster_uk and
# medicine_uk (QA 18 Nov). Applied only when the English line has the word.
GLOSSARY = [
    (re.compile(r"\bknots?\b", re.I), [
        (r"\bНоти\b", "Вузли"), (r"\bноти\b", "вузли"), (r"\bНот\b", "Вузлів"), (r"\bнот\b", "вузлів"),
        (r"\bнотами\b", "вузлами"), (r"\bнотах\b", "вузлах"), (r"\bнотам\b", "вузлам"),
        (r"\bНота\b", "Вузол"), (r"\bнота\b", "вузол"), (r"\bноту\b", "вузол"), (r"\bноти\b", "вузли"),
        (r"\bKnots\b", "Вузли"), (r"\bknots\b", "вузли"), (r"\bKnot\b", "Вузол"), (r"\bknot\b", "вузол"),
    ]),
]


def glossary(src, uk):
    for when, fixes in GLOSSARY:
        if when.search(src):
            for a, b in fixes:
                uk = re.sub(a, b, uk)
    return uk


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


WORD_RE = re.compile(r"\w[\w'’-]*", re.U)


def stem(w):
    """A word without its Ukrainian ending: the cases change the last letters
    ("кровотеча", "кровотечі", "кровотечу"), so a link is found by its stem."""
    w = w.lower()
    return w[:max(3, len(w) - 3)] if len(w) > 4 else w


def relink(src_line, uk_line, tr, title_uk):
    """The source line's links, put back into its translation. Each link's
    words (translated alone, and the translated title it points to) are looked
    for in the translated line by their stems; found, those words become the
    link, to the target's Ukrainian title (the twin's own index). Links not
    found are returned, to be listed at the article's end: none is lost."""
    links = LINK_RE.findall(src_line)
    if not links:
        return uk_line, []
    words = [(m.start(), m.end(), m.group(0)) for m in WORD_RE.finditer(uk_line)]
    taken = [False] * len(words)
    spans, lost = [], []
    for text, target in links:
        to = title_uk(target or text)
        found = None
        for cand in (tr(text), to):
            cw = [stem(w) for w in WORD_RE.findall(cand or "")]
            if not cw:
                continue
            for i in range(len(words) - len(cw) + 1):
                if any(taken[i:i + len(cw)]):
                    continue
                if all(words[i + j][2].lower().startswith(cw[j]) for j in range(len(cw))):
                    found = (i, i + len(cw))
                    break
            if found:
                break
        if found:
            for j in range(*found):
                taken[j] = True
            spans.append((words[found[0]][0], words[found[1] - 1][1], to))
        else:
            lost.append((tr(text) or to, to))
    out, last = [], 0
    for a, b, to in sorted(spans):
        out.append(uk_line[last:a] + "\x01" + uk_line[a:b] + "\x02" + to + "\x03")
        last = b
    out.append(uk_line[last:])
    return "".join(out), lost


def plain(t):
    return LINK_RE.sub(lambda m: m.group(1), t).replace("\x01", "").replace("\x02", "").replace("\x03", "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--model", default="")
    ap.add_argument("--name", default="")
    ap.add_argument("--fake", action="store_true", help='no Argos: "translate" by marking (tests)')
    ap.add_argument("--fake-strict", action="store_true",
                    help="no Argos: a different word for every different string, as a real translator may (tests)")
    ap.add_argument("--limit", type=int, default=0, help="translate the first N articles only (tests)")
    a = ap.parse_args()
    if a.fake_strict:
        import hashlib

        class Strict:
            def translate(self, s):
                return "Укр " + hashlib.md5(s.encode()).hexdigest()[:8]
        t = Strict()
    elif a.fake:
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
                memo[s] = glossary(s, t.translate(s))
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

    # A link points at an article's title; in the twin, at its Ukrainian title
    # (the one the twin's index is built from below).
    title_memo = {}
    # A link names its target the way the source page did ("Safe Water School/
    # Lesson 1"), not always as the target's own title ("Safe water school -
    # Lesson 1"). Translated as it stands, that came out differently from the
    # title the twin's index is built from, and half the links in some twins led
    # nowhere (QA, 18 Nov: water_uk 63 %, disaster_uk 49 %). So the target is
    # looked up in the source's index first, the way the device does, and the
    # translation of THAT record's title is used: the same string, the same
    # translation, the same key as the index.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from dobafold import key as fold_key
    # The index keeps a title cut at 56 bytes ("Knots for Mountaineering Camping
    # Climbing Utility Res…"); translated as it stands, the twin's title ended in
    # "Res ..." and a menu row naming the whole title opened nothing (QA 18 Nov).
    # The article's own first line holds the whole title: that is translated.
    def whole(r):
        t = r[1].rstrip(b"\0").decode("utf-8", "replace")
        if not t.endswith("…"):
            return t
        try:
            first = zlib.decompress(dta[r[2]][r[4]:r[4] + r[5]]).decode("utf-8", "replace").split("\n", 1)[0]
        except zlib.error:
            return t
        if first[:1] == "T" and fold_key(first[1:].strip()) == r[0].rstrip(b"\0"):
            return first[1:].strip()
        return t
    full = {(r[2], r[4], r[0]): whole(r) for r in recs}
    by_key = {}
    for r in recs:
        by_key.setdefault(r[0].rstrip(b"\0"), full[(r[2], r[4], r[0])])

    def title_uk(t):
        if t not in title_memo:
            own = by_key.get(fold_key(t).rstrip(b"\0")) if t else None
            title_memo[t] = tr(own or t)
        return title_memo[t]
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
        # The cache holds the translation without links (older runs too): they
        # go back in here, line by line against the source (the same lines).
        src_lines = zlib.decompress(dta[ch][off:off + ln]).decode("utf-8", "replace").split("\n")
        uk_lines = zlib.decompress(z).decode("utf-8", "replace").split("\n")
        if len(src_lines) == len(uk_lines) and any("\x01" in l for l in src_lines):
            lost = []
            for j, (sl, ul) in enumerate(zip(src_lines, uk_lines)):
                if sl[:1] in TEXT_KINDS and "\x01" in sl and ul[:1] == sl[:1]:
                    body, gone = relink(sl[1:], ul[1:], tr, title_uk)
                    uk_lines[j] = ul[:1] + body
                    lost += [g for g in gone if g not in lost]
            if lost:
                uk_lines.append("P" + "Див. також: " + ", ".join(
                    "\x01" + text + "\x02" + to + "\x03" for text, to in lost[:30]))
            z = zlib.compress("\n".join(uk_lines).encode("utf-8"), 9)
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
        uk = tr(full[(ch, off, k)])
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
