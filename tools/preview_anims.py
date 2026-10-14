"""Contact sheet of a model's animations: one row per animation, N evenly spaced
keyframes in side view. Helps to identify walk/run/idle etc.

Usage: python3 -I preview_anims.py model.json texture.png out.png [frames_per_row]
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import preview_model as pm  # noqa: E402
from imgio import write_png  # noqa: E402

pm.VIEW_W, pm.VIEW_H = 110, 180


def main():
    model = json.load(open(sys.argv[1]))
    tex = pm.read_png_rgb(sys.argv[2])
    cols = int(sys.argv[4]) if len(sys.argv) > 4 else 6
    rows = len(model["animations"])
    w, h = pm.VIEW_W, pm.VIEW_H
    out = bytearray(w * cols * h * rows * 3)
    scale = (h - 24) / 3100.0
    for r, anim in enumerate(model["animations"]):
        for c in range(cols):
            frame_id = anim[c * len(anim) // cols]
            tris = list(pm.posed_faces(model, model["skeleton"]["frames"][frame_id]))
            view = pm.render_view(tris, 0.0, tex, scale, (0, 0))
            for y in range(h):
                o = ((r * h + y) * w * cols + c * w) * 3
                out[o:o + w * 3] = view[y * w * 3:(y + 1) * w * 3]
        print(f"anim {r}: {len(anim)} keyframes")
    write_png(sys.argv[3], w * cols, h * rows, out)


if __name__ == "__main__":
    main()
