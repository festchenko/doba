"""menu_uk_subs.py LIB MODEL [BOOK...]: a twin's menu rows kept the source's
English subtitles ("For people with a water quality problem · Michigan State
University"). Each row's subtitle is made again from the SOURCE menu's row on
the same line: the words before the last " · " translated, the publisher kept.
From the source, so running it twice gives the same menu. QA 18 Nov."""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from translate_book import setup, glossary
lib, model = sys.argv[1], sys.argv[2]
only = set(sys.argv[3:])   # books to do (default: every twin)
t = setup(model)
for b in sorted(os.listdir(lib)):
    p = os.path.join(lib, b, "menu.txt")
    info = os.path.join(lib, b, "info.txt")
    if (only and b not in only) or not os.path.exists(p) or not os.path.exists(info):
        continue
    src = dict(l.rstrip("\n").split("=", 1) for l in open(info, encoding="utf-8") if "=" in l).get("twin_of")
    sp = os.path.join(lib, src or "-", "menu.txt")
    if not src or not os.path.exists(sp):
        continue
    lines = open(p, encoding="utf-8").read().split("\n")
    slines = open(sp, encoding="utf-8").read().split("\n")
    n = 0
    for i, line in enumerate(lines):
        if line.startswith("#") or "|" not in line or i >= len(slines) or "|" not in slines[i]:
            continue
        title = line.split("|", 1)[0]
        sub = slines[i].split("|", 1)[1]
        words, sep, who = sub.rpartition(" · ")
        if not sep:
            words, who = sub, ""
        if re.search(r"[A-Za-z]{3}", words):
            words = glossary(words, t.translate(words))
        lines[i] = title + "|" + words + (" · " + who if who else "")
        n += 1
    open(p, "w", encoding="utf-8").write("\n".join(lines))
    print(f"{b}: {n} subtitles from {src}")
