"""Convert a stage's voice clips (xa_on) for the DS.

xa_on 0, n (scd_op_59 -> FUN_8001260c) plays entry n of the stage's clip
table: COMMON/BIN/STAGE<s>.BIN stores its address to 0x800c6184 (the stage
code is loaded at 0x8011a000); 6 bytes per entry: u16 channel, u16 start,
u16 length, in sectors of that channel. The audio is PL0/VOICE/VOICE0<s>.XAS:
CD-XA, 16 channels interleaved sector by sector (file sector = block * 16 +
channel), ADPCM 4 bit mono at 37800 Hz.

Writes out_dir/s<s>_<nn>.bin per clip: u32 magic "RVC1", u32 sample rate,
u32 samples, then IMA-ADPCM as the DS plays it (u16 first sample, u16 step
index, then nibbles, low first), at half the rate (18900 Hz), each clip
normalised to a peak of 30000 (at most 4x).

Usage: python3 -I xa2ds.py STAGE1.BIN VOICE01.XAS.raw2352 stage out_dir
       (the .XAS as raw 2352-byte sectors, with the XA subheaders)
"""
import os
import struct
import sys

STAGE_BASE = 0x8011A000
SECTOR = 2352
MAGIC = 0x31435652  # "RVC1"
PEAK = 30000      # normalised clip peak
MAX_GAIN = 4.0
K0 = (0, 60, 115, 98)
K1 = (0, 0, -52, -55)

IMA_STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
             73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
             449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
             2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
             8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
             29794, 32767]
IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def clip_table(stage_bin):
    """Entries (channel, start, length) of the table stored to 0x800c6184."""
    regs, table = {}, None
    for o in range(4, min(len(stage_bin), 512), 4):
        i = struct.unpack_from("<I", stage_bin, o)[0]
        op, rs, rt, imm = i >> 26, (i >> 21) & 31, (i >> 16) & 31, i & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        if op == 0x0F:
            regs[rt] = imm << 16
        elif op == 0x09:
            regs[rt] = (regs.get(rs, 0) + simm) & 0xFFFFFFFF
        elif op == 0x2B and imm == 0x6184:
            table = regs.get(rt)
    if table is None:
        return []
    off = table - STAGE_BASE
    return [struct.unpack_from("<3H", stage_bin, off + 6 * i) for i in range((len(stage_bin) - off) // 6)]


def decode_sector(data, hist):
    """XA ADPCM, 4 bit mono: 18 groups of 128 bytes, 8 units of 28 samples each."""
    out = []
    old, older = hist
    for g in range(18):
        grp = data[g * 128:(g + 1) * 128]
        for unit in range(8):
            param = grp[4 + unit]
            shift, filt = param & 15, (param >> 4) & 3
            for j in range(28):
                nib = (grp[16 + j * 4 + unit // 2] >> (4 * (unit & 1))) & 15
                s = (nib << 12) & 0xFFFF
                if s & 0x8000:
                    s -= 0x10000
                s = (s >> shift) + (old * K0[filt] + older * K1[filt] + 32) // 64
                s = max(-32768, min(32767, s))
                out.append(s)
                older, old = old, s
    hist[0], hist[1] = old, older
    return out


def clip_samples(xa, channel, start, length):
    hist = [0, 0]
    pcm = []
    nsect = len(xa) // SECTOR
    for k in range(length):
        s = (start + k) * 16 + channel
        if s >= nsect:
            break
        base = s * SECTOR
        ch, submode, coding = xa[base + 17], xa[base + 18], xa[base + 19]
        if ch != channel or not submode & 4 or coding & 0x31:  # audio, mono, 4 bit only
            continue
        pcm += decode_sector(xa[base + 24:base + 24 + 18 * 128], hist)
        if submode & 0x80:  # end of the clip
            break
    return pcm


def ima_encode(pcm):
    pred, index = pcm[0] if pcm else 0, 0
    out = bytearray(struct.pack("<hH", pred, index))
    nibbles = []
    for x in pcm[1:]:
        step = IMA_STEPS[index]
        diff = x - pred
        nib = 0
        if diff < 0:
            nib = 8
            diff = -diff
        delta = step >> 3
        if diff >= step:
            nib |= 4
            diff -= step
            delta += step
        if diff >= step >> 1:
            nib |= 2
            diff -= step >> 1
            delta += step >> 1
        if diff >= step >> 2:
            nib |= 1
            delta += step >> 2
        pred = max(-32768, min(32767, pred - delta if nib & 8 else pred + delta))
        index = max(0, min(88, index + IMA_INDEX[nib & 7]))
        nibbles.append(nib)
    if len(nibbles) % 2:
        nibbles.append(0)
    out += bytes(nibbles[i] | nibbles[i + 1] << 4 for i in range(0, len(nibbles), 2))
    while len(out) % 4:
        out.append(0)
    return out


def main():
    stage_bin = open(sys.argv[1], "rb").read()
    xa = open(sys.argv[2], "rb").read()
    stage, out_dir = int(sys.argv[3]), sys.argv[4]
    os.makedirs(out_dir, exist_ok=True)
    nsect = len(xa) // SECTOR
    written = 0
    for n, (channel, start, length) in enumerate(clip_table(stage_bin)):
        if channel > 15 or not length or (start + length) * 16 > nsect + 16:
            continue  # past the real table
        pcm = clip_samples(xa, channel, start, length)
        if not pcm:
            continue
        half = [(pcm[i] + pcm[i + 1]) // 2 for i in range(0, len(pcm) - 1, 2)]
        # the PS1 mixes XA well above the sequenced music; the DS plays
        # everything through one mixer, so bring each clip up to full scale
        peak = max(1, max(abs(x) for x in half))
        gain = min(MAX_GAIN, PEAK / peak)
        half = [max(-32768, min(32767, int(x * gain))) for x in half]
        body = ima_encode(half)
        with open(os.path.join(out_dir, f"s{stage}_{n:02d}.bin"), "wb") as f:
            f.write(struct.pack("<III", MAGIC, 18900, len(half)) + body)
        written += 1
    print(f"stage {stage}: {written} voice clips")


if __name__ == "__main__":
    main()
