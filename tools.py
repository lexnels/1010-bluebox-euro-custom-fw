"""Helpers: image access by address, Thumb disassembly, BL finding."""
import struct, capstone
FW = "firmware/BLUEEURO-3.bin"
data = open(__import__("os").path.join(__import__("os").path.dirname(__file__), FW), "rb").read()
IMAGES = [(0x08040000, 0x000000, 0x0A0000), (0x080E0000, 0x0A0000, 0x0C0000), (0x08100000, 0x0C0000, len(data))]
def off(a):
    for base, lo, hi in IMAGES:
        if base <= a < base + (hi - lo): return lo + a - base
    raise ValueError(hex(a))
def u32(a): return struct.unpack_from("<I", data, off(a))[0]
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS)
def dis(a, n=40):
    o = off(a); return list(md.disasm(data[o:o + n * 4], a))[:n]
def calls(lo, hi, target):
    return [i.address for i in dis(lo, (hi - lo) // 2) if i.address < hi and i.mnemonic in ("bl", "b.w", "blx") and i.op_str == "#%#x" % target]
def find_branches(target, lo=0x08040000, hi=0x08073000):
    """Brute-force: decode a 32-bit BL/B.W at every halfword and report those landing on target."""
    out = []
    for a in range(lo, hi, 2):
        o = off(a)
        h1, h2 = struct.unpack_from("<HH", data, o)
        if (h1 & 0xF800) == 0xF000 and (h2 & 0xD000) in (0xD000, 0x9000):
            s = (h1 >> 10) & 1; j1 = (h2 >> 13) & 1; j2 = (h2 >> 11) & 1
            i1 = 1 - (j1 ^ s); i2 = 1 - (j2 ^ s)
            imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
            if s: imm -= 1 << 25
            if a + 4 + imm == target: out.append((a, "bl" if h2 & 0x4000 else "b.w"))
    return out
