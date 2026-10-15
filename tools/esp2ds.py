"""Convert the core effect sprites (COMMON/DATA/CORE00.ESP + CORE01.TIM) for
the DS: blood, smoke, sparks, shell casings (PS1 effect bank 0, spawned with
FUN_8001b8fc).

CORE00.ESP (format as in OpenBiohazard2 fileio/esp.go): 8 effect ids, and
from the end of the file one u32 block offset per slot. A block: u16 frame
count, u16 sprite count, u8 texture page, u8 CLUT row, u16; frames of 8
bytes (sprite, count, time, side, s16 x, s16 y), sprites of 4 bytes (u, v,
offset x, offset y). A frame with side 0 ends a sub-animation (the blood
effect has three: a big splash, a smaller one, drops).

CORE01.TIM has no TIM header: 512 x 256 texels at 4 bpp. The effects of
CORE00.ESP use its right half (labels "CHI" = blood, "GUN"); the left half
(smoke and fire) belongs to other banks. The palettes are not in these
files (the PS1 has them in VRAM already); the blood ramp used here is dark
red over the 16 brightness steps the sprites are drawn in.

Output effect/core.bin:
  u32 "RES1", u16 anim count, u16 frame count
  anims: u8 effect id, u8 sub-animation, u16 first frame, u16 frames, u16 0
  frames: u8 u, u8 v, u8 side, s8 offset x, s8 offset y, u8 time, u16 0
  u16 palette[16] (BGR555, 0 = transparent), u8 texels[256 * 256 / 2]

Usage: python3 -I esp2ds.py CORE00.ESP CORE01.TIM out.bin
"""
import struct
import sys

MAGIC = 0x31534552  # "RES1"


def blood_palette():
    pal = [0]
    for i in range(1, 16):
        r = min(31, 10 + i * 21 // 15)
        g = max(0, (i - 11) * 2)
        pal.append(r | g << 5 | (g // 2) << 10)
    return pal


def main():
    esp = open(sys.argv[1], "rb").read()
    tim = open(sys.argv[2], "rb").read()
    n = len(esp)
    anims, frames = [], []
    for slot in range(8):
        eid = esp[slot]
        if eid == 0xFF:
            continue
        off = struct.unpack_from("<I", esp, n - 4 - 4 * slot)[0]
        nf, ns, _, _, _ = struct.unpack_from("<HHBBH", esp, off)
        frs = [struct.unpack_from("<BBBBhh", esp, off + 8 + 8 * j) for j in range(nf)]
        sps = [struct.unpack_from("<BBbb", esp, off + 8 + 8 * nf + 4 * j) for j in range(ns)]
        sub, first = 0, len(frames)
        for sid, _, time, side, _, _ in frs:
            if side == 0:
                if len(frames) > first:
                    anims.append((eid, sub, first, len(frames) - first))
                sub, first = sub + 1, len(frames)
                continue
            u, v, ox, oy = sps[sid] if sid < ns else (0, 0, 0, 0)
            frames.append((u, v, side, ox, oy, min(time, 255)))
        if len(frames) > first:
            anims.append((eid, sub, first, len(frames) - first))
    # right half of the 512-texel rows: bytes 128..255 of each row
    tex = b"".join(tim[y * 256 + 128:(y + 1) * 256] for y in range(256))
    out = struct.pack("<IHH", MAGIC, len(anims), len(frames))
    out += b"".join(struct.pack("<BBHHH", e, s, f, c, 0) for e, s, f, c in anims)
    out += b"".join(struct.pack("<BBBbbBH", *f, 0) for f in frames)
    out += struct.pack("<16H", *blood_palette()) + tex
    open(sys.argv[3], "wb").write(out)
    print(f"{sys.argv[3]}: {len(anims)} animations, {len(frames)} frames")


if __name__ == "__main__":
    main()
