"""Convert the enemy sound banks of COMMON/SOUND/ENEMSE.VBS for the DS.

The file holds 72 sets, one per pair of enemy voice kinds. The executable's
table at 0x8009fb90 gives per set two (u32 size, u16 sector, u8 sector high,
u8) parts, sectors counted from the file start: a 32-entry sound table plus
the VAB header, and the samples. Which set a room uses comes from the voice
kinds (sce_em_set byte 7) of its enemies: the pairs at 0x8009f4cc (FUN_8004f1d0)
list (first kind, second kind) per set, 0xFF ends the list; enemies of the
second kind play sound number + 16 (FUN_800576a8, object flag 0x2000).

Output: sound/enemNN.bin (bank per set, keyed by sound number 0..31, like
vab2ds.py --vb) and sound/enem_pairs.bin (u8 count, then count x (u8, u8)).

Usage: python3 -I enemse2ds.py SLES_009.72 ENEMSE.VBS out_dir
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vab2ds  # noqa: E402

EXE_BASE = 0x80010000 - 0x800
PAIRS = 0x8009F4CC
PARTS = 0x8009FB90
SECTOR = 2048


def main():
    exe = open(sys.argv[1], "rb").read()
    vbs = open(sys.argv[2], "rb").read()
    out = sys.argv[3]
    os.makedirs(out, exist_ok=True)
    pairs, o = [], PAIRS - EXE_BASE
    while exe[o] != 0xFF:
        pairs.append((exe[o], exe[o + 1]))
        o += 2
    for i in range(len(pairs)):
        size_a, sec_a, hi_a, _, size_b, sec_b, hi_b, _ = struct.unpack_from(
            "<IHBBIHBB", exe, PARTS - EXE_BASE + 16 * i)
        a = (sec_a | hi_a << 16) * SECTOR
        b = (sec_b | hi_b << 16) * SECTOR
        head, samples = vbs[a:a + size_a], vbs[b:b + size_b]
        base = head.find(b"pBAV")
        tone, ps, vags = vab2ds.parse_vab(head, base, samples)
        picks = vab2ds.sound_table(struct.unpack_from(f"<{base // 4}I", head, 0))
        vab2ds.write_bank(tone, ps, vags, picks, os.path.join(out, f"enem{i:02d}.bin"))
    with open(os.path.join(out, "enem_pairs.bin"), "wb") as f:
        f.write(bytes([len(pairs)]) + b"".join(bytes(p) for p in pairs))


if __name__ == "__main__":
    main()
