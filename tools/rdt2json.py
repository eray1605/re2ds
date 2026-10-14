"""Extract camera, camera-switch and collision data from an RE2 room (.RDT).

The PS1 RDT layout matches the PC one; field layouts follow OpenBiohazard2's
fileio/rdt*.go readers.

Usage: python3 -I rdt2json.py ROOM1000.RDT out.json
"""
import json
import math
import struct
import sys

OFFSET_NAMES = [
    "room_sound", "room_vab_header", "room_vab_data", "enemy_vab_header",
    "enemy_vab_data", "ota", "collision", "camera_position", "camera_switches",
    "lights", "items", "floor_sound", "blocks", "lang1", "lang2",
    "scroll_texture", "init_script", "exec_script", "sprite_anim",
    "sprite_anim_offset", "sprite_image", "model_image", "rbj",
]

FLOOR_HEIGHT_UNIT = -1800


def read_header(data):
    names = ["sprites", "cameras", "models", "items", "doors", "rooms", "reverb", "sprite_max"]
    header = dict(zip(names, data[:8]))
    offsets = dict(zip(OFFSET_NAMES, struct.unpack_from("<23I", data, 8)))
    return header, offsets


def read_cameras(data, offset, count):
    """RID: per camera u16 flag, u16 dist, from xyz, to xyz (s32), u32 mask offset."""
    cams = []
    for i in range(count):
        flag, dist, fx, fy, fz, tx, ty, tz, mask = struct.unpack_from("<HH6iI", data, offset + i * 32)
        screen = dist >> 7
        fov = math.degrees(2 * math.atan(120.0 / screen)) if screen else 60.0
        cams.append({
            "flag": flag,
            "screen_distance": screen,
            "fov_deg": round(fov, 3),
            "from": [fx, fy, fz],
            "to": [tx, ty, tz],
            "mask_offset": None if mask == 0xFFFFFFFF else mask,
        })
    return cams


def read_masks(data, offset):
    """PRI list for one camera (OpenBiohazard2 fileio/rdt_pri.go).

    Returns rectangles {src:[u,v], dest:[x,y], depth, w, h}; dest is in
    320x240 screen space, depth * 32 = distance along the view direction.
    """
    if offset is None:
        return []
    ngroups, nmasks = struct.unpack_from("<HH", data, offset)
    if ngroups == 0xFFFF:
        return []
    groups = [struct.unpack_from("<HHhh", data, offset + 4 + 8 * i) for i in range(ngroups)]
    pos = offset + 4 + 8 * ngroups
    masks = []
    for count, _, gx, gy in groups:
        for _ in range(count):
            square = struct.unpack_from("<H", data, pos + 6)[0]
            if square == 0:
                sx, sy, dx, dy, depth, _, w, h = struct.unpack_from("<BBBBHHHH", data, pos)
                pos += 12
            else:
                sx, sy, dx, dy, depth, w = struct.unpack_from("<BBBBHH", data, pos)
                h = w
                pos += 8
            masks.append({"src": [sx, sy], "dest": [dx + gx, dy + gy], "depth": depth, "w": w, "h": h})
    assert len(masks) == nmasks, (len(masks), nmasks)
    return masks


def read_camera_switches(data, offset):
    """RVD: 20-byte records until flag/floor/cam0/cam1 are all 0xFF."""
    out = []
    for i in range(100):
        rec = struct.unpack_from("<4B8h", data, offset + i * 20)
        if rec[:4] == (0xFF, 0xFF, 0xFF, 0xFF):
            break
        flag, floor, cam0, cam1 = rec[:4]
        pts = [[rec[4 + 2 * k], rec[5 + 2 * k]] for k in range(4)]
        out.append({"flag": flag, "floor": floor, "cam0": cam0, "cam1": cam1, "quad_xz": pts})
    return out


def read_collision(data, offset):
    """SCA: header then (count - 1) 16-byte boxes."""
    cx, cz, count, cy, cw, cd = struct.unpack_from("<hhIiHH", data, offset)
    boxes = []
    pos = offset + 16
    for i in range(count - 1):
        x, z, w, d, flag, typ, floors = struct.unpack_from("<hhHHHHI", data, pos)
        pos += 16
        shape = flag & 0xF
        boxes.append({
            "x": x, "z": z, "w": w, "d": d,
            "shape": shape,
            "flag": flag,
            "type": typ,
            "slope_height": ((typ >> 6) & 0x1F) * FLOOR_HEIGHT_UNIT,
            "slope_dir": (typ >> 4) & 3 if shape in (11, 12) else None,
            "floors": [b for b in range(32) if floors >> b & 1],
        })
    return {"ceiling": {"x": cx, "z": cz, "y": cy, "w": cw, "d": cd}, "boxes": boxes}


def main():
    data = open(sys.argv[1], "rb").read()
    header, offsets = read_header(data)
    room = {
        "header": header,
        "offsets": {k: v for k, v in offsets.items() if v},
        "cameras": read_cameras(data, offsets["camera_position"], header["cameras"]),
        "camera_switches": read_camera_switches(data, offsets["camera_switches"]),
        "collision": read_collision(data, offsets["collision"]),
    }
    for cam in room["cameras"]:
        cam["masks"] = read_masks(data, cam["mask_offset"])
    with open(sys.argv[2], "w") as f:
        json.dump(room, f, indent=1)
    print(f"{sys.argv[1]}: {header['cameras']} cameras, "
          f"{len(room['camera_switches'])} camera switches, "
          f"{len(room['collision']['boxes'])} collision boxes, "
          f"{sum(len(c['masks']) for c in room['cameras'])} mask rectangles")


if __name__ == "__main__":
    main()
