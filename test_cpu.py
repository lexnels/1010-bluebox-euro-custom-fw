"""Run the CPU meter's two hooks from the patched image under Unicorn, with the firmware calls they make stubbed."""
import struct, sys
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import *

IMG = sys.argv[1] if len(sys.argv) > 1 else "out/cpu/BLUEEURO.BIN"
data = open(IMG, "rb").read()
RET = 0x0807FFF0
sys.path.insert(0, "patches")
from thumb import symbols
s7, s4 = symbols("out/cm7.elf"), symbols("out/cm4.elf")

def machine():
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    uc.mem_map(0x08000000, 0x200000); uc.mem_write(0x08040000, data[:0xA0000]); uc.mem_write(0x08100000, data[0xC0000:])
    for base, size in [(0x24000000, 0x80000), (0x38800000, 0x1000), (0x58024000, 0x1000), (0xE000E000, 0x1000), (0xC0000000, 0x40000)]:
        uc.mem_map(base, size)
    uc.reg_write(UC_ARM_REG_SP, 0x2407F000)
    return uc

def stub(uc, addr, fn):
    def h(uc, a, size, _):
        fn(uc)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, h, begin=addr, end=addr)

def call(uc, fn, *args):
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args): uc.reg_write(r, v)
    uc.reg_write(UC_ARM_REG_LR, RET | 1)
    uc.emu_start(fn | 1, RET, count=200000)

def check(cond, msg):
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond: sys.exit(1)

# ---- the patched call sites point at the caves
def bl_target(at):
    o = at - (0x08040000 if at < 0x080E0000 else 0x08100000 - 0xC0000)
    h1, h2 = struct.unpack_from("<HH", data, o)
    s = (h1 >> 10) & 1; i1 = 1 - (((h2 >> 13) & 1) ^ s); i2 = 1 - (((h2 >> 11) & 1) ^ s)
    imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
    return at + 4 + (imm - (1 << 25) if s else imm)
check(bl_target(0x0804CA10) == s7["cpu_prof_end"], "M7 profiler-exit call goes to cpu_prof_end")
check(bl_target(0x0813861A) == s4["cpu_present"], "M4 flip call goes to cpu_present")

# ---- M7: feed blocks of known load
uc = machine()
ENG, RING = 0x24010000, 0x24020000
uc.mem_write(ENG + 0x3E58, struct.pack("<II", RING, 0))
blocks = {"n": 0}
def prof_end(uc):                      # what FUN_0804c040 does: append {period, busy}, count++
    n = blocks["n"]; period = 640000; busy = 192000 if n % 10 else 448000   # 30 % normally, 70 % every 10th
    cnt = struct.unpack("<I", uc.mem_read(ENG + 0x3E5C, 4))[0]
    uc.mem_write(RING + (cnt & 0x7FF) * 8, struct.pack("<II", period, busy))
    uc.mem_write(ENG + 0x3E5C, struct.pack("<I", cnt + 1))
    blocks["n"] += 1
stub(uc, 0x0804C040, prof_end)
for _ in range(400):                   # 400 x 640k cycles = 256M cycles: at least one report
    call(uc, s7["cpu_prof_end"], ENG)
magic, avg, peak, seq = struct.unpack("<IHHI", uc.mem_read(0x38800F00, 12))
print(f"     shared: magic {magic:#x} avg {avg} peak {peak} seq {seq}")
check(magic == 0x43505542 and seq >= 1, "M7 published a report")
check(320 <= avg <= 345, "average load ~33 % (30 % blocks, one in ten at 70 %)")
check(695 <= peak <= 700, "peak ~70 %")
check(struct.unpack("<I", uc.mem_read(0x58024800, 4))[0] & 0x100, "backup-domain write access enabled")
check(struct.unpack("<I", uc.mem_read(0x580244E0, 4))[0] & (1 << 28), "backup SRAM clock enabled")

# ---- M4: draw into an RGB565 back buffer
for fmt, bpp in [(4, 2), (1, 4)]:
    uc = machine()
    DESC, FB, DISP = 0x24030000, 0xC0000000, 0x24040000
    uc.mem_write(DESC, struct.pack("<IHHBBH", FB, 320, 240, fmt, 0, bpp))
    uc.mem_write(0x38800F00, struct.pack("<IHHI", 0x43505542, 500, 800, 1))
    seen = {}
    def backbuf(uc):
        seen["bb"] = (uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1)); uc.reg_write(UC_ARM_REG_R0, DESC)
    def flip(uc):
        seen["flip"] = (uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1))
    stub(uc, 0x081013D2, backbuf); stub(uc, 0x08101384, flip)
    call(uc, s4["cpu_present"], DISP, 1, 0, 0)
    check(seen.get("bb") == (DISP, 1) and seen.get("flip") == (DISP, 1), f"fmt {fmt}: back buffer fetched, then flipped as stock")
    def px(x, row):
        b = uc.mem_read(FB + (row * 320 + x) * bpp, bpp)
        return struct.unpack("<H" if bpp == 2 else "<I", b)[0]
    green = 0x064A if bpp == 2 else 0xFF00C850
    grey = 0x3186 if bpp == 2 else 0xFF303030
    white = 0xFFFF if bpp == 2 else 0xFFFFFFFF
    col = [px(315, r) for r in range(3, 17)]                       # top to bottom
    print("     column:", " ".join(f"{c:x}" for c in col))
    check(col[-7:] == [green] * 7, f"fmt {fmt}: bottom 7 of 14 rows lit at 50 %")
    check(col[14 - 11] == white, f"fmt {fmt}: peak marker at 80 % (row 11 from the bottom)")
    check(col[0] == grey, f"fmt {fmt}: top row unlit")
    check(px(313, 10) == 0 and px(317, 10) == 0 and px(315, 2) == 0 and px(315, 17) == 0, f"fmt {fmt}: nothing outside the bar")

# no data from the M7 yet: draw nothing
uc = machine()
uc.mem_write(0x24030000, struct.pack("<IHHBBH", 0xC0000000, 320, 240, 4, 0, 2))
stub(uc, 0x081013D2, lambda uc: uc.reg_write(UC_ARM_REG_R0, 0x24030000)); stub(uc, 0x08101384, lambda uc: None)
call(uc, s4["cpu_present"], 0x24040000, 1, 0, 0)
check(bytes(uc.mem_read(0xC0000000, 320 * 20 * 2)) == bytes(320 * 20 * 2), "no M7 data: screen untouched")
print("all passed")
