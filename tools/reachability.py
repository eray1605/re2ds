"""Check the engine's collision rules against the room data: from every door
arrival point in a room, flood-fill the floor on a grid and report doors that
cannot be reached. Rule variants can be compared with the options.

Usage: python3 -I reachability.py build/room [--radius N] [--all-flags] [--verbose]
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


def blocked_fn(room, radius, all_flags):
    boxes = [b for b in room["collision"]["boxes"]
             if 0 in b["floors"] and b["shape"] not in (11, 12) and (all_flags or b["flag"] & 0x400)]

    def blocked(x, z):
        for b in boxes:
            if b["x"] - radius < x < b["x"] + b["w"] + radius and b["z"] - radius < z < b["z"] + b["d"] + radius:
                return True
        return False
    return blocked


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
            if target in arrivals and d["next_floor"] == 0:
                arrivals[target].append((d["next_pos"][0], d["next_pos"][2], name))
    bad_rooms = 0
    bad_doors = total = 0
    for name in sorted(rooms):
        r = rooms[name]
        blocked = blocked_fn(r, radius, all_flags)
        doors = [d for d in r["doors"] if d["floor"] == 0 and d["sat"] & 1]
        if not arrivals[name] or not doors:
            continue
        x, z, src = arrivals[name][0]
        area = reachable(r, (x, z), blocked, 40000)
        missing = []
        for d in doors:
            total += 1
            # reachable if a free cell lies in the door quad or within 700 of it
            q = d["quad_xz"]
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
