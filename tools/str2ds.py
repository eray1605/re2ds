"""Convert a PS1 movie (.STR, MDEC video + XA audio) for the DS (source/movie.c).

The STR must be raw 2352-byte sectors (the XA audio sectors are Form 2; the
2048-byte file from the ISO loses them): PL0/ZMOVIE/OPN1STL.STR.raw2352.
ffmpeg decodes it (format psxstr). RE2's movies are 320 x 160 at 15 fps,
stereo XA at 37800 Hz.

Video: scaled to 256 x 128 (shown letterboxed on the upper screen), one
palette of 255 colours per frame (indices 1..255; index 0 is the BG
backdrop), each frame LZ77 compressed for the BIOS (swiDecompressLZSSWram).
Audio: mono 16-bit PCM at RATE in one block after the frames; the player
reads it ahead of the pictures with a second file handle.

Output movie/<name>.bin:
  u32 "RMV2", u16 width, u16 height, u16 fps, u16 frames, u32 audio rate,
  u32 audio offset, u32 audio samples, u32 largest lz size
  frames[]: u32 lz size, u16 palette[256], lz data (padded to 4 bytes)
  s8 audio[samples] ("RMV1": s16)

Usage: python3 -I str2ds.py MOVIE.STR.raw2352 out.bin [--fps N]
       (default 15; the in-game movies use 10 to fit the 512 MB ROM)
"""
import os
import struct
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
MAGIC = 0x32564D52  # "RMV2": 8-bit audio (RMV1 had 16-bit)
W, H, FPS = 256, 128, 15
RATE = 22050        # the DS plays it at 16756991 / 760 = 22048.7 Hz
FILTER = (f"scale={W}:{H}:flags=lanczos,split[a][b];"
          "[a]palettegen=max_colors=255:reserve_transparent=0:stats_mode=single[p];"
          "[b][p]paletteuse=new=1:dither=bayer:bayer_scale=4")


def lz77_all(frames, tmp):
    out = []
    for k, data in enumerate(frames):
        src, dst = os.path.join(tmp, "in"), os.path.join(tmp, "out")
        with open(src, "wb") as f:
            f.write(data)
        subprocess.run([os.path.join(TOOLS, "lz77"), src, dst], check=True)
        out.append(open(dst, "rb").read())
    return out


def main():
    src, out = sys.argv[1], sys.argv[2]
    fps = int(sys.argv[sys.argv.index("--fps") + 1]) if "--fps" in sys.argv else FPS
    video = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "psxstr", "-i", src, "-an",
         "-filter_complex", FILTER, "-r", str(fps), "-f", "rawvideo", "-pix_fmt", "pal8", "-"],
        check=True, capture_output=True).stdout
    audio = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "psxstr", "-i", src, "-vn",
         "-ac", "1", "-ar", str(RATE), "-f", "s16le", "-"],
        check=True, capture_output=True).stdout
    size = W * H + 1024
    count = len(video) // size
    samples = struct.unpack(f"<{len(audio) // 2}h", audio[:len(audio) // 2 * 2])
    remap = bytes(min(i + 1, 255) for i in range(256))
    pixels, palettes = [], []
    for k in range(count):
        raw = video[k * size:(k + 1) * size]
        pixels.append(raw[:W * H].translate(remap))
        pal = [0] * 256
        for i in range(255):
            b, g, r, _ = raw[W * H + 4 * i:W * H + 4 * i + 4]
            pal[i + 1] = (r >> 3) | (g >> 3) << 5 | (b >> 3) << 10
        palettes.append(pal)
    with tempfile.TemporaryDirectory() as tmp:
        packed = lz77_all(pixels, tmp)
    body = bytearray()
    for k in range(count):
        lz = packed[k] + bytes(-len(packed[k]) % 4)
        body += struct.pack("<I", len(lz)) + struct.pack("<256H", *palettes[k]) + lz
    head = struct.pack("<IHHHHIIII", MAGIC, W, H, fps, count, RATE, 28 + len(body), len(samples),
                       max(len(p) + 3 & ~3 for p in packed))
    body = head + body + bytes((max(-128, min(127, (v + 128) >> 8)) & 0xFF) for v in samples)
    with open(out, "wb") as f:
        f.write(body)
    print(f"{out}: {count} frames ({count / fps:.1f} s), {len(samples) / RATE:.1f} s audio, "
          f"{len(body) / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
