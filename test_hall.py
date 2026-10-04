"""Run the hall reverb's hooks from the patched image under Unicorn (Cortex-M7 model).

The stock helpers the hooks call to find the audio buffers and the parameter events run for real; the stock
setter, the stock reverb process and the UI echo are stubbed and recorded.
"""
import math, struct, sys
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import *

IMG = sys.argv[1] if len(sys.argv) > 1 else "out/cpu+hall/BLUEEURO.BIN"
data = open(IMG, "rb").read()
RET = 0x0807FFF0
sys.path.insert(0, "patches")
from thumb import symbols
s7 = symbols("out/hall7.elf")

OBJ, NEXT, CTX, MEM = 0x24010000, 0x24012000, 0x30000000, 0xC0000000
NEXT_PROC = 0x24014000                     # fake process of the next node in the chain
PORT, QID, N = 3, 5, 32
BUS = CTX + PORT * 20 + 0x150E4
QUEUE = CTX + QID * 0x604 + 4
BUFL, BUFR = 0x24020000, 0x24021000

def f2u(f): return struct.unpack("<I", struct.pack("<f", f))[0]
def rd(uc, a, fmt): return struct.unpack(fmt, uc.mem_read(a, struct.calcsize(fmt)))
def check(cond, msg):
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond: sys.exit(1)

def file_off(a): return a - 0x08040000 if a < 0x08100000 else a - 0x08100000 + 0xC0000
def u32(a): return struct.unpack_from("<I", data, file_off(a))[0]
def cstr(a): return data[file_off(a):].split(b"\0")[0].decode()

uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M7)
uc.mem_map(0x08000000, 0x200000); uc.mem_write(0x08040000, data[:0xA0000]); uc.mem_write(0x08100000, data[0xC0000:])
for base, size in [(0x24000000, 0x80000), (0x30000000, 0x40000), (0x38800000, 0x1000), (0x58024000, 0x1000), (MEM, 0x100000)]:
    uc.mem_map(base, size)
uc.reg_write(UC_ARM_REG_SP, 0x2407F000)

calls = {"set": [], "process": 0, "echo": 0, "next": 0}
def stub(addr, fn, ret=None):
    def h(uc, a, size, _):
        fn(uc)
        if ret is not None: uc.reg_write(UC_ARM_REG_R0, ret)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, h, begin=addr, end=addr)
stub(0x080451EC, lambda uc: calls["set"].append((uc.reg_read(UC_ARM_REG_R1), struct.unpack("<f", struct.pack("<I", uc.reg_read(UC_ARM_REG_S0)))[0])))
stub(0x08048DE8, lambda uc: calls.__setitem__("process", calls["process"] + 1), ret=1)
stub(0x0804467C, lambda uc: calls.__setitem__("echo", calls["echo"] + 1))
stub(NEXT_PROC, lambda uc: calls.__setitem__("next", calls["next"] + 1), ret=7)

icount = {"n": 0}
def count(uc, a, s, _): icount["n"] += 1

def call(fn, *args, s0=None):
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args): uc.reg_write(r, v)
    if s0 is not None: uc.reg_write(UC_ARM_REG_S0, f2u(s0))
    uc.reg_write(UC_ARM_REG_LR, RET | 1)
    uc.emu_start(fn | 1, RET, count=50_000_000)
    return uc.reg_read(UC_ARM_REG_R0)

def events(evs):
    for i, (pid, val) in enumerate(evs):
        uc.mem_write(QUEUE + i * 24, struct.pack("<B11xHHiI", 0x39, pid, 0, val, 0))
    uc.mem_write(QUEUE + 0x600, struct.pack("<I", len(evs)))

def block(inl, inr):
    uc.mem_write(BUFL, struct.pack(f"<{N}f", *inl)); uc.mem_write(BUFR, struct.pack(f"<{N}f", *inr))
    r = call(s7["hall_process"] | 1, OBJ, CTX)
    events([])
    return r, rd(uc, BUFL, f"<{N}f"), rd(uc, BUFR, f"<{N}f")

# ---- the patches point where they should
def bl_target(at):
    h1, h2 = struct.unpack_from("<HH", data, file_off(at))
    s = (h1 >> 10) & 1; i1 = 1 - (((h2 >> 13) & 1) ^ s); i2 = 1 - (((h2 >> 11) & 1) ^ s)
    imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
    return at + 4 + (imm - (1 << 25) if s else imm)
check(bl_target(0x08048E56) == s7["hall_set"], "reverb event loop's setter call goes to hall_set")
check(u32(0x0806ADAC) == s7["hall_process"] | 1, "reverb vtable process slot goes to hall_process")
for core, lit, cnt in [("M7", 0x08055DA0, 0x08055BA2), ("M4", 0x081370D4, 0x08136EE8)]:
    lst = u32(lit)
    names = [cstr(u32(lst + 4 * i)) for i in range(16)]
    check(names[0] == "Tight Ambience" and names[14] == "Clouds" and names[15] == "Lush Hall"
          and data[file_off(cnt)] == 16, f"{core} style list has 16 entries ending in Lush Hall")

# ---- fake reverb object, buses, events
uc.mem_write(OBJ, struct.pack("<I", 0x0806ADA0))                    # vtable
uc.mem_write(OBJ + 0x08, struct.pack("<I", NEXT))
uc.mem_write(OBJ + 0x18, struct.pack("<I", QID))
uc.mem_write(OBJ + 0x1E, struct.pack("<H", PORT))
uc.mem_write(OBJ + 0x54, struct.pack("<I", MEM))
uc.mem_write(OBJ + 0x68, struct.pack("<I", 48000))
uc.mem_write(OBJ + 0x980, b"\1")
uc.mem_write(NEXT, struct.pack("<I", NEXT + 0x100)); uc.mem_write(NEXT + 0x10C, struct.pack("<I", NEXT_PROC | 1))
uc.mem_write(BUS, struct.pack("<IIIIBB", N, 0, BUFL, BUFR, 0, 0))
uc.mem_write(MEM + 0xFFFFC, struct.pack("<f", 0.5))                 # stock engine's data at the far end
events([])

# stock style: everything goes to the stock engine
r, _, _ = block([0.0] * N, [0.0] * N)
check(calls["process"] == 1 and r == 1, "stock style: hall_process runs the stock engine")
call(s7["hall_set"] | 1, OBJ, 0x143, s0=900.0)
check(calls["set"][-1] == (0x143, 900.0), "knob changes still reach the stock setter")

# select Lush Hall (as the stock event loop would deliver it)
n_set = len(calls["set"])
call(s7["hall_set"] | 1, OBJ, 0x155, s0=15.0)
check(len(calls["set"]) == n_set, "style 15 is kept from the stock setter (it has 15 presets)")
events([(0x144, 600), (0x13D, 0)])
CLEAR_BLOCKS = 0x40000 // 8192
r, l, rr = block([0.5] * N, [0.5] * N)
check(calls["process"] == 1, "Lush Hall: the stock engine no longer runs")
check(r == 7 and calls["next"] == 1, "the node chain continues to the next node")
check((0x144, 600.0) in calls["set"], "events in Hall mode still reach the stock setter")
check(max(map(abs, l + rr)) == 0.0, "silent while the delay memory changes hands")
for _ in range(CLEAR_BLOCKS - 1):
    block([0.5] * N, [0.5] * N)
check(rd(uc, MEM + 0xFFFFC, "<f")[0] == 0.0, "stock engine's data zeroed before the hall starts")
check(rd(uc, MEM, "<I")[0] == 0x4C4C4148, "hall state lives in the stock delay memory")
r, l, rr = block([1.0] + [0.0] * (N - 1), [1.0] + [0.0] * (N - 1))
out = []
for b in range(200):                                                 # ~0.13 s
    _, l, rr = block([0.0] * N, [0.0] * N)
    out += [x * x + y * y for x, y in zip(l, rr)]
check(sum(out) > 1e-4 and all(math.isfinite(x) for x in out), f"impulse makes a tail (energy {sum(out):.4f})")

# cost of one steady block
h = uc.hook_add(UC_HOOK_CODE, count, begin=0x08000000, end=0x081FFFFF)
icount["n"] = 0
block([0.1] * N, [-0.1] * N)
uc.hook_del(h)
per = icount["n"] / N
print(f"     {icount['n']} instructions per {N}-frame block = {per:.0f} per frame "
      f"(~{per * 48000 / 480e6 * 100:.1f}% of 480 MHz at 1 instruction/cycle, before memory stalls)")
check(per < 1100, "under 1100 instructions per frame")

# a style knob value past the list is ignored
n_set = len(calls["set"])
call(s7["hall_set"] | 1, OBJ, 0x155, s0=16.0)
check(len(calls["set"]) == n_set, "style values past the list never reach the stock setter")

# bypass hands back to the stock engine with its memory zeroed
uc.mem_write(OBJ + 0x6E, b"\1")
for _ in range(CLEAR_BLOCKS):
    block([0.0] * N, [0.0] * N)
check(calls["process"] == 1, "bypass: silent while the memory is zeroed")
check(rd(uc, MEM, "<I")[0] == 0 and rd(uc, MEM + 0xFFFFC, "<f")[0] == 0.0, "stock delay memory zeroed on hand-back")
block([0.0] * N, [0.0] * N)
check(calls["process"] == 2, "bypass: then the stock engine handles it")
uc.mem_write(OBJ + 0x6E, b"\0")
for _ in range(CLEAR_BLOCKS + 1):
    block([0.0] * N, [0.0] * N)
check(calls["process"] == 2 and rd(uc, MEM, "<I")[0] == 0x4C4C4148, "un-bypass: the hall restarts")

# switching to another style inside a Hall block
uc.mem_write(MEM + 0xFFFFC, struct.pack("<f", 0.5))
events([(0x155, 3)])
n_echo = calls["echo"]
r, l, rr = block([0.5] * N, [0.5] * N)
check((0x155, 3.0) in calls["set"] and calls["echo"] == n_echo + 1, "leaving: style 3 reaches the stock setter, presets echoed")
check(max(map(abs, l + rr)) == 0.0, "leaving: that block is silent")
for _ in range(CLEAR_BLOCKS - 1):
    block([0.5] * N, [0.5] * N)
check(rd(uc, MEM + 0xFFFFC, "<f")[0] == 0.0 and calls["process"] == 2, "leaving: stock delay memory zeroed first")
block([0.0] * N, [0.0] * N)
check(calls["process"] == 3, "then the stock engine runs")
print("all passed")
