"""Wrap a PS-X EXE (plus optional code overlays) in a minimal ELF32
little-endian MIPS executable so that generic ELF loaders (Ghidra via REA)
can analyze it as MIPS R3000 code.

Layout:
  .text      PROGBITS  t_addr .. t_addr+t_size   (the EXE payload)
  .ovlN      PROGBITS  overlay images (e.g. enemy code from CDEMD0.EMS)
  .ramN      NOBITS    remaining 2 MB main RAM (heap / buffers)
  .scratch   NOBITS    0x1F800000, 1 KB scratchpad
A _gp symbol is emitted when the startup code's gp value can be recovered.

Usage:
  python3 -I psxexe_to_elf.py <SLES_xxx.xx> <out.elf> [symbols.txt]
                             [--overlay FILE:OFFSET:SIZE:ADDR ...]

symbols.txt holds "0xADDRESS name" lines; each becomes a global function
symbol so Ghidra creates (and names) functions reached only via tables.
"""
import struct
import sys

RAM_END = 0x80200000
SCRATCH_ADDR, SCRATCH_SIZE = 0x1F800000, 0x400
# EF_MIPS_ARCH_32 without ABI bits: R3000 (MIPS I) is a subset of MIPS32;
# REA's Ghidra lane only admits ARCH_32 / ARCH_32R2 headers.
E_FLAGS = 0x50000000


def find_gp(code, base):
    """Return gp from the first 'lui gp,hi' followed by 'addiu/ori gp,gp,lo'."""
    for off in range(0, len(code) - 8, 4):
        ins = struct.unpack_from("<I", code, off)[0]
        if ins >> 26 == 0x0F and (ins >> 16) & 31 == 28:  # lui $gp, imm
            hi = (ins & 0xFFFF) << 16
            for k in range(1, 6):
                nxt = struct.unpack_from("<I", code, off + 4 * k)[0]
                op, rs, rt, imm = nxt >> 26, (nxt >> 21) & 31, (nxt >> 16) & 31, nxt & 0xFFFF
                if rt == 28 and rs == 28 and op == 0x09:  # addiu
                    return (hi + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF, base + off
                if rt == 28 and rs == 28 and op == 0x0D:  # ori
                    return hi | imm, base + off
    return None, None


def parse_args(argv):
    src, dst = argv[1], argv[2]
    sym_path, overlays = None, []
    rest = argv[3:]
    i = 0
    while i < len(rest):
        if rest[i] == "--overlay":
            path, off, size, addr = rest[i + 1].rsplit(":", 3)
            overlays.append((path, int(off, 0), int(size, 0), int(addr, 0)))
            i += 2
        else:
            sym_path = rest[i]
            i += 1
    return src, dst, sym_path, overlays


def main():
    src, dst, sym_path, overlay_args = parse_args(sys.argv)
    extra = []
    if sym_path:
        for line in open(sym_path):
            line = line.split("#")[0].split()
            if len(line) == 2:
                extra.append((int(line[0], 16), line[1]))

    data = open(src, "rb").read()
    assert data[:8] == b"PS-X EXE", "not a PS-X EXE"
    pc, gp_hdr, t_addr, t_size = struct.unpack_from("<IIII", data, 0x10)
    b_addr, b_size = struct.unpack_from("<II", data, 0x28)
    code = data[0x800:0x800 + t_size]

    gp, gp_at = (gp_hdr, None) if gp_hdr else find_gp(code, t_addr)
    print(f"entry=0x{pc:08X} text=0x{t_addr:08X}+0x{t_size:X} bss=0x{b_addr:08X}+0x{b_size:X}")
    print(f"gp={'0x%08X' % gp if gp else 'unknown'}" + (f" (set at 0x{gp_at:08X})" if gp_at else ""))

    # Initialized regions: (name, addr, bytes)
    progbits = [(".text", t_addr, code)]
    for n, (path, off, size, addr) in enumerate(overlay_args):
        blob = open(path, "rb").read()[off:off + size]
        assert len(blob) == size, f"overlay {path} too short"
        progbits.append((f".ovl{n}", addr, blob))
        print(f"overlay .ovl{n}: {path}+0x{off:X} size 0x{size:X} at 0x{addr:08X}")
        # The EXE reserves the first overlay slot at its own tail (2 KB of
        # padding at 0x80100000); the loaded overlay replaces it at runtime.
        if t_addr < addr < t_addr + len(code):
            code = code[:addr - t_addr]
            progbits[0] = (".text", t_addr, code)
            print(f"  .text truncated to end at 0x{addr:08X} (overlay slot)")
    progbits.sort(key=lambda r: r[1])

    # Uninitialized RAM gaps after the EXE, around overlays.
    nobits = []
    cursor = t_addr + t_size
    for _, addr, blob in progbits:
        if addr + len(blob) <= cursor:
            continue
        if addr > cursor:
            nobits.append((f".ram{len(nobits)}", cursor, addr - cursor))
        cursor = max(cursor, addr + len(blob))
    if cursor < RAM_END:
        nobits.append((f".ram{len(nobits)}", cursor, RAM_END - cursor))
    nobits.append((".scratch", SCRATCH_ADDR, SCRATCH_SIZE))

    names = [n for n, _, _ in progbits] + [n for n, _, _ in nobits] + [".symtab", ".strtab", ".shstrtab"]
    shstr = b"\0" + b"".join(n.encode() + b"\0" for n in names)

    def sh_name(n):
        return shstr.index(b"\0" + n.encode() + b"\0") + 1

    text_index = 1 + [n for n, _, _ in progbits].index(".text")

    def section_for(addr):
        for idx, (_, a, blob) in enumerate(progbits):
            if a <= addr < a + len(blob):
                return idx + 1
        return text_index

    strtab = b"\0_gp\0_start\0"
    extra_off = []
    for _, name in extra:
        extra_off.append(len(strtab))
        strtab += name.encode() + b"\0"

    syms = [struct.pack("<IIIBBH", 0, 0, 0, 0, 0, 0)]
    syms.append(struct.pack("<IIIBBH", strtab.index(b"_start"), pc, 0, (1 << 4) | 2, 0, text_index))
    if gp:
        syms.append(struct.pack("<IIIBBH", strtab.index(b"_gp"), gp, 0, (1 << 4) | 0, 0, 0xFFF1))
    for (addr, _), off in zip(extra, extra_off):
        syms.append(struct.pack("<IIIBBH", off, addr, 0, (1 << 4) | 2, 0, section_for(addr)))
    symtab = b"".join(syms)

    ehsize, phentsize, shentsize = 52, 32, 40
    phnum = len(progbits) + len(nobits)
    shnum = 1 + len(names)

    # File layout: headers, then each PROGBITS blob page-aligned.
    offset = 0x1000
    blob_offsets = []
    for _, _, blob in progbits:
        blob_offsets.append(offset)
        offset = (offset + len(blob) + 0xFFF) & ~0xFFF
    symtab_off = offset
    strtab_off = symtab_off + len(symtab)
    shstr_off = strtab_off + len(strtab)
    shoff = (shstr_off + len(shstr) + 3) & ~3

    eh = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    eh += struct.pack("<HHIIIIIHHHHHH", 2, 8, 1, pc, ehsize, shoff, E_FLAGS,
                      ehsize, phentsize, phnum, shentsize, shnum, shnum - 1)

    PT_LOAD = 1
    ph = b""
    for (_, addr, blob), off in zip(progbits, blob_offsets):
        ph += struct.pack("<IIIIIIII", PT_LOAD, off, addr, addr, len(blob), len(blob), 7, 0x1000)
    for _, addr, size in nobits:
        ph += struct.pack("<IIIIIIII", PT_LOAD, 0, addr, addr, 0, size, 6, 0x4)
    assert ehsize + len(ph) <= 0x1000

    SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_NOBITS = 1, 2, 3, 8
    W, A, X = 1, 2, 4
    sh = struct.pack("<IIIIIIIIII", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    for (name, addr, blob), off in zip(progbits, blob_offsets):
        sh += struct.pack("<IIIIIIIIII", sh_name(name), SHT_PROGBITS, A | X | W, addr, off, len(blob), 0, 0, 4, 0)
    for name, addr, size in nobits:
        sh += struct.pack("<IIIIIIIIII", sh_name(name), SHT_NOBITS, A | W, addr, 0, size, 0, 0, 4, 0)
    strtab_index = 1 + len(progbits) + len(nobits) + 1
    sh += struct.pack("<IIIIIIIIII", sh_name(".symtab"), SHT_SYMTAB, 0, 0, symtab_off, len(symtab), strtab_index, 1, 4, 16)
    sh += struct.pack("<IIIIIIIIII", sh_name(".strtab"), SHT_STRTAB, 0, 0, strtab_off, len(strtab), 0, 0, 1, 0)
    sh += struct.pack("<IIIIIIIIII", sh_name(".shstrtab"), SHT_STRTAB, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)

    out = bytearray(eh + ph)
    for (_, _, blob), off in zip(progbits, blob_offsets):
        out += b"\0" * (off - len(out))
        out += blob
    out += b"\0" * (symtab_off - len(out))
    out += symtab + strtab + shstr
    out += b"\0" * (shoff - len(out))
    out += sh
    open(dst, "wb").write(out)
    print(f"wrote {dst} ({len(out)} bytes)")


if __name__ == "__main__":
    main()
