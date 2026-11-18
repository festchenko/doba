"""menu_at.py LIB BOOK...: a first-aid menu row opens on what to do. For each
row whose article has a heading such as "First aid", "Management" or
"Treatment" ("Перша допомога", "Лікування"...), the row gets "|#Heading"
(the reader opens the article there). QA 18 Nov: Bleeding's first aid was on
page 15 of 21. Rows that already name a heading are left as they are."""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from audit import Book, LINK

WANT = ["first aid", "management", "treatment", "what to do", "emergency treatment",
        "перша допомога", "перша медична допомога", "надання першої допомоги", "лікування", "що робити"]

lib = sys.argv[1]
for name in sys.argv[2:]:
    d = os.path.join(lib, name)
    b = Book(d)
    p = os.path.join(d, "menu.txt")
    lines = open(p, encoding="utf-8").read().split("\n")
    n = 0
    for i, line in enumerate(lines):
        if line.startswith("#") or not line.strip() or "|#" in line:
            continue
        title = line.split("|", 1)[0].strip()
        r = b.find(title)
        if r is None:
            continue
        heads = [l[1:].strip() for l in LINK.sub(lambda m: m.group(1), b.text(r)).split("\n") if l[:1] in "HS"]
        low = [h.lower() for h in heads]
        pick = next((heads[low.index(w)] for w in WANT if w in low), None)
        if pick is None:
            pick = next((h for h, l in zip(heads, low) if any(l.startswith(w) for w in WANT)), None)
        if pick:
            if "|" not in line:
                line += "|"
            lines[i] = line + "|#" + pick
            n += 1
            print(f"  {title} -> #{pick}")
    open(p, "w", encoding="utf-8").write("\n".join(lines))
    print(f"{name}: {n} rows open on what to do")
