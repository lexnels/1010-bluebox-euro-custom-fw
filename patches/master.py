"""Master bus: a compressor meter (the CPU meter shows it on the settings page) and a saturator after the compressor
with a Saturate control in the settings list.

M7 code is src/mst_m7.c (out/mst7); the M4 side is in src/dly_m4.c (out/dly4: the Saturate definition and the set
add) and src/cm4_cpu.c (out/cm4: the meter). Needs the cpu and delay patchsets.
"""
import os, struct
from thumb import ROOT, bl, symbols, cave

s7 = symbols(os.path.join(ROOT, "out", "mst7.elf"))
s4 = symbols(os.path.join(ROOT, "out", "dly4.elf"))
c4 = symbols(os.path.join(ROOT, "out", "cm4.elf"))
code7 = cave("mst7")

PATCHES = [
    (0x080A0000, bytes(len(code7)), code7),
    # M7: master compressor vtable 0x0806acb0, slot 3 (process FUN_08042a90) -> mst_process
    (0x0806ACBC, struct.pack("<I", 0x08042A91), struct.pack("<I", s7["mst_process"] | 1)),
    # M4: global set (FUN_0812060c case 2), its last add (id 0x162) -> mst_set_add, which adds Saturate (0x43) after it
    (0x08120B90, bl(0x08120B90, 0x081205E2), bl(0x08120B90, s4["mst_set_add"])),
    # M4: the settings page's id list (0x0814d91c, 0-terminated, after the compressor's 0xb5, 0x130..0x13b): + 0x43
    (0x0814D966, bytes(2), struct.pack("<H", 0x43)),
    # M4: UI loop FUN_08135180, its redraw check FUN_0813aa9e -> comp_tick (src/cm4_cpu.c), which keeps the settings
    # list's gain-reduction bar moving
    (0x081351A6, bl(0x081351A6, 0x0813AA9E), bl(0x081351A6, c4["comp_tick"])),
]
