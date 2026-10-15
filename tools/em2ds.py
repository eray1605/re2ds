"""Convert the enemy / character models of PL0/PLD/CDEMD0.EMS for the DS.

The executable's table at 0x8009328c (read from SLES_009.72) holds four
(sector, size) entries per enemy type t >= 0x10: two link variants of the
type's code overlay, the texture (TIM, 8 bpp, 2 palettes = 2 pages) and the
model (EMD). Zombies 0x10-0x1F share one code block but have their own model.

EMD: u32 directory offset, u32 count; the directory lists section offsets.
The last section is the mesh (MD1); before it come (EDD, EMR) pairs from
section 1 on: animation lists and keyframes. The first EMR carries the
skeleton, later ones only keyframes (like a PLW).

Output per type (model/emXX.bin, model/emXX_tex.bin like Leon's) plus
model/emXX_sets.bin: u8 set count, then u16 first animation per set (the
animations of all sets are stored one after another).

Usage: python3 -I em2ds.py SLES_009.72 CDEMD0.EMS out_dir
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import model2ds  # noqa: E402
import pld2json  # noqa: E402

TABLE = 0x8009328C
EXE_BASE = 0x80010000 - 0x800   # PS-X EXE: RAM 0x80010000 at file offset 0x800
SECTOR = 2048
FIRST_TYPE, LAST_TYPE = 0x10, 0x5F


def model_table(exe):
    out = {}
    for t in range(FIRST_TYPE, LAST_TYPE + 1):
        o = TABLE - EXE_BASE + 32 * (t - FIRST_TYPE)
        if o + 32 > len(exe):
            break
        tim_sec, tim_size, emd_sec, emd_size = struct.unpack_from("<4I", exe, o + 16)
        if tim_size and emd_size:
            out[t] = (tim_sec * SECTOR, emd_sec * SECTOR)
    return out


def convert(ems, tim_off, emd_off):
    dir_off, count = struct.unpack_from("<II", ems, emd_off)
    secs = [emd_off + o for o in struct.unpack_from(f"<{count}I", ems, emd_off + dir_off)]
    meshes = pld2json.parse_md1(ems, secs[-1])
    skeleton, frames, anims, flags, set_starts = None, [], [], [], []
    for k in range(1, len(secs) - 2, 2):
        edd = pld2json.parse_edd(ems, secs[k])
        used = max((f for a in edd for f, _ in a), default=-1) + 1
        emr = pld2json.parse_emr(ems, secs[k + 1], used)
        if skeleton is None:
            skeleton = emr
        base = len(frames)
        frames += emr["frames"]
        set_starts.append(len(anims))
        anims += [[base + f for f, _ in a] for a in edd]
        flags += [[fl for _, fl in a] for a in edd]
    tim = pld2json.parse_tim_8bpp(ems, tim_off)
    nb = skeleton["bones"]
    model = {
        "skeleton": dict(skeleton, frames=frames),
        "meshes": meshes[:nb],
        "animations": anims,
        "animation_flags": flags,
        "texture": {"palettes": len(tim["palettes"])},
    }
    return model, tim, set_starts


def main():
    exe = open(sys.argv[1], "rb").read()
    ems = open(sys.argv[2], "rb").read()
    out_dir = sys.argv[3]
    os.makedirs(out_dir, exist_ok=True)
    for t, (tim_off, emd_off) in sorted(model_table(exe).items()):
        if emd_off >= len(ems):
            continue
        try:
            model, tim, sets = convert(ems, tim_off, emd_off)
        except Exception as e:  # noqa: BLE001 - report and keep going
            print(f"em{t:02x}: skipped ({e})")
            continue
        prefix = os.path.join(out_dir, f"em{t:02x}")
        model2ds.write_model(model, tim, prefix)
        with open(prefix + "_sets.bin", "wb") as f:
            f.write(struct.pack(f"<B{len(sets)}H", len(sets), *sets))


if __name__ == "__main__":
    main()
