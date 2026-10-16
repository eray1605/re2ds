"""Convert a room's object models (items lying around, the gun shop's shotgun,
a gate, ...) for the DS.

RDT header byte 2 = number of models; section 10 ("items") holds per model
two u32 offsets: TIM (8 bpp, shared by the room's models, one palette row per
model) and MD1 (meshes like an EMD's, without a skeleton). The scripts place
them with obj_model_set (slot = model).

A face's CLUT field says which palette row it uses (y = clut >> 6). Per row
used, the texels the faces cover are cut out into their own DS texture (8 bpp,
power-of-two size). Per model one GX display list (model2ds.pack_gx).

Writes room/<rdt>_obj.bin:
  u32 magic "ROB1", u16 models, u16 textures
  textures[]: u16 width, height (texels, powers of two), u32 offset of the
              palette (256 x u16) followed by width * height indices
  models[]:   u16 texture, u16 pad, u32 offset of the display list (0 = none)
Rooms without models get no file.

Usage: python3 -I obj2ds.py ROOM1010.RDT out.bin
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import model2ds  # noqa: E402
import tim2png  # noqa: E402

MAGIC = 0x31424F52  # "ROB1"


def pow2(n):
    p = 8
    while p < n:
        p *= 2
    return p


def read_faces(d, base):
    """[(vertices, [(u, v)], clut_row)] of all meshes of the MD1 at base."""
    _, _, num_obj = struct.unpack_from("<III", d, base)
    start = base + 12
    out = []
    for i in range(num_obj // 2):
        hdr = struct.unpack_from("<14I", d, start + i * 56)
        for (voff, _, _, _, ioff, n, toff), size in ((hdr[:7], 3), (hdr[7:], 4)):
            for k in range(n):
                idx = struct.unpack_from(f"<{2 * size}H", d, start + ioff + 4 * size * k)
                rec = struct.unpack_from("<" + "BBH" * size, d, start + toff + 4 * size * k)
                uv = [(rec[3 * j], rec[3 * j + 1]) for j in range(size)]
                clut = rec[2]
                verts = [struct.unpack_from("<3h", d, start + voff + 8 * idx[2 * j + 1]) for j in range(size)]
                out.append((verts, uv, clut >> 6))
    return out


def main():
    src, out = sys.argv[1], sys.argv[2]
    d = open(src, "rb").read()
    count = d[2]
    table = struct.unpack_from("<23I", d, 8)[10]
    if not count or not table:
        if os.path.exists(out):
            os.remove(out)
        return
    models = []
    for i in range(count):
        tim_off, md1_off = struct.unpack_from("<II", d, table + 8 * i)
        models.append((tim_off, read_faces(d, md1_off) if md1_off else []))

    # one texture per (TIM, palette row): the texels its faces use
    textures = {}
    for tim_off, faces in models:
        for _, uv, row in faces:
            box = textures.setdefault((tim_off, row), [255, 255, 0, 0])
            for u, v in uv:
                box[0], box[1] = min(box[0], u), min(box[1], v)
                box[2], box[3] = max(box[2], u), max(box[3], v)
    keys = sorted(textures)
    tex_blobs = []
    for tim_off, row in keys:
        tim = tim2png.parse_tim(d, tim_off)
        x0, y0, x1, y1 = textures[(tim_off, row)]
        w, h = pow2(x1 - x0 + 1), pow2(y1 - y0 + 1)
        pal_row = max(0, min(len(tim["palettes"]) - 1, row - tim["clut"][1]))
        pal = list(tim["palettes"][pal_row]) + [0] * 256
        idx = tim2png.tim_indices(tim)
        pix = bytearray(w * h)
        for y in range(h):
            for x in range(w):
                sx, sy = x0 + x, y0 + y
                if sx < tim["width"] and sy < tim["height"]:
                    pix[y * w + x] = idx[sy * tim["width"] + sx]
        tex_blobs.append((w, h, x0, y0, struct.pack("<256H", *[c | 0x8000 if c else 0 for c in pal[:256]]) + pix))

    lists = []
    for tim_off, faces in models:
        cmds, tex = [], 0
        for prim, size in ((model2ds.GL_TRIANGLES, 3), (model2ds.GL_QUADS, 4)):
            body = []
            for verts, uv, row in faces:
                if len(verts) != size:
                    continue
                tex = keys.index((tim_off, row))
                _, _, x0, y0, _ = tex_blobs[tex]
                for k in ((0, 1, 2) if size == 3 else (0, 1, 3, 2)):
                    body.append((model2ds.CMD_TEXCOORD, model2ds.texcoord(uv[k][0] - x0, uv[k][1] - y0)))
                    body.append((model2ds.CMD_VTX16, model2ds.vtx16(verts[k])))
            if body:
                cmds += [(model2ds.CMD_BEGIN, [prim])] + body + [(model2ds.CMD_END, [])]
        lists.append((tex, model2ds.pack_gx(cmds) if cmds else None))

    head = struct.pack("<IHH", MAGIC, len(models), len(tex_blobs))
    pos = len(head) + 8 * len(tex_blobs) + 8 * len(models)
    tex_table, data = b"", b""
    for w, h, _, _, blob in tex_blobs:
        tex_table += struct.pack("<HHI", w, h, pos + len(data))
        data += blob
    model_table = b""
    for tex, words in lists:
        if words is None:
            model_table += struct.pack("<HHI", 0, 0, 0)
            continue
        while (pos + len(data)) % 4:
            data += b"\0"
        model_table += struct.pack("<HHI", tex, 0, pos + len(data))
        data += struct.pack(f"<{len(words)}I", *words)
    with open(out, "wb") as f:
        f.write(head + tex_table + model_table + data)
    print(f"{out}: {len(models)} models, textures {[(w, h) for w, h, *_ in tex_blobs]}")


if __name__ == "__main__":
    main()
