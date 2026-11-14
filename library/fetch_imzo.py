#!/usr/bin/env python3
"""MON textbooks from lib.imzo.gov.ua through the Web Archive (the site
answers only from Ukraine).   fetch_imzo.py OUT_DIR SUBJECT_PATH [...]
SUBJECT_PATH like /yelektronn-vers-pdruchnikv/9-klas/10-fzika-9-klas/"""
import html
import os
import re
import sys
import time
import urllib.request

UA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_0) AppleWebKit/605.1.15 Safari/605.1.15"
SKIP = re.compile(r"поглиблен|профільн|мовою|навчанням|особлив|інклюз", re.I)


def wa(path, timeout=300, raw=True, year="2025"):
    time.sleep(3)
    url = f"https://web.archive.org/web/{year}{'id_' if raw else ''}/https://lib.imzo.gov.ua{path}"
    for attempt in range(3):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.read()
        except Exception as e:
            err = e
            time.sleep(30 * (attempt + 1))
    raise err


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for subj in sys.argv[2:]:
        try:
            page = wa(subj).decode("utf-8", "replace")
        except Exception as e:
            print(f"!! {subj}: {e}", flush=True)
            continue
        prods = []
        for m in re.finditer(r'href="(/[^"#]+/)" title="([^"]+)"', page):
            href, title = m.group(1), html.unescape(m.group(2)).replace("﻿", "").strip()
            if not href.startswith(subj) or len(href) <= len(subj):
                continue
            if (href, title) not in prods:
                prods.append((href, title))
        picks = [p for p in prods if not SKIP.search(p[1])][:1] or prods[:1]
        if not picks:
            print(f"?? {subj}: no books listed", flush=True)
        for href, title in picks:
            name = re.sub(r"[^a-z0-9]+", "-", subj.strip("/").split("/")[-1])[:50] + ".pdf"
            dst = os.path.join(out, name)
            if os.path.exists(dst) and os.path.getsize(dst) > 100000:
                print(f"== {name} (have)", flush=True)
                continue
            try:
                pp = wa(href).decode("utf-8", "replace")
                pdf = re.search(r'["\'](?:https?://lib\.imzo\.gov\.ua)?(/wa-data/[^"\']+\.pdf)["\']', pp)
                if not pdf:
                    print(f"?? {title[:70]}: no pdf link", flush=True)
                    continue
                data = b""
                for year in ("2025", "2024", "2023", "2022", "2026", "2021"):
                    try:
                        data = wa(pdf.group(1), timeout=900, year=year)
                    except Exception:
                        continue
                    if data.startswith(b"%PDF") and b"%%EOF" in data[-2048:] and len(data) % 1048576:
                        break
                    print(f"   {year}: incomplete copy ({len(data)} B)", flush=True)
                    data = b""
                if not data.startswith(b"%PDF"):
                    print(f"?? {title[:70]}: not archived", flush=True)
                    continue
                open(dst, "wb").write(data)
                open(dst[:-4] + ".title", "w").write(title)
                print(f"ok {name} {len(data) // 1000000} MB  {title[:80]}", flush=True)
            except Exception as e:
                print(f"!! {title[:70]}: {e}", flush=True)


if __name__ == "__main__":
    main()
