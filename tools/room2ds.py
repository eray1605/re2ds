"""Convert rdt2json output to the DS room format (source/formats.h, RoomHeader).

Also picks a spawn point for the prototype: the free floor point closest to
the centre of the chosen camera's view area (no collision box within
SPAWN_MARGIN units).

Usage: python3 -I room2ds.py room.json out.bin [spawn_camera]
"""
import json
import math
import struct
import sys

ROOM_MAGIC = 0x314D5252  # "RRM1"
SWITCH_VIEW_AREA = 0x01
SPAWN_MARGIN = 450


def view_area_flags(switches, num_cameras):
    """Per camera, the first zone with cam1 == 0 is its view area (OpenBiohazard2)."""
    roles = [0] * len(switches)
    for cam in range(num_cameras):
        for i, sw in enumerate(switches):
            if sw["cam0"] == cam and sw["cam1"] == 0:
                roles[i] = SWITCH_VIEW_AREA
                break
    return roles


def point_in_quad(x, z, quad):
    """Convex quad, any winding."""
    sign = 0
    for k in range(4):
        x0, z0 = quad[k]
        x1, z1 = quad[(k + 1) % 4]
        c = (x1 - x0) * (z - z0) - (z1 - z0) * (x - x0)
        if c != 0:
            if sign == 0:
                sign = 1 if c > 0 else -1
            elif (c > 0) != (sign > 0):
                return False
    return True


def blocked(x, z, boxes, margin):
    for b in boxes:
        if b["x"] - margin <= x <= b["x"] + b["w"] + margin and b["z"] - margin <= z <= b["z"] + b["d"] + margin:
            return True
    return False


def pick_spawn(room, cam):
    zone = next(sw for sw, role in zip(room["camera_switches"], room["_roles"])
                if role == SWITCH_VIEW_AREA and sw["cam0"] == cam)
    quad = zone["quad_xz"]
    cx = sum(p[0] for p in quad) / 4
    cz = sum(p[1] for p in quad) / 4
    xs, zs = [p[0] for p in quad], [p[1] for p in quad]
    best = None
    for gx in range(int(min(xs)), int(max(xs)), 100):
        for gz in range(int(min(zs)), int(max(zs)), 100):
            if not point_in_quad(gx, gz, quad) or blocked(gx, gz, room["collision"]["boxes"], SPAWN_MARGIN):
                continue
            d = (gx - cx) ** 2 + (gz - cz) ** 2
            if best is None or d < best[0]:
                best = (d, gx, gz)
    if best is None:
        raise SystemExit(f"no free spawn point in the view area of camera {cam}")
    return best[1], best[2]


def main():
    room = json.load(open(sys.argv[1]))
    spawn_cam = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    cams, switches, boxes = room["cameras"], room["camera_switches"], room["collision"]["boxes"]
    room["_roles"] = view_area_flags(switches, len(cams))
    sx, sz = pick_spawn(room, spawn_cam)
    # Face the camera. Models face +X; Ry(a) = [[c,0,s],[0,1,0],[-s,0,c]]
    # (PS1 RotMatrixY) turns +X into (cos a, 0, -sin a).
    eye = cams[spawn_cam]["from"]
    yaw = math.atan2(-(eye[2] - sz), eye[0] - sx)
    spawn_dir = int(round(yaw / (2 * math.pi) * 32768)) & 0x7FFF

    out = bytearray(struct.pack("<IHBBHBB3ihH", ROOM_MAGIC, 1, len(cams), len(switches), len(boxes),
                                spawn_cam, 0, sx, 0, sz, spawn_dir if spawn_dir < 16384 else spawn_dir - 32768, 0))
    for c in cams:
        out += struct.pack("<3i3iHH", *c["from"], *c["to"], c["screen_distance"], c["flag"])
    for sw, role in zip(switches, room["_roles"]):
        xs = [p[0] for p in sw["quad_xz"]]
        zs = [p[1] for p in sw["quad_xz"]]
        out += struct.pack("<4B4h4h", sw["cam0"], sw["cam1"], sw["floor"], role, *xs, *zs)
    for b in boxes:
        out += struct.pack("<hhHHHHHHI", b["x"], b["z"], b["w"], b["d"], b["shape"], b["flag"],
                           b["type"], 0, sum(1 << f for f in b["floors"]))
    with open(sys.argv[2], "wb") as f:
        f.write(out)
    views = sum(1 for r in room["_roles"] if r)
    print(f"{sys.argv[2]}: {len(out)} bytes, {len(cams)} cameras, {len(switches)} zones "
          f"({views} view areas), {len(boxes)} boxes, spawn ({sx}, 0, {sz}) in camera {spawn_cam}")


if __name__ == "__main__":
    main()
