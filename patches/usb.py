"""USB audio out: a "USB Out" setting, Multichannel (stock, 18 channels to the computer) or Master only (2 channels, the
master L/R). M7 code is src/usb_m7.c (out/usb7); the setting reaches it through src/mst_m7.c, and its M4 definition is
in src/dly_m4.c. Needs the cpu, delay and master patchsets.
"""
import os, struct
from thumb import ROOT, bl, symbols, cave

s7 = symbols(os.path.join(ROOT, "out", "usb7.elf"))
code7 = cave("usb7")

def word(at, stock, target):
    return (at, struct.pack("<I", stock), struct.pack("<I", target | 1))

def hook(at, stock, target):
    return (at, bl(at, stock), bl(at, target))

# settings page id list (0x0814d91c): 0x178 PhonesSrc, then USB Out (0x59) before 0x118 USB Rcv; the rest moves down
# one, ending ..., 0x13b, 0x43 (Saturate, patches/master.py), 0
LIST = [0x118] + list(range(0x1F7, 0x203)) + [0x172, 0x174, 0x18C, 0x175, 0x176, 0x177, 0x170, 0x171, 0x18B, 0x116,
       0xB5] + list(range(0x130, 0x13C)) + [0x43]
old = struct.pack(f"<{len(LIST) + 1}H", *LIST, 0)
new = struct.pack(f"<{len(LIST) + 1}H", 0x59, *LIST)

PATCHES = [
    (0x08090000, bytes(len(code7)), code7),
    # USB class struct 0x08076c2c: Init, DataIn
    word(0x08076C2C, 0x080658C1, s7["usb_init"]),
    word(0x08076C40, 0x08065DB1, s7["usb_datain"]),
    # descriptor callbacks 0x08076c78: serial string
    word(0x08076C88, 0x08066FED, s7["usb_serial"]),
    # audio loop FUN_0804100c: the block's write to the IN ring, the host-loss watchdog
    hook(0x0804115A, 0x08066750, s7["usb_push"]),
    hook(0x08041166, 0x080655DC, s7["usb_tick"]),
    # the fixed 6-frame sends on EP 0x82: SET_INTERFACE, iso IN incomplete, SOF watchdog
    hook(0x08065AB4, 0x08066F48, s7["usb_tx82"]),
    hook(0x08065D1A, 0x08066F48, s7["usb_tx82"]),
    hook(0x08066640, 0x08066F48, s7["usb_tx82"]),
    # M4: the settings list with USB Out
    (0x0814D91E, old + b"\0\0", new + b"\0\0"),
]
