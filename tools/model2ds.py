"""Convert pld2json output to the DS model + texture format (source/formats.h).

- One packed GX display list per (bone, texture page): BEGIN / TEXCOORD /
  VTX_16 / END. Vertices stay in bone-local game units (fit VTX_16 directly).
- PS1 quads are strips (0,1,2,3); the DS draws quads as polygons, so the
  vertex order becomes 0,1,3,2.
- Keyframe angles: 12 bit (4096 = 360 deg) -> DS units (32768 = 360 deg).
- Texture: each 128-pixel-wide page with its own 256-colour palette. PS1 and
  DS share the BGR555 layout.

Usage: python3 -I model2ds.py model.json texture_source.PLD out_prefix
  -> out_prefix.bin, out_prefix_tex.bin
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pld2json  # noqa: E402

MODEL_MAGIC = 0x31444D52  # "RMD1"
CMD_COLOR, CMD_TEXCOORD, CMD_VTX16 = 0x20, 0x22, 0x23
CMD_BEGIN, CMD_END = 0x40, 0x41
PARAMS = {CMD_COLOR: 1, CMD_TEXCOORD: 1, CMD_VTX16: 2, CMD_BEGIN: 1, CMD_END: 0}
GL_TRIANGLES, GL_QUADS = 0, 1


def pack_gx(commands):
    """commands: list of (cmd, [params]) -> packed FIFO words (glCallList format)."""
    words = []
    for i in range(0, len(commands), 4):
        group = commands[i:i + 4]
        ids = [c for c, _ in group] + [0] * (4 - len(group))
        words.append(ids[0] | ids[1] << 8 | ids[2] << 16 | ids[3] << 24)
        for cmd, params in group:
            assert len(params) == PARAMS[cmd]
            words += params
    return [len(words)] + words


def vtx16(v):
    x, y, z = (int(c) for c in v)
    assert all(-32768 <= c <= 32767 for c in (x, y, z)), v
    return [(y & 0xFFFF) << 16 | (x & 0xFFFF), z & 0xFFFF]


def texcoord(u, v):
    return [((v * 16) & 0xFFFF) << 16 | ((u * 16) & 0xFFFF)]


def build_lists(mesh, page):
    lists = []
    for prim, size in ((GL_TRIANGLES, 3), (GL_QUADS, 4)):
        cmds = []
        for face in mesh["faces"]:
            if face["page"] != page or len(face["v"]) != size:
                continue
            verts = mesh["tri_vertices"] if face["src"] == "tri" else mesh["quad_vertices"]
            order = (0, 1, 2) if size == 3 else (0, 1, 3, 2)
            for k in order:
                u, v = face["uv"][k]
                cmds.append((CMD_TEXCOORD, texcoord(u, v)))
                cmds.append((CMD_VTX16, vtx16(verts[face["v"][k]])))
        if cmds:
            lists += [(CMD_BEGIN, [prim])] + cmds + [(CMD_END, [])]
    return pack_gx(lists) if lists else None


def main():
    model = json.load(open(sys.argv[1]))
    pld_path, out = sys.argv[2], sys.argv[3]
    skel = model["skeleton"]
    nb = skel["bones"]
    pages = model["texture"]["palettes"]

    parents = [-1] * nb
    for p, kids in enumerate(skel["children"]):
        for k in kids:
            parents[k] = p

    bones = b"".join(struct.pack("<bB3h", parents[b], 0, *skel["rel_pos"][b]) for b in range(nb))

    # list_offsets: offset within lists_blob, or None when the bone has no
    # faces on that page
    lists_blob, list_offsets = bytearray(), []
    for b in range(nb):
        for p in range(pages):
            words = build_lists(model["meshes"][b], p)
            if words is None:
                list_offsets.append(None)
            else:
                list_offsets.append(len(lists_blob))
                lists_blob += struct.pack(f"<{len(words)}I", *words)

    frames = bytearray()
    for fr in skel["frames"]:
        frames += struct.pack("<6h", *fr["offset"], *fr["speed"])
        for rx, ry, rz in fr["rot"]:
            frames += struct.pack("<3h", rx * 8, ry * 8, rz * 8)

    anims, pool = bytearray(), []
    for a in model["animations"]:
        anims += struct.pack("<HH", len(pool), len(a))
        pool += a
    anims += struct.pack(f"<{len(pool)}H", *pool)

    header_size = 28  # sizeof(ModelHeader)
    off_bones = header_size
    off_lists = off_bones + len(bones)
    off_list_data = off_lists + 4 * len(list_offsets)
    off_frames = off_list_data + len(lists_blob)
    off_anims = off_frames + len(frames)
    # file offsets; 0 = no list (real lists always start after the header)
    table = [0 if o is None else off_list_data + o for o in list_offsets]

    header = struct.pack("<IHHHHIIII", MODEL_MAGIC, nb, pages, len(skel["frames"]), len(model["animations"]),
                         off_bones, off_lists, off_frames, off_anims)
    assert len(header) == header_size
    blob = header + bones + struct.pack(f"<{len(table)}I", *table) + lists_blob + frames + anims
    with open(out + ".bin", "wb") as f:
        f.write(blob)

    # texture pages
    d = open(pld_path, "rb").read()
    dir_off, _ = struct.unpack_from("<II", d, 0)
    off_tim = struct.unpack_from("<4I", d, dir_off)[3]
    tim = pld2json.parse_tim_8bpp(d, off_tim)
    pw = tim["width"] // pages
    tex = bytearray()
    for p in range(pages):
        tex += struct.pack("<256H", *tim["palettes"][p])
        for y in range(tim["height"]):
            row = tim["indices"][y * tim["width"] + p * pw: y * tim["width"] + (p + 1) * pw]
            tex += row
    with open(out + "_tex.bin", "wb") as f:
        f.write(tex)

    n_lists = sum(1 for t in table if t)
    print(f"{out}.bin: {len(blob)} bytes ({nb} bones, {n_lists} display lists = {len(lists_blob)} bytes, "
          f"{len(skel['frames'])} keyframes, {len(model['animations'])} animations); "
          f"{out}_tex.bin: {len(tex)} bytes ({pages} pages {pw}x{tim['height']})")


if __name__ == "__main__":
    main()
