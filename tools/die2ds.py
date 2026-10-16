"""The "YOU DIED" picture for the DS (main.c, death sequence).

COMMON/DATA/DIEDnn.DIE starts with a TIM, 8 bpp, 256 x 192, one palette:
the picture the PS1 shows behind the dead character (00 Leon, 01 Claire).
The rest of the file is more graphics for its effects, not converted.

Output die/diedNN.bin in the background format (formats.h, BG_MAGIC):
u32 magic, u16 palette[256], LZ77 packed 256 x 192 indices. Index 0 is
transparent on the DS bitmap, so its pixels move to an unused index.

Usage: python3 -I die2ds.py DIED00.DIE out.bin
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bg2ds import BG_MAGIC, lz77, lz77_decompress  # noqa: E402

W, H = 256, 192


def main():
    d = open(sys.argv[1], "rb").read()
    magic, flags = struct.unpack_from("<II", d, 0)
    assert magic == 0x10 and flags == 9, "expected an 8 bpp TIM with palette"
    clut_len = struct.unpack_from("<I", d, 8)[0]
    palette = list(struct.unpack_from("<256H", d, 20))
    p = 8 + clut_len
    _, _, _, iw, ih = struct.unpack_from("<IHHHH", d, p)
    assert (iw * 2, ih) == (W, H), (iw * 2, ih)
    pixels = bytearray(d[p + 12:p + 12 + W * H])
    used = set(pixels)
    if 0 in used:
        free = next(i for i in range(1, 256) if i not in used)
        palette[free] = palette[0]
        pixels = pixels.translate(bytes(free if i == 0 else i for i in range(256)))
    palette = [c & 0x7FFF for c in palette]
    packed = lz77(bytes(pixels))
    assert lz77_decompress(packed) == pixels
    with open(sys.argv[2], "wb") as f:
        f.write(struct.pack("<I256H", BG_MAGIC, *palette) + packed)


if __name__ == "__main__":
    main()
