"""The title screen for the DS (source/main.c, start_game).

COMMON/DATA/TIT_BG.TIM: the background (16 bpp, 320 x 240, the G eye).
COMMON/DATA/TMOJI.TIM: 8 bpp, 384 x 256, two palettes (0 white, 1 red): the
menu lines on the left (PRESS ANY BUTTON at y 3, NEW GAME 19, LOAD GAME 35,
the copyright 69) and the logo on the right, cut into three strips (x 256..,
"RESIDE" at y 2, "NT EVIL" + TM at y 83, "2" at y 162), 287 pixels wide in all.

The DS screen (256 x 192): the background scaled down, the logo scaled to the
screen width at the top, the menu lines 1:1 (white, the selected one red) and
the copyright at the bottom. Written in the background format (formats.h,
BgHeader + LZ77, tools/bg2ds.py): title/press.bin, title/new.bin (NEW GAME
selected), title/load.bin (LOAD GAME selected), title/normal.bin and
title/easy.bin (the difficulty after NEW GAME: NORMAL   EASY at y 146 of
TMOJI, the chosen word red).

Usage: python3 -I title2ds.py TIT_BG.TIM TMOJI.TIM out_dir
"""
import os
import struct
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
from bg2ds import BG_MAGIC, lz77, quantise  # noqa: E402

W, H = 256, 192
# TMOJI: (y0, y1, x0, x1) of each piece
LINES = {"press": (3, 13, 44, 211), "new": (19, 29, 79, 175), "load": (35, 45, 76, 175),
         "copyright": (69, 77, 16, 240),
         "normal": (146, 156, 48, 120), "easy": (146, 156, 144, 190)}  # one line: NORMAL   EASY
LOGO = [(2, 78), (83, 158), (162, 238)]   # strips at x 256.., placed side by side
LOGO_W = (128, 128, 31)
LOGO_TOP = 18
MENU_Y = {"press": 128, "new": 120, "load": 136, "normal": 128, "easy": 128}
COPYRIGHT_Y = 178


def rgb555(c):
    return ((c & 31) * 255 // 31, (c >> 5 & 31) * 255 // 31, (c >> 10 & 31) * 255 // 31)


def read_16bpp(path):
    d = open(path, "rb").read()
    _, _, _, w, h = struct.unpack_from("<IHHHH", d, 8)
    return w, h, struct.unpack_from(f"<{w * h}H", d, 20)


def read_tmoji(path):
    d = open(path, "rb").read()
    blen, _, _, cw, ch = struct.unpack_from("<IHHHH", d, 8)
    clut = struct.unpack_from(f"<{cw * ch}H", d, 20)
    p = 8 + blen
    _, _, _, iw, ih = struct.unpack_from("<IHHHH", d, p)
    w = iw * 2
    return w, ih, clut, d[p + 12:p + 12 + w * ih]


def compose(bg, tmoji, selected):
    bw, bh, bpix = bg
    tw, th, clut, tpix = tmoji
    img = [[rgb555(bpix[(y * bh // H) * bw + x * bw // W]) for x in range(W)] for y in range(H)]

    def put(x, y, src_x, src_y, palette):
        v = tpix[src_y * tw + src_x]
        if v and 0 <= x < W and 0 <= y < H:
            img[y][x] = rgb555(clut[palette * 256 + v])

    # the logo: the strips side by side (287 x 77), scaled to the screen width
    logo_w = sum(LOGO_W)
    logo_h = max(y1 - y0 + 1 for y0, y1 in LOGO)
    scale_h = logo_h * W // logo_w
    for y in range(scale_h):
        for x in range(W):
            lx, ly = x * logo_w // W, y * logo_h // scale_h
            for k, (y0, y1) in enumerate(LOGO):
                if lx < LOGO_W[k]:
                    if y0 + ly <= y1:
                        put(x, LOGO_TOP + y, 256 + lx, y0 + ly, 1)
                    break
                lx -= LOGO_W[k]
    lines = {"press": ["press"], "normal": ["normal", "easy"], "easy": ["normal", "easy"]}.get(
        selected, ["new", "load"])
    for name in lines + ["copyright"]:
        y0, y1, x0, x1 = LINES[name]
        top = COPYRIGHT_Y if name == "copyright" else MENU_Y[name]
        left = (W - (x1 - x0 + 1)) // 2
        if name in ("normal", "easy"): # both words where they are on the line, centred together
            left = (W - (LINES["easy"][3] - LINES["normal"][2] + 1)) // 2 + x0 - LINES["normal"][2]
        palette = 1 if name == selected else 0
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                put(left + x - x0, top + y - y0, x, y, palette)
    return b"".join(bytes(c) for row in img for c in row)


def main():
    bg_path, tmoji_path, out_dir = sys.argv[1:4]
    os.makedirs(out_dir, exist_ok=True)
    bg, tmoji = read_16bpp(bg_path), read_tmoji(tmoji_path)
    with tempfile.TemporaryDirectory() as tmp:
        for name in ("press", "new", "load", "normal", "easy"):
            ppm = os.path.join(tmp, name + ".ppm")
            with open(ppm, "wb") as f:
                f.write(b"P6\n%d %d\n255\n" % (W, H) + compose(bg, tmoji, name))
            palette, indices = quantise(ppm)
            with open(os.path.join(out_dir, name + ".bin"), "wb") as f:
                f.write(struct.pack("<I256H", BG_MAGIC, *palette) + lz77(indices))
    print(f"{out_dir}: press.bin, new.bin, load.bin, normal.bin, easy.bin")


if __name__ == "__main__":
    main()
