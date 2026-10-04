"""CPU meter: a thin bar at the top right, right of the clock (src/cm7_cpu.c, src/cm4_cpu.c)."""
import os
from thumb import ROOT, bl, symbols, cave

s7 = symbols(os.path.join(ROOT, "out", "cm7.elf"))
s4 = symbols(os.path.join(ROOT, "out", "cm4.elf"))

def hook(at, stock, target):
    return (at, bl(at, stock), bl(at, target))

PATCHES = [
    # code caves in the zero-filled gap of the main image
    (0x080B0000, bytes(len(cave("cm7"))), cave("cm7")),
    (0x080B8000, bytes(len(cave("cm4"))), cave("cm4")),
    # M7: audio callback FUN_0804c778, its profiler-exit call
    hook(0x0804CA10, 0x0804C040, s7["cpu_prof_end"]),
    # M4: frame presenter FUN_081385dc, its flip call
    hook(0x0813861A, 0x08101384, s4["cpu_present"]),
    # M4: mixer top bar constructor FUN_0813ab34, clock label width 140 -> 134 (movs r3, #0x8c -> #0x86)
    (0x0813ACEC, bytes.fromhex("8c23"), bytes.fromhex("8623")),
]
