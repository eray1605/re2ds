"""The ROM's icon (re2ds.nds banner: flashcart menu, DS menu) from the disc.

The title screen's own picture: the G eye of COMMON/DATA/TIT_BG.TIM, cropped
square and scaled to 32 x 32, with the "2" of the logo (COMMON/DATA/TMOJI.TIM,
the third logo strip, palette 1 = red) at the bottom right. 15 colours + one
transparent corner pixel set, as the DS icon format wants (4 bpp BMP).
The file is made from your own disc and is not part of the repository.

Usage: python3 -I icon2ds.py TIT_BG.TIM TMOJI.TIM out.bmp
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import title2ds  # noqa: E402

N = 32


def median_cut(colors, n):
    boxes = [list(colors)]
    while len(boxes) < n:
        boxes.sort(key=lambda b: len(b), reverse=True)
        b = boxes.pop(0)
        if len(b) < 2:
            boxes.append(b)
            break
        ch = max(range(3), key=lambda c: max(p[c] for p in b) - min(p[c] for p in b))
        b.sort(key=lambda p: p[ch])
        boxes += [b[:len(b) // 2], b[len(b) // 2:]]
    return [tuple(sum(p[c] for p in b) // len(b) for c in range(3)) for b in boxes]


def main():
    bw, bh, bpix = title2ds.read_16bpp(sys.argv[1])
    tw, th, clut, tpix = title2ds.read_tmoji(sys.argv[2])
    img = [[(0, 0, 0)] * N for _ in range(N)]
    side = min(bw, bh)
    x0 = (bw - side) // 2
    for y in range(N):                      # box filter down to 32 x 32
        for x in range(N):
            acc = [0, 0, 0]
            n = 0
            for sy in range(y * side // N, (y + 1) * side // N):
                for sx in range(x0 + x * side // N, x0 + (x + 1) * side // N):
                    for k, v in enumerate(title2ds.rgb555(bpix[sy * bw + sx])):
                        acc[k] += v
                    n += 1
            img[y][x] = tuple(min(255, int(a / n * 1.35)) for a in acc)   # a little brighter
    # the "2": strip at x 256.., y 162..238, 31 wide
    gy0, gy1, gw = 162, 238, 31
    gh = gy1 - gy0 + 1
    out_h, out_w = 20, 20 * gw // gh + 1
    ox, oy = N - out_w - 1, N - out_h - 1
    for y in range(out_h):
        for x in range(out_w):
            sy, sx = gy0 + y * gh // out_h, 256 + x * gw // out_w
            v = tpix[sy * tw + sx]
            if v:
                r, g, b = title2ds.rgb555(clut[256 + v])   # palette 1: red
                img[oy + y][ox + x] = (r, g, b)
                for dx, dy in ((1, 0), (0, 1)):               # a dark edge
                    X, Y = ox + x + dx, oy + y + dy
                    if X < N and Y < N and tpix[(gy0 + min(gh - 1, (y + dy) * gh // out_h)) * tw + 256 +
                                                min(gw - 1, (x + dx) * gw // out_w)] == 0:
                        img[Y][X] = (10, 0, 0)
    pal = median_cut({p for row in img for p in row}, 15)
    def near(p):
        return min(range(15), key=lambda i: sum((a - b) ** 2 for a, b in zip(p, pal[i]))) + 1
    idx = [[near(p) for p in row] for row in img]
    rows = bytearray()
    for y in range(N - 1, -1, -1):
        for x in range(0, N, 2):
            rows.append(idx[y][x] << 4 | idx[y][x + 1])
    table = b"".join(bytes((b, g, r, 0)) for r, g, b in [(255, 0, 255)] + pal)
    head = struct.pack("<2sIHHI", b"BM", 14 + 40 + 64 + len(rows), 0, 0, 14 + 40 + 64)
    info = struct.pack("<IiiHHIIiiII", 40, N, N, 1, 4, 0, len(rows), 2835, 2835, 16, 0)
    open(sys.argv[3], "wb").write(head + info + table + bytes(64 - len(table)) + rows)


if __name__ == "__main__":
    main()
