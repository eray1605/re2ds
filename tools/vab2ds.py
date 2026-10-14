"""Convert a PS1 VAB sound bank (e.g. the one in a door file, DOORnn.DO2) to
the DS sound format (source/formats.h, SoundBankHeader).

Every tone becomes 16-bit PCM at the rate the PS1 plays it for its key range:
the SPU plays a sample at 44100 Hz when the key-on note equals the tone's
centre note, so rate = 44100 * 2^((note - centre - fine/128) / 12). The door
program keys on note 60 (door opens) and 61 (door closes); each tone of the
door banks covers exactly one note (min == max), which gives the note.

Usage: python3 -I vab2ds.py in.DO2 out.bin [--offset N]   (VAB at offset 16 in a DO2)
"""
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


def parse_vab(d, base):
    magic, _, _, _, _, ps, ts, vs = struct.unpack_from("<4sIIIHHHH", d, base)
    assert magic == b"pBAV", "not a VAB"
    tones_off = base + 32 + 128 * 16
    tones = []
    for t in range(ts):
        o = tones_off + 32 * t
        _, _, vol, pan, centre, fine, lo, hi = struct.unpack_from("<8B", d, o)
        vag = struct.unpack_from("<h", d, o + 22)[0]
        tones.append({"centre": centre, "fine": fine, "lo": lo, "hi": hi, "vag": vag, "vol": vol})
    sizes_off = tones_off + 16 * 32 * ps
    sizes = [struct.unpack_from("<H", d, sizes_off + 2 * i)[0] * 8 for i in range(vs + 1)]
    vb = sizes_off + 512 + 8  # u16 sizes[256], then an 8-byte block header (DO2)
    vags, pos = {}, vb
    for i in range(1, vs + 1):
        vags[i] = d[pos:pos + sizes[i]]
        pos += sizes[i]
    return tones, vags


def main():
    src, dst = sys.argv[1], sys.argv[2]
    base = int(sys.argv[sys.argv.index("--offset") + 1]) if "--offset" in sys.argv else 16
    d = open(src, "rb").read()
    tones, vags = parse_vab(d, base)
    entries, pcm = [], bytearray()
    for t in tones:
        note = t["lo"] if t["lo"] == t["hi"] else t["centre"]
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
