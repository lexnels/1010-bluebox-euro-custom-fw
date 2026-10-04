"""Apply byte patches to the stock bluebox eurorack firmware and write an SD-card-ready image.

  python3 patch.py <patchset> [...]     e.g. python3 patch.py cpu

Each patchset in patches/ defines PATCHES = [(addr, old_bytes, new_bytes), ...]. Every patch checks the stock bytes
first, so a patch built for another firmware version fails instead of writing garbage. The file size never changes.
"""
import hashlib, importlib, os, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
STOCK = os.path.join(ROOT, "firmware", "BLUEEURO-3.bin")
STOCK_SHA256 = "fcb04565e2fd5bc15c0b2d0dc913dc99dc74732611e8cef0d7e8e4bb4af7402b"
# the file holds three images back to back: (flash address, file offset, size)
IMAGES = [(0x08040000, 0x000000, 0xA0000), (0x080E0000, 0x0A0000, 0x20000), (0x08100000, 0x0C0000, None)]

def offset(addr):
    for base, lo, size in IMAGES:
        if base <= addr and (size is None or addr < base + size):
            return lo + addr - base
    raise ValueError(f"{addr:#x} is not in the image")

data = bytearray(open(STOCK, "rb").read())
assert hashlib.sha256(data).hexdigest() == STOCK_SHA256, "stock firmware hash mismatch"
sys.path.insert(0, os.path.join(ROOT, "patches"))
names = sys.argv[1:]
for name in names:
    for addr, old, new in importlib.import_module(name).PATCHES:
        o = offset(addr)
        assert len(old) == len(new), f"{name}: size change at {addr:#x}"
        assert o + len(new) <= len(data), f"{name}: past end of file at {addr:#x}"
        assert data[o:o + len(old)] == old, f"{name}: stock bytes differ at {addr:#x}"
        data[o:o + len(new)] = new
        print(f"{name}: {addr:#010x} {len(new):6d} bytes" + (f"  {old.hex()} -> {new.hex()}" if len(new) <= 8 else ""))
out_dir = os.path.join(ROOT, "out", "+".join(names))
os.makedirs(out_dir, exist_ok=True)
out = os.path.join(out_dir, "BLUEEURO.BIN")
open(out, "wb").write(data)
print(f"wrote {out} ({len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()[:16]})")
