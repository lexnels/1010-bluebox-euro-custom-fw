"""Run the master patchset under Unicorn: the compressor meter and saturator (M7), the Saturate row and the settings-page
compressor meter (M4)."""
import struct, math, sys
from unicorn import *
from unicorn.arm_const import *

IMG = sys.argv[1] if len(sys.argv) > 1 else "out/cpu+hall+delay+master/BLUEEURO.BIN"
data = open(IMG, "rb").read()
RET = 0x0807FFF0
sys.path.insert(0, "patches")
from thumb import symbols, bl
s7, s4, d4 = symbols("out/mst7.elf"), symbols("out/cm4.elf"), symbols("out/dly4.elf")

def check(cond, msg):
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond: sys.exit(1)

def at(addr, n):
    o = addr - 0x08040000 if addr < 0x08100000 else addr - 0x08100000 + 0xC0000
    return data[o:o + n]

# ---- patch bytes
check(struct.unpack("<I", at(0x0806ACBC, 4))[0] == s7["mst_process"] | 1, "compressor process slot -> mst_process")
check(at(0x08120B90, 4) == bl(0x08120B90, d4["mst_set_add"]), "global set's last add -> mst_set_add")
ids = struct.unpack("<4H", at(0x0814D964, 8))
check(ids[:3] == (0x13B, 0x43, 0), f"settings list ends 0x13b, 0x43, 0 ({' '.join(hex(i) for i in ids)})")

def f2u(f): return struct.unpack("<I", struct.pack("<f", f))[0]

# ---- M7
def m7():
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS); uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M7)
    uc.mem_map(0x08000000, 0x200000); uc.mem_write(0x08040000, data[:0xA0000])
    for b, s in [(0x20000000, 0x20000), (0x24000000, 0x80000), (0x30000000, 0x48000), (0x38000000, 0x10000),
                 (0x38800000, 0x1000), (0x58024000, 0x1000), (0xE000E000, 0x1000), (0xC0000000, 0x2000000)]:
        uc.mem_map(b, s)
    heap = [0xC0000000]
    def alloc(uc, a, s, _):
        n = uc.reg_read(UC_ARM_REG_R0); p = heap[0]; heap[0] = (p + n + 15) & ~15
        uc.reg_write(UC_ARM_REG_R0, p); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    for f in (0x080693DC, 0x080413D0): uc.hook_add(UC_HOOK_CODE, alloc, begin=f, end=f)
    def ms(uc, a, s, _):
        if uc.reg_read(UC_ARM_REG_R0) < 0x08000000: uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, ms, begin=0x08056600, end=0x08056600)
    def f64fix(uc, a, s, _):        # Unicorn can't run the double compare here
        v = struct.unpack("<f", struct.pack("<I", uc.reg_read(UC_ARM_REG_S14)))[0]
        fl = 0x2 if v > 1e-4 else (0x6 if v == 1e-4 else 0x8)
        uc.reg_write(UC_ARM_REG_FPSCR, (uc.reg_read(UC_ARM_REG_FPSCR) & 0x0FFFFFFF) | (fl << 28))
        uc.reg_write(UC_ARM_REG_PC, 0x08042C6C | 1)
    uc.hook_add(UC_HOOK_CODE, f64fix, begin=0x08042C64, end=0x08042C64)
    uc.heap = heap
    return uc

def call(uc, fn, *args):
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args): uc.reg_write(r, v)
    uc.reg_write(UC_ARM_REG_SP, 0x2407F000); uc.reg_write(UC_ARM_REG_LR, RET | 1)
    uc.emu_start(fn | 1, RET, count=50_000_000)
    return uc.reg_read(UC_ARM_REG_R0)

N, CTX, BUFS = 32, 0x30000000, 0x24020000
QUEUE = CTX + 0xC * 0x604 + 4
NEXT, NEXT_VT, NEXT_FN = 0x24030000, 0x24030100, 0x0807FF00

def setup(evs, with_next=True):
    uc = m7()
    obj = uc.heap[0]; uc.heap[0] += 0x130
    call(uc, 0x080427A8, obj, 48000, 32)
    uc.mem_write(obj + 0x18, struct.pack("<I", 0xC)); uc.mem_write(obj + 0x1E, struct.pack("<H", 12))
    uc.mem_write(NEXT_VT + 12, struct.pack("<I", NEXT_FN | 1)); uc.mem_write(NEXT, struct.pack("<I", NEXT_VT))
    uc.mem_write(obj + 8, struct.pack("<I", NEXT if with_next else 0))
    uc.nexts = []
    def nxt(uc, a, s, _):
        uc.nexts.append((uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1),
                         struct.unpack("<I", uc.mem_read(obj + 8, 4))[0]))
        uc.reg_write(UC_ARM_REG_R0, 7); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, nxt, begin=NEXT_FN, end=NEXT_FN)
    uc.obj = obj
    events(uc, evs)
    return uc

def events(uc, evs):
    for i, (pid, val) in enumerate(evs):
        uc.mem_write(QUEUE + i * 24, struct.pack("<B7xIHHiI", 0x39, 0xC, pid, 0, val, 0))
    uc.mem_write(QUEUE + 0x600, struct.pack("<I", len(evs)))

def block(uc, l, r, fn=None):
    uc.mem_write(CTX + 0x150E4 + 12 * 20, struct.pack("<IIIIBBxx", N, N, BUFS, BUFS + 0x100, 0, 1))
    uc.mem_write(BUFS, struct.pack(f"<{N}f", *l)); uc.mem_write(BUFS + 0x100, struct.pack(f"<{N}f", *r))
    ret = call(uc, fn or s7["mst_process"], uc.obj, CTX)
    events(uc, [])
    return (struct.unpack(f"<{N}f", uc.mem_read(BUFS, 4 * N)), struct.unpack(f"<{N}f", uc.mem_read(BUFS + 0x100, 4 * N)), ret)

def sine(amp, n0, f=1000.0):
    return [amp * math.sin(2 * math.pi * f * (n0 + i) / 48000) for i in range(N)]

def meter(uc):
    return struct.unpack("<IhHBxxxI", uc.mem_read(0x38800F40, 16))

def run(evs, amp, blocks=96, fn=None, with_next=True):
    uc = setup(evs, with_next)
    out = []
    for b in range(blocks):
        s = sine(amp, b * N)
        l, r, ret = block(uc, s, s, fn)
        out.append((l, r))
    uc.ret = ret
    return uc, out

COMP_ON = [(0xB5, 1), (0x135, 0)]
uc, _ = run(COMP_ON, 1.0)
magic, over, gr, on, seq = meter(uc)
lg = struct.unpack("<f", uc.mem_read(uc.obj + 0xC0, 4))[0]; mk = struct.unpack("<f", uc.mem_read(uc.obj + 0xFC, 4))[0]
print(f"     loud: GR {gr / 10:.1f} dB (compressor's own gain now {6.0206 * (lg - mk):.1f} dB), seq {seq}")
check(magic == 0x524D4F43 and on == 1 and seq == 3, "M7 publishes a report every 32 blocks, compressor on")
check(abs(gr / 10 + 6.0206 * (lg - mk)) < 1.0, "0 dBFS sine: reported GR matches the compressor's gain (~17 dB at 4:1, -20 dB)")
check(uc.nexts and all(n == (NEXT, CTX, NEXT) for n in uc.nexts) and uc.ret == 7, "next node run once per block, its result returned, obj+8 restored")
check(len(uc.nexts) == 96, "next node not run twice (stock compressor didn't chain)")

uc, _ = run(COMP_ON, 0.01)
gr = meter(uc)[2]
print(f"     quiet: GR {gr / 10:.1f} dB")
check(gr <= 5, "-40 dBFS sine: no gain reduction")

uc, _ = run([(0xB5, 0)], 1.0)
check(meter(uc)[3] == 0, "compressor off: reported off")

uc, _ = run([], 0.5, blocks=2, with_next=False)
check(uc.ret == 1 and not uc.nexts, "no next node: returns 1 like stock")

# Drive 0 (and never set): bit-identical to the stock compressor
OFF = [(0xB5, 0)]
for evs in (OFF, COMP_ON):
    _, a = run(evs, 0.7, blocks=8)
    _, b = run(evs, 0.7, blocks=8, fn=0x08042A90)
    check(a == b, f"Drive 0, comp {'on' if evs == COMP_ON else 'off'}: output identical to stock")

# Saturate: overdrive with the level held around -14 dBFS peaks
def peak(out): return max(abs(x) for x in out[-1][0])
def rms_db(out): v = out[-1][0]; return 10 * math.log10(sum(x * x for x in v) / len(v))
lv = {}
for amp in (1.0, 0.2, 0.05):
    row = []
    for drv in (0, 250, 500, 1000):
        _, o = run(OFF + ([(0x43, drv)] if drv else []), amp, blocks=4)
        row.append((peak(o), rms_db(o)))
    lv[amp] = row
    print(f"     {20 * math.log10(amp):5.1f} dBFS sine, Drive 0/250/500/1000: peak " +
          " ".join(f"{20 * math.log10(p):6.1f}" for p, _ in row) + " dB, rms " + " ".join(f"{r:6.1f}" for _, r in row))
check(abs(20 * math.log10(lv[0.2][3][0]) + 14) < 0.5, "full Drive: output peaks sit at -14 dBFS")
check(lv[1.0][3][1] < lv[1.0][0][1] - 6, "full Drive, 0 dBFS in: quieter, not louder")
check(lv[1.0][1][1] < lv[1.0][0][1] + 0.5 and lv[0.2][1][1] < lv[0.2][0][1] + 3, "quarter Drive: no big level jump")
check(lv[0.05][3][1] - lv[0.05][0][1] < 18, "full Drive, -26 dBFS in: lifted, but less than the 36 dB of drive")
_, o = run(OFF + [(0x43, 1000)], 1.0, blocks=8)
check(max(abs(x) for blk in o for ch in blk for x in ch) <= 1.0, "turning Drive up never overshoots")
# small Drive fades in: Drive 10 changes little
_, plain = run(OFF, 0.7, blocks=4)
_, tiny = run(OFF + [(0x43, 10)], 0.7, blocks=4)
dev = max(abs(a - b) for a, b in zip(plain[-1][0], tiny[-1][0]))
check(dev < 0.05, f"Drive 1 %: barely changes the sound (max difference {dev:.3f}), no jump leaving 0")
# a ramp: first block after the change moves from dry towards wet
_, hot = run(OFF + [(0x43, 1000)], 0.7, blocks=2)
d0 = abs(hot[0][0][1] - plain[0][0][1]); d1 = abs(hot[0][0][-1] - plain[0][0][-1])
check(d0 < 0.05, f"Drive change ramps in across the block (first sample off by {d0:.3f})")

# cost per block, on top of the stock compressor
def cost(evs, fn):
    uc = setup(evs)
    for b in range(4): block(uc, sine(0.5, b * N), sine(0.5, b * N), fn)
    n = [0]
    h = uc.hook_add(UC_HOOK_CODE, lambda *a: n.__setitem__(0, n[0] + 1))
    block(uc, sine(0.5, 4 * N), sine(0.5, 4 * N), fn)
    uc.hook_del(h)
    return n[0]
base = cost(COMP_ON, 0x08042A90); m0 = cost(COMP_ON, s7["mst_process"]); m1 = cost(COMP_ON + [(0x43, 700)], s7["mst_process"])
print(f"     instructions/block: stock {base}, + meter {m0 - base}, + saturator {m1 - base}")
check(m0 - base < 250, "meter alone: under 250 instructions per block")
check(m1 - base < 3000, "saturator + meter under ~3000 instructions per 32-sample block (~0.6 % of the M7)")

# ---- M4: the CPU bar as before, the gain-reduction bar in the settings list's Thresh row
LIST_Y, LIST_H = 0, 180
def m4(page_on, mst, k_thr=27, scroll=700, hidden=False, fmt=4, bpp=2):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    uc.mem_map(0x08000000, 0x200000); uc.mem_write(0x08040000, data[:0xA0000]); uc.mem_write(0x08100000, data[0xC0000:])
    for base, size in [(0x24000000, 0x80000), (0x30000000, 0x48000), (0x38800000, 0x1000), (0x58024000, 0x1000),
                       (0xE000E000, 0x1000), (0xC0000000, 0x40000)]:
        uc.mem_map(base, size)
    APP = 0x30010000
    uc.mem_write(0x3000E63C, struct.pack("<I", APP))
    page = APP + 0x1E9E8 if page_on else APP + 0x1000
    uc.mem_write(APP + 0x2B4, struct.pack("<I", page))
    uc.mem_write(APP + 0x1E9E8 + 0xA3E0, struct.pack("<H", 0xC))
    lst = APP + 0x1E9E8 + 0x9C8
    uc.mem_write(lst + 4, struct.pack("<iiii", 0, LIST_Y, 320, LIST_H))
    uc.mem_write(lst + 0x99E8, struct.pack("<I", 38))
    for k in range(38):                                    # as FUN_081400e2 lays them out
        row = lst + 0xE8 + k * 0x330
        y = scroll + LIST_H - 30 * (k + 1)
        uc.mem_write(row + 4, struct.pack("<iiii", 0, y + LIST_Y, 320, 28))
        uc.mem_write(row + 0x30, bytes([1 if hidden or y > LIST_H or y + 28 < 0 else 0]))
        uc.mem_write(row + 0x32C, struct.pack("<H", 0x132 if k == k_thr else 0x100 + k))
    uc.mem_write(0x38800F00, struct.pack("<IHHI", 0x43505542, 500, 800, 1))
    if mst: uc.mem_write(0x38800F40, struct.pack("<IhHBxxxI", 0x524D4F43, 0, mst[0], mst[1], 1))
    DESC, FB, DISP = 0x24030000, 0xC0000000, 0x24040000
    uc.mem_write(DESC, struct.pack("<IHHBBH", FB, 320, 240, fmt, 0, bpp))
    def stub(addr, fn):
        def h(uc, a, size, _): fn(uc); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        uc.hook_add(UC_HOOK_CODE, h, begin=addr, end=addr)
    stub(0x081013D2, lambda uc: uc.reg_write(UC_ARM_REG_R0, DESC)); stub(0x08101384, lambda uc: None)
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), (DISP, 1, 0, 0)): uc.reg_write(r, v)
    uc.reg_write(UC_ARM_REG_SP, 0x2407F000); uc.reg_write(UC_ARM_REG_LR, RET | 1)
    uc.emu_start(s4["cpu_present"] | 1, RET, count=2000000)
    fb = bytes(uc.mem_read(FB, 320 * 240 * 2))
    px = lambda x, r: struct.unpack_from("<H", fb, (r * 320 + x) * 2)[0]
    cpu = [px(315, r) for r in range(3, 17)]
    rest = [(x, r) for r in range(20, 240) for x in range(0, 320, 4) if px(x, r)]
    return cpu, rest, px

G, W, K = 0x064A, 0xFFFF, 0x3186
BLUE, TICK, TRACK = 0x2CBF, 0x5332, 0x1108
cpu0, rest, _ = m4(False, (100, 1))
check(cpu0[-7:] == [G] * 7 and cpu0[3] == W and not rest, "other pages: CPU bar as before, nothing else drawn")
cpu, rest, px = m4(True, (100, 1))                         # Thresh row: y = 700 + 180 - 840 = 40 -> screen rows 172..199
y = 40
rows = sorted({r for _, r in rest})
print(f"     settings, 10 dB GR: drawn on screen rows {rows}, row 199: {px(0, 199):04x} {px(159, 199):04x} {px(160, 199):04x} {px(161, 199):04x} {px(319, 199):04x}")
check(cpu == cpu0, "settings page: CPU bar unchanged")
check(rows == [237 - y, 238 - y, 239 - y], "bar on the Thresh row's bottom 3 px, under its label")
check(all(px(x, 239 - y) == BLUE for x in range(0, 160)) and px(160, 239 - y) == TICK and px(161, 239 - y) == TRACK and px(319, 239 - y) == TRACK,
      "10 dB of GR fills half the row in blue, then the 10 dB tick and the dark track")
_, rest, px = m4(True, (0, 1))
check({px(x, 199) for x in range(320)} == {TRACK, TICK} and [x for x in range(320) if px(x, 199) == TICK] == [80, 160, 240], "no GR: dark track, ticks at 5/10/15 dB")
_, rest, px = m4(True, (400, 1))
check(all(px(x, 199) == BLUE for x in range(320)), "20 dB or more: full row")
check(not m4(True, (100, 1), scroll=0)[1], "Thresh row scrolled out of view: nothing drawn")
check(not m4(True, (100, 1), scroll=660 + 155)[1], "Thresh row half off the top of the list: nothing drawn")
check(not m4(True, (100, 0))[1], "compressor off: nothing drawn")
check(not m4(True, None)[1], "no M7 report yet: nothing drawn")

# ---- M4: comp_tick forces a redraw while the bar would move, ~15 fps
check(at(0x081351A6, 4) == bl(0x081351A6, s4["comp_tick"]), "UI loop's redraw check -> comp_tick")
def ticker(page_on=True, on=1):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    uc.mem_map(0x08000000, 0x200000); uc.mem_write(0x08040000, data[:0xA0000]); uc.mem_write(0x08100000, data[0xC0000:])
    for base, size in [(0x24000000, 0x80000), (0x30000000, 0x48000), (0x38800000, 0x1000), (0x58024000, 0x1000)]:
        uc.mem_map(base, size)
    APP = 0x30010000
    uc.mem_write(0x3000E63C, struct.pack("<I", APP))
    uc.mem_write(APP + 0x2B4, struct.pack("<I", APP + 0x1E9E8 if page_on else APP + 0x1000))
    uc.mem_write(APP + 0x1E9E8 + 0xA3E0, struct.pack("<H", 0xC))
    calls = []
    def h(uc, a, size, _):
        p = uc.reg_read(UC_ARM_REG_R0)
        calls.append(bytes(uc.mem_read(p + 0x38, 2)))
        uc.mem_write(p + 0x38, bytes(2))                       # the stock check reads and clears them
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, h, begin=0x0813AA9E, end=0x0813AA9E)
    def tick(seq, gr):
        uc.mem_write(0x38800F40, struct.pack("<IhHBxxxI", 0x524D4F43, 0, gr, on, seq))
        uc.reg_write(UC_ARM_REG_R0, APP + 0x280); uc.reg_write(UC_ARM_REG_R1, 0x24001000)
        uc.reg_write(UC_ARM_REG_SP, 0x2407F000); uc.reg_write(UC_ARM_REG_LR, RET | 1)
        uc.emu_start(s4["comp_tick"] | 1, RET, count=100000)
        return calls[-1] == b"\x01\x01"
    return tick, calls
tick, calls = ticker()
seq_gr = [(s, 50 + s) for s in range(1, 31)]                    # GR moving every report, the UI loop polling 4x per report
forced = [tick(s, g) for s, g in seq_gr for _ in range(4)]
print(f"     moving GR: {sum(forced)} redraws over {len(seq_gr)} reports ({sum(forced) * 46.875 / len(seq_gr):.1f} fps)")
check(len(calls) == 120, "stock redraw check still runs every time")
check(9 <= sum(forced) <= 11, "moving GR: a redraw every 3rd report, ~15 fps")
forced = [tick(s, 80) for s in range(31, 61) for _ in range(4)]
check(sum(forced) <= 1, "steady GR: no extra redraws")
tick, _ = ticker(page_on=False)
check(not any(tick(s, s) for s in range(1, 30)), "other pages: no extra redraws")
tick, _ = ticker(on=0)
check(not any(tick(s, s) for s in range(1, 30)), "compressor off: no extra redraws")
print("all passed")
