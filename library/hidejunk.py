#!/usr/bin/env python3
"""Mark a book's housekeeping pages hidden (flag bit 1 in index.dti): user
profiles and answer stubs of a Q&A site, Wikipedia's Category:/Portal:/Template:
pages, Wikisource's archive scans, Appropedia's translations. They stay in the
index (links to them still open) but search and A-Z skip them.

    python3 hidejunk.py /path/to/lib [book...]
"""
import os
import re
import struct
import sys
import zlib

REC, HDR = 128, 32
HIDDEN = 2

NS = ("Category", "Portal", "Template", "Wikipedia", "Help", "File", "Module", "Draft", "TimedText",
      "MediaWiki", "Book", "Special", "Talk", "User",
      "Категорія", "Портал", "Шаблон", "Вікіпедія", "Довідка", "Файл", "Модуль", "Обговорення",
      "Користувач", "Архів", "Індекс", "Сторінка")
WIKI = [re.compile(r"^(%s):" % "|".join(NS))]
QA = [re.compile(r"^User .+"),                                     # profiles
      re.compile(r"^(a|questions|users|tags|posts)/[\w/=.\-]*$"),   # answer stubs, paths, tag pages
      re.compile(r"_page=\d+$")]
TRANSLATIONS = [re.compile(r"/(?:[a-z]{2}|[a-z]{2}-[a-z]{2,4}|zh-han[st])$")]   # Appropedia's


# By what a page says, for the books where empty pages are junk (not for the
# encyclopedias, where a two-line article is still an answer):
#   stub   no picture, no link, under 120 characters of text
#   iFixit a device page left with its photo and nothing else
#   Gutenberg a book's cover / author card ("The first producer of free ebooks")
#   scanned manuals: a chapter cut from OCR noise ("Fermentation — d a")
STUB_BOOKS = {"shelf_en", "shelf_uk", "ifixit", "disaster", "food", "knots", "medicine", "appropedia", "gov"}
DOCS_BOOKS = {"disaster", "food", "knots", "medicine", "water", "gov"}
LINK = re.compile("\x01([^\x01\x02\x03]*)\x02([^\x01\x02\x03]*)\x03")


def empty_page(text, title, book):
    lines = text.split("\n")
    pics = any(x.startswith("I") for x in lines)
    links = LINK.search(text) is not None
    body = " ".join(LINK.sub(lambda m: m.group(1), x[1:]) for x in lines[1:] if x and x[0] != "I")
    body = " ".join(body.split())
    if book == "shelf_en" and "The first producer of free ebooks" in text and len(body) < 600:
        return True
    if book == "ifixit" and not links and len(body) < 60:
        return True
    return not pics and not links and len(body) < 120


def ocr_noise(title):
    if " \u2014 " not in title:
        return False
    part = title.split(" \u2014 ", 1)[1]
    letters = [w for w in re.findall(r"[^\W\d_]+", part)]
    return sum(len(w) for w in letters) < 3 or (len(letters) >= 2 and max(len(w) for w in letters) <= 2)


def junk(title, book=""):
    pats = WIKI + (QA if book.endswith("_se") else []) + (TRANSLATIONS if book == "appropedia" else [])
    return any(p.search(title) for p in pats)


def run(path):
    book = os.path.basename(path.rstrip("/"))
    p = os.path.join(path, "index.dti")
    tmp = p + ".tmp"
    hidden = 0
    texts = {}
    content = book in STUB_BOOKS

    def text_of(ch, off, ln):
        d = texts.get(ch)
        if d is None:
            d = texts[ch] = open(os.path.join(path, "a%04d.dta" % ch), "rb")
        d.seek(off)
        try:
            return zlib.decompress(d.read(ln)).decode("utf-8", "replace")
        except zlib.error:
            return ""

    with open(p, "rb") as f, open(tmp, "wb") as g:
        h = f.read(HDR)
        n = struct.unpack("<8sIII12s", h)[1]
        g.write(h)
        left = n
        while left:
            k = min(left, 65536)
            buf = bytearray(f.read(k * REC))
            for i in range(k):
                o = i * REC
                t = bytes(buf[o + 56:o + 112]).rstrip(b"\0").decode("utf-8", "replace")
                fl = buf[o + 114] | buf[o + 115] << 8
                bad = junk(t, book) or (book in DOCS_BOOKS and ocr_noise(t))
                if not bad and content and not fl & 1:
                    ch = buf[o + 112] | buf[o + 113] << 8
                    off, ln = struct.unpack("<II", bytes(buf[o + 116:o + 124]))
                    if ln < 2500:
                        bad = empty_page(text_of(ch, off, ln), t, book)
                if bad:
                    fl |= HIDDEN
                    hidden += 1
                else:
                    fl &= ~HIDDEN
                buf[o + 114], buf[o + 115] = fl & 0xFF, fl >> 8
            g.write(buf)
            left -= k
    for d in texts.values():
        d.close()
    os.replace(tmp, p)
    print(f"{os.path.basename(path)}: {hidden} of {n} hidden")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    lib = sys.argv[1]
    books = sys.argv[2:] or sorted(b for b in os.listdir(lib)
                                   if os.path.isfile(os.path.join(lib, b, "index.dti"))
                                   and not b.endswith((".new", ".old", ".part")))
    for b in books:
        run(os.path.join(lib, b))


if __name__ == "__main__":
    main()
