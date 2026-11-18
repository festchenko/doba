"""menu_drop_empty.py LIB BOOK...: menu rows whose page has nothing to read
once the reader drops its junk lines (a TCCC 'Завантажити відео' page: the
video is not on the card) are taken off the menu. The page stays in the book.
QA 18 Nov."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from audit import Book, LINK

JUNK = {"прослухати вимову", "Від ??", "Корінь: --.", "listen", "Завантажити відео",
        "Підтримайте розвиток проєкту TCCC в Україні",
        "◆ Немає прикладів слововжитку (див. рекомендації)."}

lib = sys.argv[1]
for name in sys.argv[2:]:
    d = os.path.join(lib, name)
    b = Book(d)
    p = os.path.join(d, "menu.txt")
    lines = open(p, encoding="utf-8").read().split("\n")
    out, dropped = [], []
    for line in lines:
        t = line.split("|", 1)[0].strip()
        if not t or line.startswith("#"):
            out.append(line)
            continue
        r = b.find(t)
        if r is None:
            out.append(line)
            continue
        body = [l for l in LINK.sub(lambda m: m.group(1), b.text(r)).split("\n")[1:]
                if l[:1] in "PBCI" and l[1:].strip() not in JUNK and l[1:].strip()]
        if body:
            out.append(line)
        else:
            dropped.append(t)
    if dropped:
        os.replace(p, p + ".bak")
        open(p, "w", encoding="utf-8").write("\n".join(out))
    print(f"{name}: {len(dropped)} empty rows dropped: " + "; ".join(dropped))
