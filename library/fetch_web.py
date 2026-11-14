#!/usr/bin/env python3
"""Save web pages for docs2doba: each page's main content with its pictures,
as local NN-slug.html files in one folder.

    python3 fetch_web.py OUT_DIR URL [URL ...]
    python3 fetch_web.py OUT_DIR @urls.txt
"""
import html
import os
import re
import sys
import time
import urllib.parse
import urllib.request

UA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_0) AppleWebKit/605.1.15 Safari/605.1.15"


def get(url, timeout=90):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def main_block(raw):
    for pat in (r"<main\b.*?</main>", r"<article\b.*?</article>",
                r'<div[^>]+class="[^"]*(?:guide|content|post|entry)[^"]*".*?</div>\s*</div>'):
        m = re.search(pat, raw, re.S | re.I)
        if m and len(m.group(0)) > 500:
            return m.group(0)
    m = re.search(r"<body\b.*?</body>", raw, re.S | re.I)
    return m.group(0) if m else raw


def main():
    out, urls = sys.argv[1], sys.argv[2:]
    if len(urls) == 1 and urls[0].startswith("@"):
        urls = [u.strip() for u in open(urls[0][1:]) if u.strip() and not u.startswith("#")]
    os.makedirs(out, exist_ok=True)
    img_dir = os.path.join(out, "img")
    os.makedirs(img_dir, exist_ok=True)
    for i, url in enumerate(urls, 1):
        slug = re.sub(r"[^a-z0-9]+", "-", urllib.parse.urlparse(url).path.lower()).strip("-")[-60:] or "page"
        dst = os.path.join(out, f"{i:03d}-{slug}.html")
        if os.path.exists(dst):
            continue
        try:
            raw = get(url).decode("utf-8", "replace")
        except Exception as e:
            print(f"!! {url}: {e}", flush=True)
            continue
        t = re.search(r"<h1[^>]*>(.*?)</h1>", raw, re.S | re.I) or re.search(r"<title[^>]*>(.*?)</title>", raw, re.S | re.I)
        title = html.unescape(re.sub(r"<[^>]+>", " ", t.group(1))).strip() if t else slug
        title = re.sub(r"\s+", " ", title)
        body = main_block(raw)
        body = re.sub(r"<(script|style|nav|footer|form|noscript)\b.*?</\1>", " ", body, flags=re.S | re.I)

        def fetch_img(m):
            src = html.unescape(m.group(2))
            if src.startswith("data:"):
                return ""
            full = urllib.parse.urljoin(url, src)
            name = re.sub(r"[^A-Za-z0-9._-]+", "_", urllib.parse.urlparse(full).path)[-80:]
            local = os.path.join(img_dir, name)
            if not os.path.exists(local):
                try:
                    data = get(full, 60)
                    open(local, "wb").write(data)
                except Exception:
                    return ""
            return f'{m.group(1)}"img/{name}"'

        body = re.sub(r'(<img\b[^>]*?\bsrc=)"([^"]+)"', fetch_img, body, flags=re.I)
        with open(dst, "w", encoding="utf-8") as f:
            f.write(f"<html><head><title>{html.escape(title)}</title></head><body><main>{body}</main></body></html>")
        print(f"{i}/{len(urls)} {title[:70]}", flush=True)
        time.sleep(0.5)


if __name__ == "__main__":
    main()
