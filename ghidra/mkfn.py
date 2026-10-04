"""mkfn.py <cm7|cm4> <addr>...: (re)create functions at addresses, decompile them to <addr>.c"""
import os, sys, re, struct
import pyghidra
pyghidra.start(install_dir="/opt/ghidra_11.4.2_PUBLIC")
from ghidra.base.project import GhidraProject
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import TaskMonitor
from ghidra.app.cmd.disassemble import ArmDisassembleCommand
from ghidra.app.cmd.function import CreateFunctionCmd
R = os.path.dirname(os.path.abspath(__file__)); which = sys.argv[1]
BASE = {"cm7": 0x08040000, "cm4": 0x08100000}[which]
data = open(os.path.join(R, which + ".bin"), "rb").read()
p = GhidraProject.openProject(R, which, False); prog = p.openProgram("/", which, False)
sp = prog.getAddressFactory().getDefaultAddressSpace(); fm = prog.getFunctionManager()
tx = prog.startTransaction("mkfn")
for s in sys.argv[2:]:
    a = sp.getAddress(int(s, 16))
    f = fm.getFunctionContaining(a)
    if f is not None and f.getEntryPoint() != a: fm.removeFunction(f.getEntryPoint())
    ArmDisassembleCommand(a, None, True).applyTo(prog, TaskMonitor.DUMMY)
    if fm.getFunctionAt(a) is None: CreateFunctionCmd(a).applyTo(prog, TaskMonitor.DUMMY)
prog.endTransaction(tx, True); p.save(prog)
d = DecompInterface(); d.openProgram(prog)
def sub(m):
    o = int(m.group(1), 16) - BASE
    if 0 <= o < len(data) - 3:
        v = struct.unpack_from("<I", data, o)[0]; f = struct.unpack_from("<f", data, o)[0]
        return "%s/*%#x%s*/" % (m.group(0), v, (" =%gf" % f) if 1e-6 < abs(f) < 1e7 else "")
    return m.group(0)
for s in sys.argv[2:]:
    f = fm.getFunctionAt(sp.getAddress(int(s, 16)))
    r = d.decompileFunction(f, 300, TaskMonitor.DUMMY)
    c = re.sub(r"_?DAT_([0-9a-f]{8})", sub, r.getDecompiledFunction().getC()) if r.decompileCompleted() else "FAILED " + r.getErrorMessage()
    open(os.path.join(R, s + ".c"), "w").write(c)
    print(s, f.getBody().getNumAddresses(), "bytes")
p.close()
