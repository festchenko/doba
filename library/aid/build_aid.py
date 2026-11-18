"""build_aid.py CARDS.txt OUT_BOOK NAME LANG SECTION: Doba's first-aid cards
(cards_en.txt, cards_uk.txt) as a small book: one short article a card, the
menu in the order written. "= Title | line under it", "# heading",
"- step", other lines text. info.txt says draft=1 until a medic has checked
the text: the reader then says so under every title."""
import os, struct, sys, zlib
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from dobafold import display, key

src, out, name, lang, section = sys.argv[1:6]
cards, cur = [], None
for raw in open(src, encoding="utf-8"):
    line = raw.rstrip("\n")
    if line.startswith("# ") and cur is None:
        continue                                    # the file's own comment lines
    if line.startswith("= "):
        title, _, sub = line[2:].partition("|")
        cur = {"title": title.strip(), "sub": sub.strip(), "lines": ["T" + title.strip()]}
        cards.append(cur)
    elif cur is None or not line.strip():
        continue
    elif line.startswith("# "):
        cur["lines"].append("H" + line[2:].strip())
    elif line.startswith("- "):
        cur["lines"].append("B" + line[2:].strip())
    else:
        cur["lines"].append("P" + line.strip())

os.makedirs(out, exist_ok=True)
data, recs = b"", []
for c in cards:
    z = zlib.compress("\n".join(c["lines"]).encode("utf-8"), 9)
    recs.append((key(c["title"]), display(c["title"]), 0, 0, len(data), len(z), 0))
    data += z
recs.sort()
open(os.path.join(out, "a0000.dta"), "wb").write(data)
with open(os.path.join(out, "index.dti"), "wb") as f:
    f.write(struct.pack("<8sIII12s", b"DOBAIX1\0", len(recs), 128, 1, b"\0" * 12))
    for r in recs:
        f.write(struct.pack("<56s56sHHIII", *r))
about = "Short steps for the first minutes" if lang == "en" else "Короткі кроки на перші хвилини"
open(os.path.join(out, "info.txt"), "w", encoding="utf-8").write(
    f"name={name}\nlang={lang}\narticles={len(cards)}\nredirects=0\nsource=Doba\nabout={about}\nunit=cards\ndraft=1\n")
open(os.path.join(out, "menu.txt"), "w", encoding="utf-8").write(
    f"# {section}\n" + "\n".join(f'{c["title"]}|{c["sub"]}' for c in cards) + "\n")
print(f"{out}: {len(cards)} cards, {len(data)} bytes")
