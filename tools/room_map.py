"""Top-down debug map of a room: collision boxes (red = blocks the player,
grey = not), slopes/stairs (blue), doors (green), camera view areas (outline)
and optional marker points.

Usage: python3 -I room_map.py room.json out.png [x,z ...]
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imgio import write_png  # noqa: E402

BOX_PLAYER = 0x0400
W = H = 600


def main():
    room = json.load(open(sys.argv[1]))
    marks = [tuple(int(v) for v in a.split(",")) for a in sys.argv[3:]]
    pts = [p for sw in room["camera_switches"] for p in sw["quad_xz"]]
    pts += [(b["x"], b["z"]) for b in room["collision"]["boxes"]]
    pts += [(b["x"] + b["w"], b["z"] + b["d"]) for b in room["collision"]["boxes"]]
    pts += [p for d in room["doors"] for p in d["quad_xz"]] + marks
    x0, x1 = min(p[0] for p in pts), max(p[0] for p in pts)
    z0, z1 = min(p[1] for p in pts), max(p[1] for p in pts)
    scale = min((W - 20) / (x1 - x0), (H - 20) / (z1 - z0))
    img = bytearray(W * H * 3)

    def px(x, z):
        return int(10 + (x - x0) * scale), int(H - 10 - (z - z0) * scale)

    def put(x, y, c):
        if 0 <= x < W and 0 <= y < H:
            img[(y * W + x) * 3:(y * W + x) * 3 + 3] = bytes(c)

    def rect(x, z, w, d, c, fill):
        ax, ay = px(x, z + d)
        bx, by = px(x + w, z)
        for y in range(ay, by + 1):
            for xx in range(ax, bx + 1):
                if fill or y in (ay, by) or xx in (ax, bx):
                    put(xx, y, c)

    def line(a, b, c):
        (ax, ay), (bx, by) = px(*a), px(*b)
        n = max(abs(bx - ax), abs(by - ay), 1)
        for i in range(n + 1):
            put(ax + (bx - ax) * i // n, ay + (by - ay) * i // n, c)

    for sw in room["camera_switches"]:
        q = sw["quad_xz"]
        for k in range(4):
            line(q[k], q[(k + 1) % 4], (40, 40, 60))
    for b in room["collision"]["boxes"]:
        if b["shape"] in (11, 12):
            col = (60, 90, 255)
        elif b["flag"] & BOX_PLAYER:
            col = (220, 40, 40)
        else:
            col = (110, 110, 110)
        rect(b["x"], b["z"], b["w"], b["d"], col, False)
    for d in room["doors"]:
        q = d["quad_xz"]
        for k in range(4):
            line(q[k], q[(k + 1) % 4], (40, 230, 40))
    for x, z in marks:
        cx, cy = px(x, z)
        for dy in range(-3, 4):
            for dx in range(-3, 4):
                put(cx + dx, cy + dy, (255, 255, 0))
    write_png(sys.argv[2], W, H, img)
    print(f"{sys.argv[2]}: x {x0}..{x1}, z {z0}..{z1}")


if __name__ == "__main__":
    main()
