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
print(f"     loud: over {over / 10:.1f} dB, GR {gr / 10:.1f} dB, seq {seq}")
check(magic == 0x524D4F43 and on == 1 and seq == 3, "M7 publishes a report every 32 blocks, compressor on")
check(over > 0, "0 dBFS sine: level past the threshold")
check(120 <= gr <= 250, "0 dBFS sine: gain reduction reported")
check(uc.nexts and all(n == (NEXT, CTX, NEXT) for n in uc.nexts) and uc.ret == 7, "next node run once per block, its result returned, obj+8 restored")
check(len(uc.nexts) == 96, "next node not run twice (stock compressor didn't chain)")

uc, _ = run(COMP_ON, 0.01)
magic, over, gr, on, seq = meter(uc)
print(f"     quiet: over {over / 10:.1f} dB, GR {gr / 10:.1f} dB")
check(over < 0 and gr <= 5, "-40 dBFS sine: below threshold, no gain reduction")

uc, _ = run([(0xB5, 0)], 1.0)
check(meter(uc)[3] == 0, "compressor off: reported off")

uc, _ = run([], 0.5, blocks=2, with_next=False)
check(uc.ret == 1 and not uc.nexts, "no next node: returns 1 like stock")

# Drive 0 (and never set): bit-identical to the stock compressor
for evs in ([(0xB5, 0)], COMP_ON):
    _, a = run(evs, 0.7, blocks=8)
    _, b = run(evs, 0.7, blocks=8, fn=0x08042A90)
    check(a == b, f"Drive 0, comp {'on' if evs == COMP_ON else 'off'}: output identical to stock")

# Drive 1000: full scale stays in bounds, quiet gets louder, the change ramps
_, plain = run([(0xB5, 0)], 0.1, blocks=4)
_, hot = run([(0xB5, 0), (0x43, 1000)], 0.1, blocks=4)
pk_plain = max(abs(x) for x in plain[-1][0]); pk_hot = max(abs(x) for x in hot[-1][0])
print(f"     -20 dBFS sine: {20 * math.log10(pk_plain):.1f} dB -> {20 * math.log10(pk_hot):.1f} dB at full Drive")
check(pk_hot > pk_plain * 4, "full Drive: quiet signal much louder")
_, full = run([(0xB5, 0), (0x43, 1000)], 1.0, blocks=4)
check(max(abs(x) for blk in full for ch in blk for x in ch) <= 1.0 + 1e-6, "full Drive, 0 dBFS in: peaks stay within full scale")
_, half = run([(0xB5, 0), (0x43, 500)], 0.1, blocks=4)
pk_half = max(abs(x) for x in half[-1][0])
check(pk_plain < pk_half < pk_hot, "half Drive sits between")
first = hot[0][0]; steady = hot[-1][0]                     # first block ramps from a = 0
ratios = [abs(first[i] / plain[0][0][i]) for i in range(4, N) if abs(plain[0][0][i]) > 0.02]
check(ratios[0] < ratios[-1] and ratios[0] < 2, f"Drive change ramps across the block (gain {ratios[0]:.2f} -> {ratios[-1]:.2f})")

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
check(m1 - base < 3000, "saturator + meter under ~3000 instructions per 32-sample block (~0.6 % of the M7)")

# ---- M4: the compressor meter on the settings page
def m4(page_on, mst, fmt=4, bpp=2):
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
    uc.mem_write(0x38800F00, struct.pack("<IHHI", 0x43505542, 500, 800, 1))
    if mst: uc.mem_write(0x38800F40, struct.pack("<IhHBxxxI", 0x524D4F43, mst[0], 0, mst[1], 1))
    DESC, FB, DISP = 0x24030000, 0xC0000000, 0x24040000
    uc.mem_write(DESC, struct.pack("<IHHBBH", FB, 320, 240, fmt, 0, bpp))
    def stub(addr, fn):
        def h(uc, a, size, _): fn(uc); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        uc.hook_add(UC_HOOK_CODE, h, begin=addr, end=addr)
    stub(0x081013D2, lambda uc: uc.reg_write(UC_ARM_REG_R0, DESC)); stub(0x08101384, lambda uc: None)
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), (DISP, 1, 0, 0)): uc.reg_write(r, v)
    uc.reg_write(UC_ARM_REG_SP, 0x2407F000); uc.reg_write(UC_ARM_REG_LR, RET | 1)
    uc.emu_start(s4["cpu_present"] | 1, RET, count=200000)
    return [struct.unpack("<H", uc.mem_read(FB + (r * 320 + 315) * 2, 2))[0] for r in range(3, 17)]

G, R, W, K = 0x064A, 0xF945, 0xFFFF, 0x3186
def show(c): return " ".join({G: "g", R: "R", W: "W", K: "."}.get(x, f"{x:x}") for x in c)
cpu = m4(False, (60, 1)); print("     not settings:", show(cpu))
check(cpu[-7:] == [G] * 7 and cpu[3] == W, "other pages: CPU bar as before")
c = m4(True, (-150, 1)); print("     -15 dB:", show(c))
c_r = c[::-1]                                              # bottom first
check(c_r[:5] == [G] * 5 and c_r[10] == W and c_r[5:10] == [K] * 5, "settings, 15 dB under: green, white threshold mark")
c = m4(True, (60, 1)); print("     +6 dB:", show(c)); c_r = c[::-1]
check(c_r[:10] == [G] * 10 and c_r[10:12] == [R] * 2 and c_r[12:] == [K] * 2, "settings, 6 dB over: red past the threshold")
c = m4(True, (60, 0))
check(c == cpu, "settings, compressor off: CPU bar")
c = m4(True, None)
check(c == cpu, "settings, no M7 report yet: CPU bar")
print("all passed")
