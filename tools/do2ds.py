"""Convert a door file (COMMON/DOOR/DOORnn.DO2) for the DS door sequence.

A DO2 holds a VAB (sounds, at offset 16), then 0x800-aligned a block with
u32 model offset, u32 TIM offset, the door program (SCD bytecode, run by the
door task 0x800136dc with the normal opcode table 0x8009f560), the model
(MD1, one sub-object per door_model_set part) and its texture (TIM 8 bpp,
128x256, one palette).

Writes <out>_model.bin (RMD1 like Leon: one bone per part, no animation),
<out>_tex.bin (one texture page) and <out>_prog.bin (the raw program).

Usage: python3 -I do2ds.py DOORnn.DO2 out_prefix
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import model2ds  # noqa: E402
import pld2json  # noqa: E402

MODEL_MAGIC = 0x31444D52  # "RMD1"


def door_block(d):
    vab_size = struct.unpack_from("<I", d, 16 + 12)[0]
    block = (16 + vab_size + 0x7FF) & ~0x7FF
    off_model, off_tim = struct.unpack_from("<II", d, block)
    return block, off_model, off_tim


def main():
    src, out = sys.argv[1], sys.argv[2]
    d = open(src, "rb").read()
    block, off_model, off_tim = door_block(d)
    prog = d[block + 8:block + off_model]
    parts = pld2json.parse_md1(d, block + off_model)
    tim = pld2json.parse_tim_8bpp(d, block + off_tim)
    assert tim["width"] == 128 and tim["height"] == 256 and len(tim["palettes"]) == 1

    nb, pages = len(parts), 1
    bones = b"".join(struct.pack("<bB3h", -1, 0, 0, 0, 0) for _ in range(nb))
    lists_blob, list_offs = bytearray(), []
    for b in range(nb):
        words = model2ds.build_lists(parts[b], 0)
        list_offs.append(None if words is None else len(lists_blob))
        if words is not None:
            lists_blob += struct.pack(f"<{len(words)}I", *words)
    # one neutral keyframe
    frames = struct.pack("<6h", 0, 0, 0, 0, 0, 0) + b"\0" * (6 * nb)

    header_size = 28
    off_bones = header_size
    off_lists = off_bones + len(bones)
    off_blob = off_lists + 4 * nb * pages
    table = [0 if o is None else off_blob + o for o in list_offs]
    off_frames = off_blob + len(lists_blob)
    off_anims = off_frames + len(frames)
    header = struct.pack("<IHHHHIIII", MODEL_MAGIC, nb, pages, 1, 0,
                         off_bones, off_lists, off_frames, off_anims)
    blob = header + bones + struct.pack(f"<{nb}I", *table) + lists_blob + frames
    with open(out + "_model.bin", "wb") as f:
        f.write(blob)
    with open(out + "_tex.bin", "wb") as f:
        f.write(struct.pack("<256H", *tim["palettes"][0]) + tim["indices"])
    with open(out + "_prog.bin", "wb") as f:
        f.write(prog)
    print(f"{out}: {nb} parts, {sum(len(p['faces']) for p in parts)} faces, program {len(prog)} bytes")


if __name__ == "__main__":
    main()
