"""Export the room lights (RDT section 9, "LIT") of every room.

Per camera 40 bytes, as the PS1 keeps them in memory (the scripts' light
commands 0x6a / 0x6b / 0x7c-0x7f change this table, FUN_80072454 reads it):

    +0   u8  gain: the light matrix is -gain * (unit vector light -> object)
    +1   u8  type per light (3): 0 a point light at pos, 1 a direction (pos)
    +4   u8  colour[3][3] (RGB per light)
    +13  u8  ambient[3] (the back colour)
    +16  s16 pos[3][3]
    +34  u16 range[3]: a point light fades linearly to 0 over this many
             units (distance on the floor plane); 0 = off

Output: <out_dir>/room<stage><room>0_lit.bin, the cameras' 40-byte records.

Usage: python3 -I lit2ds.py <disc>/PL0/RDT <out_dir>
"""
import os
import re
import struct
import sys

LIGHTS_SECTION = 9    # index in the RDT's offset table (rdt2json.OFFSET_NAMES)
RECORD = 40


def main():
    rdt_dir, out_dir = sys.argv[1:3]
    os.makedirs(out_dir, exist_ok=True)
    count = 0
    for name in sorted(os.listdir(rdt_dir)):
        m = re.fullmatch(r"ROOM(\d)([0-9A-F]{2})0\.RDT", name)
        if not m:
            continue
        d = open(os.path.join(rdt_dir, name), "rb").read()
        cameras = d[1]
        off = struct.unpack_from("<23I", d, 8)[LIGHTS_SECTION]
        if not off or off + cameras * RECORD > len(d):
            continue
        out = os.path.join(out_dir, f"room{m.group(1)}{m.group(2).lower()}0_lit.bin")
        with open(out, "wb") as f:
            f.write(d[off:off + cameras * RECORD])
        count += 1
    print(f"{count} rooms: *_lit.bin")


if __name__ == "__main__":
    main()
