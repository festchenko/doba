#!/usr/bin/env python3
"""Unpack a "nautilus" ZIM (zimgit-* collections: PDFs and a database.js
listing them) into a folder of files named after their titles, ready for
docs2doba.py --titles-from-filenames.

    python3 nautilus2dir.py zimgit-water_en_2024-08.zim build/water-src
    python3 docs2doba.py build/water-src build/lib/water --name Water --titles-from-filenames

Exit status 2 when the ZIM is not a nautilus collection (no database.js).
"""
import ast
import os
import re
import sys

from libzim.reader import Archive

DOCS = (".pdf", ".epub", ".html", ".htm", ".txt", ".md")


def entries(arc):
    """[(title, author, path)] from database.js, or None."""
    try:
        js = bytes(arc.get_entry_by_path("database.js").get_item().content).decode("utf-8", "replace")
    except KeyError:
        return None
    body = js[js.index("["):js.rindex("]") + 1]
    try:
        rows = ast.literal_eval(body)          # Python-literal-shaped JS
    except Exception:
        import json
        rows = json.loads(re.sub(r",\s*]", "]", body.replace("'", '"')))
    out = []
    for r in rows:
        for fp in r.get("fp") or []:
            out.append((r.get("ti") or os.path.splitext(fp)[0], r.get("aut") or "", "files/" + fp))
    return out


def safe(name):
    name = re.sub(r'[\\/:*?"<>|\x00-\x1f]', " ", name)
    return " ".join(name.split())[:150]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    zim, out = sys.argv[1], sys.argv[2]
    arc = Archive(zim)
    rows = entries(arc)
    if rows is None:
        print(f"{zim}: not a nautilus collection")
        return 2
    os.makedirs(out, exist_ok=True)
    n = 0
    for title, author, path in rows:
        ext = os.path.splitext(path)[1].lower()
        if ext not in DOCS:
            continue
        try:
            data = bytes(arc.get_entry_by_path(path).get_item().content)
        except KeyError:
            print(f"  missing {path}")
            continue
        name = safe(f"{title} ({author})" if author and author.lower() != "various" else title)
        dest = os.path.join(out, name + ext)
        k = 2
        while os.path.exists(dest):
            dest = os.path.join(out, f"{name} ({k}){ext}")
            k += 1
        with open(dest, "wb") as f:
            f.write(data)
        n += 1
    print(f"{n} documents -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
