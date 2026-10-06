"""Run the send FX (patches/sfx.py) under Unicorn: the M7 engine in node 0x30's place with the stock FX-return code
after it, and the M4 side (definitions, the reverb set, the panel's list and titles, the FX button cycle).

  python3 test_sfx.py [out/cpu+hall+delay+master+usb+sfx/BLUEEURO.BIN]
"""
import math, struct, sys
from unicorn import *
from unicorn.arm_const import *

IMG = sys.argv[1] if len(sys.argv) > 1 else "out/cpu+hall+delay+master+usb+sfx/BLUEEURO.BIN"
data = open(IMG, "rb").read()
sys.path.insert(0, "patches")
from thumb import symbols, bl
s7, s4 = symbols("out/sfx7.elf"), symbols("out/dly4.elf")
h7 = symbols("out/hall7.elf")
sys.path.insert(0, "src")
ids = {}
for line in open("src/sfx_ids.h"):
    p = line.split()
    if len(p) >= 3 and p[0] == "#define" and p[1].startswith("SFX_"): ids[p[1][4:]] = int(p[2], 0)
RET, CTX, N = 0x0807FFF0, 0x30000000, 32
R = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]

def check(cond, msg):
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond: sys.exit(1)

def at(addr, n):
    o = addr - 0x08040000 if addr < 0x08100000 else addr - 0x08100000 + 0xC0000
    return data[o:o + n]

def new():
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS); uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M7)
    uc.mem_map(0x08000000, 0x200000); uc.mem_write(0x08040000, data[:0xA0000]); uc.mem_write(0x08100000, data[0xC0000:])
    for b, s in [(0x20000000, 0x20000), (0x24000000, 0x80000), (0x30000000, 0x48000), (0x38800000, 0x1000),
                 (0x58024000, 0x1000), (0xE000E000, 0x1000), (0xC0000000, 0x400000)]:
        uc.mem_map(b, s)
    uc.mem_write(0xE000ED88, struct.pack("<I", 0xF << 20))
    return uc

def call(uc, fn, *args, count=20_000_000):
    for r, v in zip(R, args): uc.reg_write(r, v & 0xFFFFFFFF)
    uc.reg_write(UC_ARM_REG_SP, 0x2407F000); uc.reg_write(UC_ARM_REG_LR, RET | 1)
    uc.emu_start(fn | 1, RET, count=count)
    return uc.reg_read(UC_ARM_REG_R0)

def stub(uc, addr, rec=None, rv=0):
    def h(uc, a, s, _):
        if rec is not None: rec.append(tuple(uc.reg_read(r) for r in R))
        uc.reg_write(UC_ARM_REG_R0, rv() if callable(rv) else rv); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, h, begin=addr, end=addr)

u32 = lambda uc, a: struct.unpack("<I", uc.mem_read(a, 4))[0]
w32 = lambda uc, a, v: uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))

def bl_target(addr):
    h1, h2 = struct.unpack("<HH", at(addr, 4))
    s = (h1 >> 10) & 1; i1 = 1 - (((h2 >> 13) & 1) ^ s); i2 = 1 - (((h2 >> 11) & 1) ^ s)
    imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
    return addr + 4 + (imm - (1 << 25) if s else imm)

# ---- patch sites
check(struct.unpack("<I", at(0x0806ADAC, 4))[0] == s7["sfx_process"] | 1, "reverb process -> sfx_process (which then runs hall_process)")
check(struct.unpack("<I", at(0x0806B9FC, 4))[0] == 0x08050E6D, "node 0x30 (FX returns) stock")
lit = lambda a: struct.unpack("<I", at((a + 4) & ~3, 4))[0]   # ldr r1, [pc, #imm] just before each bl
check(bl_target(0x0812BC90) == s4["sfx_title"] and bl_target(0x0812BC9C) == s4["sfx_name"]
      and at(lit(0x0812BC86 + (struct.unpack("<H", at(0x0812BC86, 2))[0] & 0xFF) * 4), 4) == b"FX2\0"
      and at(lit(0x0812BC94 + (struct.unpack("<H", at(0x0812BC94, 2))[0] & 0xFF) * 4), 7) == b"Reverb\0",
      "the reverb branch's titles (FX2, Reverb) -> sfx_title, sfx_name")
check(bl_target(0x080518D2) == s7["sfx_ctor"], "graph builder's delay constructor call -> sfx_ctor")
check(bl_target(0x08120C34) == s4["sfx_rv_tail"] and bl_target(0x0812BCCA) == s4["sfx_list"], "reverb set tail and panel list -> ours")
check(bl_target(0x081247D8) == s4["sfx_to_reverb"] and bl_target(0x081247E4) == s4["sfx_from_reverb"], "FX button: delay->reverb and reverb->next -> ours")

# ---- M7: allocation at graph build
HEAP = 0x24000004
def m7():
    uc = new()
    uc.mem_write(0x24000000, at(0x0808A91C, 0x20C))
    stub(uc, 0x08052E30, rv=lambda: uc.reg_read(UC_ARM_REG_R0))
    def alloc(uc, a, s, _):
        n = uc.reg_read(UC_ARM_REG_R0); p = u32(uc, HEAP); w32(uc, HEAP, (p + n + 15) & ~15)
        uc.reg_write(UC_ARM_REG_R0, p); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, alloc, begin=0x080413D0, end=0x080413D0)
    w32(uc, HEAP, 0xC0010000)
    uc.mem_write(0xC0010000, b"\xa5" * 0x200000)     # SDRAM garbage
    return uc
uc = m7()
r = call(uc, s7["sfx_ctor"], 0x24050000)
p = u32(uc, 0x38800FA4)
size = u32(uc, HEAP) - p
print(f"     state {size} bytes at {p:#x}")
check(r == 0x24050000 and p == 0xC0010000 and u32(uc, 0x38800FA0) == 0x58464453, "sfx_ctor: runs the delay's constructor, then allocates its state")
check(0x200000 < size < 0x210000 and u32(uc, p) == 0x58464453 and u32(uc, p + 4) == p, "state about 2 MB, marked")
check(bytes(uc.mem_read(p + size - 0x1010, 0x1000)) == bytes(0x1000), "state zeroed (delay lines)")
call(uc, s7["sfx_ctor"], 0x24050000)
check(u32(uc, 0x38800FA4) == p and u32(uc, HEAP) == p + size, "a rebuilt graph keeps the same memory")
w32(uc, HEAP, 0xC0010000)                          # after a reset: the allocator is back below it
call(uc, s7["sfx_ctor"], 0x24050000)
check(u32(uc, 0x38800FA4) == 0xC0010000 and u32(uc, HEAP) == p + size, "after a reset it allocates again")

# ---- M7: processing (the stock FX-return node code runs after ours)
BUS = lambda port: CTX + 0x150E4 + port * 20
BL, BR = 0x24020000, 0x24020100
Q = CTX + 0xC * 0x604 + 4
revs = []
def setup():
    uc = m7()
    call(uc, s7["sfx_ctor"], 0x24050000)
    node = 0x24051000
    uc.mem_write(node, struct.pack("<IBBxxI", 0x0806ADA0, 0x44, 1, 0) + bytes(0x14))
    revs.clear()
    stub(uc, h7["hall_process"], revs, rv=1)      # the reverb itself: only that it runs after us
    for port in range(19):
        uc.mem_write(BUS(port), struct.pack("<IIIIBBxx", 32, 32, 0x24030000 + port * 0x200, 0x24030100 + port * 0x200, 1, 0))
    return uc, node
def events(uc, evs):
    for i, (pid, val) in enumerate(evs):
        uc.mem_write(Q + i * 24, struct.pack("<B7xIHHiI", 0x39, 0x15, pid, 0, val, 0))
    w32(uc, Q + 0x600, len(evs))
def block(uc, node, l, r, evs=(), silent=False):
    events(uc, evs)
    uc.mem_write(BUS(12), struct.pack("<IIIIBBxx", 32, 32, BL, BR, 1 if silent else 0, 1))
    uc.mem_write(BL, struct.pack("<32f", *l)); uc.mem_write(BR, struct.pack("<32f", *r))
    n = [0]
    h = uc.hook_add(UC_HOOK_CODE, lambda *a: n.__setitem__(0, n[0] + 1))
    call(uc, s7["sfx_process"], node, CTX)
    uc.hook_del(h)
    w32(uc, Q + 0x600, 0)
    return (list(struct.unpack("<32f", uc.mem_read(BL, 128))), list(struct.unpack("<32f", uc.mem_read(BR, 128))), n[0])
def run(uc, node, sig_l, sig_r, evs=(), skip=0):
    outl, outr, cost = [], [], 0
    for b in range(len(sig_l) // 32):
        l, r, n = block(uc, node, sig_l[b*32:(b+1)*32], sig_r[b*32:(b+1)*32], evs if b == 0 else ())
        outl += l; outr += r
        if b >= skip: cost = max(cost, n)
    return outl, outr, cost

uc, node = setup()
sine = [0.5 * math.sin(2 * math.pi * 440 * i / 48000) for i in range(32)]
l, r, n = block(uc, node, sine, sine)
print(f"     all off: {n} instructions")
check(max(abs(a - b) for a, b in zip(l, sine)) < 1e-7 and n < 200, "all off: bus 12 untouched, cheap")

# chorus: dry + modulated copies, L and R opposite
uc, node = setup()
T = 48000
x = [0.5 * math.sin(2 * math.pi * 440 * i / 48000) for i in range(T)]
l, r, cost = run(uc, node, x, x, [(ids["CHO_ON"], 1)])
wl = [a - b for a, b in zip(l, x)]; wr = [a - b for a, b in zip(r, x)]
rms = lambda v: math.sqrt(sum(t * t for t in v) / len(v))
print(f"     chorus I: wet rms L {rms(wl[4800:]):.3f} R {rms(wr[4800:]):.3f}, L-R {rms([a-b for a,b in zip(wl,wr)][4800:]):.3f}; {cost} instr/block")
check(0.25 < rms(wl[4800:]) < 0.4 and 0.25 < rms(wr[4800:]) < 0.4, "chorus: a wet copy at about the dry level (Send 1000, Level 1000)")
check(rms([a - b for a, b in zip(wl, wr)][4800:]) > 0.02, "chorus: left and right modulated in opposite directions")
# the delay sweep: cross-correlate a click to find the delay over time
uc, node = setup()
clicks = [0.0] * T
for k in range(0, T, 2400): clicks[k] = 1.0
l, r, _ = run(uc, node, clicks, clicks, [(ids["CHO_ON"], 1)])
def peak_delay(out, k):
    seg = out[k + 1:k + 400]
    m = max(range(len(seg)), key=lambda j: abs(seg[j]))
    return (m + 1) / 48.0
dl = [peak_delay(l, k) for k in range(2400, T - 2400, 2400)]
dr = [peak_delay(r, k) for k in range(2400, T - 2400, 2400)]
print(f"     chorus I delays L {min(dl):.2f}..{max(dl):.2f} ms, R {min(dr):.2f}..{max(dr):.2f} ms")
check(1.5 < min(dl) and max(dl) < 5.6 and max(dl) - min(dl) > 2.5, "chorus I: delay sweeps about 1.7 to 5.3 ms (Juno-60)")
uc, node = setup()
l, r, _ = run(uc, node, clicks, clicks, [(ids["CHO_ON"], 1), (ids["CHO_MODE"], 2)])
d3 = [peak_delay(l, k) for k in range(2400, T - 2400, 2400)]
check(3.0 < min(d3) and max(d3) < 4.0, f"chorus I+II: shallow and fast around 3.5 ms ({min(d3):.2f}..{max(d3):.2f})")
uc, node = setup()
l, r, _ = run(uc, node, clicks, clicks, [(ids["CHO_ON"], 1), (ids["CHO_DEPTH"], 0)])
d0 = [peak_delay(l, k) for k in range(2400, T - 2400, 2400)]
uc, node = setup()
l, r, _ = run(uc, node, clicks, clicks, [(ids["CHO_ON"], 1), (ids["CHO_DEPTH"], 1000)])
d2 = [peak_delay(l, k) for k in range(2400, T - 2400, 2400)]
print(f"     chorus I, Depth 0: {min(d0):.2f}..{max(d0):.2f} ms; Depth 1000: {min(d2):.2f}..{max(d2):.2f} ms")
check(max(d0) - min(d0) < 0.1 and max(d2) - min(d2) > 1.6 * (max(dl) - min(dl)) and min(d2) > 0.2, "chorus Depth: 0 = still, 1000 = about twice the sweep (never below 0.3 ms)")
uc, node = setup()
l, r, _ = run(uc, node, clicks, clicks, [(ids["CHO_ON"], 1), (ids["CHO_RATE"], 1000)])
d4 = [peak_delay(l, k) for k in range(2400, T - 2400, 2400)]
turns = sum(1 for i in range(1, len(d4) - 1) if (d4[i] - d4[i-1]) * (d4[i+1] - d4[i]) < 0)
check(turns >= 2, f"chorus Rate 1000: the sweep turns around within a second ({turns} turns; at 500 it takes 2 s)")

# delay 2 Beat Sync: the stock note values at the song tempo
for bpm, k, ms in ((120.0, 8, 500.0), (90.0, 5, 1000.0 / 3)):
    uc, node = setup()
    ctx0 = u32(uc, CTX) or 0x24060000
    w32(uc, CTX, ctx0); uc.mem_write(ctx0 + 0x18, struct.pack("<f", bpm))
    imp0 = [0.0] * 48000; imp0[4800] = 1.0      # after the lines have cleared
    l, r, _ = run(uc, node, imp0, imp0, [(ids["D2_ON"], 1), (ids["D2_BEAT"], 1), (ids["D2_SYNC"], k), (ids["D2_LEVEL"], 1000), (ids["D2_TONE"], 1000), (ids["D2_FB"], 0)])
    pk = max(range(4800 + 1, 48000), key=lambda j: abs(l[j] - imp0[j]))
    got = (pk - 4800) / 48.0
    check(abs(got - ms) < 0.1, f"delay 2 Beat Sync at {bpm:.0f} BPM, note value {k}: echo after {got:.2f} ms (want {ms:.2f})")

# drive: louder input gets compressed, level kept
def drive_peak(dv, amp):
    uc, node = setup()
    x = [amp * math.sin(2 * math.pi * 220 * i / 48000) for i in range(9600)]
    l, r, c = run(uc, node, x, x, [(ids["DRV_ON"], 1), (ids["DRV_DRIVE"], dv), (ids["DRV_LEVEL"], 1000), (ids["DRV_TONE"], 1000)])
    w = [a - b for a, b in zip(l, x)][4800:]
    return max(abs(t) for t in w), c
p0, _ = drive_peak(0, 0.25); p5, _ = drive_peak(500, 0.25); p10, c = drive_peak(1000, 0.25); p10f, _ = drive_peak(1000, 1.0)
print(f"     drive wet peaks at -12 dBFS in: Drive 0 {p0:.3f}, 500 {p5:.3f}, 1000 {p10:.3f}; full scale in at 1000 {p10f:.3f}; {c} instr/block")
check(0.15 < p0 < 0.35 and 0.15 < p10 < 0.35 and p10f < 0.4, "drive: the wet level stays about the same from clean to full overdrive")
def drive_wet(dv):
    uc, node = setup()
    x = [0.25 * math.sin(2 * math.pi * 220 * i / 48000) for i in range(9600)]
    l, _, _ = run(uc, node, x, x, [(ids["DRV_ON"], 1), (ids["DRV_DRIVE"], dv), (ids["DRV_LEVEL"], 1000), (ids["DRV_TONE"], 1000)])
    return [a - b for a, b in zip(l, x)][4800:]
def harm(v, k):
    s = sum(t * math.sin(2 * math.pi * 220 * k * i / 48000) for i, t in enumerate(v)); c = sum(t * math.cos(2 * math.pi * 220 * k * i / 48000) for i, t in enumerate(v))
    return math.hypot(s, c) * 2 / len(v)
w = drive_wet(300); h1, h2, h3 = harm(w, 1), harm(w, 2), harm(w, 3)
w = drive_wet(1000); g1, g3 = harm(w, 1), harm(w, 3)
print(f"     drive harmonics at -12 dBFS: Drive 300: 2nd {h2 / h1:.3f}, 3rd {h3 / h1:.3f} of the 1st; Drive 1000: 3rd {g3 / g1:.3f}")
check(h2 > 0.03 * h1, "drive: warm (2nd harmonic) at moderate Drive")
check(g3 > 0.2 * g1, "drive: distorted (strong 3rd harmonic) at full Drive")

# delay 2: lines cleared on switching on (silent), then an echo at the set time
uc, node = setup()
imp = [0.0] * 48000
imp[2400] = 1.0
l, r, cost = run(uc, node, imp, imp, [(ids["D2_ON"], 1), (ids["D2_TIME"], 500), (ids["D2_FB"], 500), (ids["D2_LEVEL"], 1000), (ids["D2_TONE"], 1000)], skip=80)
want = 10 * 200 ** 0.5                 # 141.4 ms
echoes = [i for i in range(2500, 48000) if abs(l[i]) > 0.1]
first = echoes[0] if echoes else None
print(f"     delay 2 (Time 500 = {want:.1f} ms): first echo {(first - 2400) / 48 if first else None} ms after the click; {cost} instr/block")
check(first and abs((first - 2400) / 48 - want) < 0.2, "delay 2: echo at 10 ms * 200^(Time/1000)")
e2 = [i for i in echoes if i > first + 100]
print(f"     2nd echo {(e2[0] - first) / 48 if e2 else None} ms later, {abs(l[e2[0]]) / abs(l[first]) if e2 else 0:.2f} of the first")
check(e2 and abs((e2[0] - first) / 48 - want) < 0.2 and 0.2 < abs(l[e2[0]]) / abs(l[first]) < 0.5, "delay 2: repeats at the same spacing, about Feedback down (less the Tone filter)")
uc, node = setup()
l, r, _ = run(uc, node, imp, imp, [(ids["D2_ON"], 1), (ids["D2_TIME"], 500), (ids["D2_FB"], 800), (ids["D2_LEVEL"], 1000), (ids["D2_PING"], 1)])
el = [i for i in range(2500, 48000) if abs(l[i]) > 0.05]; er = [i for i in range(2500, 48000) if abs(r[i]) > 0.05]
check(el and er and el[0] < er[0] and abs((er[0] - el[0]) / 48 - want) < 0.3, "delay 2 Ping: first repeat left, next one right")
# cleared on switching on: put junk in the lines, switch off and on again
uc, node = setup()
noise = [0.5 * math.sin(i * 1.3) for i in range(9600)]
run(uc, node, noise, noise, [(ids["D2_ON"], 1), (ids["D2_LEVEL"], 1000), (ids["D2_FB"], 900)])
run(uc, node, [0.0] * 3200, [0.0] * 3200, [(ids["D2_ON"], 0)])
l, r, _ = run(uc, node, [0.0] * 9600, [0.0] * 9600, [(ids["D2_ON"], 1)])
check(max(abs(t) for t in l) < 1e-6, "delay 2: switched back on, nothing old comes out")

# FX1 / FX2 Send: the chorus's output into the delay's and reverb's buses, ahead of them
uc, node = setup()
x = [0.3 * math.sin(2 * math.pi * 330 * i / 48000) for i in range(4800)]
run(uc, node, x, x, [(ids["CHO_ON"], 1)])
b13 = struct.unpack("<IIIIBBxx", uc.mem_read(BUS(13), 20))
check(b13[4] == 1, "FX1/FX2 Send at 0: the delay's and reverb's buses left alone (still silent)")
run(uc, node, x, x, [(ids["CHO_FX1"], 500), (ids["CHO_FX2"], 1000)])
for port in (13, 14):
    uc.mem_write(BUS(port), struct.pack("<IIIIBBxx", 32, 32, 0x24030000 + port * 0x200, 0x24030100 + port * 0x200, 1, 0))
l, r, _ = block(uc, node, x[:32], x[:32])
b13 = struct.unpack("<IIIIBBxx", uc.mem_read(BUS(13), 20)); b14 = struct.unpack("<IIIIBBxx", uc.mem_read(BUS(14), 20))
w13 = struct.unpack("<32f", uc.mem_read(b13[2], 128)); w14 = struct.unpack("<32f", uc.mem_read(b14[2], 128))
wet = [a - b for a, b in zip(l, x[:32])]
print(f"     wet into main {max(map(abs, wet)):.3f}, FX1 bus {max(map(abs, w13)):.3f}, FX2 bus {max(map(abs, w14)):.3f}")
check(b13[4] == 0 and b14[4] == 0 and all(abs(a - 0.5 * w) < 1e-5 for a, w in zip(w13, wet)) and all(abs(a - w) < 1e-5 for a, w in zip(w14, wet)),
      "FX1 Send 500 / FX2 Send 1000: half and all of the chorus's output into the delay's and reverb's buses")
check(len(revs) > 0 and revs[-1][0] == node, "the reverb (hall_process) runs after the send FX")

# all three: cost
uc, node = setup()
x = [0.3 * math.sin(2 * math.pi * 330 * i / 48000) for i in range(3200)]
_, _, cost = run(uc, node, x, x, [(ids["CHO_ON"], 1), (ids["DRV_ON"], 1), (ids["D2_ON"], 1)], skip=80)
print(f"     all three on (after delay 2 has cleared its lines): {cost} instructions per 32-sample block (~{cost / 3200:.1f}% of the M7 at ~1 instr/cycle)")
check(cost < 12000, "all three on: under 12000 instructions per block")
# silent input: chorus and drive stop after their tails
uc, node = setup()
_, _, _ = run(uc, node, x, x, [(ids["CHO_ON"], 1), (ids["DRV_ON"], 1)])
for _ in range(40): _, _, n = block(uc, node, [0.0] * 32, [0.0] * 32, silent=True)
print(f"     chorus + drive on, silent mix: {n} instructions per block")
check(n < 300, "silent mix: chorus and drive idle once their tails are out")

# ---- M4: definitions, the reverb set, the list filter, titles and the FX button
uc = new()
defs = []
cs = lambda p: bytes(uc.mem_read(p, 24)).split(b"\0")[0].decode()
def m4_def(uc, a, s, _):
    sp = uc.reg_read(UC_ARM_REG_SP)
    mn, mx, key = struct.unpack("<iiI", uc.mem_read(sp, 12))
    defs.append((uc.reg_read(UC_ARM_REG_R1), uc.reg_read(UC_ARM_REG_R2), cs(uc.reg_read(UC_ARM_REG_R3)), mn, mx, cs(key)))
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
uc.hook_add(UC_HOOK_CODE, m4_def, begin=0x08135E08, end=0x08135E08)
ldefs = []
def m4_ldef(uc, a, s, _):
    sp = uc.reg_read(UC_ARM_REG_SP)
    cnt, key = struct.unpack("<iI", uc.mem_read(sp, 8))
    ldefs.append((uc.reg_read(UC_ARM_REG_R1), cs(uc.reg_read(UC_ARM_REG_R2)), uc.reg_read(UC_ARM_REG_R3), cnt, cs(key)))
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
uc.hook_add(UC_HOOK_CODE, m4_ldef, begin=0x08135DB4, end=0x08135DB4)
STR = 0x24031000; uc.mem_write(STR, b"Lbl:\0\0\0\0key\0")
sp = 0x2407F000
uc.mem_write(sp, struct.pack("<iiI", -7, 9, STR + 8) + bytes(16))
for r_, v in zip(R, (0x24032000, 0x117, 5, STR)): uc.reg_write(r_, v)
uc.reg_write(UC_ARM_REG_SP, sp); uc.reg_write(UC_ARM_REG_LR, RET | 1)
uc.emu_start(s4["dly_defs"] | 1, RET, count=1_000_000)
ours = {d[0]: d for d in defs if d[0] in ids.values()}
print("     " + ", ".join(f"{d[2]}{d[5]}" for d in ours.values()))
lists = {d[0]: d for d in ldefs if d[0] in ids.values()}
print("     lists: " + ", ".join(f"{d[1]}{d[4]} x{d[3]}" for d in lists.values()))
check(len(ours) == 21 and ours[ids["CHO_ON"]][1] == 4 and ours[ids["D2_TIME"]][3:5] == (0, 1000) and ours[ids["D2_BEAT"]][1] == 4,
      "M4: the 21 send FX knobs and toggles defined")
names = lambda p, n: [cs(u32(uc, p + 4 * i)) for i in range(n)]
check(set(lists) == {ids["CHO_MODE"], ids["D2_SYNC"]} and lists[ids["CHO_MODE"]][3] == 3 and names(lists[ids["CHO_MODE"]][2], 3) == ["I", "II", "I+II"]
      and lists[ids["D2_SYNC"]][3] == 12 and names(lists[ids["D2_SYNC"]][2], 12)[::4] == ["1/64", "1/16D", "1/4"],
      "M4: Mode a list (I, II, I+II), Delay 2's synced Time the stock delay's 12 note values")
check(len(set(d[5] for d in ours.values()) | set(d[4] for d in lists.values())) == 23, "M4: each with its own key")
check(u32(uc, 0x38800FE4) == 0, "M4: boot leaves the FX button at the reverb")

# the reverb set: hall's table, then ours, ending at the common tail with r4, r5 intact
added = []
def m4_add(uc, a, size, _):
    added.append((uc.reg_read(UC_ARM_REG_R1), struct.unpack("<i", struct.pack("<I", uc.reg_read(UC_ARM_REG_R2)))[0]))
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
h = uc.hook_add(UC_HOOK_CODE, m4_add, begin=0x081205E2, end=0x081205E2)
uc.reg_write(UC_ARM_REG_R4, 0x24030000); uc.reg_write(UC_ARM_REG_R5, 0x1234); uc.reg_write(UC_ARM_REG_SP, 0x2407F000)
uc.emu_start(0x08120C1C | 1, 0x0812088C, count=2000)
uc.hook_del(h)
check(uc.reg_read(UC_ARM_REG_PC) == 0x0812088C and uc.reg_read(UC_ARM_REG_R4) == 0x24030000 and uc.reg_read(UC_ARM_REG_R5) == 0x1234
      and uc.reg_read(UC_ARM_REG_SP) == 0x2407F000, "M4 reverb set: reaches the common tail with r4, r5, sp intact")
check([a[0] for a in added[:10]] == [0x155, 0x159, 0x15A, 0x13E, 0x146, 0x143, 0x13D, 0x14A, 0x148, 0x14F] and len(added) == 33
      and {a[0] for a in added[10:]} == set(ids.values()), f"M4 reverb set: the reverb's 10, then the 23 send FX params ({len(added)})")

# the panel list, filtered by mode
OUT = 0x24034000
def lst(mode, slot=0x15):
    uc.mem_write(0x38800FE0, struct.pack("<II", 0x49555846, mode))
    def fake(uc, a, s, _):          # stands in for FUN_081227f0: the whole set, as it would list it
        out = uc.reg_read(UC_ARM_REG_R2)
        uc.mem_write(out, struct.pack("<I", len(added) - 1) + b"".join(struct.pack("<HHHHi", i, 0, 0, 0, v) for i, v in added[1:]))
        uc.reg_write(UC_ARM_REG_R0, 1); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    h = uc.hook_add(UC_HOOK_CODE, fake, begin=0x081227F0, end=0x081227F0)
    uc.mem_write(0x24035000, struct.pack("<H", slot))
    call(uc, s4["sfx_list"], 0x24036000, 0x24035000, OUT)
    uc.hook_del(h)
    n = u32(uc, OUT)
    return [struct.unpack("<HHHHi", uc.mem_read(OUT + 4 + 12 * i, 12)) for i in range(n)]
rv = lst(0)
check([e[0] for e in rv] == [0x159, 0x15A, 0x13E, 0x146, 0x143, 0x13D, 0x14A, 0x148, 0x14F], "panel, reverb: only the reverb's knobs")
c = lst(1); d = lst(2); e = lst(3)
check([x[0] for x in c] == [ids[k] for k in ("CHO_MODE", "CHO_RATE", "CHO_DEPTH", "CHO_LEVEL", "CHO_FX1", "CHO_FX2", "CHO_ON")] and c[3][4] == 1000, "panel, Chorus: Mode, Rate, Depth, Level, FX1 Send, FX2 Send, ON")
check([x[0] for x in d] == [ids[k] for k in ("DRV_DRIVE", "DRV_TONE", "DRV_LEVEL", "DRV_ON", "DRV_FX1", "DRV_FX2")], "panel, Drive: Drive, Tone, Level, ON, FX1 Send, FX2 Send")
check([x[0] for x in e] == [ids[k] for k in ("D2_TIME", "D2_FB", "D2_TONE", "D2_LEVEL", "D2_FX1", "D2_FX2", "D2_PING", "D2_BEAT", "D2_ON")], "panel, Delay 2: Time, Feedback, Tone, Level, FX1 Send, FX2 Send, PING, BEAT, ON")
i = [a[0] for a in added].index(ids["D2_BEAT"]); added[i] = (added[i][0], 1)
e = lst(3); added[i] = (added[i][0], 0)
check([x[0] for x in e][:2] == [ids["D2_SYNC"], ids["D2_FB"]] and ids["D2_TIME"] not in [x[0] for x in e] and e[0][4] == 8,
      "panel, Delay 2 with Beat Sync on: the note value (1/4) in Time's place")
check(len(lst(2, slot=0x14)) == len(added) - 1, "panel, delay slot: untouched")

texts = []
def text(uc, a, s, _):
    texts.append(cs(uc.reg_read(UC_ARM_REG_R1))); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
uc.hook_add(UC_HOOK_CODE, text, begin=0x081427E2, end=0x081427E2)
uc.mem_write(0x24037000, b"FX2\0Reverb\0")
for m in range(4):
    uc.mem_write(0x38800FE0, struct.pack("<II", 0x49555846, m))
    call(uc, s4["sfx_title"], 0x24038000, 0x24037000); call(uc, s4["sfx_name"], 0x24038000, 0x24037004)
check(texts == ["FX2", "Reverb", "FX3", "Chorus", "FX4", "Drive", "FX5", "Delay 2"], f"panel titles: {texts}")

views = []
stub(uc, 0x08123158, views)
uc.mem_write(0x38800FE0, struct.pack("<II", 0x49555846, 2))
call(uc, s4["sfx_to_reverb"], 0x24039000, 0x10, 0, 0)
modes = [u32(uc, 0x38800FE4)]
for _ in range(4):
    call(uc, s4["sfx_from_reverb"], 0x24039000, 0x13, 0, 0)
    modes.append(u32(uc, 0x38800FE4))
check([v[1] for v in views] == [0x10, 0x10, 0x10, 0x10, 0x13] and modes == [0, 1, 2, 3, 0],
      f"FX button: Delay -> Reverb -> Chorus -> Drive -> Delay 2 -> view 0x13 ({[hex(v[1]) for v in views]}, modes {modes})")
print("all passed")
