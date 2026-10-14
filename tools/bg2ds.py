"""Convert decoded backgrounds (bss2ppm output, 320x240) to DS bitmaps.

Each camera becomes 256x192 BGR555 with bit 15 set (see source/formats.h).
Scaling (Lanczos) and the reduction to 5 bits per channel (with dithering) are
done by ffmpeg; a PNG preview of the DS image is written next to each file.

Usage: python3 -I bg2ds.py in_prefix out_dir [preview_dir]
       (reads in_prefix_camNN.ppm)
"""
import glob
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imgio import write_png  # noqa: E402

W, H = 256, 192


def convert(src):
    raw = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", src,
         "-vf", f"scale={W}:{H}:flags=lanczos", "-sws_dither", "ed",
         "-f", "rawvideo", "-pix_fmt", "bgr555le", "-"],
        check=True, capture_output=True).stdout
    assert len(raw) == W * H * 2, f"unexpected size {len(raw)}"
    words = struct.unpack(f"<{W * H}H", raw)
    return [w | 0x8000 for w in words]


def to_rgb(words):
    rgb = bytearray(W * H * 3)
    for i, c in enumerate(words):
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
    for src in sorted(glob.glob(prefix + "_cam*.ppm")):
        name = os.path.basename(src)[:-4]
        words = convert(src)
        with open(os.path.join(out_dir, name + ".bin"), "wb") as f:
            f.write(struct.pack(f"<{W * H}H", *words))
        if preview_dir:
            write_png(os.path.join(preview_dir, name + ".png"), W, H, to_rgb(words))
        print(f"{name}.bin ({W * H * 2} bytes)")


if __name__ == "__main__":
    main()
