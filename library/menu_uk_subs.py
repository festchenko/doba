"""menu_uk_subs.py LIB MODEL: the twins' menu rows kept the source's English
subtitles ("For people with a water quality problem · Michigan State
University"). The words before the last " · " (the publisher stays as it is)
are translated. QA 18 Nov."""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from translate_book import setup
lib, model = sys.argv[1], sys.argv[2]
t = setup(model)
for b in sorted(os.listdir(lib)):
    p = os.path.join(lib, b, "menu.txt")
    if not b.endswith("_uk") or not os.path.exists(os.path.join(lib, b, "twin.map")) or not os.path.exists(p):
        continue
    lines = open(p, encoding="utf-8").read().split("\n")
    n = 0
    for i, line in enumerate(lines):
        if line.startswith("#") or "|" not in line:
            continue
        title, sub = line.split("|", 1)
        if not re.search(r"[A-Za-z]{3}", sub):
            continue
        words, sep, who = sub.rpartition(" · ")
        if not sep:
            words, who = sub, ""
        if re.search(r"[A-Za-z]{3}", words):
            words = t.translate(words)
        lines[i] = title + "|" + words + (" · " + who if who else "")
        n += 1
    open(p, "w", encoding="utf-8").write("\n".join(lines))
    print(f"{b}: {n} subtitles translated")
