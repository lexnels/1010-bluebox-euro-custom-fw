"""Send FX: a Juno-style chorus, a warm drive and a second delay, each with its own on/off, reached by pressing FX
after the delay and the reverb, fed by per-channel track sends on a third track screen page.

M7 code is src/sfx_m7.c (cave out/sfx7), M4 code is in src/dly_m4.c (out/dly4). Needs the hall patchset (its reverb
list loop is where the new params join the reverb slot's set) and the delay patchset (dly4 cave, param definitions).
"""
import os, struct
from thumb import ROOT, bl, symbols, cave

h7 = symbols(os.path.join(ROOT, "out", "hall7.elf"))
s4 = symbols(os.path.join(ROOT, "out", "dly4.elf"))
s7 = symbols(os.path.join(ROOT, "out", "sfx7.elf"))
code7 = cave("sfx7")

def u32(v):
    return struct.pack("<I", v)

def bw(at, target):
    """B.W from `at` to `target` (Thumb-2 encoding T4)."""
    b = bytearray(bl(at, target))
    b[3] &= ~0x40                   # second halfword 0b10x1 -> 0b10x1 without the link bit (bit 14)
    return bytes(b)

PATCHES = [
    (0x08098000, bytes(len(code7)), code7),
    # M7: reverb vtable 0x0806ada0 slot 3 (hall_process, patches/hall.py) -> sfx_process, which runs the send FX (into
    # the main mix and the delay's and reverb's buses, before those run), then calls hall_process
    (0x0806ADAC, u32(h7["hall_process"] | 1), u32(s7["sfx_process"] | 1)),
    # M7: each message from the M4 (bl FUN_08052444 in the audio task's drain loop) -> sfx_msg, which takes ours
    # straight into the state (the master queue they used to come through drops all but 64 per block)
    (0x0804C7A8, bl(0x0804C7A8, 0x08052444), bl(0x0804C7A8, s7["sfx_msg"])),
    # M7: the graph builder's delay constructor call (bl FUN_08052e30) -> sfx_ctor, which then allocates our memory
    (0x080518D2, bl(0x080518D2, 0x08052E30), bl(0x080518D2, s7["sfx_ctor"])),
    # M4: the reverb set's table loop (patches/hall.py) exits through sfx_rv_tail, which adds the send FX params
    (0x08120C34, bw(0x08120C34, 0x0812088C), bw(0x08120C34, s4["sfx_rv_tail"])),
    # M4: the FX panel's list (bl FUN_081227f0) -> sfx_list; its reverb titles (bl FUN_081427e2) -> sfx_title, sfx_name
    (0x0812BCCA, bl(0x0812BCCA, 0x081227F0), bl(0x0812BCCA, s4["sfx_list"])),
    (0x0812BC90, bl(0x0812BC90, 0x081427E2), bl(0x0812BC90, s4["sfx_title"])),
    (0x0812BC9C, bl(0x0812BC9C, 0x081427E2), bl(0x0812BC9C, s4["sfx_name"])),
    # M4: the FX button, delay -> reverb and reverb -> view 0x13 (bl FUN_08123158) -> through Chorus, Drive, Delay 2
    (0x081247D8, bl(0x081247D8, 0x08123158), bl(0x081247D8, s4["sfx_to_reverb"])),
    (0x081247E4, bl(0x081247E4, 0x08123158), bl(0x081247E4, s4["sfx_from_reverb"])),
    # Track sends. M7: the mixer's per-channel call (bl FUN_0805012c) -> sfx_strip, which adds each channel's track
    # sends into the FX inputs
    (0x0805070A, bl(0x0805070A, 0x0805012C), bl(0x0805070A, s7["sfx_strip"])),
    # M4: the channel set's last add (bl FUN_081205e2, id 0x173) -> ts_set_add, which adds the three sends
    (0x08120888, bl(0x08120888, 0x081205E2), bl(0x08120888, s4["ts_set_add"])),
    # M4: the track button (button 1): track screen -> our sends (the track screen's second half again, view 6) ->
    # sidechain screen (view 0x16); from another screen back to the track screen
    (0x08124758, bl(0x08124758, 0x08123158), bl(0x08124758, s4["ts_track_next"])),
    (0x0812474C, bl(0x0812474C, 0x08123158), bl(0x0812474C, s4["ts_track_back"])),
    # M4: the track screen's setup (bl FUN_08133884, event 0x8c) -> ts_tp_setup; its fill (bl FUN_081339b0) ->
    # ts_tp_fill, which rebinds FX1, FX2, OUT3 to FX3, FX4, FX5 on our sends; its encoder turn -> ts_tp_turn
    (0x08135720, bl(0x08135720, 0x08133884), bl(0x08135720, s4["ts_tp_setup"])),
    (0x081342D8, bl(0x081342D8, 0x081339B0), bl(0x081342D8, s4["ts_tp_fill"])),
    (0x081342F4, bl(0x081342F4, 0x081339B0), bl(0x081342F4, s4["ts_tp_fill"])),
    (0x08134316, bl(0x08134316, 0x081339B0), bl(0x08134316, s4["ts_tp_fill"])),
    (0x08134360, bl(0x08134360, 0x08128D9C), bl(0x08134360, s4["ts_tp_turn"])),
]
if os.environ.get("SFX_NO_TRACK_PAGE"):     # a diagnostic build: the sends without the track screen page
    PATCHES = [p for p in PATCHES if p[0] not in (0x08124758, 0x0812474C, 0x08135720, 0x081342D8, 0x081342F4, 0x08134316, 0x08134360)]
