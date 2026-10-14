"""Extract the foreground-mask images that RE2 (PS1) stores in the .BSS slots.

Layout of one 64 KB camera slot (found by tracing the background loader at
0x8002b240 in SLES_009.72):

    0x0000  MDEC "BS" frame (background, 320x240)
    h       u32 unpacked size, u16 0xFFFF, compressed stream  (mask TIM)
    ...
    o - 8   u32 h            (offset of the mask block; read by the loader)
    o - 4   u32 nonzero      (mask present)

The stream is a small LZ variant (decoder at 0x80010000..0x800104b0):

    c = next byte
    c == 0xFF            -> end
    c & 0x10 == 0        -> copy:    b = next byte
                                     offset = ((c >> 5) << 8 | b) - 2048
                                     length = (c & 0x0F) + 3, if (c & 0x0F) == 0x0F: += next byte
    else                 -> literal: count = 32 - (c & 0x1F),
                                     if count == 16: += next byte

The unpacked data is an 8 bpp TIM (256 x N) whose rectangles are placed by the
RDT's per-camera PRI lists (see OpenBiohazard2 fileio/rdt_pri.go).

Usage: python3 -I bssmask.py ROOMxxx.BSS out_prefix
       -> out_prefix_camNN_mask.tim (raw TIM) and _mask.png (palette 0)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imgio import write_png  # noqa: E402
import tim2png  # noqa: E402

SLOT = 0x10000


def unpack(data, pos):
    out = bytearray()
    while True:
        c = data[pos]
        pos += 1
        if c == 0xFF:
            return out, pos
        if c & 0x10 == 0:
            b = data[pos]
            pos += 1
            offset = ((c >> 5) << 8 | b) - 2048
            length = (c & 0x0F) + 3
            if c & 0x0F == 0x0F:
                length += data[pos]
                pos += 1
            src = len(out) + offset
            if src < 0:
                raise ValueError(f"copy before start of output (offset {offset})")
            for k in range(length):  # byte-wise: overlapping copies repeat data
                out.append(out[src + k])
        else:
            count = 32 - (c & 0x1F)
            if count == 16:
                count += data[pos]
                pos += 1
            out += data[pos:pos + count]
            pos += count


def find_mask_block(slot):
    """Return the mask block offset h, using the trailer pointer (o - 8)."""
    for h in range(4, SLOT - 8, 4):
        size = struct.unpack_from("<I", slot, h)[0]
        if not (0x200 < size < SLOT and slot[h + 4:h + 6] == b"\xff\xff"):
            continue
        for j in range(h + 8, SLOT - 4, 4):
            if struct.unpack_from("<I", slot, j)[0] == h and struct.unpack_from("<I", slot, j + 4)[0]:
                return h
    return None


def main():
    data = open(sys.argv[1], "rb").read()
    prefix = sys.argv[2]
    for cam in range(len(data) // SLOT):
        slot = data[cam * SLOT:(cam + 1) * SLOT]
        h = find_mask_block(slot)
        if h is None:
            print(f"cam {cam:02d}: no mask")
            continue
        size = struct.unpack_from("<I", slot, h)[0]
        tim, end = unpack(slot, h + 6)
        if len(tim) != size:
            raise SystemExit(f"cam {cam:02d}: unpacked {len(tim)} bytes, header says {size}")
        with open(f"{prefix}_cam{cam:02d}_mask.tim", "wb") as f:
            f.write(tim)
        info = tim2png.parse_tim(bytes(tim), 0)
        write_png(f"{prefix}_cam{cam:02d}_mask.png", info["width"], info["height"], tim2png.tim_to_rgb(info))
        print(f"cam {cam:02d}: mask block @{h:#06x}, {end - h} bytes packed -> {size} bytes, "
              f"TIM {info['width']}x{info['height']} {info['bpp']}bpp, {len(info['palettes'])} palette(s)")


if __name__ == "__main__":
    main()
