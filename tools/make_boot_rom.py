"""The boot ROM: re2ds.nds without the game data, for emulators on phones.

re2ds.nds carries the game data (NitroFS) and is several hundred MB. RetroArch's
melonDS DS core keeps three copies of a loaded ROM in memory (the frontend's,
its own, and the emulated cartridge padded to a power of two): about 1.4 GB for
re2ds.nds, and Android closes RetroArch for that.

The boot ROM is re2ds.nds cut off where the first NitroFS file starts: header,
ARM9 and ARM7 code, file name table, FAT and icon, all unchanged. Its header is
the full ROM's, so the engine takes re2ds.nds on the (virtual) SD card as its
data: with DLDI the cartridge cannot be read anyway, and nitro_init (main.c)
then reads NitroFS from that copy - the same way as melonDS on a PC.

DS mode only: the DSi code lies behind the files and is cut off.

Usage: python3 -I make_boot_rom.py re2ds.nds re2ds_boot.nds
"""
import struct
import sys

# banner (icon + titles) size per banner version
BANNER_SIZES = {0x0001: 0x840, 0x0002: 0x940, 0x0003: 0xA40, 0x0103: 0x23C0}


def crc16(data):
    """The DS header's CRC-16 (MODBUS: reflected 0x8005, start 0xFFFF)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def boot_size(f):
    """Bytes of the ROM in file f up to the first NitroFS file."""
    f.seek(0)
    header = f.read(0x200)
    if len(header) < 0x200 or crc16(header[:0x15E]) != struct.unpack_from("<H", header, 0x15E)[0]:
        raise ValueError("not a DS ROM (no valid header)")
    arm9_off, _, _, arm9_size = struct.unpack_from("<4I", header, 0x20)
    arm7_off, _, _, arm7_size = struct.unpack_from("<4I", header, 0x30)
    fnt_off, fnt_size, fat_off, fat_size = struct.unpack_from("<4I", header, 0x40)
    _, ovl9_size, _, ovl7_size = struct.unpack_from("<4I", header, 0x50)
    banner_off, = struct.unpack_from("<I", header, 0x68)
    header_size, = struct.unpack_from("<I", header, 0x84)
    if ovl9_size or ovl7_size:
        # overlays are FAT files too, and the code needs them
        raise ValueError("the ROM has overlays; a boot ROM would lose them")

    end = max(header_size, arm9_off + arm9_size, arm7_off + arm7_size,
              fnt_off + fnt_size, fat_off + fat_size)
    if banner_off:
        f.seek(banner_off)
        version, = struct.unpack("<H", f.read(2))
        end = max(end, banner_off + BANNER_SIZES.get(version, 0x840))

    f.seek(fat_off)
    fat = f.read(fat_size)
    starts = [start for start, stop in struct.iter_unpack("<II", fat) if stop > start]
    if not starts:
        raise ValueError("the ROM has no NitroFS files")
    first = min(starts)
    if first < end:
        raise ValueError(f"a NitroFS file starts at 0x{first:x}, inside the code or tables")
    return first


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1:]
    with open(src, "rb") as f:
        try:
            size = boot_size(f)
        except (ValueError, struct.error) as e:
            sys.exit(f"{src}: {e}")
        f.seek(0)
        data = f.read(size)
    with open(dst, "wb") as f:
        f.write(data)
    print(f"{dst}: {size // 1024} KB (the game data stays in {src})")


if __name__ == "__main__":
    main()
