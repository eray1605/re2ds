"""Convert a PS1 movie (.STR, MDEC video + XA audio) for the DS (source/movie.c).

The STR must be raw 2352-byte sectors (the XA audio sectors are Form 2; the
2048-byte file from the ISO loses them): PL0/ZMOVIE/OPN1STL.STR.raw2352.
ffmpeg decodes it (format psxstr). RE2's movies are 320 x 160 at 15 fps,
stereo XA at 37800 Hz.

Video: scaled to 256 x 128 (shown letterboxed on the upper screen) and cut
into groups of up to GROUP frames that share one palette of 255 colours
(indices 1..255; index 0 is the BG backdrop; ffmpeg palettegen over the group,
light ordered dithering, after a temporal denoise). The first frame of a group is stored whole (a key frame
with the palette), the others only as the 8 x 8 tiles that changed visibly
(tools/rmvdelta.c: a mask of the 512 tiles, then their pixels). Each frame is
LZ77 packed for the BIOS (swiDecompressLZSSWram). The still parts of a
picture are not stored again.
Audio: mono 8-bit PCM at RATE in one block after the frames; the player reads
it ahead of the pictures with a second file handle.

Output movie/<name>.bin:
  u32 "RMV3", u16 width, u16 height, u16 fps, u16 frames, u32 audio rate,
  u32 audio offset, u32 audio samples, u32 largest lz size
  frames[]: u32 lz size | 0x80000000 for a key frame, [key frame: u16
            palette[256]], lz data (padded to 4 bytes)
  s8 audio[samples]

Usage: python3 -I str2ds.py MOVIE.STR.raw2352 out.bin [--fps N]
       (default 15; the in-game movies use 10)
"""
import os
import struct
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
MAGIC = 0x33564D52  # "RMV3": key frames + tile deltas, 8-bit audio
W, H, FPS = 256, 128, 15
RATE = 22050        # the DS plays it at 16756991 / 760 = 22048.7 Hz
GROUP = 30          # frames sharing a palette (a key frame at least this often)
# tools/rmvdelta.c: what counts as a change worth sending. Looser values save
# more but leave blocks on screen (8 / 56 left a fifth of the tiles visibly
# wrong, tested on the DS); at 2 / 16 none is.
MEAN, BIGGEST = "2", "16"
KEY = 0x80000000
# the PS1's video compression changes still parts of the picture a little from
# frame to frame: smoothed over time they stay the same, and need not be sent
DENOISE = "hqdn3d=0:0:4:4"
PALETTE = ("split[a][b];[a]palettegen=max_colors=255:reserve_transparent=0:stats_mode=full[p];"
           "[b][p]paletteuse=dither=bayer:bayer_scale=5")


def lz77(data, tmp):
    src, dst = os.path.join(tmp, "in"), os.path.join(tmp, "out")
    with open(src, "wb") as f:
        f.write(data)
    subprocess.run([os.path.join(TOOLS, "lz77"), src, dst], check=True)
    return open(dst, "rb").read()


def encode_group(rgb, tmp):
    """One group of RGB frames -> (palette as DS colours, [packets])."""
    pal8 = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
         "-s", f"{W}x{H}", "-i", "-", "-filter_complex", PALETTE, "-f", "rawvideo", "-pix_fmt", "pal8", "-"],
        input=rgb, check=True, capture_output=True).stdout
    size = W * H + 1024
    count = len(pal8) // size
    remap = bytes(min(i + 1, 255) for i in range(256))  # index 0 is the backdrop
    rgb_pal = bytearray(3)  # entry 0 unused
    ds_pal = [0] * 256
    for i in range(255):
        b, g, r, _ = pal8[W * H + 4 * i:W * H + 4 * i + 4]
        rgb_pal += bytes((r, g, b))
        ds_pal[i + 1] = (r >> 3) | (g >> 3) << 5 | (b >> 3) << 10
    rgb_pal += bytes(3 * 256 - len(rgb_pal))
    frames = b"".join(pal8[k * size:k * size + W * H].translate(remap) for k in range(count))
    out = subprocess.run([os.path.join(TOOLS, "rmvdelta"), str(W), str(H), MEAN, BIGGEST],
                         input=struct.pack("<I", count) + bytes(rgb_pal) + frames,
                         check=True, capture_output=True).stdout
    packets, p = [], 0
    for _ in range(count):
        n = struct.unpack_from("<I", out, p)[0]
        packets.append(lz77(out[p + 4:p + 4 + n], tmp))
        p += 4 + n
    return ds_pal, packets


def main():
    src, out = sys.argv[1], sys.argv[2]
    fps = int(sys.argv[sys.argv.index("--fps") + 1]) if "--fps" in sys.argv else FPS
    for tool in ("lz77", "rmvdelta"):
        if not os.path.exists(os.path.join(TOOLS, tool)):
            subprocess.run(["cc", "-O2", "-o", os.path.join(TOOLS, tool), os.path.join(TOOLS, tool + ".c")],
                           check=True)
    audio = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "psxstr", "-i", src, "-vn",
         "-ac", "1", "-ar", str(RATE), "-f", "s16le", "-"],
        check=True, capture_output=True).stdout
    video = subprocess.Popen(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "psxstr", "-i", src, "-an",
         "-vf", f"scale={W}:{H}:flags=lanczos,{DENOISE}", "-r", str(fps), "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        stdout=subprocess.PIPE)
    body, count, largest = bytearray(), 0, 0
    with tempfile.TemporaryDirectory() as tmp:
        while True:
            rgb = video.stdout.read(W * H * 3 * GROUP)
            if len(rgb) < W * H * 3:
                break
            palette, packets = encode_group(rgb[:len(rgb) // (W * H * 3) * W * H * 3], tmp)
            for k, lz in enumerate(packets):
                lz += bytes(-len(lz) % 4)
                largest = max(largest, len(lz))
                body += struct.pack("<I", len(lz) | (KEY if k == 0 else 0))
                if k == 0:
                    body += struct.pack("<256H", *palette)
                body += lz
                count += 1
    video.wait()
    samples = struct.unpack(f"<{len(audio) // 2}h", audio[:len(audio) // 2 * 2])
    head = struct.pack("<IHHHHIIII", MAGIC, W, H, fps, count, RATE, 28 + len(body), len(samples), largest)
    data = head + body + bytes((max(-128, min(127, (v + 128) >> 8)) & 0xFF) for v in samples)
    with open(out, "wb") as f:
        f.write(data)
    print(f"{out}: {count} frames ({count / fps:.1f} s), {len(samples) / RATE:.1f} s audio, "
          f"{len(data) / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
