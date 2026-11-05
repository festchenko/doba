"""Pictures for the device: .dgi files, shared by docs2doba and zim2doba.

    "DGI1", u16 width, u16 height (little endian), then zlib of 4-bit greys
    (0 black .. 15 white), two pixels a byte, high nibble first, rows padded
    to whole bytes. Only the four greys the e-ink panel shows are used.

The device finds <book>/img/<name>.dgi, or <book>/img/<n0n1>/<name>.dgi when
the name is 8 characters or longer (big books: a FAT folder stays small).
"""
import hashlib
import os
import struct
import zlib

IMG_W, IMG_MAX_H = 484, 640      # the article column, the tallest picture on a page


def encode(img, max_w=IMG_W, max_h=IMG_MAX_H):
    """PIL image -> (dgi bytes, w, h): scaled to fit, 4 greys, dithered."""
    from PIL import Image, ImageOps
    if img.mode in ("RGBA", "LA", "P"):
        img = img.convert("RGBA")
        bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
        img = Image.alpha_composite(bg, img)
    img = img.convert("L")
    w, h = img.size
    k = min(1.0, max_w / w, max_h / h)
    if k < 1.0:
        img = img.resize((max(1, int(w * k)), max(1, int(h * k))), Image.LANCZOS)
    img = ImageOps.autocontrast(img, cutoff=0.5)
    pal = Image.new("P", (1, 1))
    pal.putpalette([0, 0, 0, 85, 85, 85, 170, 170, 170, 255, 255, 255] + [255] * (252 * 3))
    q = img.convert("RGB").quantize(palette=pal, dither=Image.Dither.FLOYDSTEINBERG)
    w, h = q.size
    data = q.tobytes()
    grey = bytes([0, 5, 10, 15]) + bytes(252)
    rows = []
    for r in range(h):
        row = data[r * w:(r + 1) * w].translate(grey)
        if w & 1:
            row += b"\x0f"
        rows.append(bytes((a << 4) | b for a, b in zip(row[0::2], row[1::2])))
    return b"DGI1" + struct.pack("<HH", w, h) + zlib.compress(b"".join(rows), 9), w, h


def hashed_name(key):
    """A stable 10-character name for a picture known by a path or URL."""
    return hashlib.sha1(key.encode("utf-8")).hexdigest()[:10]


def path_for(book_dir, name):
    if len(name) >= 8:
        return os.path.join(book_dir, "img", name[:2], name + ".dgi")
    return os.path.join(book_dir, "img", name + ".dgi")


def save(book_dir, name, blob):
    p = path_for(book_dir, name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    tmp = p + ".%d.part" % os.getpid()
    with open(tmp, "wb") as f:
        f.write(blob)
    os.replace(tmp, p)
