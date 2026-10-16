"""Item pictures for the DS status screen (source/menu.c).

COMMON/DATA/ITPS.ITP: 100 TIMs, one per 12288 bytes, 8 bpp, 112 x 72 pixels,
each with its own 256-colour palette. Picture n shows item id n (the PS1
shows it when an item is picked up and on CHECK).

The lower screen is one 8 bpp bitmap in the palette of the item icons
(menu/items.bin, tools/menu2ds.py), so the pictures are converted to that
palette, with Floyd-Steinberg dithering.

Output menu/pictures.bin:
  u32 "RIP1", u16 count, u16 width, u16 height, u16 pad,
  u8 pixels[count][height][width] (palette indices of items.bin)

Usage: python3 -I itps2ds.py ITPS.ITP items.bin out.bin
"""
import struct
import sys

MAGIC = 0x31504952  # "RIP1"
SLOT = 12288
W, H = 112, 72


def rgb(c):
    return ((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3)


def main():
    itp = open(sys.argv[1], "rb").read()
    items = open(sys.argv[2], "rb").read()
    target = [rgb(c) for c in struct.unpack_from("<256H", items, 20)]
    cache = {}

    def nearest(r, g, b):
        key = (r >> 2, g >> 2, b >> 2)
        if key not in cache:
            cache[key] = min(range(1, 256), key=lambda i: (target[i][0] - r) ** 2 * 3 + (target[i][1] - g) ** 2 * 4 +
                             (target[i][2] - b) ** 2 * 2)
        return cache[key]

    count = len(itp) // SLOT
    out = bytearray(struct.pack("<IHHHH", MAGIC, count, W, H, 0))
    for k in range(count):
        pos = k * SLOT
        magic, flags = struct.unpack_from("<II", itp, pos)
        if magic != 0x10 or not flags & 8:
            out += bytes(W * H)
            continue
        clut_len = struct.unpack_from("<I", itp, pos + 8)[0]
        pal = [rgb(c) for c in struct.unpack_from("<256H", itp, pos + 20)]
        p = pos + 8 + clut_len
        _, _, _, iw, ih = struct.unpack_from("<IHHHH", itp, p)
        w = iw * 2
        pix = itp[p + 12:p + 12 + w * ih]
        err = [[0.0, 0.0, 0.0] for _ in range((W + 2) * (H + 1))]
        for y in range(H):
            for x in range(W):
                src = pal[pix[y * w + x]] if x < w and y < ih else (0, 0, 0)
                e = err[y * (W + 2) + x + 1]
                r, g, b = (max(0, min(255, int(src[c] + e[c]))) for c in range(3))
                i = nearest(r, g, b)
                out.append(i)
                d = (r - target[i][0], g - target[i][1], b - target[i][2])
                for dx, dy, f in ((1, 0, 7 / 16), (-1, 1, 3 / 16), (0, 1, 5 / 16), (1, 1, 1 / 16)):
                    t = err[(y + dy) * (W + 2) + x + 1 + dx]
                    for c in range(3):
                        t[c] += d[c] * f
    open(sys.argv[3], "wb").write(out)
    print(f"{sys.argv[3]}: {count} pictures")


if __name__ == "__main__":
    main()
