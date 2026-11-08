"""The ending's pictures and results texts for the DS (source/ending.c).

COMMON/BIN/ENDING.BIN (loaded at 0x8011a000) draws its texts from
COMMON/DATA/END00.TIM (8 bpp, 1024 x 256, 8 palettes, in VRAM from x 512 / y
480 on): a glyph table at 0x8011c914 (10 bytes per glyph: tpage, clut, u, v,
w, h, width and height on the screen), strings of glyph numbers at
0x8011ccc4 (0xFF ends one), and a table of 31 texts at 0x8011cb50 (12 bytes:
flags, ?, ?, string, x, y, colour; PS1 screen 320 x 240). The clear time and
the number of saves are 10 glyphs at the positions 0x8011a070 / 0x8011a084.
COMMON/DATA/END10..END14.TIM are raw 320 x 240 16 bpp pictures (no header).

Output (out_dir):
  pic0.bin .. pic4.bin  END10..END14 in the background format (formats.h,
                        BgHeader + LZ77), only palette indices 1..223
  text.bin              u32 "REND", u16 palette[32] (indices 224..255),
                        u16 sprite count, per sprite i16 x, i16 y, u16 w, u16 h
                        and w * h bytes (0 transparent, else 224 + n); texts
                        0..30 (the table's, at their places; the ones the DS
                        does not show are empty), then the glyphs 0..9 and ':'
                        (11, at 0, 0); then u16 x[10], y[10] of the numbers.
  All in DS pixels: the PS1 screen scaled by 4 / 5.

Usage: python3 -I ending2ds.py ENDING.BIN DATA_DIR out_dir
"""
import os
import struct
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
from bg2ds import BG_MAGIC, lz77  # noqa: E402

BASE = 0x8011A000
GLYPHS, STRINGS, TEXTS, NUM_X, NUM_Y = 0x8011C914, 0x8011CCC4, 0x8011CB50, 0x8011A070, 0x8011A084
TEXT_MAGIC = 0x444E4552  # "REND"
W, H = 256, 192
PIC_COLOURS = 223
TEXT_FIRST, TEXT_COLOURS = 224, 32
# the texts the DS shows: rank letters 4-8, scenario names 9-12, the results' labels 13-17
SHOWN = set(range(4, 18))
DIGITS = [23 + d for d in range(10)] + [36]


def rgb555(c):
    return ((c & 31) * 255 // 31, (c >> 5 & 31) * 255 // 31, (c >> 10 & 31) * 255 // 31)


def read_end00(path):
    d = open(path, "rb").read()
    clen, cx, cy, cw, ch = struct.unpack_from("<IHHHH", d, 8)
    clut = struct.unpack_from(f"<{cw * ch}H", d, 20)
    p = 8 + clen
    _, _, _, iw, ih = struct.unpack_from("<IHHHH", d, p)
    return clut, cw, iw * 2, ih, d[p + 12:p + 12 + iw * 2 * ih]


def render(ending, end00, string, scale=True):
    """A string of glyphs as rows of (r, g, b) or None, at PS1 size."""
    clut, cw, tw, th, pix = end00
    glyphs = [struct.unpack_from("<HHBBBBBB", ending, GLYPHS - BASE + g * 10) for g in string]
    width = sum(g[6] for g in glyphs)
    height = max((g[7] for g in glyphs), default=0)
    img = [[None] * width for _ in range(height)]
    x0 = 0
    for tpage, cl, u, v, w, h, sw, sh in glyphs:
        row = (cl >> 6) - 480
        for j in range(sh):
            for i in range(sw):
                sx = ((tpage & 15) - 8) * 128 + u + i * w // sw
                sy = v + j * h // sh + (256 if tpage & 16 else 0)
                if 0 <= sx < tw and 0 <= sy < th:
                    c = clut[row * cw + pix[sy * tw + sx]]
                    if c:
                        img[j][x0 + i] = rgb555(c)
        x0 += sw
    return img


def shrink(img):
    """4 / 5 of the size, nearest pixel."""
    h, w = len(img), len(img[0]) if img else 0
    nw, nh = w * 4 // 5, h * 4 // 5
    return [[img[j * 5 // 4][i * 5 // 4] for i in range(nw)] for j in range(nh)]


def palette_of(images):
    """Up to TEXT_COLOURS colours for all text pixels (repeated halving of the widest box)."""
    count = {}
    for img in images:
        for row in img:
            for c in row:
                if c is not None:
                    count[c] = count.get(c, 0) + 1
    boxes = [list(count)]
    while len(boxes) < TEXT_COLOURS:
        best = max(boxes, key=lambda b: max(max(c[k] for c in b) - min(c[k] for c in b) for k in range(3))
                   if len(b) > 1 else -1)
        if len(best) < 2:
            break
        k = max(range(3), key=lambda k: max(c[k] for c in best) - min(c[k] for c in best))
        best.sort(key=lambda c: c[k])
        boxes.remove(best)
        boxes += [best[:len(best) // 2], best[len(best) // 2:]]
    pal = []
    for b in boxes:
        n = sum(count[c] for c in b)
        pal.append(tuple(sum(c[k] * count[c] for c in b) // n for k in range(3)))
    return pal


def nearest(pal, c, cache):
    if c not in cache:
        cache[c] = min(range(len(pal)), key=lambda i: sum((pal[i][k] - c[k]) ** 2 for k in range(3)))
    return cache[c]


def quantise_picture(rgb):
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "in.ppm")
        with open(src, "wb") as f:
            f.write(b"P6\n320 240\n255\n" + rgb)
        flt = (f"scale={W}:{H}:flags=lanczos,split[a][b];"
               f"[a]palettegen=max_colors={PIC_COLOURS}:reserve_transparent=0:stats_mode=full[p];"
               "[b][p]paletteuse=dither=sierra2_4a")
        raw = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", src, "-filter_complex", flt,
                              "-f", "rawvideo", "-pix_fmt", "pal8", "-"], check=True, capture_output=True).stdout
    assert len(raw) == W * H + 1024, f"unexpected size {len(raw)}"
    indices = raw[:W * H].translate(bytes(min(i + 1, PIC_COLOURS) for i in range(256)))
    palette = [0] * 256
    for i in range(PIC_COLOURS):
        b, g, r, _ = raw[W * H + 4 * i:W * H + 4 * i + 4]
        palette[i + 1] = (r >> 3) | (g >> 3) << 5 | (b >> 3) << 10
    return palette, indices


def main():
    ending_path, data_dir, out_dir = sys.argv[1:4]
    os.makedirs(out_dir, exist_ok=True)
    ending = open(ending_path, "rb").read()
    end00 = read_end00(os.path.join(data_dir, "END00.TIM"))

    # the strings
    strings, cur, p = [], [], STRINGS - BASE
    while len(strings) < 64 and p < len(ending):
        if ending[p] == 0xFF:
            strings.append(cur)
            cur = []
        else:
            cur.append(ending[p])
        p += 1

    sprites = []
    for k in range(31):
        flags, _, _, s, x, y = struct.unpack_from("<BBBBhh", ending, TEXTS - BASE + 12 * k)
        if k in SHOWN and s < len(strings):
            sprites.append((x * 4 // 5, y * 4 // 5, shrink(render(ending, end00, strings[s]))))
        else:
            sprites.append((0, 0, []))
    for g in DIGITS:
        sprites.append((0, 0, shrink(render(ending, end00, [g]))))
    pal = palette_of([img for _, _, img in sprites])
    cache = {}
    out = bytearray(struct.pack("<I", TEXT_MAGIC))
    out += struct.pack("<32H", *[(r >> 3) | (g >> 3) << 5 | (b >> 3) << 10 for r, g, b in pal]
                       + [0] * (32 - len(pal)))
    out += struct.pack("<H", len(sprites))
    for x, y, img in sprites:
        h, w = len(img), len(img[0]) if img else 0
        out += struct.pack("<hhHH", x, y, w, h)
        for row in img:
            out += bytes(0 if c is None else TEXT_FIRST + nearest(pal, c, cache) for c in row)
    out += struct.pack("<10H", *[v * 4 // 5 for v in struct.unpack_from("<10H", ending, NUM_X - BASE)])
    out += struct.pack("<10H", *[v * 4 // 5 for v in struct.unpack_from("<10H", ending, NUM_Y - BASE)])
    open(os.path.join(out_dir, "text.bin"), "wb").write(out)

    for n in range(5):
        d = open(os.path.join(data_dir, f"END1{n}.TIM"), "rb").read()
        rgb = bytearray()
        for i in range(320 * 240):
            rgb += bytes(rgb555(struct.unpack_from("<H", d, 2 * i)[0]))
        palette, indices = quantise_picture(bytes(rgb))
        with open(os.path.join(out_dir, f"pic{n}.bin"), "wb") as f:
            f.write(struct.pack("<I256H", BG_MAGIC, *palette) + lz77(indices))
    print(f"{out_dir}: text.bin ({len(sprites)} sprites, {len(pal)} colours), pic0..4.bin")


if __name__ == "__main__":
    main()
