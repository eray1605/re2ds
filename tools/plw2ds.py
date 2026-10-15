"""Leon with a weapon: one model per weapon set PL00Wnn.PLW.

A PLW holds the 17 moves of that weapon (walk / run / stand for Fine,
Caution and Danger, raise, aim and fire straight / up / down, ...) and the
right hand holding it (mesh 0; mesh 1 is the muzzle flash). The hand
replaces bone 11 of the PLD (the right hand: its shape matches bone 11, and
only there does the barrel point along the aim) and uses Leon's texture page 1.

The PLW's own TIM (section 3, 8 bpp 56x32) is the weapon's texture: the
game copies it over the bottom right corner of page 1 (u 72, v 224, left
grey in the PLD) and its colours 240..255 into page 1's palette (unused
there, 0x8000). The hand's UVs point into that corner.

Output out_prefix.bin like model2ds.py (Leon's texture stays model/leon_tex.bin)
and out_prefix_tex.bin, the patch: u16 page, x, y, width, height, first
colour, colour count, 0; the colours; width * height indices.

Usage: python3 -I plw2ds.py PL00.PLD PL00W02.PLW work_dir out_prefix
"""
import json
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pld2json  # noqa: E402
import tim2png  # noqa: E402

HAND_BONE = 11
PATCH_PAGE, PATCH_FIRST_COLOR, PATCH_COLORS = 1, 240, 16
PAGE_W, PAGE_H = 128, 256


def weapon_patch(d, off):
    tim = tim2png.parse_tim(d, off)
    assert tim and tim["bpp"] == 8, "PLW texture: expected an 8 bpp TIM"
    w, h = tim["width"], tim["height"]
    pal = tim["palettes"][0][PATCH_FIRST_COLOR:PATCH_FIRST_COLOR + PATCH_COLORS]
    head = struct.pack("<8H", PATCH_PAGE, PAGE_W - w, PAGE_H - h, w, h, PATCH_FIRST_COLOR, PATCH_COLORS, 0)
    return head + struct.pack(f"<{PATCH_COLORS}H", *pal) + bytes(tim2png.tim_indices(tim))


def main():
    pld, plw, work, out = sys.argv[1:5]
    tools = os.path.dirname(os.path.abspath(__file__))
    os.makedirs(work, exist_ok=True)
    base = os.path.join(work, os.path.basename(out))
    subprocess.run([sys.executable, "-I", os.path.join(tools, "pld2json.py"), pld, base, "--plw", plw],
                   check=True, stdout=subprocess.DEVNULL)
    model = json.load(open(base + ".json"))
    d = open(plw, "rb").read()
    dir_off, count = struct.unpack_from("<II", d, 0)
    secs = struct.unpack_from(f"<{count}I", d, dir_off)
    meshes = pld2json.parse_md1(d, secs[2])
    model["meshes"][HAND_BONE] = meshes[0]
    json.dump(model, open(base + ".json", "w"))
    subprocess.run([sys.executable, "-I", os.path.join(tools, "model2ds.py"), base + ".json", pld, out],
                   check=True, stdout=subprocess.DEVNULL)
    with open(out + "_tex.bin", "wb") as f:
        f.write(weapon_patch(d, secs[3]))
    print(f"{out}.bin: {os.path.basename(plw)}, hand {len(meshes[0]['faces'])} faces")


if __name__ == "__main__":
    main()
