#!/usr/bin/env python3
"""Download the US government publications in gov_library.txt (public domain)
into <out>/<section>/<title>.pdf|.html, ready for docs2doba.py.

    python3 fetch_gov.py build/gov
    python3 docs2doba.py build/gov build/lib/gov --name "US Gov field library" \\
        --source "US Government · public domain" --titles-from-filenames

Pages already downloaded are kept; failures are listed at the end (rerun to
retry). --save asks the Internet Archive to capture a page it has no copy of
(for agency sites that turn scripts away) and takes that copy. Run it where the agency sites are reachable (a normal internet line).
"""
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def entries(path=os.path.join(HERE, "gov_library.txt")):
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        sec, agency, title, url = [x.strip() for x in line.split("|", 3)]
        yield sec, agency, title, url


WAYBACK = "https://web.archive.org/web/2026id_/"
UA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_0) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15"


def fetch(url, part, ext):
    if url.startswith(WAYBACK):
        time.sleep(8)    # the Archive shuts its door on more than ~15 requests a minute
    r = subprocess.run(["curl", "-fsSL", "--compressed", "--retry", "2", "-m", "300", "-A", UA, "-o", part, url])
    ok = r.returncode == 0 and os.path.exists(part) and os.path.getsize(part) > 2000
    if ok and ext == ".pdf":
        ok = open(part, "rb").read(5) == b"%PDF-"
    if not ok and os.path.exists(part):
        os.remove(part)
    return ok


def safe(name):
    return re.sub(r'[/\\\\:*?"<>|]+', " ", name).strip()


SAVE = "--save" in sys.argv     # ask the Archive to capture pages it lacks


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    out = args[0] if args else "build/gov"
    failed = []
    for sec, agency, title, url in entries():
        ext = ".pdf" if url.split()[0].lower().split("?")[0].endswith(".pdf") else ".html"
        d = os.path.join(out, sec)
        os.makedirs(d, exist_ok=True)
        dest = os.path.join(d, safe(f"{title} ({agency})") + ext)
        if os.path.exists(dest) and os.path.getsize(dest) > 2000:
            continue
        # The agency first; some (cdc.gov, ready.gov) turn scripts away, so then
        # the Internet Archive's unmodified copy of the same address (id_).
        ok = False
        urls = url.split()            # a line may give mirrors of the same paper
        for src in urls + [WAYBACK + u for u in urls]:
            ok = fetch(src, dest + ".part", ext)
            if ok:
                break
        if not ok and SAVE:
            # Nobody archived it yet: ask the Internet Archive to fetch the
            # agency's page now (it is let in where scripts are not), then
            # take that copy.
            for u in urls:
                time.sleep(8)
                subprocess.run(["curl", "-s", "-o", "/dev/null", "-m", "120", "-A", UA,
                                "https://web.archive.org/save/" + u])
                time.sleep(20)
                ok = fetch(WAYBACK + u, dest + ".part", ext)
                if ok:
                    break
        if ok:
            os.replace(dest + ".part", dest)
            print(f"ok    {sec:9} {title}")
        else:
            failed.append((title, url))
            print(f"FAIL  {sec:9} {title}  <- {url}")
    print(f"\n{len(failed)} failed")
    for t, u in failed:
        print(f"  {t}: {u}")


if __name__ == "__main__":
    main()
