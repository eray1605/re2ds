"""Item icons and names for the DS inventory screen (source/menu.c).

COMMON/DATA/ITEMALL.PIX: 106 item icons, 40 x 30 pixels at 8 bpp, no header;
their colours are palette 1 of the CLUT in ST0.TIM (the status screen's
graphics). Icon n belongs to item id n (icon 0 is the empty slot). The item names are in the
executable (file 0x871d4): one string per id from 1 on, each starting with
0xF7, in the room message encoding (tools/rdt2json.py MSG_CHARS).

Output menu/items.bin:
  u32 "RIT1", u16 icon count, u16 name count, u8 ui colours[8] (palette
  indices: black, dark, grey, light, white, green, yellow, red; set in
  entries the icons do not use), u16 palette[256], u8 icons[count][30][40],
  u16 name offsets[count] (from the start of the names), ASCII names, NUL each.

Usage: python3 -I menu2ds.py SLES_009.72 ST0.TIM ITEMALL.PIX out.bin
"""
import struct
import sys

MAGIC = 0x31544952  # "RIT1"
MSG = (" .___()_____0123456789:_,\"!?_ABCDEFGHIJKLMNOPQRSTUVWXYZ[/]'-_abc"
       "defghijklmnopqrstuvwxyz_________")
NAMES = 0x871D4
UI = [(0, 0, 0), (5, 6, 7), (12, 13, 14), (20, 21, 22), (31, 31, 31), (6, 26, 8), (30, 27, 6), (28, 4, 3),
      (2, 9, 3), (11, 9, 2), (10, 2, 2), (0, 0, 0)]


def names(exe):
    out, p = [], NAMES
    while exe[p] == 0xF7:
        p += 1
        s = ""
        while exe[p] != 0xF7:
            c = exe[p]
            if 0xEA <= c <= 0xFB and c != 0xF7: # control code with one argument
                s += "&" if c == 0xEE else ""
                p += 2
                continue
            if c >= 0x60:
                return out
            s += MSG[c]
            p += 1
        if s.strip() in ("", "_") or len(out) > 200:
            break
        out.append(s.replace("_", "").strip())
    return out


def main():
    exe = open(sys.argv[1], "rb").read()
    tim = open(sys.argv[2], "rb").read()
    pix = open(sys.argv[3], "rb").read()
    clen, _, _, cw, _ = struct.unpack_from("<IHHHH", tim, 8)
    pal = list(struct.unpack_from(f"<{cw}H", tim, 20 + 2 * cw))  # palette 1
    count = len(pix) // 1200
    # the icons use all 256 colours: take the nearest ones for the UI
    def rgb(v):
        return v & 31, v >> 5 & 31, v >> 10 & 31
    free = []
    for want in UI:
        best = min(range(1, 256), key=lambda i: sum((a - b) ** 2 for a, b in zip(rgb(pal[i]), want)))
        free.append(best)
    nm = names(exe)
    blob, offs = b"", []
    for n in nm:
        offs.append(len(blob))
        blob += n.encode("ascii") + b"\0"
    out = struct.pack("<IHH", MAGIC, count, len(nm)) + bytes(free) + struct.pack("<256H", *pal)
    # Leon's portrait (palette 2 of ST0.TIM shows the pictures in colour)
    o = 8 + clen
    _, _, _, iw, ih = struct.unpack_from("<IHHHH", tim, o)
    img, w = tim[o + 12:], iw * 2
    pal2 = struct.unpack_from(f"<{cw}H", tim, 20 + 4 * cw)
    cache = {}

    def nearest(v):
        if v not in cache:
            cache[v] = min(range(1, 256), key=lambda i: sum((a - b) ** 2 for a, b in zip(rgb(pal[i]), rgb(v))))
        return cache[v]
    pw, ph = 40, 46
    portrait = bytes(nearest(pal2[img[(210 + y) * w + x]]) for y in range(ph) for x in range(pw))
    out += pix[:count * 1200] + struct.pack("<HH", pw, ph) + portrait
    out += struct.pack(f"<{len(offs)}H", *offs) + blob
    open(sys.argv[4], "wb").write(out)
    print(f"{sys.argv[4]}: {count} icons, {len(nm)} names ({nm[0]} .. {nm[-1]}), ui colours {free}")


if __name__ == "__main__":
    main()
