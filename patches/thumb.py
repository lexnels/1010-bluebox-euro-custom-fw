"""Thumb-2 encoding helpers for patchsets."""
import os, struct, subprocess
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def bl(at, target):
    """BL from `at` to Thumb function `target` (bit 0 ignored)."""
    off = (target & ~1) - (at + 4)
    assert -(1 << 24) <= off < (1 << 24) and off % 2 == 0, f"BL out of range {at:#x}->{target:#x}"
    s = (off >> 24) & 1
    i1, i2 = (off >> 23) & 1, (off >> 22) & 1
    j1, j2 = (1 - i1) ^ s, (1 - i2) ^ s
    return struct.pack("<HH", 0xF000 | (s << 10) | ((off >> 12) & 0x3FF), 0xD000 | (j1 << 13) | (j2 << 11) | ((off >> 1) & 0x7FF))

def symbols(elf):
    out = subprocess.run(["arm-none-eabi-nm", elf], capture_output=True, text=True, check=True).stdout
    return {n: int(a, 16) for a, k, n in (l.split() for l in out.splitlines() if len(l.split()) == 3)}

def cave(name):
    return open(os.path.join(ROOT, "out", name + ".bin"), "rb").read()
