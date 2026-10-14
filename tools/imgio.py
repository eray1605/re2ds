"""Tiny image helpers (stdlib only): PPM read, PNG write."""
import struct
import zlib


def read_ppm(path):
    """Return (width, height, bytearray RGB)."""
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    width, height = map(int, parts[1].split())
    return width, height, bytearray(parts[3][:width * height * 3])


def write_png(path, width, height, rgb):
    """Write 8-bit RGB pixels (bytes of length width*height*3) as PNG."""
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))
