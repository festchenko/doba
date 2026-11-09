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


def junk(title, book=""):
    pats = WIKI + (QA if book.endswith("_se") else []) + (TRANSLATIONS if book == "appropedia" else [])
    return any(p.search(title) for p in pats)


def run(path):
    book = os.path.basename(path.rstrip("/"))
    p = os.path.join(path, "index.dti")
    tmp = p + ".tmp"
    hidden = 0
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
                if junk(t, book):
                    fl |= HIDDEN
                    hidden += 1
                else:
                    fl &= ~HIDDEN
                buf[o + 114], buf[o + 115] = fl & 0xFF, fl >> 8
            g.write(buf)
            left -= k
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
