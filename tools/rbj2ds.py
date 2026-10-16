"""Extract the room's own animations (RDT section RBJ) for the DS.

RBJ: u32 table offset, u32 count, then data; the table holds count pairs of
offsets relative to RBJ: the first points to a u32 mask followed by the EMR
(keyframes, no armature, like a PLW), the second to the EDD. The mask
says who uses the set (FUN_800187a8): bit 0 the player, bit n >= 2 the
enemy of sce_em_set slot n - 2 (bit 1 is the partner; later sets win).

Player sets usually hold the special moves at animations 6..9 (pl_move_08 /
09: climb up, jump down, falling, landing; lengths 50, 15, 20, 20); others
hold a room's cutscene motions (plc_motion mode 0), e.g. room 101's 14
animations shared by Leon and Kendo.

Writes an RMD1 file without display lists (the character's own are used)
with all animations of a set: out.bin for the player's, out_e<slot>.bin for
enemies. Rooms without such sets get no files.

Usage: python3 -I rbj2ds.py ROOM1010.RDT out.bin
"""
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pld2json  # noqa: E402
from rdt2json import read_header  # noqa: E402

MODEL_MAGIC = 0x31444D52  # "RMD1"
MAX_SLOTS = 16


def read_sets(d):
    """[(mask, emr_offset, animations)]"""
    _, offs = read_header(d)
    r = offs.get("rbj")
    if not r:
        return []
    table, count = struct.unpack_from("<II", d, r)
    base = r + 4
    sets = []
    for k in range(count):
        emr, edd = struct.unpack_from("<II", d, r + table + 8 * k)
        mask = struct.unpack_from("<I", d, r + emr)[0]
        anims = pld2json.parse_edd(d, r + edd)  # FUN_800187a8: EDD at RBJ + offset
        if anims:
            sets.append((mask, base + emr, anims))
    return sets


def frames_in_file(d, emr):
    """How many keyframes of the EMR at emr fit in the file."""
    _, off_frames, _, elem_size = struct.unpack_from("<4H", d, emr)
    return max(0, (len(d) - emr - off_frames) // elem_size) if elem_size else 0


def write_set(d, emr, anims, out):
    # a few sets list keyframes past the end of the file: those frames show
    # the last one there is
    last = frames_in_file(d, emr) - 1
    if last < 0:
        return
    anims = [[(min(f, last), fl) for f, fl in a] for a in anims]
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
    off_bones = header_size                    # no bones: the character's own are used
    off_lists = off_bones
    off_frames = off_lists
    off_anims = off_frames + len(frames)
    header = struct.pack("<IHHHHIIII", MODEL_MAGIC, nb, 0, len(used), len(anims),
                         off_bones, off_lists, off_frames, off_anims)
    with open(out, "wb") as f:
        f.write(header + frames + table)
    print(f"{out}: {len(used)} keyframes, animations {[len(a) for a in anims]}")


def main():
    src, out = sys.argv[1], sys.argv[2]
    stem = out[:-4] if out.endswith(".bin") else out
    for old in [out] + glob.glob(stem + "_e*.bin"):
        if os.path.exists(old):
            os.remove(old)
    d = open(src, "rb").read()
    player, enemies = None, {}
    for mask, emr, anims in read_sets(d):
        if mask & 1:
            player = (emr, anims)
        for slot in range(MAX_SLOTS):
            if mask >> (slot + 2) & 1:
                enemies[slot] = (emr, anims)
    if player:
        write_set(d, *player, out)
    for slot, s in sorted(enemies.items()):
        write_set(d, *s, f"{stem}_e{slot}.bin")
    if not player and not enemies:
        print(f"{src}: no room animations")


if __name__ == "__main__":
    main()
