"""Check the engine's collision rules against the room data: from every door
arrival point in a room, flood-fill the floor on a grid and report doors that
cannot be reached. Rule variants can be compared with the options.

Floors, stairs, climbing and jumping down are modelled (--no-moves: without
climbing and jumping).

Usage: python3 -I reachability.py build/room [--radius N] [--all-flags] [--no-moves] [--verbose]
"""
import glob
import json
import os
import sys
from collections import deque

STEP = 100


def load_rooms(d):
    rooms = {}
    for p in glob.glob(os.path.join(d, "ROOM*.json")):
        rooms[os.path.basename(p)[4:8].upper()] = json.load(open(p))
    return rooms


def in_quad(x, z, q):
    s = 0
    for k in range(4):
        x0, z0 = q[k]
        x1, z1 = q[(k + 1) % 4]
        c = (x1 - x0) * (z - z0) - (z1 - z0) * (x - x0)
        if c:
            if not s:
                s = 1 if c > 0 else -1
            elif (c > 0) != (s > 0):
                return False
    return True


def blocked_fn(room, radius, all_flags, floor=0):
    boxes = [b for b in room["collision"]["boxes"]
             if floor in b["floors"] and b["shape"] not in (11, 12) and (all_flags or b["flag"] & 0x400)]

    def blocked(x, z):
        for b in boxes:
            if b["x"] - radius < x < b["x"] + b["w"] + radius and b["z"] - radius < z < b["z"] + b["d"] + radius:
                return True
        return False
    return blocked


def inside(b, x, z, margin=0):
    return b["x"] - margin <= x <= b["x"] + b["w"] + margin and b["z"] - margin <= z <= b["z"] + b["d"] + margin


def reachable3d(room, start, radius, all_flags, limit, use_moves=True, start_floor=0):
    """Flood fill over (x, z, floor): walking, stairs/slopes (floor changes
    within the slope box's floors), climbing (shape 9: next to the box ->
    on top, floor + 1) and jumping down (shape 10: floor - 1, 1000 ahead)."""
    boxes = room["collision"]["boxes"]
    blocked = {}

    def blk(f, x, z):
        if f not in blocked:
            blocked[f] = blocked_fn(room, radius, all_flags, f)
        return blocked[f](x, z)
    sx, sz = (start[0] // STEP) * STEP, (start[1] // STEP) * STEP
    seen = {(sx, sz, start_floor)}
    q = deque(seen)
    while q and len(seen) < 300000:
        x, z, f = q.popleft()
        nxt = [(x + STEP, z, f), (x - STEP, z, f), (x, z + STEP, f), (x, z - STEP, f)]
        for b in boxes:
            if b["shape"] in (11, 12) and inside(b, x, z):
                nxt += [(x, z, g) for g in b["floors"] if g != f]
            if use_moves and b["shape"] == 9 and f in b["floors"] and inside(b, x, z, 800) and not inside(b, x, z):
                cx, cz = b["x"] + b["w"] // 2, b["z"] + b["d"] // 2
                nxt.append(((cx // STEP) * STEP, (cz // STEP) * STEP, f + 1))
            if use_moves and b["shape"] == 10 and f in b["floors"] and inside(b, x, z, 800):
                for dx, dz in ((1200, 0), (-1200, 0), (0, 1200), (0, -1200)):
                    nxt.append((((x + dx) // STEP) * STEP, ((z + dz) // STEP) * STEP, f - 1))
        for n in nxt:
            if n in seen or abs(n[0]) > limit or abs(n[1]) > limit or n[2] < 0:
                continue
            if blk(n[2], n[0], n[1]) and not blk(f, x, z) and n[2] == f:
                continue
            if n[2] != f and blk(n[2], n[0], n[1]):
                continue
            seen.add(n)
            q.append(n)
    return seen


def reachable(room, start, blocked, limit):
    sx, sz = (start[0] // STEP) * STEP, (start[1] // STEP) * STEP
    seen = {(sx, sz)}
    q = deque([(sx, sz)])
    while q and len(seen) < 200000:
        x, z = q.popleft()
        for dx, dz in ((STEP, 0), (-STEP, 0), (0, STEP), (0, -STEP)):
            n = (x + dx, z + dz)
            if n in seen or abs(n[0]) > limit or abs(n[1]) > limit:
                continue
            # like the engine: may step if not deeper into an obstacle; on the
            # grid this means: the target must be free unless the start is not
            if blocked(*n) and not blocked(x, z):
                continue
            seen.add(n)
            q.append(n)
    return seen


def main():
    rooms = load_rooms(sys.argv[1])
    radius = int(sys.argv[sys.argv.index("--radius") + 1]) if "--radius" in sys.argv else 400
    all_flags = "--all-flags" in sys.argv
    verbose = "--verbose" in sys.argv
    # arrival points: every door of every room that leads into this room
    arrivals = {k: [] for k in rooms}
    for name, r in rooms.items():
        for d in r["doors"]:
            target = f"{d['next_stage']}{d['next_room']:02X}0"
            if target in arrivals:
                arrivals[target].append((d["next_pos"][0], d["next_pos"][2], name, d["next_floor"]))
    bad_rooms = 0
    bad_doors = total = 0
    for name in sorted(rooms):
        r = rooms[name]
        blocked = blocked_fn(r, radius, all_flags)
        doors = [d for d in r["doors"] if d["sat"] & 1]
        if not arrivals[name] or not doors:
            continue
        x, z, src, start_floor = arrivals[name][0]
        area3 = reachable3d(r, (x, z), radius, all_flags, 40000, "--no-moves" not in sys.argv, start_floor)
        missing = []
        for d in doors:
            total += 1
            # reachable if a free cell lies in the door quad or within 700 of it
            q = d["quad_xz"]
            area = {(ax, az) for ax, az, af in area3 if af == d["floor"]}
            ok = any(in_quad(cx, cz, q) for cx, cz in area)
            if not ok:
                cxm = sum(p[0] for p in q) / 4
                czm = sum(p[1] for p in q) / 4
                ok = any(abs(cx - cxm) < 700 + max(abs(p[0] - cxm) for p in q) and
                         abs(cz - czm) < 700 + max(abs(p[1] - czm) for p in q) for cx, cz in area)
            if not ok:
                missing.append(f"{d['next_stage']}{d['next_room']:02X}")
        if missing:
            bad_rooms += 1
            bad_doors += len(missing)
            if verbose:
                print(f"ROOM {name[:3]}: from door of {src[:3]} at ({x}, {z}) cannot reach doors to {missing} "
                      f"({len(area)} cells)")
    print(f"radius {radius}, {'all boxes' if all_flags else 'player boxes'}: "
          f"{bad_doors} of {total} doors unreachable in {bad_rooms} rooms")


if __name__ == "__main__":
    main()
