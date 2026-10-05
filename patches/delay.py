"""FX1 delay: Resonance, Pitch (with its on/off button), Flutter and Send to Reverb; FILT and PITCH right of the knobs.

M7 code is in src/dly_m7.h (built into the hall cave, out/hall7), M4 code in src/dly_m4.c (out/dly4). Needs the hall
patchset too: the send reaches the reverb through hall_process, and the panel page limit it raises is shared.
"""
import os, struct
from thumb import ROOT, bl, symbols, cave

s7 = symbols(os.path.join(ROOT, "out", "hall7.elf"))
s4 = symbols(os.path.join(ROOT, "out", "dly4.elf"))
code4 = cave("dly4")

def u32(v):
    return struct.pack("<I", v)

# M4: the delay slot's param list (FUN_0812060c, case 4 @0x08120bba, 98 bytes up to case 5), the same table loop as
# the reverb's. List order is screen order: columns of 2, column-major; knobs first, so the toggles end up on the
# right, in the fifth column like the reverb's Freeze. The panel draws 10, so BEAT, PING and QUAD are left off it (and
# 0x34, the beat-synced time, with BEAT); dly_process holds them at ms time, ping-pong on, QUAD off.
LIST_AT = 0x08120BBA
LIST_STOCK = bytes.fromhex(
    "4ff4c87233212046fff70efd4ff4c87239212046fff708fd78220e212046fff703fd4ff47a72cb212046fff7fdfc"
    "012235212046fff7f8fc0122ca212046fff7f3fc012236212046fff7eefc002237212046fff7e9fc062234212046fff7e4fc37e6")
LIST_IDS = [(0x33, 400), (0x39, 400),       # column 1: Delay, Feedback
            (0x0E, 120), (0xCB, 1000),      # column 2: Cutoff, Width (stock band-pass)
            (0x43, 0), (0x4A, 12),          # column 3: Resonance, Pitch (+12 semitones)
            (0x3A, 0), (0x3D, 0),           # column 4: Flutter, Send (to reverb)
            (0xCA, 1), (0x4B, 0)]           # column 5: FILT, PITCH (on/off); BEAT, PING, QUAD are fixed on M7
LIST_CODE = bytes.fromhex(
    "48b4"          # push {r3, r6}
    "06a6"          # adr r6, table (0x08120bd8)
    "3188"          # loop: ldrh r1, [r6]
    "31b1"          # cbz r1, done
    "b6f90220"      # ldrsh.w r2, [r6, #2]
    "2046"          # mov r0, r4 (the set)
    "fff70bfd"      # bl FUN_081205e2 (append id, value)
    "0436"          # adds r6, #4
    "f6e7"          # b loop
    "48bc"          # done: pop {r3, r6}
    "fff75bbe"      # b.w 0x0812088c (common tail: needs r4, r5)
    "00bf")         # nop (table alignment)
def list_patch():
    t = LIST_CODE + b"".join(struct.pack("<Hh", i, v) for i, v in LIST_IDS) + b"\0\0"
    assert len(LIST_IDS) <= 16 and len(t) <= len(LIST_STOCK)
    return t + bytes.fromhex("00bf") * ((len(LIST_STOCK) - len(t)) // 2)

PATCHES = [
    (0x080D8000, bytes(len(code4)), code4),
    # M4: param table, last definition (id 0x117) -> dly_defs, which then defines the four new ids
    (0x0813714A, bl(0x0813714A, 0x08135E08), bl(0x0813714A, s4["dly_defs"])),
    (LIST_AT, LIST_STOCK, list_patch()),
    # M4: FX panel pages: stock pins the delay's encoders to page 0 (its page 1 was only toggles); give it the
    # reverb's paging instead (beq to the 0x15 branch), so the knobs on page 2 can be turned
    (0x0812BF90, bytes.fromhex("02d0"), bytes.fromhex("06d0")),
    (0x0812BFCE, bytes.fromhex("02d0"), bytes.fromhex("06d0")),
    # M7: delay vtable 0x0806ba64, slot 3 (process FUN_08053b30) -> dly_process
    (0x0806BA70, u32(0x08053B31), u32(s7["dly_process"] | 1)),
    # M7: the four S0 line reads (bl FUN_08059d54) -> dly_read (flutter)
    *[(a, bl(a, 0x08059D54), bl(a, s7["dly_read"])) for a in (0x08053574, 0x08053594, 0x08053816, 0x080538EC)],
    # M7: the band-pass calls (bl FUN_08056dd8) -> dly_tone (Resonance after the stock band-pass)
    *[(a, bl(a, 0x08056DD8), bl(a, s7["dly_tone"])) for a in (0x08053602, 0x08053610)],
]
