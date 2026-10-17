"""Effect sprites the scripts start with sce_espr_on (fires, smoke, sparks)
for the DS (source/roomfx.c): a room's own ones, or the core ones.

Room: RDT header offsets 18 / 19 / 20: the ESP (8 effect ids, then blocks;
from offset 19 backwards one u32 block offset per slot) and the TIMs, one per
used slot in slot order (4 bpp, 256 texels wide, 1-3 palettes of 16 colours).

Core (--core): COMMON/DATA/CORE00.ESP (block offsets from the end of the
file). Its texels are CORE01.TIM, no TIM header: 512 x 256 at 4 bpp, in VRAM
at (896, 256), i.e. texture pages 0x1e (left half: smoke, fire) and 0x1f
(right: blood, gun). The palettes are in VRAM at (272, 480 + row), as in
COMMON/DATA/TEX_PAL.TIM (a VRAM image with its CLUT block at (256, 480),
32 x 19). Block header: u16 frames, u16 sprites, u16 CLUT id, u16 tpage.

A block (FUN_8001b8fc reads it): frames of 8 bytes (first sprite, sprite
count, time, size, s16, s16): time 255 = go to frame <first sprite>, time 0 =
end; sprites of 4 bytes (u, v, s8 x, s8 y: the top left corner from the
anchor, each a square of the frame's size); then a u16 table of sub effects
(in u32 units from the table) whose records start with u16 part count, u16,
then 24 bytes per part: u16, u16, u16 mode (11 = repeats from the first
frame at the end), u16 first frame, u16 scale x, u16 scale y (4096 = 1),
u16 fall (glass shards: 3072 / 9728, 0 for fires and smoke), u16, u16 speed
(shards: 32..128, thrown along the sce_espr_on direction), ..., the last u16
0x0800 = drawn mirrored. Only the first part of a sub effect is used here.

Output room/<rdt>_esp.bin or effect/core_fx.bin:
  u32 "RRE1", u16 effects, u16 0
  effects[]: u8 id, u8 palettes, u8 frames, u8 sprites, u8 subs, u8 tex height
             log2, u16 0, u32 data offset
  data: u16 palettes[n][16], frames (u8 first sprite, u8 count, u8 time,
        u8 size), sprites (u8 u, u8 v, s8 x, s8 y), subs (u8 first frame,
        u8 flags: 1 mirrored, 2 repeats, 4 falls; u16 scale, 4096 = 1;
        u16 speed, u16 0), 4 bpp texels 256 x 2^log2 (padded)
Rooms without effects get no file.

Usage: python3 -I resp2ds.py ROOM1000.RDT out.bin
       python3 -I resp2ds.py --core CORE00.ESP CORE01.TIM TEX_PAL.TIM out.bin
"""
import os
import struct
import sys

MAGIC = 0x31455252  # "RRE1"
CORE_PALETTES = 4   # rows from the effect's own (sce_espr_on adds type >> 3)


def parse_block(d, b):
    nf, ns = struct.unpack_from("<HH", d, b)
    frames = [struct.unpack_from("<BBBBhh", d, b + 8 + 8 * j) for j in range(nf)]
    sprites = [struct.unpack_from("<BBbb", d, b + 8 + 8 * nf + 4 * j) for j in range(ns)]
    table = b + (nf * 2 + ns + 2) * 4
    subs = []
    for s in range(8):
        t = struct.unpack_from("<H", d, table + 2 * s)[0]
        if s and (t == 0 or t <= struct.unpack_from("<H", d, table + 2 * (s - 1))[0]):
            break
        rec = table + 4 * t
        mode, first, scale, _, fall, _, speed = struct.unpack_from("<7H", d, rec + 8)
        flags = struct.unpack_from("<H", d, rec + 26)[0]
        subs.append((min(first, max(nf - 1, 0)),
                     (1 if flags & 0x800 else 0) | (2 if mode == 11 else 0) | (4 if fall else 0),
                     min(scale, 0xFFFF), speed if fall else 0))
    return frames, sprites, subs


def write(out, effects):
    head = struct.pack("<IHH", MAGIC, len(effects), 0)
    pos = len(head) + 12 * len(effects)
    table, data = b"", b""
    for eid, frames, sprites, subs, log2, palettes, texels in effects:
        table += struct.pack("<BBBBBBHI", eid, len(palettes) // 16, len(frames), len(sprites), len(subs), log2,
                             0, pos + len(data))
        blob = struct.pack(f"<{len(palettes)}H", *[c & 0x7FFF for c in palettes])
        blob += b"".join(struct.pack("<BBBB", s, max(c, 1), t, size) for s, c, t, size, _, _ in frames)
        blob += b"".join(struct.pack("<BBbb", *sp) for sp in sprites)
        blob += b"".join(struct.pack("<BBHHH", f, fl, sc, sp, 0) for f, fl, sc, sp in subs)
        blob += bytes(-len(blob) % 4) + texels
        data += blob
    with open(out, "wb") as f:
        f.write(head + table + data)
    print(f"{out}: effects {[e[0] for e in effects]}")


def room(path, out):
    d = open(path, "rb").read()
    offs = struct.unpack_from("<23I", d, 8)
    esp, esp_end, tim = offs[18], offs[19], offs[20]
    if not esp or not esp_end or not tim:
        if os.path.exists(out):
            os.remove(out)
        return
    ids = d[esp:esp + 8]
    effects = []
    for slot in range(8):
        if ids[slot] == 0xFF:
            continue
        frames, sprites, subs = parse_block(d, esp + struct.unpack_from("<I", d, esp_end - 4 * slot)[0])
        magic, flags = struct.unpack_from("<II", d, tim)
        assert magic == 0x10 and flags == 8, f"slot {slot}: expected a 4 bpp TIM"
        clen, _, _, cw, ch = struct.unpack_from("<IHHHH", d, tim + 8)
        palettes = struct.unpack_from(f"<{cw * ch}H", d, tim + 20)
        p = tim + 8 + clen
        blen, _, _, iw, ih = struct.unpack_from("<IHHHH", d, p)
        assert iw * 4 == 256, iw
        texels = d[p + 12:p + 12 + iw * 2 * ih]
        tim = p + blen
        log2 = 3
        while (1 << log2) < ih:
            log2 += 1
        texels += bytes(128 * ((1 << log2) - ih))
        effects.append((ids[slot], frames, sprites, subs, log2, palettes, texels))
    write(out, effects)


def core(esp_path, core_tim, tex_pal, out):
    esp = open(esp_path, "rb").read()
    vram = open(core_tim, "rb").read()          # 512 x 256 texels, 256 bytes a row
    t = open(tex_pal, "rb").read()
    clen, cx, cy, cw, ch = struct.unpack_from("<IHHHH", t, 8)
    clut = struct.unpack_from(f"<{cw * ch}H", t, 20)
    effects = []
    n = len(esp)
    for slot in range(8):
        eid = esp[slot]
        if eid == 0xFF:
            continue
        b = struct.unpack_from("<I", esp, n - 4 - 4 * slot)[0]
        clut_id, tpage = struct.unpack_from("<HH", esp, b + 4)
        frames, sprites, subs = parse_block(esp, b)
        half = 0 if (tpage & 15) == 14 else 1   # page 0x1e = left half, 0x1f = right
        texels = b"".join(vram[y * 256 + half * 128:y * 256 + half * 128 + 128] for y in range(256))
        x, y = (clut_id & 63) * 16, clut_id >> 6
        palettes = []
        for r in range(CORE_PALETTES):
            row = y - cy + r
            base = row * cw + (x - cx)
            palettes += clut[base:base + 16] if 0 <= row < ch else [0] * 16
        effects.append((eid, frames, sprites, subs, 8, palettes, texels))
    write(out, effects)


def main():
    if sys.argv[1] == "--core":
        core(*sys.argv[2:6])
    else:
        room(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    main()
