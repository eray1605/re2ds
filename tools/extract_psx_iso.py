"""Extract the ISO9660 file system from a raw MODE2/2352 PS1 .bin track.

Form 1 sectors (normal data) are written as plain files.
Files containing Form 2 sectors (XA audio, the audio of STR movies) are
written as raw 2352-byte sectors with a .raw2352 suffix so nothing is lost
(tools/xa2ds.py and tools/str2ds.py read those).

Usage: python3 -I extract_psx_iso.py <track1.bin> <out_dir>
"""
import os
import struct
import sys

SECTOR = 2352


def read_sector(f, lba):
    f.seek(lba * SECTOR)
    return f.read(SECTOR)


def user_data(raw):
    # 12 sync + 4 header + 8 subheader; submode bit 5 => Form 2
    form2 = bool(raw[18] & 0x20)
    if form2:
        return raw[24:24 + 2324], True
    return raw[24:24 + 2048], False


def read_extent(f, lba, size):
    out = bytearray()
    count = (size + 2047) // 2048
    for i in range(count):
        data, _ = user_data(read_sector(f, lba + i))
        out += data[:2048]
    return bytes(out[:size])


def parse_dir(f, lba, size):
    data = read_extent(f, lba, size)
    entries = []
    pos = 0
    while pos < len(data):
        rec_len = data[pos]
        if rec_len == 0:
            # skip to next sector boundary
            pos = (pos // 2048 + 1) * 2048
            continue
        rec = data[pos:pos + rec_len]
        ext_lba = struct.unpack_from("<I", rec, 2)[0]
        ext_size = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        name_len = rec[32]
        name = rec[33:33 + name_len]
        pos += rec_len
        if name in (b"\x00", b"\x01"):
            continue
        name = name.decode("ascii", "replace").split(";")[0]
        entries.append((name, ext_lba, ext_size, bool(flags & 0x02)))
    return entries


def extract(f, lba, size, out_dir, rel, listing):
    for name, e_lba, e_size, is_dir in parse_dir(f, lba, size):
        path = os.path.join(out_dir, name)
        rel_path = f"{rel}/{name}" if rel else name
        if is_dir:
            os.makedirs(path, exist_ok=True)
            extract(f, e_lba, e_size, path, rel_path, listing)
            continue
        count = (e_size + 2047) // 2048
        total = os.fstat(f.fileno()).st_size // SECTOR
        if e_lba >= total:
            listing.append((rel_path, e_lba, e_size, "OUT-OF-IMAGE (skipped)"))
            continue
        count = min(count, total - e_lba)
        # STR movies interleave one audio sector every 8: look a bit further
        has_form2 = any(user_data(read_sector(f, e_lba + i))[1] for i in range(min(count, 32)))
        if has_form2:
            with open(path + ".raw2352", "wb") as o:
                f.seek(e_lba * SECTOR)
                o.write(f.read(count * SECTOR))
            listing.append((rel_path + ".raw2352", e_lba, e_size, "XA/STR"))
        else:
            with open(path, "wb") as o:
                o.write(read_extent(f, e_lba, e_size))
            listing.append((rel_path, e_lba, e_size, ""))


def main():
    src, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    with open(src, "rb") as f:
        pvd, _ = user_data(read_sector(f, 16))
        assert pvd[1:6] == b"CD001", "no ISO9660 primary volume descriptor"
        root = pvd[156:156 + 34]
        root_lba = struct.unpack_from("<I", root, 2)[0]
        root_size = struct.unpack_from("<I", root, 10)[0]
        print("Volume:", pvd[40:72].decode("ascii", "replace").strip())
        listing = []
        extract(f, root_lba, root_size, out_dir, "", listing)
    for rel_path, lba, size, kind in listing:
        print(f"{size:>10}  {kind:6}  {rel_path}")
    print(f"{len(listing)} files")


if __name__ == "__main__":
    main()
