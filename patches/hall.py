"""Extra reverb styles after the stock 15: Lush Hall, MVerb, Squall, Freeverb (src/hall_m7.c and the engines it includes)."""
import os, struct
from thumb import ROOT, bl, symbols, cave

NAMES = [b"Lush Hall", b"MVerb", b"Squall", b"Freeverb"]   # order = style 15, 16, 17, 18
s7 = symbols(os.path.join(ROOT, "out", "hall7.elf"))
code = cave("hall7")

def u32(v):
    return struct.pack("<I", v)

COUNT = 15 + len(NAMES)

def style_list(base, stock_names):
    """COUNT string pointers at base, the new names right after them."""
    strings, ptrs, at = b"", [], base + 4 * COUNT
    for n in NAMES:
        ptrs.append(at + len(strings))
        strings += n + b"\0"
    return b"".join(u32(p) for p in stock_names + ptrs) + strings

# the 15 stock style names, as the stock lists point at them
M7_NAMES = [0x0806A2D8, 0x0806A2E8, 0x0806A2F4, 0x0806A300, 0x0806A30C, 0x0806A31C, 0x0806A328, 0x0806A334,
            0x0806A33C, 0x0806A348, 0x0806A354, 0x0806A35C, 0x0806A364, 0x0806A378, 0x0806A384]
M4_NAMES = [0x0814A3C4, 0x0814A3D4, 0x0814A3E0, 0x0814A3EC, 0x0814A3F8, 0x0814A408, 0x0814A414, 0x0814A420,
            0x0814A428, 0x0814A434, 0x0814A440, 0x0814A448, 0x0814A450, 0x0814A464, 0x0814A470]
M7_LIST, M4_LIST = 0x080CF000, 0x080D0000
assert 0x080C0000 + len(code) <= M7_LIST, "hall cave overlaps its style list"
m7_list, m4_list = style_list(M7_LIST, M7_NAMES), style_list(M4_LIST, M4_NAMES)

# M4: the reverb slot's param list (FUN_0812060c, case 5 @0x08120c1c). That list decides which knobs the FX2 panel
# shows (in list order, Style skipped), what projects and presets save, and what is re-sent to the M7 on a full sync.
# Stock adds 7 ids with straight-line calls; this replaces them with a loop over a table that adds Diffusion and Spread.
# The panel reaches 8 knobs with the encoders (2 pages of 4), so this fills it. Style must stay first: on a sync it is
# applied before the values it would otherwise overwrite.
PANEL_AT = 0x08120C1C
PANEL_STOCK = bytes.fromhex(
    "002240f255112046fff7ddfc4ff47a7240f259112046fff7d6fc4ff47a724ff4ad712046fff7cffc4ff47a724ff4a4712046fff7c8fc"
    "00224ff4a5712046fff7c2fc40f2db1240f23d112046fff7bbfc002240f24f112046fff7b5fc08e6")
PANEL_IDS = [(0x155, 0),                                        # Style (not a knob)
             (0x159, 1000), (0x15A, 1000), (0x13E, 800), (0x146, 0),  # page 1: Time, Level, Diffusion, Spread
             (0x13D, 0x1DB), (0x14A, 0), (0x148, 1000), (0x14F, 0)]   # page 2: Pre Delay, Low Cut, HI C, Freeze
PANEL_CODE = bytes.fromhex(
    "48b4"          # push {r3, r6} (r3 keeps the stack 8-byte aligned)
    "06a6"          # adr r6, table (0x08120c38)
    "3188"          # loop: ldrh r1, [r6]
    "31b1"          # cbz r1, done
    "b6f90220"      # ldrsh.w r2, [r6, #2]
    "2046"          # mov r0, r4 (the set)
    "fff7dafc"      # bl FUN_081205e2 (append id, value)
    "0436"          # adds r6, #4
    "f6e7"          # b loop
    "48bc"          # done: pop {r3, r6}
    "fff72abe")     # b.w 0x0812088c (common tail: needs r4, r5)
def panel_patch():
    t = PANEL_CODE + b"".join(struct.pack("<Hh", i, v) for i, v in PANEL_IDS) + b"\0\0"
    assert len(PANEL_IDS) <= 16 and len(t) <= len(PANEL_STOCK)
    return t + bytes.fromhex("00bf") * ((len(PANEL_STOCK) - len(t)) // 2)

PATCHES = [
    (0x080C0000, bytes(len(code)), code),
    (M7_LIST, bytes(len(m7_list)), m7_list),
    (M4_LIST, bytes(len(m4_list)), m4_list),
    # M7: reverb process FUN_08048de8, its event loop's setter call -> hall_set
    (0x08048E56, bl(0x08048E56, 0x080451EC), bl(0x08048E56, s7["hall_set"])),
    # M7: reverb vtable 0x0806ada0, slot 3 (process) -> hall_process
    (0x0806ADAC, u32(0x08048DE9), u32(s7["hall_process"] | 1)),
    # M7: param table init FUN_08053de4, Style (0x155) entry: name list and count 15 -> COUNT (movs r0, #0xf)
    (0x08055DA0, u32(0x0806BADC), u32(M7_LIST)),
    (0x08055BA2, bytes.fromhex("0f20"), bytes([COUNT, 0x20])),
    # M4: reverb page params, Style (0x155): name list and count 15 -> COUNT (movs r3, #0xf)
    (0x081370D4, u32(0x0814E70C), u32(M4_LIST)),
    (0x08136EE8, bytes.fromhex("0f23"), bytes([COUNT, 0x23])),
    # M4: Diffusion and Spread on the reverb panel
    (PANEL_AT, PANEL_STOCK, panel_patch()),
    # M7: reverb process, after a style change: also send Diffusion and Spread back to the M4 (bl FUN_0804467c)
    (0x08048E6E, bl(0x08048E6E, 0x0804467C), bl(0x08048E6E, s7["hall_echo"])),
]
