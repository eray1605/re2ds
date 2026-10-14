"""Debug view: draw a room's collision boxes (on the floor, y = 0) into each
camera background, using the camera from the RDT. If the outlines line up
with walls and kerbs, the camera math is right and can be reused on the DS.

Usage: python3 -I check_projection.py room.json bg_prefix out_prefix
       (bg_prefix_camNN.ppm in, out_prefix_camNN.ppm out)
"""
import json
import math
import sys

W, H = 320, 240


def sub(a, b):
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def norm(a):
    length = math.sqrt(dot(a, a))
    return [a[0] / length, a[1] / length, a[2] / length]


class Camera:
    def __init__(self, cam):
        self.eye = cam["from"]
        self.forward = norm(sub(cam["to"], cam["from"]))
        up = [0, -1, 0]  # RE2 world: negative y is up
        # Verified against ROOM100 camera 2: collision boxes then frame the
        # newspaper boxes and the lamp post exactly.
        self.right = norm(cross(self.forward, up))
        self.down = cross(self.forward, self.right)
        self.screen = cam["screen_distance"]

    def project(self, p):
        d = sub(p, self.eye)
        z = dot(d, self.forward)
        if z <= 1:
            return None
        return (W / 2 + self.screen * dot(d, self.right) / z,
                H / 2 + self.screen * dot(d, self.down) / z)


def read_ppm(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    return bytearray(parts[3])


def write_ppm(path, pixels):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (W, H))
        f.write(pixels)


def draw_line(pixels, a, b, color):
    steps = int(max(abs(b[0] - a[0]), abs(b[1] - a[1]), 1))
    if steps > 4000:
        return
    for i in range(steps + 1):
        x = int(a[0] + (b[0] - a[0]) * i / steps)
        y = int(a[1] + (b[1] - a[1]) * i / steps)
        if 0 <= x < W and 0 <= y < H:
            o = (y * W + x) * 3
            pixels[o:o + 3] = bytes(color)


def draw_segment_3d(cam, pixels, p0, p1, color, samples=64):
    """Draw a 3D segment by sampling, so partly-behind-camera lines still show."""
    prev = None
    for i in range(samples + 1):
        t = i / samples
        p = [p0[k] + (p1[k] - p0[k]) * t for k in range(3)]
        s = cam.project(p)
        if s and prev:
            draw_line(pixels, prev, s, color)
        prev = s


def main():
    room = json.load(open(sys.argv[1]))
    bg_prefix, out_prefix = sys.argv[2], sys.argv[3]
    for idx, c in enumerate(room["cameras"]):
        cam = Camera(c)
        pixels = read_ppm(f"{bg_prefix}_cam{idx:02d}.ppm")
        for box in room["collision"]["boxes"]:
            x0, z0, x1, z1 = box["x"], box["z"], box["x"] + box["w"], box["z"] + box["d"]
            corners = [[x0, 0, z0], [x1, 0, z0], [x1, 0, z1], [x0, 0, z1]]
            for k in range(4):
                draw_segment_3d(cam, pixels, corners[k], corners[(k + 1) % 4], (255, 0, 255))
        for sw in room["camera_switches"]:
            if sw["cam0"] != idx:
                continue
            q = [[p[0], 0, p[1]] for p in sw["quad_xz"]]
            for k in range(4):
                draw_segment_3d(cam, pixels, q[k], q[(k + 1) % 4], (0, 255, 0))
        write_ppm(f"{out_prefix}_cam{idx:02d}.ppm", pixels)
    print("done")


if __name__ == "__main__":
    main()
