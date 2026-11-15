"""The PS1's message font for the DS's upper screen (source/msgtop.c).

COMMON/DATA/TEX.TIM (4 bpp, 1024 x 256, in VRAM from x 512) holds two fonts in
its first texture page: an 8 x 8 one in ASCII order (rows y 0 / 8 / 16:
0x20-0x3F, 0x40-0x5F, 0x60-0x7F) and the larger one of the message window,
in cells of 14 x 14 from y 28, 18 to a row, in the order of the game's own
code (see LARGE below). Letters, digits and the clear marks come from the
large one, scaled to the DS screen (4 / 5); the rest from the small one.

Output: u32 "RFN1", u8 height, u8 count, then per glyph u8 ascii, u8 width,
width * height bytes: 0 empty, 1..7 its shade (7 the brightest).

Usage: python3 -I font2ds.py TEX.TIM out.bin
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tim2png  # noqa: E402

CELL, PER_ROW, TOP = 14, 18, 28
# the large font's cells (row, column) for the characters it has
LARGE = {'.': (0, 1), '(': (0, 5), ')': (0, 6), ',': (1, 5), '"': (1, 7), "'": (3, 4), '-': (3, 5),
         '[': (3, 1), '/': (3, 2), ']': (3, 3)}
for i, ch in enumerate('012345'):
    LARGE[ch] = (0, 12 + i)
for i, ch in enumerate('6789'):
    LARGE[ch] = (1, i)
for i, ch in enumerate('ABCDEFG'):
    LARGE[ch] = (1, 11 + i)
for i, ch in enumerate('HIJKLMNOPQRSTUVWXY'):
    LARGE[ch] = (2, i)
LARGE['Z'] = (3, 0)
for i, ch in enumerate('abcdefghijk'):
    LARGE[ch] = (3, 7 + i)
for i, ch in enumerate('lmnopqrstuvwxyz'):
    LARGE[ch] = (4, i)
HEIGHT = CELL * 4 // 5          # 11
SPACE = 4


def main():
    d = open(sys.argv[1], 'rb').read()
    t = tim2png.parse_tim(d, 0)
    idx = tim2png.tim_indices(t)
    w = t['width']
    pal = t['palettes'][0]
    # shades: the palette's brightness, 1..7
    def bright(i):
        c = pal[i]
        return (c & 31) + (c >> 5 & 31) + (c >> 10 & 31)
    used = [i for i in range(1, 16) if pal[i]]
    lo, hi = min(bright(i) for i in used), max(bright(i) for i in used)
    shade = [0] * 16
    for i in used:
        shade[i] = 1 + (bright(i) - lo) * 6 // max(1, hi - lo)

    def cell(x0, y0, cw, ch):
        return [[shade[idx[(y0 + j) * w + x0 + i]] for i in range(cw)] for j in range(ch)]

    glyphs = []
    for code in range(0x21, 0x7f):
        ch = chr(code)
        if ch in LARGE:
            r, c = LARGE[ch]
            px = cell(c * CELL, TOP + r * CELL, CELL, CELL)
            cols = [i for i in range(CELL) if any(px[j][i] for j in range(CELL))]
            if not cols:
                continue
            x0, x1 = cols[0], cols[-1] + 1
            sw = max(1, (x1 - x0) * 4 // 5)
            img = [[px[j * 5 // 4][x0 + i * 5 // 4] for i in range(sw)] for j in range(HEIGHT)]
        else: # the small font, sat on the large one's baseline
            row, col = (code - 0x20) // 32, (code - 0x20) % 32
            px = cell(col * 8, row * 8, 8, 8)
            cols = [i for i in range(8) if any(px[j][i] for j in range(8))]
            if not cols:
                continue
            x0, x1 = cols[0], cols[-1] + 1
            img = [[0] * (x1 - x0) for _ in range(HEIGHT - 8)] + [r[x0:x1] for r in px]
        glyphs.append((code, img))
    out = bytearray(struct.pack('<4sBB', b'RFN1', HEIGHT, len(glyphs) + 1))
    out += struct.pack('<BB', 0x20, SPACE) + bytes(SPACE * HEIGHT)
    for code, img in glyphs:
        gw = len(img[0])
        out += struct.pack('<BB', code, gw) + bytes(v for row in img for v in row)
    open(sys.argv[2], 'wb').write(out)
    print(f"{sys.argv[2]}: {len(glyphs) + 1} glyphs, {HEIGHT} high")


if __name__ == '__main__':
    main()
