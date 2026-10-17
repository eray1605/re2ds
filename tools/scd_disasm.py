"""Disassemble the room scripts (SCD) of an RE2 room (.RDT).

Prints every function of the init and main script with indentation for the
block opcodes. With --check, walks all given rooms and reports where the
length fields of the block opcodes point (used to pin down the semantics the
DS interpreter relies on).

Usage: python3 -I scd_disasm.py ROOM1000.RDT
       python3 -I scd_disasm.py --check ROOM*.RDT
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rdt2json import SCD_SIZES, read_header, script_instructions_from  # noqa: E402

NAMES = {
    0: "nop", 1: "evt_end", 2: "evt_next", 3: "evt_chain", 4: "evt_exec", 5: "evt_kill",
    6: "if", 7: "else", 8: "endif", 9: "sleep", 10: "sleeping", 11: "wsleep", 12: "wsleeping",
    13: "for", 14: "next", 15: "while", 16: "ewhile", 17: "do", 18: "edwhile", 19: "switch",
    20: "case", 21: "default", 22: "eswitch", 23: "goto", 24: "gosub", 25: "return", 26: "break",
    29: "work_copy", 32: "nop20", 33: "ck", 34: "set", 35: "cmp", 36: "save", 37: "copy",
    38: "calc", 39: "calc2", 40: "sce_rnd", 41: "cut_chg", 42: "cut_old", 43: "message_on",
    44: "aot_set", 45: "obj_model_set", 46: "work_set", 47: "speed_set", 48: "add_speed",
    49: "add_aspeed", 50: "pos_set", 51: "dir_set", 52: "member_set", 53: "member_set2",
    54: "se_on", 55: "sca_id_set", 57: "dir_ck", 58: "sce_espr_on", 59: "door_aot_set",
    60: "cut_auto", 61: "member_copy", 62: "member_cmp", 63: "plc_motion", 64: "plc_dest",
    65: "plc_neck", 66: "plc_ret", 67: "plc_flag", 68: "sce_em_set", 70: "aot_reset",
    71: "aot_on", 72: "super_set", 75: "cut_replace", 76: "sce_espr_kill", 77: "door_model_set",
    78: "item_aot_set", 80: "sce_trg_ck", 81: "sce_bgm_control", 82: "sce_espr_control",
    83: "sce_fade_set", 84: "sce_espr3d_on", 87: "sce_bgmtbl_set", 88: "plc_rot", 89: "xa_on",
    90: "weapon_chg", 91: "plc_cnt", 92: "sce_shake_on", 93: "mizu_div_set", 94: "keep_item_ck",
    95: "xa_vol", 96: "kage_set", 97: "cut_be_set", 98: "sce_item_lost", 99: "plc_gun_eff",
    100: "sce_espr_on2", 101: "sce_espr_kill2", 102: "plc_stop", 103: "aot_set_4p",
    104: "door_aot_set_4p", 105: "item_aot_set_4p", 106: "light_pos_set", 107: "light_kido_set",
    108: "rbj_reset", 109: "sce_scr_move", 110: "parts_set", 111: "movie_on",
    122: "sce_parts_bomb", 123: "sce_parts_down",
}
BLOCK_OPEN = {6, 13, 15, 17, 19}
BLOCK_CLOSE = {8, 14, 16, 18, 22}


def functions(data, offset):
    """[(start, end)] of every function in an SCD block (end = after EVT_END)."""
    first = struct.unpack_from("<H", data, offset)[0]
    starts = [offset + struct.unpack_from("<H", data, offset + i)[0] for i in range(0, first, 2)]
    out = []
    for start in starts:
        pos = start
        for _, pos in script_instructions_from(data, start):
            pass
        out.append((start, pos))
    return out


def instructions(data, start, end):
    pos = start
    while pos < end:
        op = data[pos]
        yield pos, op, data[pos:pos + SCD_SIZES[op]]
        pos += SCD_SIZES[op]


def disasm(path):
    data = open(path, "rb").read()
    _, offsets = read_header(data)
    for key in ("init_script", "exec_script"):
        if not offsets.get(key):
            continue
        for n, (start, end) in enumerate(functions(data, offsets[key])):
            print(f"\n{key} function {n}:")
            depth = 0
            for pos, op, raw in instructions(data, start, end):
                if op in BLOCK_CLOSE or op in (7, 20, 21):
                    depth = max(depth - 1, 0)
                args = raw[1:].hex(" ")
                extra = ""
                if op in (6, 7, 15, 17):
                    extra = f"  -> {pos + SCD_SIZES[op] + struct.unpack_from('<H', raw, 2)[0] - start:+}"
                print(f"  {pos - start:5d}  {'  ' * depth}{NAMES.get(op, op)} {args}{extra}")
                if op in BLOCK_OPEN or op in (7, 20, 21):
                    depth += 1


def check(paths):
    """Where do the length fields point? Histogram of the opcode found there."""
    stats = collections.defaultdict(collections.Counter)
    for path in paths:
        data = open(path, "rb").read()
        _, offsets = read_header(data)
        for key in ("init_script", "exec_script"):
            if not offsets.get(key):
                continue
            for start, end in functions(data, offsets[key]):
                for pos, op, raw in instructions(data, start, end):
                    if op not in (6, 7, 13, 15, 17, 19, 20):
                        continue
                    length = struct.unpack_from("<H", raw, 2)[0]
                    target = pos + SCD_SIZES[op] + length
                    before = [p for p, _, _ in instructions(data, start, end) if p < target]
                    prev_op = data[before[-1]] if before else None
                    stats[NAMES[op]][f"target={NAMES.get(data[target], data[target])}, "
                                     f"before={NAMES.get(prev_op, prev_op)}"] += 1
    for op, counter in stats.items():
        print(op)
        for k, v in counter.most_common(6):
            print(f"   {v:5d}  {k}")


if __name__ == "__main__":
    if sys.argv[1] == "--check":
        check(sys.argv[2:])
    else:
        disasm(sys.argv[1])
