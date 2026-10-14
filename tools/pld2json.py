"""Convert an RE2 player model (.PLD, PS1 or PC) to JSON + texture PNG.

A PLD file starts with (u32 dir_offset, u32 count). The directory holds four
offsets: EDD animation table, EMR skeleton + keyframes, MD1 mesh, TIM texture.
Field layouts follow OpenBiohazard2's fileio/{pld,edd,emr,md1,tim}.go.

A PLW file (weapon set, e.g. PL00W00.PLW) has the same four sections, but its
EMR holds keyframes only. Its animations (walk, run, idle, aim, ...) are
appended after the PLD's own animations when --plw is given; the PLD's
special animations come first.

Usage: python3 -I pld2json.py PL00.PLD out_dir/leon [--plw PL00W00.PLW]
  -> out_dir/leon.json, out_dir/leon_tex.png
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imgio import write_png  # noqa: E402


def read_lsb_bits(data, offset, length):
    """Bit reader over little-endian bytes, least significant bit first."""
    value = int.from_bytes(data[offset:offset + length], "little")
    pos = 0

    def take(n):
        nonlocal pos
        out = (value >> pos) & ((1 << n) - 1)
        pos += n
        return out
    return take


def parse_edd(d, base):
    """Animations: list of [(frame_id, flags), ...]."""
    first_count, first_off = struct.unpack_from("<HH", d, base)
    headers = [struct.unpack_from("<HH", d, base + 4 * i) for i in range(first_off // 4)]
    anims = []
    for count, off in headers:
        take = read_lsb_bits(d, base + off, count * 4)
        anims.append([(take(12), take(20)) for _ in range(count)])
    return anims


def parse_emr(d, base, num_frames):
    off_arm, off_frames, count, elem_size = struct.unpack_from("<4H", d, base)
    if off_arm:
        rel = [list(struct.unpack_from("<3h", d, base + 8 + 6 * i)) for i in range(count)]
        arm = [struct.unpack_from("<HH", d, base + off_arm + 4 * i) for i in range(count)]
        children = [list(d[base + off_arm + o: base + off_arm + o + n]) for n, o in arm]
    else:
        # PLW files carry keyframes only and reuse the PLD skeleton
        rel, children = None, None
    frames = []
    angle_count = (elem_size - 12) * 8 // 12
    for f in range(num_frames):
        fo = base + off_frames + f * elem_size
        header = list(struct.unpack_from("<6h", d, fo))
        take = read_lsb_bits(d, fo + 12, elem_size - 12)
        angles = [[take(12), take(12), take(12)] for _ in range(angle_count // 3)]
        frames.append({"offset": header[:3], "speed": header[3:], "rot": angles[:count]})
    return {"bones": count, "rel_pos": rel, "children": children, "frames": frames}


def parse_md1(d, base):
    _, _, num_obj = struct.unpack_from("<III", d, base)
    objs = []
    start = base + 12
    for i in range(num_obj // 2):
        hdr = struct.unpack_from("<14I", d, start + i * 56)
        tri, quad = hdr[:7], hdr[7:]

        def verts(off, n):
            return [list(struct.unpack_from("<3h", d, start + off + 8 * k)) for k in range(n)]

        faces = []
        # Triangles: index records (n0,v0,n1,v1,n2,v2), texture records 12 bytes
        for k in range(tri[5]):
            idx = struct.unpack_from("<6H", d, start + tri[4] + 12 * k)
            u0, v0, clut, u1, v1, page, u2, v2, _ = struct.unpack_from("<BBHBBHBBH", d, start + tri[6] + 12 * k)
            faces.append({"v": [idx[1], idx[3], idx[5]], "n": [idx[0], idx[2], idx[4]],
                          "uv": [[u0, v0], [u1, v1], [u2, v2]], "page": page & 3, "clut": clut & 0x3F,
                          "src": "tri"})
        for k in range(quad[5]):
            idx = struct.unpack_from("<8H", d, start + quad[4] + 16 * k)
            u0, v0, clut, u1, v1, page, u2, v2, _, u3, v3, _ = struct.unpack_from(
                "<BBHBBHBBHBBH", d, start + quad[6] + 16 * k)
            faces.append({"v": [idx[1], idx[3], idx[5], idx[7]], "n": [idx[0], idx[2], idx[4], idx[6]],
                          "uv": [[u0, v0], [u1, v1], [u2, v2], [u3, v3]], "page": page & 3,
                          "clut": clut & 0x3F, "src": "quad"})
        objs.append({
            "tri_vertices": verts(tri[0], tri[1]), "tri_normals": verts(tri[2], tri[3]),
            "quad_vertices": verts(quad[0], quad[1]), "quad_normals": verts(quad[2], quad[3]),
            "faces": faces,
        })
    return objs


def parse_tim_8bpp(d, base):
    magic, flags = struct.unpack_from("<II", d, base)
    assert magic == 0x10 and flags & 3 == 1 and flags & 8, "expected 8bpp TIM with CLUT"
    clut_len, _, _, ncolors, nclut = struct.unpack_from("<I4H", d, base + 8)
    pal_base = base + 20
    palettes = [[struct.unpack_from("<H", d, pal_base + 2 * (p * ncolors + c))[0] for c in range(ncolors)]
                for p in range(nclut)]
    img = base + 8 + clut_len
    _, _, _, w_words, height = struct.unpack_from("<I4H", d, img)
    width = w_words * 2
    pixels = d[img + 12: img + 12 + width * height]
    return {"width": width, "height": height, "palettes": palettes, "indices": pixels}


def tim_to_rgb(tim):
    """Each 1/N horizontal region of the image uses palette N (as in OpenBiohazard2)."""
    w, h, pals = tim["width"], tim["height"], tim["palettes"]
    rgb = bytearray(w * h * 3)
    for y in range(h):
        for x in range(w):
            c = pals[x * len(pals) // w][tim["indices"][y * w + x]]
            o = (y * w + x) * 3
            rgb[o] = (c & 31) << 3
            rgb[o + 1] = ((c >> 5) & 31) << 3
            rgb[o + 2] = ((c >> 10) & 31) << 3
    return rgb


def main():
    src, out = sys.argv[1], sys.argv[2]
    d = open(src, "rb").read()
    dir_off, count = struct.unpack_from("<II", d, 0)
    off_edd, off_emr, off_md1, off_tim = struct.unpack_from("<4I", d, dir_off)

    anims = parse_edd(d, off_edd)
    num_frames = 1 + max(fid for a in anims for fid, _ in a)
    skeleton = parse_emr(d, off_emr, num_frames)
    meshes = parse_md1(d, off_md1)
    tim = parse_tim_8bpp(d, off_tim)

    anim_sets = [{"source": src.split("/")[-1], "first": 0, "count": len(anims)}]
    if "--plw" in sys.argv:
        plw_path = sys.argv[sys.argv.index("--plw") + 1]
        p = open(plw_path, "rb").read()
        p_dir, _ = struct.unpack_from("<II", p, 0)
        p_edd, p_emr, _, _ = struct.unpack_from("<4I", p, p_dir)
        plw_anims = parse_edd(p, p_edd)
        plw_frames = parse_emr(p, p_emr, 1 + max(fid for a in plw_anims for fid, _ in a))["frames"]
        base = len(skeleton["frames"])
        skeleton["frames"] += plw_frames
        anim_sets.append({"source": plw_path.split("/")[-1], "first": len(anims), "count": len(plw_anims)})
        anims += [[(fid + base, fl) for fid, fl in a] for a in plw_anims]

    model = {
        "source": src.split("/")[-1],
        "animation_sets": anim_sets,
        "texture": {"width": tim["width"], "height": tim["height"], "palettes": len(tim["palettes"]),
                    "page_width": tim["width"] // len(tim["palettes"])},
        "skeleton": skeleton,
        "animations": [[fid for fid, _ in a] for a in anims],
        "animation_flags": [[fl for _, fl in a] for a in anims],
        "meshes": meshes,
    }
    with open(out + ".json", "w") as f:
        json.dump(model, f)
    write_png(out + "_tex.png", tim["width"], tim["height"], tim_to_rgb(tim))

    tris = sum(1 for m in meshes for fc in m["faces"] if len(fc["v"]) == 3)
    quads = sum(1 for m in meshes for fc in m["faces"] if len(fc["v"]) == 4)
    print(f"{src}: {skeleton['bones']} bones, {len(meshes)} meshes, {tris} triangles + {quads} quads, "
          f"{len(anims)} animations, {len(skeleton['frames'])} keyframes, "
          f"texture {tim['width']}x{tim['height']}")


if __name__ == "__main__":
    main()
