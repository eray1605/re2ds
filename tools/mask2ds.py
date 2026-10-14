"""Convert a room's foreground masks to the DS format (source/formats.h, MaskHeader).

Inputs: rdt2json output (cameras with "masks") and the mask TIMs written by
bssmask.py. Each mask rectangle becomes a world-space quad at its PS1 depth;
the atlas becomes a 256 x 2^n 8 bpp texture whose index 0 is transparent
(PS1: colour 0x0000 is transparent).

Usage: python3 -I mask2ds.py room.json mask_prefix out_dir/<room>
       (reads mask_prefix_camNN_mask.tim, writes out_dir/<room>_camNN.bin)
"""
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tim2png  # noqa: E402

MASK_MAGIC = 0x314B4D52  # "RMK1"
PS1_W, PS1_H = 320, 240


def norm(v):
    length = math.sqrt(sum(c * c for c in v))
    return [c / length for c in v]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def camera_basis(cam):
    """Same convention as check_projection.py (verified on room 100)."""
    f = norm([cam["to"][k] - cam["from"][k] for k in range(3)])
    r = norm(cross(f, [0, -1, 0]))
    d = cross(f, r)
    return cam["from"], f, r, d, cam["screen_distance"]


def unproject(basis, sx, sy, dist):
    eye, f, r, d, screen = basis
    a = (sx - PS1_W / 2) * dist / screen
    b = (sy - PS1_H / 2) * dist / screen
    return [int(round(eye[k] + f[k] * dist + r[k] * a + d[k] * b)) for k in range(3)]


def remap_transparent(palette, indices):
    """Make index 0 the only transparent colour (PS1 colour 0x0000)."""
    transparent = {i for i, c in enumerate(palette) if c == 0}
    opaque = [i for i in range(256) if i not in transparent]
    if len(opaque) > 255:
        raise SystemExit("palette uses all 256 colours, no slot for transparency")
    new_index = {old: n + 1 for n, old in enumerate(opaque)}
    new_palette = [0] + [palette[i] | 0x8000 for i in opaque]
    new_palette += [0] * (256 - len(new_palette))
    return new_palette, bytes(0 if v in transparent else new_index[v] for v in indices)


def main():
    room = json.load(open(sys.argv[1]))
    mask_prefix, out = sys.argv[2], sys.argv[3]
    os.makedirs(os.path.dirname(out), exist_ok=True)
    for cam_id, cam in enumerate(room["cameras"]):
        if not cam["masks"]:
            continue
        tim_path = f"{mask_prefix}_cam{cam_id:02d}_mask.tim"
        tim = tim2png.parse_tim(open(tim_path, "rb").read(), 0)
        assert tim["bpp"] == 8 and tim["width"] == 256
        palette, indices = remap_transparent(tim["palettes"][0], tim2png.tim_indices(tim))
        tex_h = 1 << (tim["height"] - 1).bit_length()
        pixels = indices + bytes(256 * (tex_h - tim["height"]))

        basis = camera_basis(cam)
        quads = bytearray()
        for m in cam["masks"]:
            x0, y0 = m["dest"]
            x1, y1 = x0 + m["w"], y0 + m["h"]
            dist = m["depth"] * 32
            corners = [unproject(basis, x, y, dist) for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1))]
            quads += struct.pack("<BBHHH12i", m["src"][0], m["src"][1], m["w"], m["h"], m["depth"],
                                 *[c for p in corners for c in p])

        blob = struct.pack("<IHHHH", MASK_MAGIC, len(cam["masks"]), tex_h, tim["height"], 0)
        blob += struct.pack("<256H", *palette) + pixels + quads
        path = f"{out}_cam{cam_id:02d}.bin"
        with open(path, "wb") as f:
            f.write(blob)
        print(f"{path}: {len(cam['masks'])} quads, texture 256x{tex_h} (used {tim['height']}), {len(blob)} bytes")


if __name__ == "__main__":
    main()
