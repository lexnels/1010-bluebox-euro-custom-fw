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
check(added == [(0x33, 400), (0x39, 400), (0x0E, 120), (0xCB, 1000), (0x43, 0), (0x4A, 12), (0x3A, 0), (0x3D, 0),
                (0xCA, 1), (0x4B, 0), (0x35, 1), (0x36, 1), (0x37, 0), (0x34, 6)],
      "M4 delay list: Delay, Feedback, Cutoff, Width, Resonance, Pitch, Flutter, Send, then FILT, PITCH, BEAT, PING, QUAD")

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
               (0x43, 8, "Resonance:", 0, 1000, "dlyreso"), (0x4A, 8, "Pitch:", -12, 12, "dlypitch"),
               (0x4B, 4, "Pitch:", 0, 1, "dlypitchon")],
      "M4 param table: the stock definition passes through, then Flutter, Send, Resonance, Pitch, Pitch on/off")
# ---- M4: the sliding FX panel (dly_populate / dly_page / dly_layout around the stock populate, page select, layout)
P, VT, STUB = 0x24040000, 0x24038000, 0x24039000
uc.mem_write(P, bytes(0x12000)); uc.mem_write(0x38800EC0, bytes(0x40))
uc.mem_write(STUB, bytes.fromhex("7047") * 8)                    # bx lr
uc.mem_write(VT, struct.pack("<16I", *[(STUB + 2 * (k == 0x20 // 4) + 4 * (k == 0x2c // 4)) | 1 for k in range(16)]))
KW = lambda i: P + 0xC3B0 + i * 0x3D0
TW = lambda i: P + 0x100B0 + i * 0x1D4
rects, X0, Y0 = {}, 10, 100
for i in range(16):
    uc.mem_write(KW(i), struct.pack("<I", VT)); uc.mem_write(TW(i), struct.pack("<I", VT))
uc.mem_write(P + 4, struct.pack("<ii", X0, Y0))
def on_rect(uc, a, sz, _):
    w = uc.reg_read(UC_ARM_REG_R0); rects[w] = rd(uc.reg_read(UC_ARM_REG_R1), "<4i")
def on_hide(uc, a, sz, _):
    uc.mem_write(uc.reg_read(UC_ARM_REG_R0) + 0x30, bytes([uc.reg_read(UC_ARM_REG_R1) & 1]))
layout = []
def on_page(uc, a, sz, _):
    uc.mem_write(P + 0xC3A0, struct.pack("<i", uc.reg_read(UC_ARM_REG_R1))); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
hooks = [uc.hook_add(UC_HOOK_CODE, on_rect, begin=STUB + 2, end=STUB + 2), uc.hook_add(UC_HOOK_CODE, on_hide, begin=STUB + 4, end=STUB + 4),
         uc.hook_add(UC_HOOK_CODE, on_page, begin=0x0812BBFC, end=0x0812BBFC)]
def shown():
    out = {}
    for i in range(16):
        for w, kind in ((KW(i), 1), (TW(i), 2)):
            if rd(w + 0x30, "<B")[0] == 0:
                x, y, _, _ = rects[w]; out[i] = (kind, (x - X0) // 0x30 - 1, 0 if y == Y0 + 0x2D else 1)
    return out
def page(n): call(s4["dly_page"], P, n, 0, 0); return rd(P + 0xC3A0, "<i")[0]
SLOT = 0x24037000; uc.mem_write(SLOT, struct.pack("<h", 0x14))
check(data[0x0812BEDE - 0x08100000 + 0xC0000:][:4] == bytes.fromhex("b9f10f0f"), "M4 panel: populate's hide-past-10 check patched out")
def on_populate(uc, a, sz, _):                                   # stand-in: knob or toggle per entry, the rest hidden
    for i in range(16):
        k = layout[i] if i < len(layout) else 0
        uc.mem_write(KW(i) + 0x30, bytes([k != 1])); uc.mem_write(TW(i) + 0x30, bytes([k != 2]))
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
hooks.append(uc.hook_add(UC_HOOK_CODE, on_populate, begin=0x0812BC60, end=0x0812BC60))
layout[:] = [1] * 8 + [2] * 5                                    # the delay in 3&4 mode: 8 knobs, 5 buttons
uc.mem_write(P + 0xC3A0, struct.pack("<i", 0))
call(s4["dly_populate"], P, SLOT)
sh = shown()
check(sh == {i: (1 if i < 8 else 2, i // 2, i % 2) for i in range(10)}, "M4 panel, delay page 1: knobs in columns 1-4, FILT and PITCH in 5")
check(page(1) == 1 and shown() == sh, "page 2: same view")
page(2); sh = shown()
check(sh == {i: (1 if i < 8 else 2, i // 2 - 1, i % 2) for i in range(2, 12)}, "page 3: slides one column, BEAT and PING in view")
page(3); sh = shown()
check(sh == {i: (1 if i < 8 else 2, i // 2 - 2, i % 2) for i in range(4, 13)}, "page 4: slides two columns, QUAD in view")
check(page(4) == 0 and shown() == {i: (1 if i < 8 else 2, i // 2, i % 2) for i in range(10)}, "next page after QUAD: back to page 1, view back")
layout[:] = [1] * 8 + [2] * 4                                    # 3&4 mode off: no QUAD
call(s4["dly_populate"], P, SLOT)
check(page(3) == 0 and page(2) == 2 and max(c for _, c, _ in shown().values()) == 4, "without QUAD: 3 pages, the last slides one column")
uc.mem_write(SLOT, struct.pack("<h", 0x15)); layout[:] = [1] * 8 + [2]  # the reverb: 8 knobs and Freeze
call(s4["dly_populate"], P, SLOT)
ref = {i: (1 if i < 8 else 2, i // 2, i % 2) for i in range(9)}
check(shown() == ref and page(2) == 2 and shown() == ref and page(3) == 0, "reverb: stock layout on every page, still 3 pages")
def on_layout(uc, a, sz, _):                                      # stand-in for the stock layout: the stock rects
    for i in range(10):
        for w in (KW(i), TW(i)): rects[w] = (X0 + 0x30 * (i // 2 + 1), Y0 + 0x2D - 0x2C * (i % 2), 0x30, 0x2C)
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
hooks.append(uc.hook_add(UC_HOOK_CODE, on_layout, begin=0x0812C110, end=0x0812C110))
uc.mem_write(SLOT, struct.pack("<h", 0x14)); layout[:] = [1] * 8 + [2] * 5
call(s4["dly_populate"], P, SLOT); page(3)
call(s4["dly_layout"], P)
check(shown() == {i: (1 if i < 8 else 2, i // 2 - 2, i % 2) for i in range(4, 13)}, "layout (screen redraw): keeps the slid view")
for h in hooks: uc.hook_del(h)

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
R = []                                                               # right channel of the last run
def run(sig, blocks):
    out = []; R.clear()
    for b in range(blocks):
        l, r, _ = block([sig(b * N + i) for i in range(N)])
        out += l; R.extend(r)
    return out

T = 2880                                                             # Delay 10: 10/1000 x 288000 samples
STASH_N = 0x38800D00 + 4 + 4 + 24 + 20 + 16 + 20 + 8 + 4             # struct dly_shared .stash_n
events([("on", 0), (0x35, 0), (0x36, 1), (0x33, 10), (0x39, 500), (0xCA, 0), (0x43, 0), (0x4A, 12), (0x4B, 0),
        (0x3A, 0), (0x3D, 0)])
_, _, r = block([0.0] * N)
check(r == 1, "dly_process returns like the stock process (no next node)")
run(lambda t: 0.0, (288000 - T) // 16 + 200)                         # let the stock read glide to the new time
out = run(lambda t: 1.0 if t == 0 else 0.0, 12000 // N)
pk = max(range(len(out)), key=lambda i: abs(out[i]))
echoes = lambda x: [(i, round(v, 3)) for i, v in enumerate(x) if abs(v) > 1e-3][:4]
print(f"     Filt off: echoes L {echoes(out)}, R {echoes(R)}")
check(echoes(out) == [(T, 1.0), (8608, 0.5)] and echoes(R) == [(2 * T, 1.0), (4 * T, 0.5)], "Filt off, Pitch off, PING on: the stock ping-pong echoes, untouched")
check(max(abs(x) for x in out[:T - 1]) < 1e-6, "wet only: nothing before the first echo")

# the stock band-pass with Filt on, then Resonance: ringing at the Cutoff lasts longer, and stays bounded
def ring_energy(x, a, b): return sum(v * v for v in x[a:b])
events([(0xCA, 1), (0x0E, 500), (0xCB, 0), (0x39, 0)])               # centre 80 Hz x 2^3.5 = 905 Hz, half an octave wide
run(lambda t: 0.0, 400)
out = run(lambda t: 1.0 if t == 0 else 0.0, 4000 // N)
e0 = ring_energy(out, T + 200, T + 1200) / max(1e-12, ring_energy(out, T, T + 1200))
events([(0x43, 1000)])
run(lambda t: 0.0, 400)
out = run(lambda t: 1.0 if t == 0 else 0.0, 4000 // N)
e1 = ring_energy(out, T + 200, T + 1200) / max(1e-12, ring_energy(out, T, T + 1200))
print(f"     share of the echo's energy ringing after 4 ms: {e0 * 100:.1f} % without Resonance, {e1 * 100:.1f} % with")
check(e1 > 2 * e0, "Resonance makes the band ring longer")
events([(0x39, 1000)])
out = run(lambda t: 0.3 * math.sin(2 * math.pi * 905 * t / 48000) if t < 4800 else 0.0, 4 * 48000 // N)
check(all(math.isfinite(x) for x in out) and max(abs(x) for x in out) <= 2.0, f"full Resonance and Feedback: rings, bounded (peak {max(abs(x) for x in out):.2f})")
events([(0x43, 0), (0xCA, 0), (0x39, 0)])
run(lambda t: 0.0, 4 * 48000 // N)

# Pitch: a 480 Hz tone (a whole number of cycles per grain, so grain seams don't skew the measured frequency); +12 with the switch on: the first echo is an octave up, the second (fed back) two
def freq(x):                                                          # strongest DFT bin, 2 Hz steps
    import cmath
    def mag(f): return abs(sum(v * cmath.exp(-2j * math.pi * f * i / 48000) for i, v in enumerate(x)))
    c = max(range(100, 4000, 25), key=mag)
    return max(range(c - 26, c + 27, 2), key=mag)
tone = lambda t: 0.5 * math.sin(2 * math.pi * 480 * t / 48000) if t < 2000 else 0.0
events([(0x4B, 1), (0x39, 700)])
run(lambda t: 0.0, 10)
out = run(tone, 12800 // N)                                          # the shifter adds up to 50 ms per pass
f1 = freq(out[T + 900:T + 2900]); f2 = freq(out[8608 + 1700:8608 + 3500])   # ping-pong: L's next echo, pitched twice
print(f"     Pitch +12: first echo {f1:.0f} Hz, second {f2:.0f} Hz")
check(abs(f1 - 960) < 6 and abs(f2 - 1920) < 12, "Pitch +12: each repeat an octave above the last")
events([(0x4A, -7)])
run(lambda t: 0.0, 3 * 48000 // N)
out = run(tone, 6000 // N)
f1 = freq(out[T + 900:T + 2900])
print(f"     Pitch -7: first echo {f1:.0f} Hz")
check(abs(f1 - 480 * 2 ** (-7 / 12)) < 8, "Pitch -7: a fifth down")
events([(0x4B, 0)])
run(lambda t: 0.0, 3 * 48000 // N)
out = run(tone, 6000 // N)
check(abs(freq(out[T + 100:T + 1900]) - 480) < 4, "Pitch switch off: echoes at the original pitch")
events([(0x4B, 1), (0x37, 1)])
run(lambda t: 0.0, 10)
check(rd(STASH_N + 4 + 2 * 4 * N, "<I")[0] == 0, "QUAD with PING: pitch stays off (QUAD uses lines E and F)")
events([(0x37, 0), (0x39, 0)])
run(lambda t: 0.0, (288000 - T) // 16 + 200)

# Flutter: a held 1 kHz sine through the delay; the echo's zero-crossing spacing wobbles with Flutter, not without
def crossings(x):
    z = [i + x[i] / (x[i] - x[i + 1]) for i in range(len(x) - 1) if x[i] <= 0 < x[i + 1]]
    d = [b - a for a, b in zip(z, z[1:])]
    m = sum(d) / len(d)
    return m, max(abs(v - m) for v in d)
events([(0x4B, 0), (0x3A, 0)])
out = run(lambda t: math.sin(2 * math.pi * 1000 * t / 48000), 2 * 48000 // N)
m0, dev0 = crossings(out[T + 1000:])
events([(0x3A, 1000)])
out = run(lambda t: math.sin(2 * math.pi * 1000 * t / 48000), 3 * 48000 // N)
m1, dev1 = crossings(out[24000:])
print(f"     1 kHz period {m0:.3f} samples, wobble {dev0 / m0 * 100:.3f} % without flutter, {dev1 / m1 * 100:.3f} % with")
check(dev0 / m0 < 0.0005 and 0.003 < dev1 / m1 < 0.02, "Flutter wobbles the pitch slowly (under 2 %)")
check(all(math.isfinite(x) and abs(x) < 1.5 for x in out), "Flutter: output stays clean")

# cost: our additions on top of the stock body, with everything on
icount = {"n": 0}
def count(uc, a, s, _): icount["n"] += 1
events([(0x3D, 500), (0x4B, 1), (0x4A, 12), (0xCA, 1), (0x43, 800)])
block([0.1] * N)
h = uc.hook_add(UC_HOOK_CODE, count, begin=0x08000000, end=0x081FFFFF)
block([0.1] * N)
ours = icount["n"]
uc.hook_del(h)
print(f"     delay: {ours / N:.0f} instructions per frame with everything on")
check(ours / N < 600, "under 600 instructions per frame with everything on")

# Send: Send x the wet output is kept for the reverb's next block
events([(0x3D, 1000), (0x3A, 0), (0x4B, 0)])
block([0.0] * N)
block([0.0] * N)
check(rd(STASH_N, "<I")[0] == N, "Send: the block's wet output is kept for the reverb")
events([(0x3D, 0)])
block([0.0] * N); block([0.0] * N)
check(rd(STASH_N, "<I")[0] == 0, "Send 0: nothing kept")
print("all passed")
