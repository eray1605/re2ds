"""Convert rdt2json output to the DS room format (source/formats.h, RoomHeader).

Also picks a debug spawn point (used when warping into a room without a
door): the free floor point closest to the centre of a camera's view area
(no collision box within SPAWN_MARGIN units), trying the preferred camera
first.

Usage: python3 -I room2ds.py room.json out.bin [spawn_camera]
"""
import json
import math
import struct
import sys

ROOM_MAGIC = 0x314D5252  # "RRM1"
ROOM_VERSION = 3
HEADER_SIZE = 52
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
    zone = next((sw for sw, role in zip(room["camera_switches"], room["_roles"])
                 if role == SWITCH_VIEW_AREA and sw["cam0"] == cam), None)
    if zone is None:
        return None
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
    return None if best is None else (best[1], best[2])


def main():
    room = json.load(open(sys.argv[1]))
    spawn_cam = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    cams, switches, boxes = room["cameras"], room["camera_switches"], room["collision"]["boxes"]
    room["_roles"] = view_area_flags(switches, len(cams))
    spawn = None
    for cam in [spawn_cam] + [c for c in range(len(cams)) if c != spawn_cam]:
        spawn = pick_spawn(room, cam)
        if spawn:
            spawn_cam = cam
            break
    if spawn is None:   # no usable view area: stand at the first door's exit
        spawn_cam, spawn = 0, (cams[0]["to"][0], cams[0]["to"][2])
    sx, sz = spawn
    doors = room["doors"]
    # Face the camera. Models face +X; Ry(a) = [[c,0,s],[0,1,0],[-s,0,c]]
    # (PS1 RotMatrixY) turns +X into (cos a, 0, -sin a).
    eye = cams[spawn_cam]["from"]
    yaw = math.atan2(-(eye[2] - sz), eye[0] - sx)
    spawn_dir = int(round(yaw / (2 * math.pi) * 32768)) & 0x7FFF

    out = bytearray(HEADER_SIZE)   # header written at the end, once the offsets are known
    for c in cams:
        out += struct.pack("<3i3iHH", *c["from"], *c["to"], c["screen_distance"], c["flag"])
    for sw, role in zip(switches, room["_roles"]):
        xs = [p[0] for p in sw["quad_xz"]]
        zs = [p[1] for p in sw["quad_xz"]]
        out += struct.pack("<4B4h4h", sw["cam0"], sw["cam1"], sw["floor"], role, *xs, *zs)
    for b in boxes:
        out += struct.pack("<hhHHHHHHI", b["x"], b["z"], b["w"], b["d"], b["shape"], b["flag"],
                           b["type"], 0, sum(1 << f for f in b["floors"]))
    for d in doors:
        xs = [p[0] for p in d["quad_xz"]]
        zs = [p[1] for p in d["quad_xz"]]
        # RE2 angles are 4096 per turn, the engine uses 32768
        out += struct.pack("<4h4h3hH4B4B4B", *xs, *zs, *d["next_pos"], (d["next_dir"] * 8) & 0xFFFF,
                           d["next_stage"], d["next_room"], d["next_camera"], d["next_floor"],
                           d["sat"], d["floor"], d["door_type"], d["key_id"],
                           d["key_type"], d["aot"], 0, 0)

    def section(blob):
        while len(out) % 4:
            out.append(0)
        start = len(out)
        out.extend(blob)
        return start, len(blob)

    init = bytes.fromhex(room.get("init_script_hex", ""))
    exe = bytes.fromhex(room.get("exec_script_hex", ""))
    off_init, size_init = section(init)
    off_exec, size_exec = section(exe)
    # messages: u16 offsets (relative to the section), then NUL-terminated ASCII
    msgs = room.get("messages", [])
    table = bytearray()
    text = bytearray()
    for m in msgs:
        table += struct.pack("<H", 2 * len(msgs) + len(text))
        text += m.encode("ascii", "replace") + b"\0"
    off_msgs, _ = section(bytes(table + text))
    struct.pack_into("<IHBBHBB3ihH5IHH", out, 0, ROOM_MAGIC, ROOM_VERSION, len(cams), len(switches),
                     len(boxes), spawn_cam, len(doors), sx, 0, sz,
                     spawn_dir if spawn_dir < 16384 else spawn_dir - 32768, 0,
                     off_init, size_init, off_exec, size_exec, off_msgs, len(msgs), 0)
    with open(sys.argv[2], "wb") as f:
        f.write(out)
    views = sum(1 for r in room["_roles"] if r)
    print(f"{sys.argv[2]}: {len(out)} bytes, {len(cams)} cameras, {len(switches)} zones "
          f"({views} view areas), {len(boxes)} boxes, {len(doors)} doors, spawn ({sx}, 0, {sz}) in camera {spawn_cam}")


if __name__ == "__main__":
    main()
