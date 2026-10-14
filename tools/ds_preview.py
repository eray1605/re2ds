"""Render a DS frame from the converted DS files only (no source data), the way
the engine will: background bitmap + Leon decoded from packed GX display lists,
posed from DS-format keyframes, projected with the room camera scaled to
256x192. Proves the formats in source/formats.h are complete and consistent.

Usage:
  python3 -I ds_preview.py build/ds room1000 room100 leon out.png [anim] [frame] [camera]
"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imgio import write_png  # noqa: E402

W, H = 256, 192
DS_ANGLE = 2 * math.pi / 32768


# ---------------------------------------------------------------- file parsing
def load_room(path):
    d = open(path, "rb").read()
    magic, ver, ncam, nsw, nbox, start_cam, _, sx, sy, sz, sdir, _ = struct.unpack_from("<IHBBHBB3ihH", d, 0)
    assert magic == 0x314D5252 and ver == 1
    pos = 28
    cams = []
    for _ in range(ncam):
        v = struct.unpack_from("<3i3iHH", d, pos)
        cams.append({"from": v[0:3], "to": v[3:6], "screen": v[6]})
        pos += 28
    return {"cams": cams, "start_cam": start_cam, "spawn": (sx, sy, sz), "dir": sdir}


def load_model(path):
    d = open(path, "rb").read()
    magic, nb, npages, nframes, nanims, ob, ol, of, oa = struct.unpack_from("<IHHHHIIII", d, 0)
    assert magic == 0x31444D52
    bones = [struct.unpack_from("<bB3h", d, ob + 8 * i) for i in range(nb)]
    table = struct.unpack_from(f"<{nb * npages}I", d, ol)
    frame_size = 12 + 6 * nb
    frames = []
    for i in range(nframes):
        o = of + i * frame_size
        hdr = struct.unpack_from("<6h", d, o)
        rot = [struct.unpack_from("<3h", d, o + 12 + 6 * b) for b in range(nb)]
        frames.append({"offset": hdr[:3], "speed": hdr[3:], "rot": rot})
    anims = [struct.unpack_from("<HH", d, oa + 4 * i) for i in range(nanims)]
    pool_off = oa + 4 * nanims
    pool_len = sum(c for _, c in anims)
    pool = struct.unpack_from(f"<{pool_len}H", d, pool_off)
    lists = {}
    for i, off in enumerate(table):
        if off:
            count = struct.unpack_from("<I", d, off)[0]
            lists[(i // npages, i % npages)] = struct.unpack_from(f"<{count}I", d, off + 4)
    return {"bones": bones, "npages": npages, "frames": frames,
            "anims": [pool[f:f + c] for f, c in anims], "lists": lists}


def load_texture(path, npages):
    d = open(path, "rb").read()
    page_size = 512 + 128 * 256
    pages = []
    for p in range(npages):
        o = p * page_size
        pal = struct.unpack_from("<256H", d, o)
        idx = d[o + 512:o + page_size]
        pages.append((pal, idx))
    return pages


def decode_gx(words):
    """Packed GX FIFO -> list of polygons [(x,y,z,s,t), ...]."""
    nparams = {0x00: 0, 0x20: 1, 0x22: 1, 0x23: 2, 0x40: 1, 0x41: 0}
    polys, cur, mode, tex = [], [], 0, (0, 0)
    i = 0
    while i < len(words):
        ids = [(words[i] >> (8 * k)) & 0xFF for k in range(4)]
        i += 1
        for cmd in ids:
            params = words[i:i + nparams[cmd]]
            i += nparams[cmd]
            if cmd == 0x40:
                mode, cur = params[0], []
            elif cmd == 0x22:
                s = params[0] & 0xFFFF
                t = params[0] >> 16
                tex = ((s - 0x10000 if s & 0x8000 else s) / 16, (t - 0x10000 if t & 0x8000 else t) / 16)
            elif cmd == 0x23:
                def s16(v):
                    return v - 0x10000 if v & 0x8000 else v
                x, y, z = s16(params[0] & 0xFFFF), s16(params[0] >> 16), s16(params[1] & 0xFFFF)
                cur.append((x, y, z) + tex)
                size = 3 if mode == 0 else 4
                if len(cur) == size:
                    polys.append(cur)
                    cur = []
    return polys


# ---------------------------------------------------------------- math
def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def T(x, y, z):
    return [[1, 0, 0, x], [0, 1, 0, y], [0, 0, 1, z], [0, 0, 0, 1]]


def Rx(a):
    c, s = math.cos(a), math.sin(a)
    return [[1, 0, 0, 0], [0, c, -s, 0], [0, s, c, 0], [0, 0, 0, 1]]


def Ry(a):  # PS1 RotMatrixY convention, see formats.h
    c, s = math.cos(a), math.sin(a)
    return [[c, 0, s, 0], [0, 1, 0, 0], [-s, 0, c, 0], [0, 0, 0, 1]]


def Rz(a):
    c, s = math.cos(a), math.sin(a)
    return [[c, -s, 0, 0], [s, c, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]


def apply(m, v):
    return [m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2] + m[r][3] for r in range(3)]


def pose(model, frame, world):
    bones = model["bones"]
    mats = [None] * len(bones)
    for b, (parent, _, *pos) in enumerate(bones):  # parents always precede children? enforce:
        pass
    order = []
    def visit(b):
        order.append(b)
        for c, (p, *_rest) in enumerate(bones):
            if p == b:
                visit(c)
    visit(next(b for b, (p, *_r) in enumerate(bones) if p == -1))
    for b in order:
        parent, _, px, py, pz = bones[b]
        base = world if parent < 0 else mats[parent]
        pos = frame["offset"] if parent < 0 else (px, py, pz)
        rx, ry, rz = (a * DS_ANGLE for a in frame["rot"][b])
        mats[b] = mat_mul(base, mat_mul(T(*pos), mat_mul(Rx(rx), mat_mul(Ry(ry), Rz(rz)))))
    return mats


def norm(v):
    length = math.sqrt(sum(c * c for c in v))
    return [c / length for c in v]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


class Camera:
    def __init__(self, cam):
        self.eye = cam["from"]
        self.f = norm([cam["to"][k] - cam["from"][k] for k in range(3)])
        self.r = norm(cross(self.f, [0, -1, 0]))
        self.d = cross(self.f, self.r)
        self.screen = cam["screen"] * W / 320.0

    def project(self, p):
        v = [p[k] - self.eye[k] for k in range(3)]
        z = sum(v[k] * self.f[k] for k in range(3))
        if z < 50:
            return None
        return (W / 2 + self.screen * sum(v[k] * self.r[k] for k in range(3)) / z,
                H / 2 + self.screen * sum(v[k] * self.d[k] for k in range(3)) / z, z)


# ---------------------------------------------------------------- raster
def raster(color, zbuf, pts, uvs, page):
    pal, idx = page
    (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = pts
    area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
    if abs(area) < 1e-9:
        return
    minx, maxx = max(int(min(x0, x1, x2)), 0), min(int(max(x0, x1, x2)) + 1, W - 1)
    miny, maxy = max(int(min(y0, y1, y2)), 0), min(int(max(y0, y1, y2)) + 1, H - 1)
    for py in range(miny, maxy + 1):
        for px in range(minx, maxx + 1):
            fx, fy = px + 0.5, py + 0.5
            w0 = ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) / area
            w1 = ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) / area
            w2 = 1 - w0 - w1
            if w0 < 0 or w1 < 0 or w2 < 0:
                continue
            z = w0 * z0 + w1 * z1 + w2 * z2
            i = py * W + px
            if z >= zbuf[i]:
                continue
            zbuf[i] = z
            s = int(w0 * uvs[0][0] + w1 * uvs[1][0] + w2 * uvs[2][0]) & 127
            t = min(max(int(w0 * uvs[0][1] + w1 * uvs[1][1] + w2 * uvs[2][1]), 0), 255)
            c = pal[idx[t * 128 + s]]
            color[i * 3:i * 3 + 3] = bytes(((c & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31,
                                            ((c >> 10) & 31) * 255 // 31))


def main():
    ds, room_name, bg_name, model_name, out = sys.argv[1:6]
    anim = int(sys.argv[6]) if len(sys.argv) > 6 else 10
    step = int(sys.argv[7]) if len(sys.argv) > 7 else 0
    room = load_room(f"{ds}/room/{room_name}.bin")
    cam_id = int(sys.argv[8]) if len(sys.argv) > 8 else room["start_cam"]
    model = load_model(f"{ds}/model/{model_name}.bin")
    pages = load_texture(f"{ds}/model/{model_name}_tex.bin", model["npages"])

    bg = struct.unpack(f"<{W * H}H", open(f"{ds}/bg/{bg_name}_cam{cam_id:02d}.bin", "rb").read())
    color = bytearray()
    for c in bg:
        color += bytes(((c & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31, ((c >> 10) & 31) * 255 // 31))
    zbuf = [1e30] * (W * H)

    cam = Camera(room["cams"][cam_id])
    world = mat_mul(T(*room["spawn"]), Ry(room["dir"] * DS_ANGLE))
    frame = model["frames"][model["anims"][anim][step]]
    mats = pose(model, frame, world)
    drawn = 0
    for (bone, page), words in model["lists"].items():
        for poly in decode_gx(words):
            proj = [cam.project(apply(mats[bone], v[:3])) for v in poly]
            if any(p is None for p in proj):
                continue
            uvs = [v[3:] for v in poly]
            tris = [(0, 1, 2)] if len(poly) == 3 else [(0, 1, 2), (0, 2, 3)]
            for a, b, c in tris:
                raster(color, zbuf, [proj[a], proj[b], proj[c]], [uvs[a], uvs[b], uvs[c]], pages[page])
                drawn += 1
    masked = draw_masks(f"{ds}/mask/{bg_name}_cam{cam_id:02d}.bin", cam, color, zbuf)
    write_png(out, W, H, color)
    print(f"{out}: camera {cam_id}, spawn {room['spawn']}, {drawn} triangles drawn, "
          f"{masked} mask pixels in front of the model")


def draw_masks(path, cam, color, zbuf):
    """Draw mask quads with a depth test, like the engine will on the 3D layer."""
    if not os.path.exists(path):
        return 0
    d = open(path, "rb").read()
    magic, count, tex_h, _, _ = struct.unpack_from("<IHHHH", d, 0)
    assert magic == 0x314B4D52
    pal = struct.unpack_from("<256H", d, 12)
    pix = d[12 + 512:12 + 512 + 256 * tex_h]
    pos = 12 + 512 + 256 * tex_h
    hidden = 0
    for _ in range(count):
        u, v, w, h, _depth, *c = struct.unpack_from("<BBHHH12i", d, pos)
        pos += 56
        tl = cam.project(c[0:3])
        br = cam.project(c[6:9])
        if tl is None or br is None:
            continue
        z = tl[2]
        x0, y0, x1, y1 = tl[0], tl[1], br[0], br[1]
        for py in range(max(int(y0), 0), min(int(math.ceil(y1)), H)):
            for px in range(max(int(x0), 0), min(int(math.ceil(x1)), W)):
                tu = u + int((px + 0.5 - x0) / (x1 - x0) * w)
                tv = v + int((py + 0.5 - y0) / (y1 - y0) * h)
                idx = pix[min(tv, tex_h - 1) * 256 + min(tu, 255)]
                i = py * W + px
                if idx == 0 or zbuf[i] <= z:
                    continue
                if zbuf[i] < 1e29:
                    hidden += 1
                zbuf[i] = z
                c16 = pal[idx]
                color[i * 3:i * 3 + 3] = bytes(((c16 & 31) * 255 // 31, ((c16 >> 5) & 31) * 255 // 31,
                                                ((c16 >> 10) & 31) * 255 // 31))
    return hidden


if __name__ == "__main__":
    main()
