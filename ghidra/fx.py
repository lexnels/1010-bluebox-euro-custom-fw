"""fx.py <cm7|cm4> <addr> [regex] [ctx]: print a decompiled function, optionally grep it."""
import re, sys, os
txt = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), sys.argv[1] + "_all.c")).read()
funcs = {f.split("\n", 1)[0].split()[0]: f for f in re.split(r"(?m)^// ===== ", txt)[1:]}
decl = re.compile(r"^\s*(undefined\d?|int|uint|char|short|ushort|byte|bool|float|double|longlong|ulonglong|code|float10) \**\w+( \[\d+\])?;$")
key = sys.argv[2].lower().replace("0x", "").zfill(8)
import struct
BASE = {"cm7": 0x08040000, "cm4": 0x08100000}[sys.argv[1]]
data = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), sys.argv[1] + ".bin"), "rb").read()
def ann(m):
    o = int(m.group(1), 16) - BASE
    if 0 <= o < len(data) - 3:
        v = struct.unpack_from("<I", data, o)[0]; f = struct.unpack_from("<f", data, o)[0]
        return m.group(0) + "/*%#x%s*/" % (v, (" =%gf" % f) if 1e-6 < abs(f) < 1e7 else "")
    return m.group(0)
lines = [re.sub(r"\bDAT_([0-9a-f]{8})", ann, l) for l in funcs[key].splitlines() if not decl.match(l)]
if len(sys.argv) > 3:
    ctx = int(sys.argv[4]) if len(sys.argv) > 4 else 8
    for k, l in enumerate(lines):
        if re.search(sys.argv[3], l): print("\n".join(lines[max(0, k-ctx):k+ctx+1])); print("    ...")
else:
    print("\n".join(lines))
