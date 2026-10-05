"""Write a standalone patcher for a release: one Python file that turns the stock firmware into the modded one.

  python3 tools/make_patcher.py cpu+hall v4     # -> out/release/bluebox-mod-v4-patcher.py

It carries only the bytes the mod changes (our code and the small hooks), never 1010music's firmware: every run of
bytes that differs from the stock image, with no stock bytes in between. The stock file is checked by its SHA-256
before patching and the result by its SHA-256 after.
"""
import base64, hashlib, os, struct, sys, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
build, version = sys.argv[1], sys.argv[2]
stock = open(os.path.join(ROOT, "firmware", "BLUEEURO-3.bin"), "rb").read()
mod = open(os.path.join(ROOT, "out", build, "BLUEEURO.BIN"), "rb").read()
assert len(stock) == len(mod)

runs, i = [], 0
while i < len(mod):
    if mod[i] == stock[i]:
        i += 1
        continue
    j = i
    while j < len(mod) and mod[j] != stock[j]:
        j += 1
    runs.append((i, mod[i:j]))
    i = j
blob = b"".join(struct.pack("<II", o, len(b)) + b for o, b in runs)
payload = base64.b64encode(zlib.compress(blob, 9)).decode()
lines = "\n".join(payload[k:k + 100] for k in range(0, len(payload), 100))

script = f'''#!/usr/bin/env python3
"""bluebox eurorack custom firmware {version} ({build}): https://github.com/lexnels/1010-bluebox-euro-custom-fw

Turns the stock 1010music firmware file into the modded one. Needs Python 3, nothing else.

  python3 {"bluebox-mod-" + version + "-patcher.py"} <stock firmware file> [output file]

The output defaults to BLUEEURO.BIN next to this script. This file holds only the mod's changes, not 1010music's
firmware: you need your own copy of the stock file (eurorack edition, SHA-256 {hashlib.sha256(stock).hexdigest()}).
"""
import base64, hashlib, os, struct, sys, zlib

STOCK_SHA256 = "{hashlib.sha256(stock).hexdigest()}"
MOD_SHA256 = "{hashlib.sha256(mod).hexdigest()}"
PATCH = """
{lines}
"""

if len(sys.argv) < 2:
    sys.exit(__doc__)
data = bytearray(open(sys.argv[1], "rb").read())
if hashlib.sha256(data).hexdigest() != STOCK_SHA256:
    sys.exit("That isn't the stock firmware this mod was built for (SHA-256 mismatch). Nothing written.")
blob, at = zlib.decompress(base64.b64decode("".join(PATCH.split()))), 0
while at < len(blob):
    ofs, n = struct.unpack_from("<II", blob, at)
    data[ofs:ofs + n] = blob[at + 8:at + 8 + n]
    at += 8 + n
assert hashlib.sha256(data).hexdigest() == MOD_SHA256, "patched result doesn't match; nothing written"
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "BLUEEURO.BIN")
open(out, "wb").write(data)
print("wrote", out, "- copy it to the root of the microSD card and install it like a 1010music update.")
'''
os.makedirs(os.path.join(ROOT, "out", "release"), exist_ok=True)
path = os.path.join(ROOT, "out", "release", f"bluebox-mod-{version}-patcher.py")
open(path, "w").write(script)
print(f"{path}: {len(runs)} changed runs, {sum(len(b) for _, b in runs)} bytes")
