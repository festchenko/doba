"""voyage_menu.py LIB: a first page for Wikivoyage (it opened on a bare A-Z of
34,000 places, QA 18 Nov): Europe by country, phrasebooks, travel topics.
Only titles the book has are written."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dobafold import key
lib = sys.argv[1]
b = os.path.join(lib, "voyage_en")
idx = open(os.path.join(b, "index.dti"), "rb").read()
n = struct.unpack_from("<I", idx, 8)[0]
have = {idx[32 + i * 128:32 + i * 128 + 56].rstrip(b"\0") for i in range(n)}
S = [
 ("CLOSE TO HOME", ["Netherlands", "Gelderland", "Achterhoek", "Germany", "North Rhine-Westphalia", "Belgium", "Ukraine", "Kyiv", "Lviv", "Cherkasy", "Cherkasy Oblast"]),
 ("EUROPE", ["Europe", "Poland", "Moldova", "Romania", "Hungary", "Slovakia", "Czech Republic", "Austria", "Switzerland",
             "France", "Luxembourg", "Italy", "Spain", "Portugal", "United Kingdom", "Ireland", "Denmark", "Sweden",
             "Norway", "Finland", "Estonia", "Latvia", "Lithuania", "Croatia", "Slovenia", "Serbia", "Bulgaria", "Greece"]),
 ("PHRASEBOOKS", ["Ukrainian phrasebook", "Dutch phrasebook", "German phrasebook", "Polish phrasebook",
                  "French phrasebook", "Romanian phrasebook", "Russian phrasebook", "Spanish phrasebook",
                  "Italian phrasebook", "Hungarian phrasebook", "Czech phrasebook", "English phrasebook"]),
 ("TRAVEL TOPICS", ["Stay safe", "Stay healthy", "Wilderness backpacking", "Camping", "Hiking", "Hitchhiking",
                    "Rail travel in Europe", "Driving in Europe", "Border crossing", "Schengen Area", "Tips for travel in winter",
                    "Cold weather", "Hot weather", "Drinking water", "Lost and stolen", "Emergency"]),
]
out = []
for sec, titles in S:
    rows = [t for t in titles if key(t).rstrip(b"\0") in have]
    if rows:
        out.append("# " + sec)
        out += [t + "|" for t in rows]
    missing = [t for t in titles if t not in rows]
    if missing:
        print(sec, "not in the book:", ", ".join(missing))
p = os.path.join(b, "menu.txt")
if not os.path.exists(p):
    open(p, "w", encoding="utf-8").write("\n".join(out) + "\n")
    print("menu.txt:", sum(1 for x in out if not x.startswith("#")), "rows")
else:
    print("menu.txt exists, left alone")
