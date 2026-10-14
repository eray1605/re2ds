"""Convert decoded backgrounds (bss2ppm output, 320x240) to DS bitmaps.

Each camera becomes a 256x192 8 bpp image with its own 256-colour palette,
LZ77-compressed for the DS BIOS (see source/formats.h, BgHeader). ffmpeg
scales (Lanczos) and quantises to 255 colours with error diffusion; index 0
stays unused because it is transparent on an 8 bpp bitmap background.
A PNG preview of the DS image is written next to each file when asked.

Usage: python3 -I bg2ds.py in_prefix out_dir [preview_dir]
       (reads in_prefix_camNN.ppm, needs tools/lz77)
"""
import glob
import os
import struct
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
from imgio import write_png  # noqa: E402

W, H = 256, 192
BG_MAGIC = 0x31474252  # "RBG1"
FILTER = (f"scale={W}:{H}:flags=lanczos,split[a][b];"
          "[a]palettegen=max_colors=255:reserve_transparent=0:stats_mode=full[p];"
          "[b][p]paletteuse=dither=sierra2_4a")


def quantise(src):
    """Return (palette as 256 BGR555 words, indices 1..255)."""
    raw = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", src, "-filter_complex", FILTER,
         "-f", "rawvideo", "-pix_fmt", "pal8", "-"],
        check=True, capture_output=True).stdout
    assert len(raw) == W * H + 1024, f"unexpected size {len(raw)}"
    indices = raw[:W * H].translate(bytes(min(i + 1, 255) for i in range(256)))
    palette = [0] * 256
    for i in range(255):
        b, g, r, _ = raw[W * H + 4 * i:W * H + 4 * i + 4]
        palette[i + 1] = (r >> 3) | (g >> 3) << 5 | (b >> 3) << 10
    return palette, indices


def lz77(data):
    with tempfile.TemporaryDirectory() as tmp:
        src, dst = os.path.join(tmp, "in"), os.path.join(tmp, "out")
        with open(src, "wb") as f:
            f.write(data)
        subprocess.run([os.path.join(TOOLS, "lz77"), src, dst], check=True)
        return open(dst, "rb").read()


def lz77_decompress(blob):
    """Reference decoder, used to check every file."""
    size = struct.unpack_from("<I", blob)[0] >> 8
    out, pos = bytearray(), 4
    while len(out) < size:
        flags = blob[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                length = (blob[pos] >> 4) + 3
                dist = ((blob[pos] & 0xF) << 8 | blob[pos + 1]) + 1
                pos += 2
                for _ in range(length):
                    out.append(out[-dist])
            else:
                out.append(blob[pos])
                pos += 1
    return bytes(out)


def to_rgb(palette, indices):
    rgb = bytearray(W * H * 3)
    for i, v in enumerate(indices):
        c = palette[v]
        rgb[i * 3] = (c & 31) * 255 // 31
        rgb[i * 3 + 1] = ((c >> 5) & 31) * 255 // 31
        rgb[i * 3 + 2] = ((c >> 10) & 31) * 255 // 31
    return rgb


def main():
    prefix, out_dir = sys.argv[1], sys.argv[2]
    preview_dir = sys.argv[3] if len(sys.argv) > 3 else None
    os.makedirs(out_dir, exist_ok=True)
    if preview_dir:
        os.makedirs(preview_dir, exist_ok=True)
    total = 0
    for src in sorted(glob.glob(prefix + "_cam*.ppm")):
        name = os.path.basename(src)[:-4]
        palette, indices = quantise(src)
        packed = lz77(indices)
        assert lz77_decompress(packed) == indices, f"{name}: LZ77 round trip failed"
        blob = struct.pack("<I256H", BG_MAGIC, *palette) + packed
        with open(os.path.join(out_dir, name + ".bin"), "wb") as f:
            f.write(blob)
        if preview_dir:
            write_png(os.path.join(preview_dir, name + ".png"), W, H, to_rgb(palette, indices))
        total += len(blob)
    print(f"{prefix}: {total // 1024} KB")


if __name__ == "__main__":
    main()
