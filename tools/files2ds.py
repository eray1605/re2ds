"""Convert the documents ("files", items 0x68-0x80) for the DS (source/main.c).

COMMON/DATA/FILES.TIM holds TIMs, each starting on a 2048-byte sector: per
document first its paper (8 bpp, 256 x 256, one 256-colour CLUT), then its
text pages (4 bpp, 256 x 112 .. 176, one 16-colour CLUT; colour 0x0000 is
transparent). The 25 documents are in item order 0x68.. (25 papers).

Each page is drawn onto its paper (dimmed to 45 % so the text reads well),
scaled to the DS screen (256 x 192, the text centred vertically) and written
in the background format (formats.h, BgHeader + LZ77, tools/bg2ds.py):
files/fileNN_P.bin. files/index.bin: u8 document count, then u8 page count
per document. Page 0 is the title page.

Usage: python3 -I files2ds.py FILES.TIM out_dir
"""
import os
import struct
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
from bg2ds import BG_MAGIC, lz77, quantise  # noqa: E402

W, H = 256, 192
SECTOR = 2048
PAPER_LEVEL = 45  # percent


def rgb555(c):
    return ((c & 31) * 255 // 31, (c >> 5 & 31) * 255 // 31, (c >> 10 & 31) * 255 // 31)


def read_tim(d, pos):
    """(bpp, width, height, clut, pixel indices as a list of rows)."""
    flags = struct.unpack_from("<I", d, pos + 4)[0]
    bpp, p, clut = flags & 3, pos + 8, None
    if flags & 8:
        blen, _, _, cw, ch = struct.unpack_from("<IHHHH", d, p)
        clut = struct.unpack_from(f"<{cw}H", d, p + 12)
        p += blen
    _, _, _, iw, ih = struct.unpack_from("<IHHHH", d, p)
    raw = d[p + 12:p + 12 + iw * 2 * ih]
    if bpp == 0:  # 4 bpp: low nibble first
        w = iw * 4
        rows = [[(raw[y * iw * 2 + x // 2] >> (4 * (x & 1))) & 15 for x in range(w)] for y in range(ih)]
    else:         # 8 bpp
        w = iw * 2
        rows = [list(raw[y * w:(y + 1) * w]) for y in range(ih)]
    return bpp, w, ih, clut, rows


def documents(d):
    docs = []
    for pos in range(0, len(d) - 16, SECTOR):
        tag, flags = struct.unpack_from("<II", d, pos)
        if tag != 0x10 or flags not in (8, 9):
            continue
        tim = read_tim(d, pos)
        if tim[0] == 1:
            docs.append([tim, []])
        elif docs:
            docs[-1][1].append(tim)
    return docs


def dim(c):
    """The paper at PAPER_LEVEL, so the text reads well on it."""
    r, g, b = c & 31, c >> 5 & 31, c >> 10 & 31
    return r * PAPER_LEVEL // 100 | (g * PAPER_LEVEL // 100) << 5 | (b * PAPER_LEVEL // 100) << 10


def compose(paper, page):
    _, pw, ph, pclut, prow = paper
    _, tw, th, tclut, trow = page
    rgb = bytearray(W * H * 3)
    top = (H - th) // 2
    for y in range(H):
        sy = y * ph // H
        for x in range(W):
            c = dim(pclut[prow[sy][x * pw // W]] & 0x7FFF)
            ty = y - top
            if 0 <= ty < th:
                t = tclut[trow[ty][x * tw // W]]
                if t & 0x7FFF:
                    c = t & 0x7FFF
            rgb[(y * W + x) * 3:(y * W + x) * 3 + 3] = bytes(rgb555(c))
    return rgb


def main():
    src, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    docs = documents(open(src, "rb").read())
    total = 0
    with tempfile.TemporaryDirectory() as tmp:
        for k, (paper, pages) in enumerate(docs):
            for p, page in enumerate(pages):
                ppm = os.path.join(tmp, "page.ppm")
                with open(ppm, "wb") as f:
                    f.write(b"P6\n%d %d\n255\n" % (W, H) + compose(paper, page))
                palette, indices = quantise(ppm)
                blob = struct.pack("<I256H", BG_MAGIC, *palette) + lz77(indices)
                with open(os.path.join(out_dir, f"file{k:02d}_{p}.bin"), "wb") as f:
                    f.write(blob)
                total += len(blob)
    with open(os.path.join(out_dir, "index.bin"), "wb") as f:
        f.write(bytes([len(docs)] + [len(pages) for _, pages in docs]))
    print(f"{out_dir}: {len(docs)} documents, {sum(len(p) for _, p in docs)} pages, {total // 1024} KB")


if __name__ == "__main__":
    main()
