"""Convert the PS1 background music (COMMON/SOUND/MAINxx.BGM, SUB_xx.BGM) for
the DS sequencer (source/music.c), and the room -> music table.

A BGM file holds a SEQ (pQES, MIDI-like, played by libsnd), a VAB header and
its samples; the last 12 bytes give the offsets of VB, VH and SEQ
(FUN_80056410). Output music/<name>.bin:
  u32 "RMU1", u32 seq offset, u32 seq size, u16 tone count, u16 0
  tones (24 bytes): u8 program, u8 note min, u8 note max, u8 centre,
                    s8 fine, u8 volume, u8 pan, u8 release (ADSR2 bits 0-4),
                    u32 sample offset, u32 sample bytes, u32 loop start bytes
                    (0xFFFFFFFF = plays once), u32 0
  SEQ bytes, 16-bit PCM samples (each sample's rate is 44100 Hz at its centre note).

Table music/bgm_table.bin (--table SLES INIT_TBL.DAT out): the new game's
BGM table (INIT_TBL.DAT is the starting save state; the table lies at RAM
0x800cc2cc = file 0x22c, two bytes per room: main, sub; 0xFF = none), then
the first room of each stage (0x800a0110): u8 stages, u8 first[stages],
u16 rooms, then the pairs.

Usage: python3 -I bgm2ds.py MAIN0B.BGM out.bin
       python3 -I bgm2ds.py --table SLES_009.72 INIT_TBL.DAT out.bin
"""
import struct
import sys

MAGIC = 0x31554D52  # "RMU1"
F0 = (0, 60, 115, 98, 122)
F1 = (0, 0, -52, -55, -60)


def decode_vag(data):
    """PS1 ADPCM -> (samples, loop start sample or None)."""
    out, s1, s2, loop = [], 0, 0, None
    looping = False
    for blk in range(0, len(data) - 15, 16):
        head, flags = data[blk], data[blk + 1]
        if flags & 4 and loop is None:
            loop = len(out)
        shift, flt = head & 0x0F, min(head >> 4, 4)
        for i in range(28):
            byte = data[blk + 2 + i // 2]
            nib = (byte >> 4) if i & 1 else (byte & 0x0F)
            if nib >= 8:
                nib -= 16
            s = ((nib << 12) >> shift) + ((s1 * F0[flt] + s2 * F1[flt] + 32) >> 6)
            s = max(-32768, min(32767, s))
            out.append(s)
            s2, s1 = s1, s
        if flags & 1:
            looping = bool(flags & 2)
            break
    return out, (loop if looping and loop is not None else None)


def convert(src, dst):
    d = open(src, "rb").read()
    vb, vh, seq = struct.unpack_from("<III", d, len(d) - 12)
    magic, _, _, _, _, ps, _, vs = struct.unpack_from("<4sIIIHHHH", d, vh)
    assert magic == b"pBAV"
    seq_end = min(o for o in (vh, vb, len(d) - 12) if o > seq)
    seq_data = d[seq:seq_end]
    tones_off = vh + 32 + 128 * 16
    sizes_off = tones_off + 16 * 32 * ps
    sizes = [struct.unpack_from("<H", d, sizes_off + 2 * i)[0] * 8 for i in range(vs + 1)]
    vags, pos = {}, vb
    for i in range(1, vs + 1):
        vags[i] = d[pos:pos + sizes[i]]
        pos += sizes[i]
    decoded = {i: decode_vag(v) for i, v in vags.items()}

    tones, pcm, placed = [], bytearray(), {}
    prog_no = 0
    for p in range(128):
        nt = d[vh + 32 + 16 * p]
        if not nt:
            continue
        for t in range(nt):
            o = tones_off + prog_no * 512 + t * 32
            _, _, vol, pan, centre, fine, lo, hi = struct.unpack_from("<8B", d, o)
            adsr2 = struct.unpack_from("<H", d, o + 18)[0]
            vag = struct.unpack_from("<h", d, o + 22)[0]
            if vag not in decoded:
                continue
            if vag not in placed:
                samples, loop = decoded[vag]
                if len(samples) % 2:
                    samples.append(0)
                placed[vag] = (len(pcm), 2 * len(samples), 0xFFFFFFFF if loop is None else 2 * (loop & ~1))
                pcm += struct.pack(f"<{len(samples)}h", *samples)
            off, size, loop = placed[vag]
            fine_s = fine - 256 if fine > 127 else fine
            tones.append((p, lo, hi, centre, fine_s, vol, pan, adsr2 & 0x1F, off, size, loop))
        prog_no += 1
    head_size = 16 + 24 * len(tones)
    seq_off = head_size
    pcm_off = (seq_off + len(seq_data) + 3) & ~3
    out = bytearray(struct.pack("<IIIHH", MAGIC, seq_off, len(seq_data), len(tones), 0))
    for p, lo, hi, centre, fine, vol, pan, rel, off, size, loop in tones:
        out += struct.pack("<BBBBbBBBIIII", p, lo, hi, centre, fine, vol, pan, rel,
                           pcm_off + off, size, loop, 0)
    out += seq_data
    out += b"\0" * (pcm_off - len(out))
    out += pcm
    open(dst, "wb").write(out)
    print(f"{dst}: {len(tones)} tones, seq {len(seq_data)} bytes, pcm {len(pcm)} bytes")


def table(exe_path, init_path, dst):
    exe = open(exe_path, "rb").read()
    init = open(init_path, "rb").read()
    o = 0x800a0110 - 0x80010000 + 0x800
    firsts = list(exe[o:o + 7])
    rooms = 141
    pairs = init[0x22C:0x22C + 2 * rooms]
    open(dst, "wb").write(bytes([len(firsts)]) + bytes(firsts) + struct.pack("<H", rooms) + pairs)
    print(f"{dst}: {rooms} rooms, stage starts {firsts}")


if __name__ == "__main__":
    if sys.argv[1] == "--table":
        table(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        convert(sys.argv[1], sys.argv[2])
