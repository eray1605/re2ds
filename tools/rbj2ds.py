"""Extract the room's climb / jump animations (RDT section RBJ) for the DS.

RBJ: u32 table offset, u32 count, then data; the table holds count pairs
(EMR offset, EDD offset), both relative to RBJ + 4. Each pair is an animation
set for the player skeleton (EMR without armature, like a PLW). In the sets
that have them, animations 5..8 are the special moves the player states use
(pl_move_08 / 09, numbered from 1 there): 5 climb up (root rises 1800 and
moves 1000 forward at frame 29), 6 jump down, 7 falling pose, 8 landing.
They differ between rooms (4 variants on disc 1), so every room gets its own.

Writes an RMD1 file without display lists (Leon's are used): 15 bones, the
keyframes and 4 animations in the order climb, jump, fall, land. Rooms
without such a set get no file.

Usage: python3 -I rbj2ds.py ROOM1180.RDT out.bin
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pld2json  # noqa: E402
from rdt2json import read_header  # noqa: E402

MODEL_MAGIC = 0x31444D52  # "RMD1"
SPECIAL = (5, 6, 7, 8)
LENGTHS = [50, 15, 20, 20]


def find_set(d):
    _, offs = read_header(d)
    r = offs.get("rbj")
    if not r:
        return None
    table, count = struct.unpack_from("<II", d, r)
    base = r + 4
    for k in range(count):
        emr, edd = struct.unpack_from("<II", d, r + table + 8 * k)
        anims = pld2json.parse_edd(d, base + edd)
        if len(anims) > max(SPECIAL) and [len(anims[i]) for i in SPECIAL] == LENGTHS:
            return base + emr, [anims[i] for i in SPECIAL]
    return None


def main():
    src, out = sys.argv[1], sys.argv[2]
    d = open(src, "rb").read()
    found = find_set(d)
    if not found:
        if os.path.exists(out):
            os.remove(out)
        print(f"{src}: no climb animations")
        return
    emr, anims = found
    used = sorted({f for a in anims for f, _ in a})
    remap = {f: i for i, f in enumerate(used)}
    sk = pld2json.parse_emr(d, emr, max(used) + 1)
    nb = sk["bones"]

    frames = bytearray()
    for f in used:
        fr = sk["frames"][f]
        frames += struct.pack("<6h", *fr["offset"], *fr["speed"])
        for rx, ry, rz in fr["rot"]:
            frames += struct.pack("<3h", rx * 8, ry * 8, rz * 8)
    table, pool = bytearray(), []
    for a in anims:
        table += struct.pack("<HH", len(pool), len(a))
        pool += [remap[f] | (fl & 0xF) << 12 for f, fl in a]
    table += struct.pack(f"<{len(pool)}H", *pool)

    header_size = 28
    off_bones = header_size                    # no bones: Leon's are used
    off_lists = off_bones
    off_frames = off_lists
    off_anims = off_frames + len(frames)
    header = struct.pack("<IHHHHIIII", MODEL_MAGIC, nb, 0, len(used), len(anims),
                         off_bones, off_lists, off_frames, off_anims)
    with open(out, "wb") as f:
        f.write(header + frames + table)
    print(f"{out}: {len(used)} keyframes, animations {[len(a) for a in anims]}")


if __name__ == "__main__":
    main()
