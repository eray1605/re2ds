"""Decode PlayStation TIM images (4/8/16 bpp) found at given offsets of a file.

Usage: python3 -I tim2png.py FILE out_prefix [offset ...]
       (without offsets: every TIM header found in the file)
Writes out_prefix_<offset>.png; for 4/8 bpp the first palette is used.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imgio import write_png  # noqa: E402


def bgr555_to_rgb(c):
    return ((c & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31, ((c >> 10) & 31) * 255 // 31)


def parse_tim(d, off):
    magic, flags = struct.unpack_from("<II", d, off)
    if magic != 0x10 or flags & ~0xB:
        return None
    bpp = {0: 4, 1: 8, 2: 16}.get(flags & 3)
    if bpp is None:
        return None
    pos = off + 8
    palettes = []
    clut_info = None
    if flags & 8:
        clen, cx, cy, cw, ch = struct.unpack_from("<I4H", d, pos)
        palettes = [struct.unpack_from(f"<{cw}H", d, pos + 12 + 2 * cw * r) for r in range(ch)]
        clut_info = (cx, cy, cw, ch)
        pos += clen
    blen, ix, iy, iw, ih = struct.unpack_from("<I4H", d, pos)
    width = iw * 16 // bpp
    # Size from the dimensions: RE2's mask TIMs store the pixel byte count in
    # blen, without the 12-byte block header the TIM format normally includes.
    nbytes = iw * 2 * ih
    pix = d[pos + 12:pos + 12 + nbytes]
    return {"bpp": bpp, "width": width, "height": ih, "vram": (ix, iy), "clut": clut_info,
            "palettes": palettes, "pixels": pix, "end": pos + 12 + nbytes}


def tim_indices(tim):
    w, h, pix = tim["width"], tim["height"], tim["pixels"]
    if tim["bpp"] == 4:
        return [(pix[(y * w + x) >> 1] >> (4 * (x & 1))) & 15 for y in range(h) for x in range(w)]
    if tim["bpp"] == 8:
        return list(pix[:w * h])
    return list(struct.unpack_from(f"<{w * h}H", pix))


def tim_to_rgb(tim, palette=0):
    rgb = bytearray()
    for v in tim_indices(tim):
        c = v if tim["bpp"] == 16 else tim["palettes"][palette][v]
        rgb += bytes(bgr555_to_rgb(c))
    return rgb


def main():
    d = open(sys.argv[1], "rb").read()
    offsets = [int(o, 0) for o in sys.argv[3:]] or [
        i for i in range(0, len(d) - 8, 4) if struct.unpack_from("<II", d, i)[0] == 0x10]
    for off in offsets:
        tim = parse_tim(d, off)
        if not tim:
            continue
        path = f"{sys.argv[2]}_{off:06x}.png"
        write_png(path, tim["width"], tim["height"], tim_to_rgb(tim))
        print(f"{path}: {tim['width']}x{tim['height']} {tim['bpp']}bpp, vram {tim['vram']}, "
              f"clut {tim['clut']}, ends {tim['end']:#x}")


if __name__ == "__main__":
    main()
