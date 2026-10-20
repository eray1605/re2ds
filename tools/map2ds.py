"""The map for the DS status screen (source/main.c, open_map).

COMMON/DATA/MAPS.PIX holds 20 map pages, 256 x 256 pixels at 4 bpp each (no
header): the page's title strip at the top ("POLICE STATION 1F", compass,
scale) and every room of that area as a separate piece somewhere below. The
PS1 assembles the map from tables in SLES_009.72 (map screen 0x8006a4a4,
drawing 0x800695ec):

  0x800a2a08 + page * 8   u32 address of the page's pieces, u8 piece count,
                          u8 "map found" flag (flag array 33), u16 image index
  0x800a13e0 + image * 8  u32 bytes, u16 + u8 sector offset into MAPS.PIX
  piece (16 bytes)        u8 u, v, w, h (rectangle in the page),
                          s16 x, y (on the PS1's 320 x 240 screen),
                          s16 mx, my (origin of the player marker: it sits at
                          ((x + 28000) / 450 + mx, my - (z + 28000) / 450)),
                          u8 visited flag (array 9), u8 cleared flag (array 32),
                          u8 hide-unvisited masks for Leon / Claire (bit 0 A, 1 B)

Output (menu/maps.bin): u32 "RMP1", u16 pages, u16 pieces; per page u16 first
piece, u8 count, u8 found flag, u32 image offset, u32 image bytes; the pieces
as above (16 bytes); per page the image as 8 bpp (values 0..15), LZ77 for
the DS BIOS.

Usage: python3 -I map2ds.py SLES_009.72 MAPS.PIX out.bin
"""
import os
import struct
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
from bg2ds import lz77  # noqa: E402

PAGES = 20
PAGE_TABLE = 0x800a2a08
IMAGE_TABLE = 0x800a13e0
MAGIC = 0x31504D52  # "RMP1"


def main():
    exe = open(sys.argv[1], "rb").read()
    pix = open(sys.argv[2], "rb").read()

    def rd(addr, n):
        o = addr - 0x80010000 + 0x800
        return exe[o:o + n]

    pages, pieces, images = [], [], []
    for p in range(PAGES):
        ptr, count, found, image = struct.unpack("<IBBH", rd(PAGE_TABLE + 8 * p, 8))
        size, sec_lo, sec_hi, _ = struct.unpack("<IHBB", rd(IMAGE_TABLE + 8 * image, 8))
        off = (sec_hi << 16 | sec_lo) * 2048
        raw = pix[off:off + size]
        assert len(raw) == 32768, f"page {p}: image at {off} is {len(raw)} bytes"
        px = bytearray()
        for b in raw:
            px.append(b & 15)
            px.append(b >> 4)
        pages.append((len(pieces), count, found))
        pieces += [rd(ptr + 16 * i, 16) for i in range(count)]
        images.append(lz77(bytes(px)))

    head = struct.pack("<IHH", MAGIC, PAGES, len(pieces))
    table_size = PAGES * 12
    pos = len(head) + table_size + 16 * len(pieces)
    table = b""
    for (first, count, found), img in zip(pages, images):
        table += struct.pack("<HBBII", first, count, found, pos, len(img))
        pos += len(img)
    with open(sys.argv[3], "wb") as f:
        f.write(head + table + b"".join(pieces) + b"".join(images))
    print(f"{sys.argv[3]}: {PAGES} pages, {len(pieces)} rooms, {pos // 1024} KB")


if __name__ == "__main__":
    main()
