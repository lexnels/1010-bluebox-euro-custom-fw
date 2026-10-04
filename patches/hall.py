"""Lush hall: a 16th reverb style running an 8-line modulated FDN (src/hall_dsp.h, src/hall_m7.c)."""
import os, struct
from thumb import ROOT, bl, symbols, cave

NAME = b"Lush Hall\0"
s7 = symbols(os.path.join(ROOT, "out", "hall7.elf"))
code = cave("hall7")

def u32(v):
    return struct.pack("<I", v)

def style_list(base, stock_names):
    """16 string pointers at base, the new name right after them."""
    names = stock_names + [base + 0x40]
    return b"".join(u32(p) for p in names) + NAME

# the 15 stock style names, as the stock lists point at them
M7_NAMES = [0x0806A2D8, 0x0806A2E8, 0x0806A2F4, 0x0806A300, 0x0806A30C, 0x0806A31C, 0x0806A328, 0x0806A334,
            0x0806A33C, 0x0806A348, 0x0806A354, 0x0806A35C, 0x0806A364, 0x0806A378, 0x0806A384]
M4_NAMES = [0x0814A3C4, 0x0814A3D4, 0x0814A3E0, 0x0814A3EC, 0x0814A3F8, 0x0814A408, 0x0814A414, 0x0814A420,
            0x0814A428, 0x0814A434, 0x0814A440, 0x0814A448, 0x0814A450, 0x0814A464, 0x0814A470]
M7_LIST, M4_LIST = 0x080CF000, 0x080D0000
assert 0x080C0000 + len(code) <= M7_LIST, "hall cave overlaps its style list"
m7_list, m4_list = style_list(M7_LIST, M7_NAMES), style_list(M4_LIST, M4_NAMES)

PATCHES = [
    (0x080C0000, bytes(len(code)), code),
    (M7_LIST, bytes(len(m7_list)), m7_list),
    (M4_LIST, bytes(len(m4_list)), m4_list),
    # M7: reverb process FUN_08048de8, its event loop's setter call -> hall_set
    (0x08048E56, bl(0x08048E56, 0x080451EC), bl(0x08048E56, s7["hall_set"])),
    # M7: reverb vtable 0x0806ada0, slot 3 (process) -> hall_process
    (0x0806ADAC, u32(0x08048DE9), u32(s7["hall_process"] | 1)),
    # M7: param table init FUN_08053de4, Style (0x155) entry: name list and count 15 -> 16 (movs r0, #0xf)
    (0x08055DA0, u32(0x0806BADC), u32(M7_LIST)),
    (0x08055BA2, bytes.fromhex("0f20"), bytes.fromhex("1020")),
    # M4: reverb page params, Style (0x155): name list and count 15 -> 16 (movs r3, #0xf)
    (0x081370D4, u32(0x0814E70C), u32(M4_LIST)),
    (0x08136EE8, bytes.fromhex("0f23"), bytes.fromhex("1023")),
]
