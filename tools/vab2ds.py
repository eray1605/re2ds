"""Convert a PS1 VAB sound bank (e.g. the one in a door file, DOORnn.DO2) to
the DS sound format (source/formats.h, SoundBankHeader). Every bank is keyed
by sound number (door: 0 = opens, 1 = closes; se_on / FUN_80057364).

Every tone becomes 16-bit PCM at the rate the PS1 plays it for its key range:
the SPU plays a sample at 44100 Hz when the key-on note equals the tone's
centre note, so rate = 44100 * 2^((note - centre - fine/128) / 12). The door
program keys on note 60 (door opens) and 61 (door closes); each tone of the
door banks covers exactly one note (min == max), which gives the note.

Core and weapon banks come as COMMON/SOUND/xxx.EDH (a 64-byte sound table,
then the VAB header) plus xxx.VB (the samples). The sound table holds one u32
per sound number; bits 20..23 are the tone. For these banks the output has one
entry per sound number with note_lo = note_hi = that number.

Room banks: the RDT holds a 48-entry sound table (u32: byte 1 & 0x7F =
program, byte 2 >> 4 = tone), the VAB header and the samples; footsteps use
entry <floor sound> + 0 (walk) / + 1 (run), see source/main.c.

Usage: python3 -I vab2ds.py in.DO2 out.bin            (VAB at offset 16 in a DO2)
       python3 -I vab2ds.py ROOM1000.RDT out.bin --rdt (room bank)
       python3 -I vab2ds.py in.EDH out.bin --vb in.VB  (core / weapon bank)
"""
import os
import struct
import sys

SOUND_MAGIC = 0x31534252  # "RBS1"
F0 = (0, 60, 115, 98, 122)
F1 = (0, 0, -52, -55, -60)


def decode_vag(data):
    """PS1 SPU ADPCM -> list of 16-bit samples (stops at the end flag)."""
    out, s1, s2 = [], 0, 0
    for blk in range(0, len(data) - 15, 16):
        head, flags = data[blk], data[blk + 1]
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
        if flags & 1:   # loop end / end of sample
            break
    return out


def parse_vab(d, base, vb_data=None):
    """Returns (tone(prog, index) -> dict, vags). Tone attributes lie at
    base + 0x820 + prog * 0x200 + index * 0x20 (16 slots per program), as the
    PS1 reads them (FUN_8005797c)."""
    magic, _, _, _, _, ps, ts, vs = struct.unpack_from("<4sIIIHHHH", d, base)
    assert magic == b"pBAV", "not a VAB"
    tones_off = base + 32 + 128 * 16

    def tone(prog, index):
        o = tones_off + prog * 512 + index * 32
        _, _, vol, pan, centre, fine, lo, hi = struct.unpack_from("<8B", d, o)
        vag = struct.unpack_from("<h", d, o + 22)[0]
        return {"centre": centre, "fine": fine, "lo": lo, "hi": hi, "vag": vag, "vol": vol}

    sizes_off = tones_off + 16 * 32 * ps
    sizes = [struct.unpack_from("<H", d, sizes_off + 2 * i)[0] * 8 for i in range(vs + 1)]
    if vb_data is None:
        vb_data, pos = d, sizes_off + 512 + 8  # u16 sizes[256], then an 8-byte block header (DO2)
    else:
        pos = 0
    vags = {}
    for i in range(1, vs + 1):
        vags[i] = vb_data[pos:pos + sizes[i]]
        pos += sizes[i]
    return tone, ps, vags


def sound_table(words):
    """Sound number -> (program, tone) from a table of u32 entries
    (byte 1 & 0x7F = program, byte 2 >> 4 = tone; 0xFFFFFFFF = unused)."""
    return [(i, (w >> 8) & 0x7F, (w >> 20) & 0xF) for i, w in enumerate(words) if w != 0xFFFFFFFF]


def main():
    src, dst = sys.argv[1], sys.argv[2]
    base = int(sys.argv[sys.argv.index("--offset") + 1]) if "--offset" in sys.argv else 16
    d = open(src, "rb").read()
    if "--vb" in sys.argv:          # core / weapon bank: EDH + VB
        vb = open(sys.argv[sys.argv.index("--vb") + 1], "rb").read()
        base = d.find(b"pBAV")
        tone, ps, vags = parse_vab(d, base, vb)
        picks = sound_table(struct.unpack_from(f"<{base // 4}I", d, 0))
    elif "--rdt" in sys.argv:       # room bank: 48-entry table, VH and VB in the RDT
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from rdt2json import read_header
        _, offs = read_header(d)
        if not offs.get("room_vab_header") or not offs.get("room_sound"):
            open(dst, "wb").write(struct.pack("<IHH", SOUND_MAGIC, 0, 0))
            print(f"{dst}: no room sounds")
            return
        tone, ps, vags = parse_vab(d, offs["room_vab_header"], d[offs["room_vab_data"]:])
        picks = sound_table(struct.unpack_from("<48I", d, offs["room_sound"]))
    else:                           # door bank: sound table in the first 16 bytes
        tone, ps, vags = parse_vab(d, base)
        picks = sound_table(struct.unpack_from("<4I", d, 0))
    entries, pcm = [], bytearray()
    if picks is None:
        items = [(None, tone(0, i)) for i in range(16) if tone(0, i)["vag"] > 0]
    else:
        items = [(sid, tone(prog, ti)) for sid, prog, ti in picks if prog < ps]
    for sound_id, t in items:
        # the PS1 keys on the tone's lowest note (+ a random 0/1, ignored)
        t = dict(t, note=t["lo"])
        if sound_id is not None:
            t["lo"] = t["hi"] = sound_id
        note = t.get("note", t["lo"] if t["lo"] == t["hi"] else t["centre"])
        rate = int(round(44100 * 2 ** ((note - t["centre"] - t["fine"] / 128) / 12)))
        samples = decode_vag(vags.get(t["vag"], b""))
        while len(samples) % 2:
            samples.append(0)
        entries.append((t["lo"], t["hi"], min(rate, 65535), len(pcm), 2 * len(samples)))
        pcm += struct.pack(f"<{len(samples)}h", *samples)
    head = struct.pack("<IHH", SOUND_MAGIC, len(entries), 0)
    table = b"".join(struct.pack("<BBHII", lo, hi, rate, 0, size) for lo, hi, rate, _, size in entries)
    data_start = len(head) + len(table)
    table = b"".join(struct.pack("<BBHII", lo, hi, rate, data_start + off, size)
                     for lo, hi, rate, off, size in entries)
    with open(dst, "wb") as f:
        f.write(head + table + pcm)
    print(f"{dst}: " + ", ".join(f"notes {lo}-{hi}: {size // 2} samples @ {rate} Hz"
                                 for lo, hi, rate, _, size in entries))


if __name__ == "__main__":
    main()
