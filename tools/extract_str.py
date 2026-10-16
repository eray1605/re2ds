"""Copy the movies (.STR) of a raw MODE2/2352 PS1 track as raw 2352-byte
sectors (<out_dir>/<path>.STR.raw2352). Their XA audio sectors are Form 2,
which the 2048-byte files of the ISO file system lose; tools/str2ds.py needs
them whole.

Usage: python3 -I extract_str.py <track1.bin> <out_dir>
"""
import os
import struct
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "re2ds-notes", "tools"))
import extract_psx_iso as X
src, out_dir = sys.argv[1], sys.argv[2]
with open(src, "rb") as f:
    pvd, _ = X.user_data(X.read_sector(f, 16))
    root = pvd[156:190]
    lba, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
    def walk(lba, size, rel):
        for name, e_lba, e_size, is_dir in X.parse_dir(f, lba, size):
            if is_dir:
                walk(e_lba, e_size, rel + name + "/")
            elif name.endswith(".STR"):
                count = (e_size + 2047) // 2048
                f.seek(e_lba * X.SECTOR)
                p = os.path.join(out_dir, rel + name + ".raw2352")
                os.makedirs(os.path.dirname(p), exist_ok=True)
                open(p, "wb").write(f.read(count * X.SECTOR))
                print(p, count)
    walk(lba, size, "")
