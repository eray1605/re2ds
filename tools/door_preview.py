"""Render frames of a door sequence like the DS does (camera eye 10000,0,0
looking at the origin, screen 290). Part states come from tests/door_host
(DOOR_DUMP=frames). Usage: python3 -I door_preview.py door_dir/doorNN states.jsonl out.png
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ds_preview as P  # noqa: E402
from imgio import write_png  # noqa: E402

RE2 = 2 * math.pi / 4096


def main():
    prefix, states, out = sys.argv[1], sys.argv[2], sys.argv[3]
    model = P.load_model(prefix + "_model.bin")
    page = P.load_texture(prefix + "_tex.bin", 1)[0]
    cam = P.Camera({"from": [10000, 0, 0], "to": [0, 0, 0], "screen": 290})
    frames = [json.loads(l[5:]) for l in open(states) if l.startswith("JSON ")]
    W, H = P.W, P.H
    sheet = bytearray(W * H * 3 * len(frames))
    for n, fr in enumerate(frames):
        color, zbuf = bytearray(W * H * 3), [1e18] * (W * H)
        parts = {p["i"]: p for p in fr["parts"]}

        def world(p, depth=0):
            m = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]
            if p["flags"] & 0x10 and depth < 4 and (p["flags"] & 15) in parts:
                m = world(parts[p["flags"] & 15], depth + 1)
            rx, ry, rz = (a * RE2 for a in p["rot"])
            return P.mat_mul(m, P.mat_mul(P.T(*p["pos"]), P.mat_mul(P.Rx(rx), P.mat_mul(P.Ry(ry), P.Rz(rz)))))
        for p in fr["parts"]:
            words = model["lists"].get((p["part"], 0))
            if not words:
                continue
            m = world(p)
            for poly in P.decode_gx(words):
                pts = [cam.project(P.apply(m, v[:3])) for v in poly]
                if None in pts:
                    continue
                uvs = [v[3:] for v in poly]
                P.raster(color, zbuf, pts[:3], uvs[:3], page)
                if len(pts) == 4:
                    P.raster(color, zbuf, [pts[0], pts[2], pts[3]], [uvs[0], uvs[2], uvs[3]], page)
        sheet[n * W * H * 3:(n + 1) * W * H * 3] = color
    write_png(out, W, H * len(frames), sheet)
    print(out, len(frames), "frames")


if __name__ == "__main__":
    main()
