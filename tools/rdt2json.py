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

# SCD instruction sizes in bytes (opcode included), from OpenBiohazard2
# fileio/rdt_scd.go. Used to walk the room scripts linearly.
SCD_SIZES = {
    0: 1, 1: 1, 2: 1, 3: 4, 4: 4, 5: 2, 6: 4, 7: 4, 8: 1, 9: 4, 10: 3, 11: 1, 12: 1,
    13: 6, 14: 2, 15: 4, 16: 2, 17: 4, 18: 2, 19: 4, 20: 6, 21: 2, 22: 2, 23: 6, 24: 2,
    25: 2, 26: 2, 29: 4, 32: 1, 33: 4, 34: 4, 35: 6, 36: 4, 37: 3, 38: 6, 39: 4, 40: 1,
    41: 2, 42: 1, 43: 6, 44: 20, 45: 38, 46: 3, 47: 4, 48: 1, 49: 1, 50: 8, 51: 8, 52: 4,
    53: 3, 54: 12, 55: 4, 57: 8, 58: 16, 59: 32, 60: 2, 61: 3, 62: 6, 63: 4, 64: 8, 65: 10,
    66: 1, 67: 4, 68: 22, 70: 10, 71: 2, 72: 16, 75: 3, 76: 5, 77: 22, 78: 22, 80: 4,
    81: 6, 82: 6, 83: 6, 84: 22, 87: 8, 88: 4, 89: 4, 90: 2, 91: 2, 92: 3, 93: 2, 94: 2,
    95: 2, 96: 14, 97: 4, 98: 2, 99: 1, 100: 16, 101: 2, 102: 1, 103: 28, 104: 40,
    105: 30, 106: 6, 107: 4, 108: 1, 109: 4, 110: 6, 111: 2, 122: 16, 123: 16,
    # from the PS1 handlers (jump table 0x8009f560), see source/script.c
    28: 1, 56: 3, 69: 5, 73: 8, 74: 2, 79: 4, 85: 6, 86: 4, 112: 1, 113: 1, 114: 16, 115: 8,
    116: 4, 117: 22, 118: 3, 119: 4, 120: 6, 121: 1, 124: 6, 125: 6, 126: 6, 127: 6, 128: 2,
    129: 3, 130: 3, 131: 1, 132: 2, 133: 6, 134: 1, 135: 1, 136: 3, 137: 1,
}
OP_EVT_END, OP_DOOR_AOT_SET, OP_DOOR_AOT_SET_4P = 1, 59, 104
DOOR_TAIL = "<4h3B7B"   # next x/y/z/dir, stage, room, camera, next floor, texture,
                        # door type, knock type, key id, key type, free


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


# Block opcodes with a u16 length at +2: if, for, while, do, switch (the block
# ends that many bytes after the instruction) and else (counted from itself).
BLOCK_AFTER = {6, 13, 15, 17, 19}
OP_ELSE = 7


def block_reach(op, pos, data):
    """End of the block op at pos opens (0 if none), as source/script.c."""
    if op in BLOCK_AFTER:
        return pos + SCD_SIZES[op] + struct.unpack_from("<H", data, pos + 2)[0]
    if op == OP_ELSE:
        return pos + struct.unpack_from("<H", data, pos + 2)[0]
    return 0


def script_instructions(data, offset):
    """Yield (opcode, position) for every instruction of an SCD block.

    The block starts with a table of u16 function offsets (the first one is
    also the table size). A function ends at an EVT_END outside every if /
    loop block - one inside a block only ends it when the condition holds
    (room 207's init: "if flag 1:6, evt_end", then the doors). Unknown
    opcodes stop the walk of that function.
    """
    first = struct.unpack_from("<H", data, offset)[0]
    funcs = [struct.unpack_from("<H", data, offset + i)[0] for i in range(0, first, 2)]
    for start in funcs:
        for op, pos in script_instructions_from(data, offset + start):
            yield op, pos - SCD_SIZES[op]


def script_block(data, offset):
    """Raw bytes of an SCD block: function table plus all functions."""
    first = struct.unpack_from("<H", data, offset)[0]
    end = offset + first
    for start in (struct.unpack_from("<H", data, offset + i)[0] for i in range(0, first, 2)):
        pos = offset + start
        for op, pos in script_instructions_from(data, pos):
            pass
        end = max(end, pos)
    return data[offset:end]


def script_instructions_from(data, pos):
    """Walk one function from pos; yields (opcode, position after it)."""
    reach = 0
    while pos < len(data) and data[pos] in SCD_SIZES:
        op = data[pos]
        reach = max(reach, block_reach(op, pos, data))
        pos += SCD_SIZES[op]
        yield op, pos
        if op == OP_EVT_END and pos >= reach:
            break


# Message text (RDT lang1). Characters below 0x60 index this table; the DS
# engine gets plain ASCII with "\n" for line breaks.
MSG_CHARS = (" .___()_____0123456789:_,\"!?_ABCDEFGHIJKLMNOPQRSTUVWXYZ[/]'-_abc"
             "defghijklmnopqrstuvwxyz_________")
MSG_NEWLINE, MSG_END = 0xFC, 0xFE
MSG_ARG_CODES = set(range(0xEA, 0xFC)) - {0xF3} | {0xFD}   # control codes with one argument


def read_messages(data, offset):
    """Decoded lang1 messages, or [] if the table there is not a message table
    (some rooms point the offset at unrelated data)."""
    if not offset:
        return []
    first = struct.unpack_from("<H", data, offset)[0]
    if first == 0 or first & 1 or first > 2048:
        return []
    offs = [struct.unpack_from("<H", data, offset + i)[0] for i in range(0, first, 2)]
    if offs != sorted(offs) or offs[0] != first:
        return []
    out = []
    for o in offs:
        pos, text = offset + o, ""
        while pos < len(data) and data[pos] != MSG_END and len(text) < 512:
            c = data[pos]
            pos += 1
            if c < 0x60:
                text += MSG_CHARS[c]
            elif c == MSG_NEWLINE:
                text += "\n"
            elif c in MSG_ARG_CODES:
                pos += 1
        if pos >= len(data) or data[pos] != MSG_END:
            return []
        out.append(text.strip("\n").replace("_", ""))
    return out


def read_floor_sounds(data, offset):
    """FLR: u16 count, then {x, z, w, d, sound, floor} (FUN_8004f078: floor
    & 0x80 = disabled, & 0x7F = floor level; the last matching area wins,
    default sound 29)."""
    if not offset:
        return []
    n = struct.unpack_from("<H", data, offset)[0]
    out = []
    for i in range(n):
        x, z, w, d, sound, floor = struct.unpack_from("<hhHHHH", data, offset + 2 + 12 * i)
        out.append({"x": x, "z": z, "w": w, "d": d, "sound": sound & 0xFF, "floor": floor})
    return out


def read_doors(data, offsets):
    """Door AOTs (DOOR_AOT_SET / DOOR_AOT_SET_4P) from the init and main scripts.

    A door set several times (e.g. in both branches of an IF) keeps its
    first definition; flags and locks are the script interpreter's job later.
    """
    doors = {}
    for key in ("init_script", "exec_script"):
        if not offsets.get(key):
            continue
        for op, pos in script_instructions(data, offsets[key]):
            if op == OP_DOOR_AOT_SET:
                aot, sce, sat, floor, sup, x, z, w, d = struct.unpack_from("<5B4h", data, pos + 1)
                quad = [[x, z], [x + w, z], [x + w, z + d], [x, z + d]]
                tail = struct.unpack_from(DOOR_TAIL, data, pos + 14)
            elif op == OP_DOOR_AOT_SET_4P:
                aot, sce, sat, floor, sup = struct.unpack_from("<5B", data, pos + 1)
                pts = struct.unpack_from("<8h", data, pos + 6)
                quad = [[pts[2 * k], pts[2 * k + 1]] for k in range(4)]
                tail = struct.unpack_from(DOOR_TAIL, data, pos + 22)
            else:
                continue
            if aot in doors:
                continue
            nx, ny, nz, ndir, stage, room, cam, nfloor, tex, dtype, knock, key_id, key_type, _ = tail
            doors[aot] = {
                "aot": aot, "sce": sce, "sat": sat, "floor": floor, "quad_xz": quad,
                "next_pos": [nx, ny, nz], "next_dir": ndir, "next_stage": stage + 1,
                "next_room": room, "next_camera": cam, "next_floor": nfloor,
                "door_type": dtype, "knock": knock, "key_id": key_id, "key_type": key_type,
            }
    return [doors[k] for k in sorted(doors)]


def main():
    data = open(sys.argv[1], "rb").read()
    header, offsets = read_header(data)
    room = {
        "header": header,
        "offsets": {k: v for k, v in offsets.items() if v},
        "cameras": read_cameras(data, offsets["camera_position"], header["cameras"]),
        "camera_switches": read_camera_switches(data, offsets["camera_switches"]),
        "collision": read_collision(data, offsets["collision"]),
        "doors": read_doors(data, offsets),
        "messages": read_messages(data, offsets.get("lang1")),
        "floor_sounds": read_floor_sounds(data, offsets.get("floor_sound")),
        "init_script_hex": script_block(data, offsets["init_script"]).hex() if offsets.get("init_script") else "",
        "exec_script_hex": script_block(data, offsets["exec_script"]).hex() if offsets.get("exec_script") else "",
    }
    for cam in room["cameras"]:
        cam["masks"] = read_masks(data, cam["mask_offset"])
    with open(sys.argv[2], "w") as f:
        json.dump(room, f, indent=1)
    print(f"{sys.argv[1]}: {header['cameras']} cameras, "
          f"{len(room['camera_switches'])} camera switches, "
          f"{len(room['collision']['boxes'])} collision boxes, "
          f"{sum(len(c['masks']) for c in room['cameras'])} mask rectangles, "
          f"{len(room['doors'])} doors, {len(room['messages'])} messages, "
          f"scripts {len(room['init_script_hex']) // 2}+{len(room['exec_script_hex']) // 2} bytes")


if __name__ == "__main__":
    main()
