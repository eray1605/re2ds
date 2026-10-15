"""Copy the room's path zones (RDT section 12, "blocks") for the enemies.

The PS1 keeps them as u16 count, u16 pad, then per zone 12 bytes: s16 x0,
z0, x1, z1 (x1 / z1 = far edges), u16 unknown, u16 neighbour mask (bit n =
touches zone n). Zombies without status bit 0x1000 head for the next shared
edge on the way to Leon's zone (FUN_80046fb0 / FUN_80047308). Written
unchanged; rooms without zones get no file.

Usage: python3 -I blk2ds.py ROOM1020.RDT out.bin
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rdt2json import read_header  # noqa: E402


def main():
    src, out = sys.argv[1], sys.argv[2]
    d = open(src, "rb").read()
    _, offs = read_header(d)
    o = offs.get("blocks")
    count = struct.unpack_from("<H", d, o)[0] if o else 0
    if not count or count > 16:  # the path search keeps 16 zones (u16 masks)
        if os.path.exists(out):
            os.remove(out)
        return
    with open(out, "wb") as f:
        f.write(d[o:o + 4 + 12 * count])
    print(f"{out}: {count} zones")


if __name__ == "__main__":
    main()
