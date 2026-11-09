"""Doba tile pack (.dtp) — container for 4-bit grayscale map tiles.

Designed to be trivial to read on the ESP32-S3 from a FAT32 microSD:
open one file, binary-search a fixed-size index, read one zlib blob.

Layout (all little-endian):

  Header, 32 bytes
    0   8s  magic      b"DOBATP1\\0"
    8   u8  z_min      lowest zoom stored in this pack
    9   u8  z_max      highest zoom stored in this pack
    10  u8  cell_z     zoom of the cell this pack covers (255 = base pack)
    11  u8  bpp        bits per pixel, always 4
    12  u16 tile_size  pixels per tile side, always 256
    14  u16 flags      bit0 = x, y in the index are relative to the cell's
                       top-left tile at that zoom (detail packs, z >= 17,
                       whose absolute x, y would not fit in u16)
    16  u32 cell_x
    20  u32 cell_y
    24  u32 count      number of index entries
    28  u32 reserved

  Index, count * 16 bytes, sorted by key (z, x, y)
    0   u8  z
    1   u8  flags      bit0 = solid tile (no data, whole tile = `fill`)
    2   u16 x
    4   u16 y
    6   u16 fill       grey level 0..15 for solid tiles
    8   u32 offset     absolute file offset of the zlib blob
    12  u32 length     blob length in bytes (0 for solid tiles)

  Data
    zlib (RFC 1950) streams. Each inflates to tile_size*tile_size/2 bytes:
    rows top to bottom, 2 pixels per byte, high nibble = left pixel,
    0 = black, 15 = white.

Detail packs (z17) sit beside a cell's pack as <cell_y>.17.dtp, flag bit0 set.

Packs are split per z8 cell so every file stays far below the FAT32 4 GB
limit and a region can be added or replaced by copying files.
"""
import bisect
import os
import struct
import zlib

import numpy as np

MAGIC = b"DOBATP1\0"
HEADER = struct.Struct("<8sBBBBHHIIII")
ENTRY = struct.Struct("<BBHHHII")
BASE_CELL_Z = 255
TILE = 256
FLAG_SOLID = 1

assert HEADER.size == 32 and ENTRY.size == 16


def pack_4bpp(levels):
    """uint8 array (TILE, TILE) with values 0..15 -> packed bytes."""
    a = levels.astype(np.uint8)
    return ((a[:, 0::2] << 4) | a[:, 1::2]).tobytes()


def unpack_4bpp(raw):
    b = np.frombuffer(raw, dtype=np.uint8).reshape(TILE, TILE // 2)
    out = np.empty((TILE, TILE), dtype=np.uint8)
    out[:, 0::2] = b >> 4
    out[:, 1::2] = b & 0x0F
    return out


DETAIL_Z = 17
FLAG_RELATIVE = 1


def pack_path(root, cell_z, cell_x, cell_y, detail=False):
    if cell_z == BASE_CELL_Z:
        return os.path.join(root, "base.dtp")
    name = f"{cell_y}.{DETAIL_Z}.dtp" if detail else f"{cell_y}.dtp"
    return os.path.join(root, str(cell_z), str(cell_x), name)


class PackWriter:
    """Collects tiles in memory-light fashion (blobs go to a temp file)."""

    def __init__(self, path, cell_z, cell_x, cell_y, relative=False):
        self.path = path
        self.cell = (cell_z, cell_x, cell_y)
        self.relative = relative
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        self.tmp_path = path + ".part"
        self.tmp = open(self.tmp_path, "wb")
        self.entries = []  # (z, x, y, flags, fill, rel_offset, length)
        self.pos = 0

    def add(self, z, x, y, levels):
        if self.relative:
            k = 2 ** (z - self.cell[0])
            x, y = x - self.cell[1] * k, y - self.cell[2] * k
        lo, hi = int(levels.min()), int(levels.max())
        if lo == hi:
            self.entries.append((z, x, y, FLAG_SOLID, lo, 0, 0))
            return
        blob = zlib.compress(pack_4bpp(levels), 9)
        self.tmp.write(blob)
        self.entries.append((z, x, y, 0, 0, self.pos, len(blob)))
        self.pos += len(blob)

    def close(self):
        self.tmp.close()
        if not self.entries:
            os.remove(self.tmp_path)
            return None
        self.entries.sort(key=lambda e: (e[0], e[1], e[2]))
        zs = [e[0] for e in self.entries]
        data_start = HEADER.size + ENTRY.size * len(self.entries)
        cz, cx, cy = self.cell
        with open(self.path + ".tmp", "wb") as f:
            f.write(HEADER.pack(MAGIC, min(zs), max(zs), cz, 4, TILE,
                                FLAG_RELATIVE if self.relative else 0,
                                cx, cy, len(self.entries), 0))
            for z, x, y, flags, fill, rel, length in self.entries:
                off = data_start + rel if length else 0
                f.write(ENTRY.pack(z, flags, x, y, fill, off, length))
            with open(self.tmp_path, "rb") as src:
                while True:
                    chunk = src.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)
        os.replace(self.path + ".tmp", self.path)
        os.remove(self.tmp_path)
        return self.path


class PackReader:
    """Reference reader. The firmware implements the same algorithm in C."""

    def __init__(self, path):
        self.f = open(path, "rb")
        hdr = HEADER.unpack(self.f.read(HEADER.size))
        if hdr[0] != MAGIC:
            raise ValueError(f"{path}: not a Doba tile pack")
        (_, self.z_min, self.z_max, self.cell_z, self.bpp, self.tile_size,
         self.flags, self.cell_x, self.cell_y, self.count, _) = hdr
        raw = self.f.read(ENTRY.size * self.count)
        self.entries = [ENTRY.unpack_from(raw, i * ENTRY.size)
                        for i in range(self.count)]
        self.keys = [(e[0], e[2], e[3]) for e in self.entries]

    def get(self, z, x, y):
        """(TILE, TILE) uint8 levels 0..15 for absolute (z, x, y), or None."""
        if self.flags & FLAG_RELATIVE:
            k = 2 ** (z - self.cell_z)
            x, y = x - self.cell_x * k, y - self.cell_y * k
        i = bisect.bisect_left(self.keys, (z, x, y))
        if i == self.count or self.keys[i] != (z, x, y):
            return None
        _, flags, _, _, fill, off, length = self.entries[i]
        if flags & FLAG_SOLID:
            return np.full((TILE, TILE), fill, dtype=np.uint8)
        self.f.seek(off)
        return unpack_4bpp(zlib.decompress(self.f.read(length)))

    def close(self):
        self.f.close()


class PackLibrary:
    """Finds the right pack for any tile in a directory of packs."""

    def __init__(self, root, cell_z=8):
        self.root, self.cell_z = root, cell_z
        self.cache = {}

    def _reader(self, cz, cx, cy):
        key = (cz, cx, cy)
        if key not in self.cache:
            p = pack_path(self.root, cz, cx, cy)
            self.cache[key] = PackReader(p) if os.path.exists(p) else None
        return self.cache[key]

    def get(self, z, x, y):
        """Tile levels by absolute (z, x, y), or None; z17 from the detail pack."""
        if z >= DETAIL_Z:
            s = z - self.cell_z
            key = ("d", x >> s, y >> s)
            if key not in self.cache:
                p = pack_path(self.root, self.cell_z, x >> s, y >> s, detail=True)
                self.cache[key] = PackReader(p) if os.path.exists(p) else None
            r = self.cache[key]
            return r.get(z, x, y) if r else None
        if z <= self.cell_z:
            r = self._reader(BASE_CELL_Z, 0, 0)
        else:
            s = z - self.cell_z
            r = self._reader(self.cell_z, x >> s, y >> s)
        return r.get(z, x, y) if r else None
