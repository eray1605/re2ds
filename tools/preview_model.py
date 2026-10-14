"""Software-render a converted RE2 model (from pld2json.py) as a PNG preview.

Draws front, side and back views of one animation keyframe with a z-buffer,
nearest-neighbour texturing and simple directional shading.

Usage:
  python3 -I preview_model.py model.json texture.png out.png [anim] [frame_in_anim]
  (anim/frame default to 0/0; anim -1 renders the bind pose without rotations)
"""
import json
import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imgio import write_png  # noqa: E402

VIEW_W, VIEW_H = 220, 360


def read_png_rgb(path):
    """Minimal PNG reader for the 8-bit RGB, non-interlaced files imgio writes."""
    data = open(path, "rb").read()
    pos, idat = 8, b""
    while pos < len(data):
        length, tag = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + length]
        if tag == b"IHDR":
            width, height = struct.unpack_from(">II", body)
        elif tag == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 3
    rgb = bytearray()
    for y in range(height):
        assert raw[y * (stride + 1)] == 0, "only filter type 0 supported"
        rgb += raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)]
    return width, height, rgb


# --- matrices (row-major 4x4 as lists) ---------------------------------------
def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def translate(x, y, z):
    return [[1, 0, 0, x], [0, 1, 0, y], [0, 0, 1, z], [0, 0, 0, 1]]


def rot_x(a):
    c, s = math.cos(a), math.sin(a)
    return [[1, 0, 0, 0], [0, c, -s, 0], [0, s, c, 0], [0, 0, 0, 1]]


def rot_y(a):
    c, s = math.cos(a), math.sin(a)
    return [[c, 0, s, 0], [0, 1, 0, 0], [-s, 0, c, 0], [0, 0, 0, 1]]


def rot_z(a):
    c, s = math.cos(a), math.sin(a)
    return [[c, -s, 0, 0], [s, c, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]


def apply(m, v):
    return [m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2] + m[0][3],
            m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2] + m[1][3],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2] + m[2][3]]


def bone_transforms(skel, frame):
    """Parent-relative translate, then rotate X*Y*Z (as OpenBiohazard2 does)."""
    n = skel["bones"]
    out = [None] * n
    to_rad = 2 * math.pi / 4096

    def visit(bone, parent):
        m = out[parent] if parent >= 0 else [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]
        m = mat_mul(m, translate(*skel["rel_pos"][bone]))
        if frame is not None:
            rx, ry, rz = frame["rot"][bone]
            m = mat_mul(m, mat_mul(rot_x(rx * to_rad), mat_mul(rot_y(ry * to_rad), rot_z(rz * to_rad))))
        out[bone] = m
        for child in skel["children"][bone]:
            visit(child, bone)

    visit(0, -1)
    return out


def posed_faces(model, frame):
    """Yield (3 world-space points, 3 uv pairs) per triangle."""
    page_w = model["texture"]["page_width"]
    mats = bone_transforms(model["skeleton"], frame)
    for bone, mesh in enumerate(model["meshes"]):
        m = mats[bone]
        tv = [apply(m, v) for v in mesh["tri_vertices"]]
        qv = [apply(m, v) for v in mesh["quad_vertices"]]
        for face in mesh["faces"]:
            verts = tv if face["src"] == "tri" else qv
            pts = [verts[i] for i in face["v"]]
            uvs = [[u + face["page"] * page_w, v] for u, v in face["uv"]]
            if len(pts) == 3:
                yield pts, uvs
            else:
                # PS1 quad vertex order is 0-1-2-3 as a strip: (0,1,2) + (1,3,2)
                yield [pts[0], pts[1], pts[2]], [uvs[0], uvs[1], uvs[2]]
                yield [pts[1], pts[3], pts[2]], [uvs[1], uvs[3], uvs[2]]


def render_view(tris, yaw, tex, scale, center):
    tw, th, trgb = tex
    color = bytearray([40, 40, 48] * VIEW_W * VIEW_H)
    zbuf = [1e30] * (VIEW_W * VIEW_H)
    cy, sy = math.cos(yaw), math.sin(yaw)
    light = (0.35, -0.6, -0.7)
    for pts, uvs in tris:
        # rotate around Y, orthographic: x -> screen x, y -> screen y (RE2 y points down)
        p = [(x * cy + z * sy, y, -x * sy + z * cy) for x, y, z in pts]
        s = [(VIEW_W / 2 + (q[0] - center[0]) * scale, VIEW_H - 12 + (q[1] - center[1]) * scale, q[2]) for q in p]
        # flat shading from the face normal
        ax, ay, az = (p[1][i] - p[0][i] for i in range(3))
        bx, by, bz = (p[2][i] - p[0][i] for i in range(3))
        nx, ny, nz = ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx
        ln = math.sqrt(nx * nx + ny * ny + nz * nz) or 1
        shade = 0.55 + 0.45 * abs((nx * light[0] + ny * light[1] + nz * light[2]) / ln)
        minx, maxx = max(int(min(q[0] for q in s)), 0), min(int(max(q[0] for q in s)) + 1, VIEW_W - 1)
        miny, maxy = max(int(min(q[1] for q in s)), 0), min(int(max(q[1] for q in s)) + 1, VIEW_H - 1)
        (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = s
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-6:
            continue
        for py in range(miny, maxy + 1):
            for px in range(minx, maxx + 1):
                fx, fy = px + 0.5, py + 0.5
                w0 = ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) / area
                w1 = ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) / area
                w2 = 1 - w0 - w1
                if w0 < 0 or w1 < 0 or w2 < 0:
                    continue
                z = w0 * z0 + w1 * z1 + w2 * z2
                i = py * VIEW_W + px
                if z >= zbuf[i]:
                    continue
                zbuf[i] = z
                u = int(w0 * uvs[0][0] + w1 * uvs[1][0] + w2 * uvs[2][0])
                v = int(w0 * uvs[0][1] + w1 * uvs[1][1] + w2 * uvs[2][1])
                t = (min(max(v, 0), th - 1) * tw + min(max(u, 0), tw - 1)) * 3
                color[i * 3:i * 3 + 3] = bytes(min(255, int(c * shade)) for c in trgb[t:t + 3])
    return color


def main():
    model = json.load(open(sys.argv[1]))
    tex = read_png_rgb(sys.argv[2])
    anim = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    step = int(sys.argv[5]) if len(sys.argv) > 5 else 0
    frame = None
    if anim >= 0:
        frame = model["skeleton"]["frames"][model["animations"][anim][step]]
    tris = list(posed_faces(model, frame))
    ys = [q[1] for pts, _ in tris for q in pts]
    xs = [q[0] for pts, _ in tris for q in pts]
    zs = [q[2] for pts, _ in tris for q in pts]
    height = max(ys) - min(ys)
    scale = (VIEW_H - 24) / height
    center = ((max(xs) + min(xs)) / 2 * 0 + 0, max(ys))  # feet at the bottom, x centred on origin

    # side (facing left), front, other side; Leon's model faces -X at yaw 0
    views = [render_view(tris, yaw, tex, scale, center) for yaw in (0, math.pi / 2, math.pi)]
    out = bytearray(VIEW_W * 3 * VIEW_H * 3)
    for y in range(VIEW_H):
        for k, view in enumerate(views):
            o = (y * VIEW_W * 3 + k * VIEW_W) * 3
            out[o:o + VIEW_W * 3] = view[y * VIEW_W * 3:(y + 1) * VIEW_W * 3]
    write_png(sys.argv[3], VIEW_W * 3, VIEW_H, out)
    print(f"{sys.argv[3]}: {len(tris)} triangles, height {height:.0f} units, "
          f"x {min(xs):.0f}..{max(xs):.0f}, z {min(zs):.0f}..{max(zs):.0f}")


if __name__ == "__main__":
    main()
