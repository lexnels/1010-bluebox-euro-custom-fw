"""Run the delay additions on the real stock delay under Unicorn (Cortex-M7 model): the stock constructor builds the
object, the stock delay body runs with our hooks patched in. Also checks the M4 side (param list, definitions).

  python3 test_delay.py [out/cpu+hall+delay/BLUEEURO.BIN]
"""
import math, struct, sys
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UC_ERR_INSN_INVALID
from unicorn.arm_const import *

IMG = sys.argv[1] if len(sys.argv) > 1 else "out/cpu+hall+delay/BLUEEURO.BIN"
data = open(IMG, "rb").read()
sys.path.insert(0, "patches")
from thumb import symbols
s7, s4 = symbols("out/hall7.elf"), symbols("out/dly4.elf")
RET, CTX, N = 0x0807FFF0, 0x30000000, 32

def check(cond, msg):
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond: sys.exit(1)
def f2u(f): return struct.unpack("<I", struct.pack("<f", f))[0]

uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M7)
uc.mem_map(0x08000000, 0x200000); uc.mem_write(0x08040000, data[:0xA0000]); uc.mem_write(0x08100000, data[0xC0000:])
for b, s in [(0x20000000, 0x20000), (0x24000000, 0x80000), (0x30000000, 0x48000), (0x38000000, 0x10000),
             (0x38800000, 0x1000), (0x58024000, 0x1000), (0xC0000000, 0x2000000)]:
    uc.mem_map(b, s)
heap = [0xC0000000]
def alloc(uc, a, s, _):
    n = uc.reg_read(UC_ARM_REG_R0); p = heap[0]; heap[0] = (p + n + 15) & ~15
    uc.reg_write(UC_ARM_REG_R0, p); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
for f in (0x080693DC, 0x080413D0): uc.hook_add(UC_HOOK_CODE, alloc, begin=f, end=f)

def call(fn, *args, stack=()):
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args): uc.reg_write(r, v)
    sp = 0x2407F000
    uc.mem_write(sp, b"".join(struct.pack("<I", v & 0xFFFFFFFF) for v in stack) + bytes(16))
    uc.reg_write(UC_ARM_REG_SP, sp); uc.reg_write(UC_ARM_REG_LR, RET | 1)
    pc = fn | 1
    while True:
        try:
            uc.emu_start(pc, RET, count=50_000_000); break
        except UcError as e:            # the constructor uses a few instructions Unicorn lacks; they don't matter here
            if e.errno != UC_ERR_INSN_INVALID: raise
            pc = (uc.reg_read(UC_ARM_REG_PC) + 4) | 1
    return uc.reg_read(UC_ARM_REG_R0)
rd = lambda a, f: struct.unpack(f, uc.mem_read(a, struct.calcsize(f)))

# ---- M4: the delay list (patched case 4 of FUN_0812060c)
SET, added = 0x24030000, []
def m4_add(uc, a, size, _):
    added.append((uc.reg_read(UC_ARM_REG_R1), struct.unpack("<i", struct.pack("<I", uc.reg_read(UC_ARM_REG_R2)))[0]))
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
h = uc.hook_add(UC_HOOK_CODE, m4_add, begin=0x081205E2, end=0x081205E2)
uc.reg_write(UC_ARM_REG_R4, SET); uc.reg_write(UC_ARM_REG_R5, 0x1234); uc.reg_write(UC_ARM_REG_SP, 0x2407F000)
uc.emu_start(0x08120BBA | 1, 0x0812088C, count=1000)
uc.hook_del(h)
check(uc.reg_read(UC_ARM_REG_PC) == 0x0812088C and uc.reg_read(UC_ARM_REG_R4) == SET and uc.reg_read(UC_ARM_REG_R5) == 0x1234
      and uc.reg_read(UC_ARM_REG_SP) == 0x2407F000, "M4 delay list: reaches the common tail with r4, r5, sp intact")
check(added == [(0x33, 400), (0x39, 400), (0x43, 200), (0x4A, 600), (0x3A, 0), (0x3D, 0), (0x35, 1), (0x36, 1), (0x37, 0), (0x34, 6)],
      "M4 delay list: Delay, Feedback, Low Cut, High Cut, Flutter, Send, then BEAT, PING, QUAD")

# ---- M4: param definitions (the last stock one, then ours)
defs = []
def m4_def(uc, a, size, _):
    sp = uc.reg_read(UC_ARM_REG_SP)
    mn, mx, key = rd(sp, "<iiI")
    cs = lambda p: bytes(uc.mem_read(p, 16)).split(b"\0")[0].decode()
    defs.append((uc.reg_read(UC_ARM_REG_R1), uc.reg_read(UC_ARM_REG_R2), cs(uc.reg_read(UC_ARM_REG_R3)), mn, mx, cs(key)))
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
h = uc.hook_add(UC_HOOK_CODE, m4_def, begin=0x08135E08, end=0x08135E08)
STR = 0x24031000; uc.mem_write(STR, b"Lbl:\0\0\0\0key\0")
call(s4["dly_defs"], 0x24032000, 0x117, 5, STR, stack=(-7, 9, STR + 8))
uc.hook_del(h)
check(defs == [(0x117, 5, "Lbl:", -7, 9, "key"), (0x3A, 8, "Flutter:", 0, 1000, "dlyflutter"), (0x3D, 8, "Send:", 0, 1000, "dlysend"),
               (0x43, 8, "Low Cut:", 0, 1000, "dlylowcut"), (0x4A, 8, "High Cut:", 0, 1000, "dlyhicut")],
      "M4 param table: the stock definition passes through, then Flutter, Send, Low Cut, High Cut")
def bl_target(at):
    o = at - 0x08040000 if at < 0x08100000 else at - 0x08100000 + 0xC0000
    h1, h2 = struct.unpack_from("<HH", data, o)
    s = (h1 >> 10) & 1; i1 = 1 - (((h2 >> 13) & 1) ^ s); i2 = 1 - (((h2 >> 11) & 1) ^ s)
    imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
    return at + 4 + (imm - (1 << 25) if s else imm)
check(bl_target(0x0813714A) == s4["dly_defs"], "M4 last param definition goes to dly_defs")
check(all(bl_target(a) == s7["dly_read"] for a in (0x08053574, 0x08053594, 0x08053816, 0x080538EC))
      and all(bl_target(a) == s7["dly_tone"] for a in (0x08053602, 0x08053610))
      and struct.unpack_from("<I", data, 0x0806BA70 - 0x08040000)[0] == s7["dly_process"] | 1,
      "M7 delay: process, line reads and filter calls hooked")

# ---- M7: a real delay object from the stock constructor
BUFS = 0x24020000
def mkbus(addr, buf, silent=0):
    uc.mem_write(addr, struct.pack("<IIIIBBxx", N, N, buf, buf + 0x100, silent, 1))
B13 = CTX + 0x150E4 + 13 * 20
mkbus(B13, BUFS); mkbus(CTX + 0x150E4 + 16 * 20, BUFS + 0x400)
mkbus(CTX + 0x15260, BUFS + 0x600); mkbus(CTX + 0x15274, BUFS + 0x800)
OBJ = heap[0]; heap[0] += 0x280
call(0x08052E30, OBJ)
uc.mem_write(OBJ + 0x08, struct.pack("<I", 0)); uc.mem_write(OBJ + 0x18, struct.pack("<I", 0x14))
uc.mem_write(OBJ + 0x1E, struct.pack("<H", 13)); uc.mem_write(OBJ + 0x273, b"\0")
uc.mem_write(CTX, struct.pack("<I", 0x24030000))
QUEUE = CTX + 0x14 * 0x604 + 4
def events(evs):
    for i, (pid, val) in enumerate(evs):
        t = 0x39
        if pid == "on": t, pid, val = 2, 0, 0
        uc.mem_write(QUEUE + i * 24, struct.pack("<B7xIHHiI", t, 0x14, pid, 0, val, 0))
    uc.mem_write(QUEUE + 0x600, struct.pack("<I", len(evs)))
def block(inl, inr=None):
    mkbus(B13, BUFS)
    uc.mem_write(BUFS, struct.pack(f"<{N}f", *inl)); uc.mem_write(BUFS + 0x100, struct.pack(f"<{N}f", *(inr or inl)))
    r = call(0x08053B30 if False else s7["dly_process"], OBJ, CTX)
    events([])
    return rd(BUFS, f"<{N}f"), rd(BUFS + 0x100, f"<{N}f"), r
def run(sig, blocks):
    out = []
    for b in range(blocks):
        l, r, _ = block([sig(b * N + i) for i in range(N)])
        out += l
    return out

T = 2880                                                             # Delay 10: 10/1000 x 288000 samples
events([("on", 0), (0x35, 0), (0x36, 0), (0x33, 10), (0x39, 500), (0x43, 0), (0x4A, 1000), (0x3A, 0), (0x3D, 0)])
_, _, r = block([0.0] * N)
check(r == 1, "dly_process returns like the stock process (no next node)")
check(rd(OBJ + 0x270, "<B")[0] == 1, "Filt forced on, so the tone hook runs")
run(lambda t: 0.0, (288000 - T) // 16 + 200)                         # let the stock read glide to the new time
out = run(lambda t: 1.0 if t == 0 else 0.0, 12000 // N)
pk = max(range(len(out)), key=lambda i: abs(out[i]))
# area over 50 samples (the 20 kHz low-pass lowers an impulse's peak; the 20 Hz high-pass takes ~15 % off this window)
e1, e2 = sum(out[T - 10:T + 40]), sum(out[2 * T - 10:2 * T + 40])
print(f"     open filters: first echo peak at {pk}, area {e1:.3f}, second {e2:.3f}")
check(abs(pk - T) <= 3 and 0.8 < e1 < 1.05 and 0.38 < e2 / e1 < 0.5, "open cuts: echoes at the delay time, halving with Feedback 500")
check(max(abs(x) for x in out[:T - 10]) < 1e-6, "wet only: nothing before the first echo")

# High Cut at 0 (500 Hz): the echo is a soft bump, much lower than open
events([(0x4A, 0)])
run(lambda t: 0.0, 400)
out = run(lambda t: 1.0 if t == 0 else 0.0, 4000 // N)
dark = max(abs(x) for x in out)
print(f"     High Cut 0: first echo peak {dark:.3f}")
check(dark < 0.15, "High Cut darkens the echo")
# Low Cut at 1000 (2 kHz): a held DC input gives (almost) no echo
events([(0x4A, 1000), (0x43, 1000), (0x39, 0)])
run(lambda t: 0.0, 400)
out = run(lambda t: 1.0, 12000 // N)
dc = max(abs(x) for x in out[T + 500:])                              # after the step's own short click
print(f"     Low Cut 1000, DC in: echo {dc:.4f}")
check(dc < 0.02, "Low Cut removes the lows")

# Flutter: a held 1 kHz sine through the delay; the echo's zero-crossing spacing wobbles with Flutter, not without
def crossings(x):
    z = [i + x[i] / (x[i] - x[i + 1]) for i in range(len(x) - 1) if x[i] <= 0 < x[i + 1]]
    d = [b - a for a, b in zip(z, z[1:])]
    m = sum(d) / len(d)
    return m, max(abs(v - m) for v in d)
events([(0x43, 0), (0x39, 0), (0x3A, 0)])
run(lambda t: 0.0, 400)
out = run(lambda t: math.sin(2 * math.pi * 1000 * t / 48000), 2 * 48000 // N)
m0, dev0 = crossings(out[T + 1000:])
events([(0x3A, 1000)])
out = run(lambda t: math.sin(2 * math.pi * 1000 * t / 48000), 3 * 48000 // N)
m1, dev1 = crossings(out[24000:])
print(f"     1 kHz period {m0:.3f} samples, wobble {dev0 / m0 * 100:.3f} % without flutter, {dev1 / m1 * 100:.3f} % with")
check(dev0 / m0 < 0.0005 and 0.003 < dev1 / m1 < 0.02, "Flutter wobbles the pitch slowly (under 2 %)")
check(all(math.isfinite(x) and abs(x) < 1.5 for x in out), "Flutter: output stays clean")

# cost: our additions on top of the stock body, flutter and send on
icount = {"n": 0}
def count(uc, a, s, _): icount["n"] += 1
events([(0x3D, 500)])
block([0.1] * N)
h = uc.hook_add(UC_HOOK_CODE, count, begin=0x08000000, end=0x081FFFFF)
block([0.1] * N)
ours = icount["n"]; icount["n"] = 0
uc.hook_del(h)
uc.mem_write(OBJ + 0x270, b"\0")
h = uc.hook_add(UC_HOOK_CODE, count, begin=0x08000000, end=0x081FFFFF)
call(0x08053234, OBJ, CTX)          # the stock body alone, filter off (no hooks reached but the reads)
stock = icount["n"]
uc.hook_del(h)
print(f"     delay: {ours / N:.0f} instructions per frame with the additions, stock body alone {stock / N:.0f}")
check((ours - stock) / N < 250, "the additions cost under 250 instructions per frame")

# Send: Send x the wet output is kept for the reverb's next block
DS = 0x38800D00
events([(0x3D, 1000), (0x3A, 0)])
block([0.0] * N)
l, r, _ = block([0.0] * N)
n = rd(DS + 4 + 4 + 16 + 8 + 40 + 32 + 20 + 8 + 4, "<I")[0]
check(n == N, "Send: the block's wet output is kept for the reverb")
events([(0x3D, 0)])
block([0.0] * N); block([0.0] * N)
n = rd(DS + 4 + 4 + 16 + 8 + 40 + 32 + 20 + 8 + 4, "<I")[0]
check(n == 0, "Send 0: nothing kept")
print("all passed")
