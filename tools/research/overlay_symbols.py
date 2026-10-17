"""List the function-pointer tables inside a RE2 enemy code overlay and emit
them as symbols for psxexe_to_elf.py.

The enemy loader table in SLES_009.72 (0x8009328c) gives, per enemy type and
slot, a sector offset into PL0/PLD/CDEMD0.EMS and a byte size. Slot 0 code is
linked at 0x80100000, slot 1 at 0x8010D000.

Usage:
  python3 -I overlay_symbols.py <SLES_xxx.xx> <CDEMD0.EMS> <type_hex> <slot> <prefix>
Prints "--overlay" argument on stderr and symbol lines on stdout.
"""
import struct
import sys

SLOT_ADDR = (0x80100000, 0x8010D000)
LOADER_TABLE = 0x8009328c


def exe_read(exe, addr, n):
    off = addr - 0x80010000 + 0x800
    return exe[off:off + n]


def main():
    exe = open(sys.argv[1], "rb").read()
    ems_path = sys.argv[2]
    etype, slot, prefix = int(sys.argv[3], 16), int(sys.argv[4]), sys.argv[5]
    idx = ((etype - 0x10) * 4 + 1) - slot
    sector, size = struct.unpack("<II", exe_read(exe, LOADER_TABLE + idx * 8, 8))
    base = SLOT_ADDR[slot]
    blob = open(ems_path, "rb").read()[sector * 2048:sector * 2048 + size]
    print(f"--overlay {ems_path}:{sector * 2048:#x}:{size:#x}:{base:#x}", file=sys.stderr)

    runs, cur = [], None
    for i in range(0, len(blob) - 3, 4):
        v = struct.unpack_from("<I", blob, i)[0]
        if base <= v < base + size and v & 3 == 0:
            if cur and cur[1] == i:
                cur[1] = i + 4
                cur[2].append(v)
            else:
                cur = [i, i + 4, [v]]
                runs.append(cur)
    in_table = set()
    for start, end, vals in runs:
        if len(vals) >= 2:
            in_table.update(range(start, end))

    def looks_like_code(addr):
        off = addr - base
        if off in in_table or off + 4 > len(blob):
            return False
        word = struct.unpack_from("<I", blob, off)[0]
        if word == 0 or base <= word < base + size:
            return False
        # first instruction must be a known MIPS opcode family
        return (word >> 26) in (0x00, 0x02, 0x03, 0x04, 0x05, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
                                0x0D, 0x0F, 0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B)

    seen = set()
    print(f"# type 0x{etype:02x} overlay (CDEMD0.EMS sector {sector:#x}, size {size:#x}) at {base:#x}")
    for start, _, vals in runs:
        if len(vals) < 2:
            continue
        for k, v in enumerate(vals):
            if v in seen or not looks_like_code(v):
                continue
            seen.add(v)
            print(f"0x{v:08x} {prefix}_t{(base + start) & 0xffff:04x}_{k:02d}")


if __name__ == "__main__":
    main()
