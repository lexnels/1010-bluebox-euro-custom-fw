"""Second pass: create functions at Thumb pointers found in data (vtables, callbacks), re-analyse, re-dump with literals resolved."""
import os, sys, re, struct
import pyghidra
pyghidra.start(install_dir="/opt/ghidra_11.4.2_PUBLIC")
from ghidra.base.project import GhidraProject
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import TaskMonitor
from ghidra.program.flatapi import FlatProgramAPI
from ghidra.app.cmd.disassemble import ArmDisassembleCommand
from ghidra.app.cmd.function import CreateFunctionCmd
R = os.path.dirname(os.path.abspath(__file__))
which = sys.argv[1]
BASE = {"cm7": 0x08040000, "cm4": 0x08100000, "alt": 0x080E0000}[which]
data = open(os.path.join(R, which + ".bin"), "rb").read()
p = GhidraProject.openProject(R, which, False)
prog = p.openProgram("/", which, False)
api = FlatProgramAPI(prog); space = prog.getAddressFactory().getDefaultAddressSpace()
listing = prog.getListing(); fm = prog.getFunctionManager()
# prologue-ish first halfword: push {..lr} (b5xx), stmdb sp! (e92d), or sub sp / mov
def plausible(o):
    h = struct.unpack_from("<H", data, o)[0]
    return (h & 0xff00) == 0xb500 or h == 0xe92d or (h & 0xff00) == 0xb400 or (h & 0xff80) == 0xb080
cands = set()
for o in range(0, len(data) - 3, 4):
    v = struct.unpack_from("<I", data, o)[0]
    if v & 1 and BASE <= v < BASE + len(data):
        t = (v & ~1) - BASE
        if plausible(t): cands.add(v & ~1)
# also scan all halfword-aligned push {...,lr} in gaps not yet code
for o in range(0, len(data) - 1, 2):
    if struct.unpack_from("<H", data, o)[0] & 0xff00 == 0xb500:
        a = space.getAddress(BASE + o)
        if listing.getInstructionContaining(a) is None and listing.getDefinedDataContaining(a) is None:
            cands.add(BASE + o)
tx = prog.startTransaction("pass2")
n = 0
tmode = prog.getRegister("TMode")
for v in sorted(cands):
    a = space.getAddress(v)
    if fm.getFunctionAt(a) is not None: continue
    if listing.getInstructionContaining(a) is not None and listing.getInstructionAt(a) is None: continue
    cmd = ArmDisassembleCommand(a, None, True); cmd.applyTo(prog, TaskMonitor.DUMMY)
    if CreateFunctionCmd(a).applyTo(prog, TaskMonitor.DUMMY): n += 1
prog.endTransaction(tx, True)
print("created", n)
p.analyze(prog)
p.save(prog)
print("functions:", fm.getFunctionCount())
d = DecompInterface(); d.openProgram(prog)
def sub(m):
    a = int(m.group(1), 16); o = a - BASE
    return "0x%08x" % struct.unpack_from("<I", data, o)[0] if 0 <= o < len(data) - 3 else m.group(0)
with open(os.path.join(R, which + "_all.c"), "w") as f:
    for fn in fm.getFunctions(True):
        r = d.decompileFunction(fn, 60, TaskMonitor.DUMMY)
        if r.decompileCompleted():
            c = re.sub(r"_DAT_([0-9a-f]{8})", sub, r.getDecompiledFunction().getC())
            f.write(f"// ===== {fn.getEntryPoint()} {fn.getName()}\n{c}\n")
p.close()
