#!/usr/bin/env python3
"""Convert a Kiwix ZIM into a Doba book (/doba/lib/<book>/ on the SD card).

ZIM keeps articles in big zstd clusters the ESP32 cannot unpack, so each
article is cleaned to plain text lines and stored as its own small zlib
stream, with a sorted fixed-width title index for binary search on the card.
Format: src/doba_library.h in the firmware.

    python3 zim2doba.py wikipedia_ru_all_nopic_2026-10.zim out/wiki_ru --name "Википедия RU"
    python3 zim2doba.py mdwiki_en_all_maxi.zim out/med_en --name "WikiMed EN"

Then copy out/<book> to /doba/lib/<book> on the card (tools/prepare-sd-mac.sh).
"""
import argparse
import multiprocessing as mp
import os
import re
import struct
import sys
import time
import zlib
from html.parser import HTMLParser

try:
    from libzim.reader import Archive
except ImportError:          # docs2doba imports the HTML cleaner without libzim
    Archive = None

import dobapic
from dobafold import KEY_BYTES, fold

REC = struct.Struct("<56s56sHHIII")      # 128 bytes
HDR = struct.Struct("<8sIII12s")         # 32 bytes
assert REC.size == 128 and HDR.size == 32

SKIP_TAGS = {"script", "style", "table", "sup", "nav", "footer", "figure", "math",
             "noscript", "svg", "button", "form"}
SKIP_CLASSES = ("infobox", "navbox", "reflist", "references", "mw-references",
                "hatnote", "thumb", "toc", "metadata", "noprint", "mw-editsection",
                "sidebar", "ambox", "catlinks", "printfooter", "gallery")
# Sections dropped whole: their heading and everything to the next one.
SKIP_SECTIONS = {
    "references", "notes", "external links", "see also", "further reading",
    "bibliography", "sources", "footnotes",
    "примечания", "литература", "ссылки", "см также", "источники",
    "примітки", "джерела", "посилання", "див також", "література",
}
BLOCKS = {"p": "P", "li": "B", "dd": "P", "blockquote": "P", "h2": "H", "h3": "S",
          "h4": "S", "h5": "S", "h6": "S", "pre": "P"}
VOID = {"br", "img", "hr", "meta", "link", "input", "wbr", "source", "area", "col"}

# Links inside article text: \x01 visible text \x02 target title \x03. The
# target is the ZIM path made title-like ("Water_purification" -> "Water
# purification"); the device folds it and looks it up in the book's index.
LINK_A, LINK_B, LINK_C = "\x01", "\x02", "\x03"


def link_target(href):
    """A title-like target for an in-book link, or None (external, anchors, files)."""
    if not href or href.startswith(("#", "http:", "https:", "//", "mailto:", "javascript:")):
        return None
    from urllib.parse import unquote
    h = unquote(href.split("#", 1)[0].split("?", 1)[0])
    while h.startswith(("./", "../")):
        h = h[2:] if h.startswith("./") else h[3:]
    if h.startswith("A/"):
        h = h[2:]
    if not h or "/" in h or h.startswith(("File:", "Special:", "Help:", "Category:", "Файл:", "Категория:")):
        return None
    t = " ".join(h.replace("_", " ").split())
    if not t or any(c in t for c in (LINK_A, LINK_B, LINK_C, "\n")):
        return None
    return t[:120]


PIC_SKIP_TAGS = SKIP_TAGS - {"figure"}
PIC_SKIP_CLASSES = tuple(c for c in SKIP_CLASSES if c != "thumb")
CAPTIONS = ("figcaption", "thumbcaption")
MAX_PICS = 16                    # per article


class Cleaner(HTMLParser):
    def __init__(self, picture=None):
        super().__init__(convert_charrefs=True)
        self.picture = picture   # (src, attrs) -> "name w h" or None
        self.pics = 0
        self.skip_tags = PIC_SKIP_TAGS if picture else SKIP_TAGS
        self.skip_classes = PIC_SKIP_CLASSES if picture else SKIP_CLASSES
        self.lines = []          # (kind, text)
        self.stack = []          # open tags: (tag, skipping)
        self.skip = 0
        self.kind = None
        self.buf = []
        self.skip_section = False
        self.links = []          # open <a>: target or None, per nesting level

    def _flush(self):
        if self.kind:
            text = " ".join("".join(self.buf).split())
            text = tidy_links(text) if self.kind in ("P", "B") else strip_links(text)
            if text:
                if self.kind == "H" or self.kind == "S":
                    self.skip_section = fold(text).decode() in SKIP_SECTIONS
                    if not self.skip_section:
                        self.lines.append((self.kind, text))
                elif not self.skip_section:
                    self.lines.append((self.kind, text))
        self.kind, self.buf = None, []

    def handle_starttag(self, tag, attrs):
        if tag in VOID:
            if tag == "br" and self.kind and not self.skip:
                self.buf.append(" ")
            if tag == "img" and self.picture and not self.skip and not self.skip_section \
                    and self.pics < MAX_PICS:
                a = dict(attrs)
                try:
                    small = int(a.get("width") or 999) < 60 or int(a.get("height") or 999) < 60
                except ValueError:
                    small = False
                ref = None if small else self.picture(a.get("src") or "", a)
                if ref:
                    kind = self.kind
                    self._flush()
                    self.lines.append(("I", ref))
                    self.pics += 1
                    self.kind = kind
            return
        cls = dict(attrs).get("class") or ""
        # Class tokens that start with a skipped name ("thumbinner", "navbox-inner"),
        # not any substring: <html class="vector-feature-toc-pinned"> is no "toc".
        toks = cls.split()
        skipping = tag in self.skip_tags or any(t.startswith(c) for t in toks for c in self.skip_classes)
        self.stack.append((tag, skipping, False))
        if skipping:
            self.skip += 1
            return
        if self.skip:
            return
        if tag in BLOCKS or (self.picture and (tag == "figcaption" or "thumbcaption" in cls)):
            self._flush()
            self.kind = BLOCKS.get(tag, "P")
            self.stack[-1] = (tag, False, True)   # a block opened here: closes with it
        if tag == "a":
            t = link_target(dict(attrs).get("href"))
            # Not nested, and only inside a block that's being kept.
            if t and self.kind and not any(self.links):
                self.buf.append(LINK_A)
                self.links.append(t)
            else:
                self.links.append(None)

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        # Pop to the matching tag (HTML in the wild is not always balanced).
        block = False
        while self.stack:
            t, skipping, opened = self.stack.pop()
            if skipping:
                self.skip -= 1
            if t == tag:
                block = opened
                break
        if tag == "a" and self.links:
            t = self.links.pop()
            if t and self.kind:
                self.buf.append(LINK_B + t + LINK_C)
        if not self.skip and (tag in BLOCKS or block):
            self._flush()

    def handle_data(self, data):
        if not self.skip and self.kind:
            self.buf.append(data)

    def close(self):
        super().close()
        self._flush()


_LINK_RE = re.compile("\x01([^\x01\x02\x03]*)\x02([^\x01\x02\x03]*)\x03")


def strip_links(text):
    text = _LINK_RE.sub(lambda m: m.group(1), text)
    return " ".join(text.replace(LINK_A, "").replace(LINK_B, "").replace(LINK_C, "").split())


def tidy_links(text):
    """Well-formed links only, their visible text trimmed and non-empty."""
    out, pos = [], 0
    for m in _LINK_RE.finditer(text):
        out.append(m.string[pos:m.start()])
        vis, tgt = m.group(1).strip(), m.group(2)
        lead = " " if m.group(1)[:1] == " " else ""
        tail = " " if m.group(1)[-1:] == " " else ""
        out.append(lead + (LINK_A + vis + LINK_B + tgt + LINK_C if vis else "") + tail)
        pos = m.end()
    out.append(text[pos:])
    s = "".join(out)
    # Stray markers (a link cut by a block boundary) go.
    s = re.sub("\x01(?![^\x01\x02\x03]*\x02[^\x01\x02\x03]*\x03)", "", s)
    s = re.sub("\x02[^\x01\x03]*$", "", s)
    return " ".join(s.split())


def to_text(title, html, picture=None):
    c = Cleaner(picture)
    try:
        c.feed(html)
        c.close()
    except Exception:
        pass
    out = ["T" + title]
    for kind, text in c.lines:
        out.append(kind + text)
    return "\n".join(out)


def utf8_cut(s, n):
    """At most n bytes of s in UTF-8, never splitting a character."""
    return s.encode("utf-8")[:n].decode("utf-8", "ignore").encode("utf-8")


# --- workers -------------------------------------------------------------------

_ARCHIVE = None
_OUT = None          # book folder for pictures, or None: text only
_PICS = {}           # zim path -> "name w h" | None, this worker's memory


def _init(path, out=None):
    global _ARCHIVE, _OUT
    _ARCHIVE = Archive(path)
    _OUT = out


def _picture_for(article_path):
    """A resolver for <img src> in one article: the picture from the ZIM,
    stored once as a .dgi named after its path."""
    import posixpath
    from urllib.parse import unquote

    def resolve(src, attrs):
        if not src or src.startswith(("data:", "http:", "https:", "//")):
            return None
        p = posixpath.normpath(posixpath.join(posixpath.dirname(article_path), unquote(src.split("?")[0])))
        p = p.lstrip("/")
        while p.startswith("../"):
            p = p[3:]
        if p.lower().endswith(".svg"):
            return None
        if p in _PICS:
            return _PICS[p]
        ref = None
        for cand in (p, p[2:] if p.startswith(("I/", "-/")) else "I/" + p):
            try:
                item = _ARCHIVE.get_entry_by_path(cand).get_item()
            except Exception:
                continue
            if not item.mimetype.startswith("image/") or "svg" in item.mimetype:
                break
            name = dobapic.hashed_name(cand)
            dest = dobapic.path_for(_OUT, name)
            try:
                if os.path.exists(dest):
                    with open(dest, "rb") as f:
                        w, h = struct.unpack("<HH", f.read(8)[4:8])
                else:
                    import io
                    from PIL import Image
                    img = Image.open(io.BytesIO(bytes(item.content)))
                    img.load()
                    if img.width < 60 or img.height < 60:
                        break
                    blob, w, h = dobapic.encode(img)
                    dobapic.save(_OUT, name, blob)
                ref = f"{name} {w} {h}"
            except Exception:
                ref = None
            break
        _PICS[p] = ref
        return ref
    return resolve


def _work(idx):
    """Returns ('A', path, title, zbytes) | ('R', title, target_path) | None."""
    try:
        e = _ARCHIVE.get_entry_by_id(idx)
    except Exception:
        return None
    path = e.path
    if path.startswith(("-/", "M/", "X/", "W/", "I/", "_")):
        return None
    if e.is_redirect:
        try:
            return ("R", e.title or path, e.get_redirect_entry().path)
        except Exception:
            return None
    try:
        item = e.get_item()
    except Exception:
        return None
    if not item.mimetype.startswith("text/html"):
        return None
    html = bytes(item.content).decode("utf-8", "replace")
    title = e.title or path.rsplit("/", 1)[-1].replace("_", " ")
    text = to_text(title, html, _picture_for(path) if _OUT else None)
    if len(text) < len(title) + 40:     # empty stubs, disambiguation shells
        return None
    return ("A", path, title, zlib.compress(text.encode("utf-8"), 9))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("zim")
    ap.add_argument("out")
    ap.add_argument("--name", help="shown on the device (default: the ZIM's title)")
    ap.add_argument("--source", help='shown under every article, e.g. "Wikipedia EN · CC BY-SA" '
                    "(default: the ZIM's creator and source URL)")
    ap.add_argument("--chunk-mb", type=int, default=512)
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--limit", type=int, default=0, help="first N entries only (testing)")
    ap.add_argument("--no-images", action="store_true", help="text only (pictures in the ZIM are skipped)")
    a = ap.parse_args()

    arc = Archive(a.zim)
    def meta(k):
        try:
            return bytes(arc.get_metadata(k)).decode("utf-8", "replace")
        except Exception:
            return ""
    name = a.name or meta("Title") or os.path.basename(a.out)
    lang = meta("Language")
    total = arc.entry_count if not a.limit else min(a.limit, arc.entry_count)
    os.makedirs(a.out, exist_ok=True)

    chunk, chunk_f, chunk_pos = 0, None, 0
    limit = a.chunk_mb << 20
    def open_chunk():
        nonlocal chunk_f, chunk_pos
        if chunk_f:
            chunk_f.close()
        chunk_f = open(os.path.join(a.out, "a%04d.dta" % chunk), "wb")
        chunk_pos = 0
    open_chunk()

    where = {}        # article path -> (chunk, off, len)
    records = []      # (key, title bytes, chunk, flags, off, len)
    redirects = []    # (title, target path)
    t0 = time.time()
    done = arts = 0
    pics = None if a.no_images else a.out
    if pics:
        try:
            import PIL  # noqa: F401
        except ImportError:
            print("  (no Pillow: pictures skipped — pip install pillow)")
            pics = None
    with mp.Pool(a.jobs, initializer=_init, initargs=(a.zim, pics)) as pool:
        for r in pool.imap(_work, range(total), chunksize=64):
            done += 1
            if r is None:
                pass
            elif r[0] == "R":
                redirects.append((r[1], r[2]))
            else:
                _, path, title, z = r
                if chunk_pos + len(z) > limit:
                    chunk += 1
                    open_chunk()
                chunk_f.write(z)
                where[path] = (chunk, chunk_pos, len(z))
                records.append((fold(title), utf8_cut(title, KEY_BYTES), chunk, 0, chunk_pos, len(z)))
                chunk_pos += len(z)
                arts += 1
            if done % 20000 == 0:
                rate = done / max(time.time() - t0, 1e-6)
                print(f"  {done}/{total} entries, {arts} articles, {rate:.0f}/s", flush=True)
    chunk_f.close()

    linked = 0
    for title, target in redirects:
        w = where.get(target)
        if w:
            records.append((fold(title), utf8_cut(title, KEY_BYTES), w[0], 1, w[1], w[2]))
            linked += 1
    records = [r for r in records if r[0]]
    records.sort(key=lambda r: (r[0].ljust(KEY_BYTES, b"\0"), r[3], r[1]))

    with open(os.path.join(a.out, "index.dti"), "wb") as f:
        f.write(HDR.pack(b"DOBAIX1\0", len(records), REC.size, chunk + 1, b"\0" * 12))
        for key, title, ch, flags, off, ln in records:
            f.write(REC.pack(key, title, ch, flags, off, ln, 0))
    source = a.source
    if not source:
        meta = {}
        for k in ("Creator", "Source", "Publisher"):
            try:
                meta[k] = bytes(Archive(a.zim).get_metadata(k)).decode("utf-8", "ignore").strip()
            except Exception:
                pass
        source = " · ".join(x for x in (meta.get("Creator"), meta.get("Source")) if x) or os.path.basename(a.zim)
    source = " ".join(source.split())[:60]
    with open(os.path.join(a.out, "info.txt"), "w", encoding="utf-8") as f:
        f.write(f"name={name}\nlang={lang}\narticles={arts}\nredirects={linked}\n"
                f"source={source}\nfile={os.path.basename(a.zim)}\n")
    size = sum(os.path.getsize(os.path.join(a.out, x)) for x in os.listdir(a.out))
    print(f"done: {arts} articles + {linked} redirects, {chunk + 1} chunks, "
          f"{size / 1e6:.1f} MB, {time.time() - t0:.0f}s -> {a.out}")


if __name__ == "__main__":
    sys.exit(main())
