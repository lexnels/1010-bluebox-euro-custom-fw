"""Run the USB audio out mode (patches/usb.py) under Unicorn: the USB Out setting, the mode switch and re-enumeration,
the 2-channel descriptor, the ring, DataIn and the fixed sends, against the stock USB code where it runs unchanged."""
import struct, sys
from unicorn import *
from unicorn.arm_const import *

IMG = sys.argv[1] if len(sys.argv) > 1 else "out/cpu+hall+delay+master+usb/BLUEEURO.BIN"
data = open(IMG, "rb").read()
stock = open("firmware/BLUEEURO-3.bin", "rb").read()
RET = 0x0807FFF0
R = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
sys.path.insert(0, "patches")
from thumb import symbols, bl
u7, m7s, d4 = symbols("out/usb7.elf"), symbols("out/mst7.elf"), symbols("out/dly4.elf")

def check(cond, msg):
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond: sys.exit(1)

def at(addr, n, img=data):
    o = addr - 0x08040000 if addr < 0x08100000 else addr - 0x08100000 + 0xC0000
    return img[o:o + n]

DESC = 0x240000A8
FLASH_DESC = at(0x0808A9C4, 326, stock)

def new():
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS); uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M7)
    uc.mem_map(0x08000000, 0x200000); uc.mem_write(0x08040000, data[:0xA0000]); uc.mem_write(0x08100000, data[0xC0000:])
    for b, s in [(0x20000000, 0x20000), (0x24000000, 0x80000), (0x30000000, 0x48000), (0x38800000, 0x1000),
                 (0x40040000, 0x1000), (0x58024000, 0x1000), (0xE000E000, 0x1000), (0xC0000000, 0x100000)]:
        uc.mem_map(b, s)
    uc.mem_write(0x24000000, at(0x0808A91C, 0x20C))          # .data, as the reset handler copies it
    uc.mem_write(0xE000ED88, struct.pack("<I", 0xF << 20))
    return uc

def call(uc, fn, *args, stack=()):
    sp = 0x2407F000 - 4 * len(stack)
    uc.mem_write(sp, b"".join(struct.pack("<I", v & 0xFFFFFFFF) for v in stack))
    for r, v in zip(R, args): uc.reg_write(r, v & 0xFFFFFFFF)
    uc.reg_write(UC_ARM_REG_SP, sp); uc.reg_write(UC_ARM_REG_LR, RET | 1)
    uc.emu_start(fn | 1, RET, count=5_000_000)
    return uc.reg_read(UC_ARM_REG_R0)

def stub(uc, addr, rec=None, rv=0):
    def h(uc, a, s, _):
        if rec is not None:
            sp = uc.reg_read(UC_ARM_REG_SP)
            rec.append(tuple(uc.reg_read(r) for r in R) + (struct.unpack("<I", uc.mem_read(sp, 4))[0],))
        uc.reg_write(UC_ARM_REG_R0, rv() if callable(rv) else rv); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, h, begin=addr, end=addr)

u32 = lambda uc, a: struct.unpack("<I", uc.mem_read(a, 4))[0]
w32 = lambda uc, a, v: uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))
desc = lambda uc: bytes(uc.mem_read(DESC, 326))

def bl_target(addr):
    h1, h2 = struct.unpack("<HH", at(addr, 4))
    s = (h1 >> 10) & 1; i1 = 1 - (((h2 >> 13) & 1) ^ s); i2 = 1 - (((h2 >> 11) & 1) ^ s)
    imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
    return addr + 4 + (imm - (1 << 25) if s else imm)

# ---- patch sites
word = lambda a: struct.unpack("<I", at(a, 4))[0]
check(word(0x08076C2C) == u7["usb_init"] | 1 and word(0x08076C40) == u7["usb_datain"] | 1, "USB class Init and DataIn -> ours")
check(word(0x08076C88) == u7["usb_serial"] | 1, "serial string callback -> ours")
check(bl_target(0x0804115A) == u7["usb_push"] and bl_target(0x08041166) == u7["usb_tick"], "audio loop's ring write and USB watchdog -> ours")
check(all(bl_target(a) == u7["usb_tx82"] for a in (0x08065AB4, 0x08065D1A, 0x08066640)), "the three fixed EP 0x82 sends -> usb_tx82")
ids = struct.unpack("<42H", at(0x0814D91C, 84))
check(ids[:4] == (0x178, 0x59, 0x118, 0x1F7) and ids[37:40] == (0x13B, 0x43, 0), "settings list: PhonesSrc, USB Out, USB Rcv, ..., Gain, Saturate")

# ---- M4: the setting's definition and its place in the global set
uc = new()
defs, lists = [], []
cs = lambda p: bytes(uc.mem_read(p, 24)).split(b"\0")[0].decode()
def m4_def(uc, a, s, _):
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
def m4_list(uc, a, s, _):
    sp = uc.reg_read(UC_ARM_REG_SP)
    count, key = struct.unpack("<iI", uc.mem_read(sp, 8))
    names = [cs(u32(uc, uc.reg_read(UC_ARM_REG_R3) + 4 * i)) for i in range(count)]
    lists.append((uc.reg_read(UC_ARM_REG_R1), cs(uc.reg_read(UC_ARM_REG_R2)), names, cs(key)))
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
uc.hook_add(UC_HOOK_CODE, m4_def, begin=0x08135E08, end=0x08135E08)
uc.hook_add(UC_HOOK_CODE, m4_list, begin=0x08135DB4, end=0x08135DB4)
STR = 0x24031000; uc.mem_write(STR, b"Lbl:\0\0\0\0key\0")
call(uc, d4["dly_defs"], 0x24032000, 0x117, 5, STR, stack=(-7, 9, STR + 8))
check(lists == [(0x59, "USB Out:", ["Multichannel", "Master only"], "usbout")], f"M4: USB Out defined as a list: {lists}")
added = []
stub(uc, 0x081205E2, added)
call(uc, d4["mst_set_add"], 0x24033000, 0x162, 0)
check([(a[1], a[2]) for a in added] == [(0x162, 0), (0x43, 0), (0x59, 0)], "M4: global set gets Saturate and USB Out (default Multichannel)")

# ---- M7: the setting arrives with the master compressor's events (src/mst_m7.c)
def comp_with_events(evs):
    uc = new()
    heap = [0xC0000000]
    def alloc(uc, a, s, _):
        n = uc.reg_read(UC_ARM_REG_R0); p = heap[0]; heap[0] = (p + n + 15) & ~15
        uc.reg_write(UC_ARM_REG_R0, p); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    for f in (0x080693DC, 0x080413D0): uc.hook_add(UC_HOOK_CODE, alloc, begin=f, end=f)
    def ms(uc, a, s, _):
        if uc.reg_read(UC_ARM_REG_R0) < 0x08000000: uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, ms, begin=0x08056600, end=0x08056600)
    def f64fix(uc, a, s, _):
        uc.reg_write(UC_ARM_REG_FPSCR, (uc.reg_read(UC_ARM_REG_FPSCR) & 0x0FFFFFFF) | (0x8 << 28))
        uc.reg_write(UC_ARM_REG_PC, 0x08042C6C | 1)
    uc.hook_add(UC_HOOK_CODE, f64fix, begin=0x08042C64, end=0x08042C64)
    obj = heap[0]; heap[0] += 0x130
    CTX = 0x30000000
    call(uc, 0x080427A8, obj, 48000, 32)
    uc.mem_write(obj + 0x18, struct.pack("<I", 0xC)); uc.mem_write(obj + 0x1E, struct.pack("<H", 12)); w32(uc, obj + 8, 0)
    Q = CTX + 0xC * 0x604 + 4
    for i, (pid, val) in enumerate(evs):
        uc.mem_write(Q + i * 24, struct.pack("<B7xIHHiI", 0x39, 0xC, pid, 0, val, 0))
    w32(uc, Q + 0x600, len(evs))
    uc.mem_write(CTX + 0x150E4 + 12 * 20, struct.pack("<IIIIBBxx", 32, 32, 0x24020000, 0x24020100, 0, 1))
    call(uc, m7s["mst_process"], obj, CTX)
    return u32(uc, 0x38800F84)
check(comp_with_events([(0x59, 1)]) == 1 and comp_with_events([(0x59, 0)]) == 0, "M7: USB Out events reach the USB state")

# ---- M7: mode switch (usb_tick in place of the stock host-loss watchdog)
def tick_rig(want, started=1):
    uc = new()
    now = [1000]; wd = []
    stub(uc, 0x0805E188, rv=lambda: now[0]); stub(uc, 0x080655DC, wd)
    uc.mem_write(0x38800F80, struct.pack("<IIII", 0x4253554D, want, 0, 0))
    w32(uc, 0x2400B9B4, started); w32(uc, 0x40040804, 0)
    w32(uc, 0x2400B9A0, 7); w32(uc, 0x2400B998, 1); w32(uc, 0x2400B9A4, 3); w32(uc, 0x2400B9A8, 99)
    def tick(t):
        now[0] = 1000 + t
        call(uc, u7["usb_tick"], 11, 22, 33, 44)
        return u32(uc, 0x40040804) & 2, desc(uc)
    return uc, tick, wd

uc, tick, wd = tick_rig(0)
sdis, d = tick(0)
check(not sdis and d == FLASH_DESC and wd == [(11, 22, 33, 44, wd[0][4])], "no change wanted: stock watchdog runs with its arguments, descriptor untouched")

uc, tick, wd = tick_rig(1)
sdis0, d0 = tick(0)
sdis1, d1 = tick(10)
sdis2, d2 = tick(25)
sdis3, d3 = tick(200)
sdis4, d4_ = tick(260)
print(f"     master only: t=0 SDIS {sdis0 >> 1}, t=25 desc changed {d2 != FLASH_DESC}, t=260 SDIS {sdis4 >> 1}; stock watchdog calls {len(wd)}")
check(sdis0 and d0 == FLASH_DESC and sdis1 and d1 == FLASH_DESC, "switch: soft-disconnects first, descriptor unchanged for 20 ms")
check(sdis2 and sdis3 and d2 != FLASH_DESC and d3 == d2, "switch: descriptor rewritten while disconnected")
check(not sdis4 and not wd, "switch: reconnects at 250 ms; the stock watchdog sat out")
check(u32(uc, 0x2400B9A0) == 0 and u32(uc, 0x2400B998) == 0 and u32(uc, 0x2400B9A4) == 99, "switch: the stock watchdog starts fresh")
tick(300)
check(len(wd) == 1, "after the switch the stock watchdog runs again")

# the 2-channel descriptor
def walk(cfg):
    out, j = [], 0
    while j < len(cfg):
        out.append(cfg[j:j + cfg[j]]); j += cfg[j]
    return out
diff = [i for i in range(326) if d2[i] != FLASH_DESC[i]]
print("     changed bytes:", " ".join(f"+{i:#x}:{FLASH_DESC[i]:02x}->{d2[i]:02x}" for i in diff))
it4 = [x for x in walk(d2) if x[1] == 0x24 and x[2] == 2 and x[3] == 4][0]
asg = [x for x in walk(d2) if len(x) == 16 and x[1] == 0x24 and x[2] == 1 and x[3] == 2][0]
ep82 = [x for x in walk(d2) if x[1] == 5 and x[2] == 0x82][0]
check(sum(len(x) for x in walk(d2)) == 326 and d2[2] | d2[3] << 8 == 326, "2-ch descriptor: still 326 bytes of well-formed descriptors")
check(it4[8] == 2 and struct.unpack("<I", it4[9:13])[0] == 3, "input terminal 4 (to the computer): 2 channels, front L/R")
check(asg[10] == 2 and struct.unpack("<I", asg[11:15])[0] == 3, "streaming interface 2: 2 channels, front L/R")
check(struct.unpack("<H", ep82[4:6])[0] == 48 and ep82[3] == 5, "EP 0x82: 48-byte max packet (7 frames of 6 = 42), still iso async")
check(len(diff) == 6 and all(FLASH_DESC[i] == d2[i] for i in range(326) if i not in diff), "nothing else in the descriptor changed (OUT side, MIDI)")
r0 = call(uc, 0x080657F4, 0x24070000)
check(r0 == DESC and bytes(uc.mem_read(r0, 326)) == d2, "GetConfigDescriptor hands out the 2-ch descriptor")

# and back
uc.mem_write(0x38800F84, struct.pack("<I", 0))
tick(400); tick(425); sdis, d = tick(700)
check(d == FLASH_DESC and not sdis, "back to Multichannel: descriptor byte-identical to stock, reconnected")

uc, tick, wd = tick_rig(1, started=0)
sdis, d = tick(0)
check(not sdis and d != FLASH_DESC and len(wd) == 1, "USB not started yet: switches at once, no disconnect")

# ---- M7: the ring, DataIn and the fixed sends in Master only
def master_uc():
    uc = new()
    uc, tick, _ = tick_rig(1, started=0)
    tick(0)
    return uc

RING, BUF, TX = 0x24064000, 0xC0000000, 0xC0010000
def ring(uc, size, fb, wr, rd, fill=None):
    if fill: uc.mem_write(BUF, fill)
    uc.mem_write(RING, struct.pack("<8I", BUF, size, fb, 0, wr, rd, 1, 0))
    w32(uc, 0x24000200, RING); w32(uc, 0x2400BD00, TX)

# usb_push: master L/R (channels 13/14 = bytes 36..41) of each 54-byte frame
block = bytes((f * 54 + b) * 7 & 0xFF for f in range(32) for b in range(54))
want = b"".join(block[f * 54 + 36:f * 54 + 42] for f in range(32))
uc = master_uc(); ring(uc, 0x480, 6, 0, 0)
uc.mem_write(0x24068000, block)
n = call(uc, u7["usb_push"], RING, 0x24068000, 0x6C0)
check(n == 192 and bytes(uc.mem_read(BUF, 192)) == want and u32(uc, RING + 0x10) == 192, "Master only: each block's master L/R goes to the ring, 6 bytes a frame")
uc = new(); ring(uc, 0x2880, 0x36, 0x100, 0)
uc.mem_write(0x24068000, block)
n = call(uc, u7["usb_push"], RING, 0x24068000, 0x6C0)
check(n == 0x6C0 and bytes(uc.mem_read(BUF + 0x100, 0x6C0)) == block, "Multichannel: the block goes to the ring untouched")

# usb_datain
pattern = bytes((i * 5 + 1) & 0xFF for i in range(0x480))
for adj, wr, rd, rate, wantlen in [(0, 0x240, 0, 48000, 36), (1, 0x240, 0, 48000, 42), (-1, 0x240, 0, 48000, 30),
                                   (0, 0x018, 0, 48000, 24), (0, 0x00C, 0x480 - 0x0C, 48000, 24), (0, 0x240, 0, 44100, 36)]:
    uc = master_uc(); ring(uc, 0x480, 6, wr, rd, pattern)
    w32(uc, 0x2400BC94, adj); w32(uc, 0x240001F8, rate); w32(uc, 0x2400BD44, 5)
    tx = []; stub(uc, 0x08066F48, tx)
    call(uc, u7["usb_datain"], 0x24070000, 0x82)
    ln = tx[0][3]
    sent = bytes(uc.mem_read(TX, ln))
    exp = bytes(pattern[(rd + i) % 0x480] for i in range(ln)) if rate == 48000 else bytes(ln)
    check(tx[0][1] == 0x82 and ln == wantlen and sent == exp and u32(uc, 0x2400BC94) == 0 and u32(uc, 0x2400BD44) == 0
          and (rate != 48000 or u32(uc, RING + 0x14) == (rd + ln) % 0x480) and u32(uc, RING + 0x1C) == (rate == 48000),
          f"Master only DataIn adj {adj:+d}, {(wr - rd) % 0x480} bytes queued, {rate} Hz: sends {ln} bytes, right data")
uc = new(); ring(uc, 0x2880, 0x36, 0x1440, 0)
tx = []; stub(uc, 0x08066F48, tx)
call(uc, u7["usb_datain"], 0x24070000, 0x82)
check(tx[0][3] == 324, "Multichannel DataIn: the stock routine (6 frames of 54 bytes)")
for ep in (0x81, 0x83):
    uc = master_uc()
    hit = []; stub(uc, 0x08065DB0, hit)
    call(uc, u7["usb_datain"], 0x24070000, ep)
    check(len(hit) == 1, f"Master only DataIn on EP {ep:#x} (feedback / MIDI): handed to the stock routine")

# SET_INTERFACE(2, alt 1) and iso IN incomplete in the stock class code, through usb_tx82
for label, mk in [("Master only", master_uc), ("Multichannel", new)]:
    uc = mk()
    pdev = 0x24070000; uc.mem_write(pdev + 0x29C, b"\x03")
    ring(uc, 0x480 if mk is master_uc else 0x2880, 6 if mk is master_uc else 0x36, 0, 0)
    tx = []; stub(uc, 0x08066F48, tx); stub(uc, 0x08066E94); stub(uc, 0x08066EC8)
    uc.mem_write(0x24071000, bytes([0x01, 0x0B, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00]))
    call(uc, 0x080659A0, pdev, 0x24071000)
    call(uc, 0x08065CD8, pdev, 0x82)
    exp = 36 if mk is master_uc else 324
    check([t[3] for t in tx] == [exp, exp], f"{label}: SET_INTERFACE and iso IN incomplete send {exp} bytes")
    if mk is master_uc:
        check(u32(uc, RING + 0x10) == 0x240, "Master only: SET_INTERFACE prefills half the 2-ch ring (2 ms)")

# class Init: the 2-ch ring
uc = master_uc(); ring(uc, 0x2880, 0x36, 0x1440, 0x80)
stub(uc, 0x080658C0, rv=0)
call(uc, u7["usb_init"], 0x24070000, 1)
check(struct.unpack("<6I", uc.mem_read(RING, 24))[1:] == (0x480, 6, 0, 0, 0), "Master only Init: ring set to 0x480 bytes of 6-byte frames, emptied")
uc = new(); ring(uc, 0x2880, 0x36, 0x1440, 0x80)
stub(uc, 0x080658C0, rv=0)
call(uc, u7["usb_init"], 0x24070000, 1)
check(struct.unpack("<6I", uc.mem_read(RING, 24))[1:] == (0x2880, 0x36, 0, 0x1440, 0x80), "Multichannel Init: stock")

# serial string
for label, mk, exp in [("Master only", master_uc, "ABCM"), ("Multichannel", new, "ABC")]:
    uc = mk()
    uc.mem_write(0x2400C26C, b"\x08\x03A\0B\0C\0" + bytes(16))
    def ser(uc, a, s, _):
        w = uc.reg_read(UC_ARM_REG_R1); uc.mem_write(w, struct.pack("<H", 8))
        uc.reg_write(UC_ARM_REG_R0, 0x2400C26C); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
    uc.hook_add(UC_HOOK_CODE, ser, begin=0x08066FEC, end=0x08066FEC)
    p = call(uc, u7["usb_serial"], 0, 0x24070000)
    ln = struct.unpack("<H", uc.mem_read(0x24070000, 2))[0]
    s = bytes(uc.mem_read(p, ln))
    check(s[0] == ln and s[1] == 3 and s[2:].decode("utf-16-le") == exp, f"{label}: serial number {exp!r}")

# cost: Master only adds a copy per block and replaces DataIn
uc = master_uc(); ring(uc, 0x480, 6, 0, 0); uc.mem_write(0x24068000, block)
n = [0]; h = uc.hook_add(UC_HOOK_CODE, lambda *a: n.__setitem__(0, n[0] + 1))
call(uc, u7["usb_push"], RING, 0x24068000, 0x6C0)
uc.hook_del(h)
print(f"     usb_push in Master only: {n[0]} instructions per block (ring write included)")
check(n[0] < 3000, "Master only: under 3000 instructions per audio block")
print("all passed")
