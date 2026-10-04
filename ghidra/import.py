"""Import one image of BLUEEURO.BIN into a Ghidra project and auto-analyse it.
  import.py cm7|alt|cm4"""
import os, sys
import pyghidra
GHIDRA = "/opt/ghidra_11.4.2_PUBLIC"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(ROOT, "firmware", "BLUEEURO-3.bin")
IMAGES = {"cm7": (0x000000, 0x0A0000, 0x08040000), "alt": (0x0A0000, 0x0C0000, 0x080E0000),
          "cm4": (0x0C0000, None, 0x08100000)}
which = sys.argv[1]
lo, hi, BASE = IMAGES[which]
data = open(FW, "rb").read()[lo:hi]
img = os.path.join(ROOT, "ghidra", which + ".bin")
open(img, "wb").write(data)
RAM = [("ITCM", 0x00000000, 0x10000), ("DTCM", 0x20000000, 0x20000), ("AXI_SRAM", 0x24000000, 0x80000),
       ("SRAM123", 0x30000000, 0x48000), ("SRAM4", 0x38000000, 0x10000), ("BKPSRAM", 0x38800000, 0x1000),
       ("PERIPH", 0x40000000, 0x20000000), ("SDRAM", 0xC0000000, 0x4000000), ("CORE", 0xE0000000, 0x100000)]
pyghidra.start(install_dir=GHIDRA)
from java.io import File
from ghidra.base.project import GhidraProject
from ghidra.program.util import DefaultLanguageService
from ghidra.program.model.lang import LanguageID, CompilerSpecID
from ghidra.util.task import TaskMonitor
from ghidra.program.flatapi import FlatProgramAPI
from ghidra.program.model.data import PointerDataType
proj_dir = os.path.join(ROOT, "ghidra")
project = GhidraProject.createProject(proj_dir, which, False)
lang = DefaultLanguageService.getLanguageService().getLanguage(LanguageID("ARM:LE:32:Cortex"))
program = project.importProgram(File(img), lang, lang.getCompilerSpecByID(CompilerSpecID("default")))
tx = program.startTransaction("setup")
mem = program.getMemory(); space = program.getAddressFactory().getDefaultAddressSpace()
blk = mem.getBlocks()[0]
mem.moveBlock(blk, space.getAddress(BASE), TaskMonitor.DUMMY)
blk.setName("FLASH"); blk.setExecute(True); blk.setWrite(False)
for n, start, size in RAM:
    b = mem.createUninitializedBlock(n, space.getAddress(start), size, False)
    b.setRead(True); b.setWrite(True); b.setExecute(n in ("ITCM", "AXI_SRAM", "DTCM"))
    if n in ("PERIPH", "CORE"): b.setVolatile(True)
api = FlatProgramAPI(program)
for i in range(166):
    a = space.getAddress(BASE + 4 * i)
    api.createData(a, PointerDataType())
    v = mem.getInt(a) & 0xFFFFFFFF
    if i > 0 and v & 1 and BASE <= v < BASE + blk.getSize():
        t = space.getAddress(v & ~1)
        if api.getFunctionAt(t) is None:
            api.createFunction(t, "Reset_Handler" if i == 1 else f"vec_{i}")
program.endTransaction(tx, True)
project.analyze(program)
project.saveAs(program, "/", which, True)
print("functions:", program.getFunctionManager().getFunctionCount())
d = __import__("ghidra.app.decompiler", fromlist=["DecompInterface"]).DecompInterface(); d.openProgram(program)
with open(os.path.join(proj_dir, which + "_all.c"), "w") as f:
    for fn in program.getFunctionManager().getFunctions(True):
        r = d.decompileFunction(fn, 60, TaskMonitor.DUMMY)
        if r.decompileCompleted():
            f.write(f"// ===== {fn.getEntryPoint()} {fn.getName()}\n{r.getDecompiledFunction().getC()}\n")
project.close()
