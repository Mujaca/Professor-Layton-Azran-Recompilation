"""Einen Decoder-Aufruf aus einem Port-Speicherabbild in Unicorn nachrechnen.

  python replay.py <dumpdir> [--trace blocks.txt] [--maxinsn N]

Liest out/watchdump_*.bin (Basisadresse im Dateinamen) und
out/watchdump_regs.txt, startet bei r15 und laeuft bis zur Rueckkehr nach lr.
Gibt aus: Anzahl Instruktionen, hoechste gelesene Adresse im Bereich ab r1
(Bitstrom), Schreibzugriffe in [r0+0x1B8 .. r0+0x3BC] mit Index, und die
Blockfolge (optional) zum Vergleich mit der Port-Blockspur.
"""
import glob
import os
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_MODE_THUMB, UC_HOOK_CODE, \
    UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_HOOK_BLOCK
from unicorn.arm_const import *

SENTINEL = 0x0FFF0000


def load(dumpdir):
    regs = {}
    for line in open(os.path.join(dumpdir, "watchdump_regs.txt")):
        k, v = line.split()
        regs[k] = int(v, 16)
    regions = []
    for f in glob.glob(os.path.join(dumpdir, "watchdump_*.bin")):
        base = int(os.path.basename(f)[10:18], 16)
        regions.append((base, open(f, "rb").read()))
    return regs, regions


def run(dumpdir, trace=None, maxinsn=50_000_000, watch_obj=True):
    regs, regions = load(dumpdir)
    mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    for base, data in regions:
        size = (len(data) + 0xFFF) & ~0xFFF
        mu.mem_map(base, size)
        mu.mem_write(base, data)
    mu.mem_map(SENTINEL, 0x1000)
    # VFP einschalten
    mu.reg_write(UC_ARM_REG_C1_C0_2, mu.reg_read(UC_ARM_REG_C1_C0_2) | (0xF << 20))
    mu.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
    names = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4,
             UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9,
             UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP]
    cpsr = 0x10 | (regs["n"] << 31) | (regs["z"] << 30) | (regs["c"] << 29) | (regs["v"] << 28)
    mu.reg_write(UC_ARM_REG_CPSR, cpsr)
    for i, r in enumerate(names):
        mu.reg_write(r, regs["r%d" % i])
    mu.reg_write(UC_ARM_REG_LR, regs["r14"])
    mu.reg_write(UC_ARM_REG_FPSCR, regs.get("fpscr", 0x03C00010))
    for k in range(32):
        mu.reg_write(UC_ARM_REG_S0 + k, regs.get("s%d" % k, 0))
    mu.reg_write(UC_ARM_REG_C13_C0_3, regs.get("tpidruro", 0))
    r0, r1 = regs["r0"], regs["r1"]
    stats = {"maxread": 0, "tblw": [], "blocks": []}

    def on_read(uc, access, addr, size, value, data):
        if r1 <= addr < r1 + 0x100000 and addr > stats["maxread"]:
            stats["maxread"] = addr

    def on_write(uc, access, addr, size, value, data):
        if watch_obj and stats.get("obj") and stats["obj"] + 0x1B8 <= addr < stats["obj"] + 0x3C0:
            pc = uc.reg_read(UC_ARM_REG_PC)
            stats["tblw"].append((pc, (addr - stats["obj"] - 0x1B8) // 4, value))

    def on_block(uc, addr, size, data):
        if trace is not None:
            stats["blocks"].append(addr)

    mu.hook_add(UC_HOOK_MEM_READ, on_read)
    mu.hook_add(UC_HOOK_MEM_WRITE, on_write)
    mu.hook_add(UC_HOOK_BLOCK, on_block)
    start = int(os.environ.get("START", "%X" % regs["r15"]), 16)
    stats["obj"] = regs.get("obj", 0)
    err = None
    try:
        mu.emu_start(start, regs["r14"], count=maxinsn)
    except UcError as e:
        err = "%s pc=%08X" % (e, mu.reg_read(UC_ARM_REG_PC))
    out = {k: mu.reg_read(n) for k, n in zip(["r%d" % i for i in range(14)], names)}
    out["pc"] = mu.reg_read(UC_ARM_REG_PC)
    return mu, stats, out, err, regs


if __name__ == "__main__":
    d = sys.argv[1]
    tr = None
    if "--trace" in sys.argv:
        tr = sys.argv[sys.argv.index("--trace") + 1]
    mu, st, out, err, regs = run(d, trace=tr)
    print("Start pc=%s r0=%08X r1=%08X" % (os.environ.get("START"), regs["r0"], regs["r1"]))
    print("Ende pc=%08X err=%s r0=%08X" % (out["pc"], err, out["r0"]))
    print("hoechster Lesezugriff ab r1: %08X (= r1 + %d)" % (st["maxread"], st["maxread"] - regs["r1"]))
    print("Bloecke:", len(st["blocks"]))
    if tr:
        open(tr, "w").write("\n".join("%08X" % b for b in st["blocks"]))
