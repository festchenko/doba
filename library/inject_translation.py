"""inject_translation.py LIB TRANSLATED.jsonl BOOK_uk [BOOK_uk ...]

Puts careful translations (made article by article, checked line by line) into
a twin book in place of its machine translation. TRANSLATED.jsonl holds
{"id", "part", "lines"} objects; the parts of one article, in order, are the
source article's lines translated one for one. An article is matched by the
SHA-1 of its source text, so the same document in two books (medicine and
disaster both carry some manuals) is replaced in both.

In each replaced article:
- the title line is the careful one too (the reader only shows it; the twin's
  index, menus and links keep their own titles);
- line 2 says who translated it: "CПереклад: ШІ Claude, не перевірено медиком."
  (the reader then adds no machine-translation note of its own);
- links: the source book's target is looked up the way the device does it, and
  the twin title of that article is put in; "#heading" targets get the
  translated heading; web, geo, phone and mail links stay; a target the twin
  does not have becomes plain words.

Writes BOOK_uk.new/ beside the book (a full copy with the new a0000.dta,
index.dti and twin.map); swapping it in is left to the caller.
"""
import hashlib, json, os, re, shutil, struct, sys, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dobafold import key, fold

HDR, REC, KEY_MAX = 32, 128, 55
LINK = re.compile(r"\x01([^\x01\x02\x03]*)\x02([^\x01\x02\x03]*)\x03")
KEEP = ("geo:", "tel:", "mailto:", "http")
NOTE = "CПереклад: ШІ Claude, не перевірено медиком."
TW = struct.Struct("<HHIIII56s56s")


def records(path):
    idx = open(path, "rb").read()
    n = struct.unpack_from("<I", idx, 8)[0]
    return idx, [struct.unpack_from("<56s56sHHIII", idx, HDR + i * REC) for i in range(n)]


def load_translations(path):
    parts = {}
    for line in open(path, encoding="utf-8"):
        if line.strip():
            o = json.loads(line)
            parts.setdefault(o["id"], []).append((o["part"], o["lines"]))
    return {k: [l for _, ls in sorted(v) for l in ls] for k, v in parts.items()}


def main():
    lib, tpath, books = sys.argv[1], sys.argv[2], sys.argv[3:]
    tr = load_translations(tpath)
    src_meta = {}
    for line in open(os.path.join(os.path.dirname(os.path.abspath(tpath)), "src.jsonl"), encoding="utf-8"):
        o = json.loads(line)
        if o["id"] in tr:
            src_meta[o["sha"]] = (o["id"], o["lines"])
    bad = [s for s, (i, ls) in src_meta.items() if len(tr[i]) != len(ls)]
    for s in bad:   # parts still missing: that article keeps its old text
        print("  incomplete, skipped:", src_meta.pop(s)[0])
    print(f"{len(src_meta)} translated articles")

    for name in books:
        d = os.path.join(lib, name)
        info = dict(l.rstrip("\n").split("=", 1) for l in open(os.path.join(d, "info.txt"), encoding="utf-8") if "=" in l)
        src = os.path.join(lib, info["twin_of"])
        _, srecs = records(os.path.join(src, "index.dti"))
        by_key = {r[0].rstrip(b"\0"): (r[2], r[4]) for r in srecs}
        sdta = {}

        def src_text(ch, off, ln):
            if ch not in sdta:
                sdta[ch] = open(os.path.join(src, "a%04d.dta" % ch), "rb")
            sdta[ch].seek(off)
            return zlib.decompress(sdta[ch].read(ln)).decode("utf-8")

        tmap = open(os.path.join(d, "twin.map"), "rb").read()
        n = struct.unpack_from("<I", tmap, 8)[0]
        pairs = [list(TW.unpack_from(tmap, 12 + i * TW.size)) for i in range(n)]
        twin_title = {(p[0], p[2]): p[7].rstrip(b"\0").decode("utf-8", "replace") for p in pairs}

        def find(t):
            k = key(t).rstrip(b"\0")
            if k not in by_key and len(fold(t, 10 ** 6)) > KEY_MAX:
                k = fold(t).rstrip(b"\0")
            return twin_title.get(by_key.get(k))

        dta = open(os.path.join(d, "a0000.dta"), "rb").read()
        new_raw = {}   # twin (off, len) -> new compressed article
        stats = [0, 0, 0]
        for p in pairs:
            ch, _, off, ln, toff, tln = p[:6]
            if (toff, tln) in new_raw:
                continue
            text = src_text(ch, off, ln)
            sha = hashlib.sha1(text.encode("utf-8")).hexdigest()
            if sha not in src_meta:
                continue
            aid, en = src_meta[sha]
            uk = list(tr[aid])
            heads = {}
            for e, u in zip(en, uk):
                if e[:1] in "HS" and u[:1] == e[:1]:
                    heads[LINK.sub(lambda m: m.group(1), e[1:]).strip()] = LINK.sub(lambda m: m.group(1), u[1:]).strip()

            def relink(m):
                stats[0] += 1
                shown, t = m.group(1), m.group(2)
                if t.startswith(KEEP):
                    return m.group(0)
                if t.startswith("#"):
                    h = heads.get(t[1:].strip())
                    return "\x01" + shown + "\x02#" + h + "\x03" if h else shown
                to = find(t)
                if to:
                    return "\x01" + shown + "\x02" + to + "\x03"
                stats[1] += 1
                return shown
            uk = [l[:1] + LINK.sub(relink, l[1:]) if "\x01" in l else l for l in uk]
            uk.insert(1, NOTE)
            new_raw[(toff, tln)] = zlib.compress("\n".join(uk).encode("utf-8"), 9)
            stats[2] += 1
        if not new_raw:
            print(f"{name}: nothing to replace")
            continue

        # rewrite a0000.dta in the old order; index and twin.map follow
        out = d + ".new"
        if os.path.exists(out):
            shutil.rmtree(out)
        shutil.copytree(d, out, ignore=shutil.ignore_patterns(".cache", "._*", "a0000.dta", "index.dti", "twin.map"))
        idx, recs = records(os.path.join(d, "index.dti"))
        idx = bytearray(idx)
        spans = sorted({(r[4], r[5]) for r in recs} | {(p[4], p[5]) for p in pairs})
        moved, pos = {}, 0
        with open(os.path.join(out, "a0000.dta"), "wb") as f:
            for o, l in spans:
                raw = new_raw.get((o, l), dta[o:o + l])
                f.write(raw)
                moved[(o, l)] = (pos, len(raw))
                pos += len(raw)
        for i, r in enumerate(recs):
            no, nl = moved[(r[4], r[5])]
            struct.pack_into("<HHIII", idx, HDR + i * REC + 112, r[2], r[3], no, nl, 0)
        open(os.path.join(out, "index.dti"), "wb").write(idx)
        with open(os.path.join(out, "twin.map"), "wb") as f:
            f.write(tmap[:12])
            for p in pairs:
                p[4], p[5] = moved[(p[4], p[5])]
                f.write(TW.pack(*p))
        print(f"{name}: {stats[2]} articles replaced; {stats[1]} of {stats[0]} links have no page in the twin -> plain words; written to {out}")


if __name__ == "__main__":
    main()
