"""Co-Simulation: recompilierter Port-Code vs. Originalbytes, Block fuer Block.

Das Werkzeug vergleicht **jeden Basisblock** eines Gastlaufs mit der
Referenzausfuehrung derselben Originalbytes in Unicorn:

1. ``snapshot`` startet ``port.exe`` (Diagnose ``ARM_DUMP``, nur lesend) und
   schreibt Register, Flags und Gast-Speicher beim ersten Erreichen eines
   Basisblocks.
2. ``run`` laedt den Schnappschuss, fuehrt die Originalbytes ab derselben
   Adresse in Unicorn aus (jede Instruktion gehookt) und ruft im Port-Build
   (``<root>/build/gen_boot.o``) dieselbe Funktion mit identischem Zustand
   auf. Der Port zeichnet jeden betretenen Basisblock mit r0-r15 und Flags
   auf (Blocktrace des Test-Harness in `func_difftest.py`).
3. Beide Blockfolgen werden im Gleichschritt verglichen. Bei der ersten
   Abweichung werden Block, Vorgaengerblock mit Disassemblierung, der
   Register-/Flag-Zustand und - ueber einen zweiten Port-Lauf mit Blocklimit -
   die abweichenden Speicherbereiche ausgegeben.

Das Werkzeug veraendert keinen Gastzustand und gehoert nicht ins Produkt.

Beispiele:

  python tools/cosim.py snapshot --root . --watch 0x0016A500 --out scratch/cosim/snap.bin
  python tools/cosim.py run --root . --snapshot scratch/cosim/snap.bin
  python tools/cosim.py run --root ../3DS-fork --snapshot scratch/cosim/snap.bin --context 16

Exit-Code 0 = kein Unterschied an Blockgrenzen (Register, Flags, Speicher),
1 = Abweichung, 2 = Werkzeug-/Eingabefehler.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)
import func_difftest as fd  # gemeinsamer Schnappschuss-/Harness-Code


def _root(path):
    return os.path.abspath(path)


def _flags(cs):
    return ((cs >> 31) & 1, (cs >> 30) & 1, (cs >> 29) & 1,
            (cs >> 28) & 1, (cs >> 16) & 0xF)


def _uc_start(snap, entry):
    from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_MODE_THUMB,
                         UC_MODE_LITTLE_ENDIAN)
    import unicorn.arm_const as U

    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM | UC_MODE_LITTLE_ENDIAN)
    uc.mem_map(0, 0x20000000)
    code = open(os.path.join(fd.ROOT, "out", "exefs", "code_decomp.bin"),
                "rb").read()
    uc.mem_write(fd.CODE_BASE, code)
    for addr, blob in snap["regions"]:
        uc.mem_write(addr, blob)
    f = snap["flags"]
    cpsr = (f["cpsr_mode"] & 0x1F) | ((f["t"] & 1) << 5)
    cpsr |= (f["n"] & 1) << 31
    cpsr |= (f["z"] & 1) << 30
    cpsr |= (f["c"] & 1) << 29
    cpsr |= (f["v"] & 1) << 28
    cpsr |= (f["qflag"] & 1) << 27
    cpsr |= (f["ge"] & 0xF) << 16
    uc.reg_write(U.UC_ARM_REG_CPSR, cpsr)
    for i in range(16):
        uc.reg_write(getattr(U, "UC_ARM_REG_R%d" % i), snap["regs"][i])
    return uc, U, entry | (1 if f["t"] else 0)


def unicorn_trace(snap, entry, limit):
    """Vollstaendige Instruktionsspur (addr, regs, flags) aus Unicorn."""
    from unicorn import UC_HOOK_CODE

    uc, U, begin = _uc_start(snap, entry)
    trace = []
    state = {"n": 0}

    def hook(uc_, address, size, user):
        state["n"] += 1
        cs = uc_.reg_read(U.UC_ARM_REG_CPSR)
        trace.append((address,
                      [uc_.reg_read(getattr(U, "UC_ARM_REG_R%d" % i))
                       for i in range(16)], _flags(cs)))
        if state["n"] > limit:
            uc_.emu_stop()

    uc.hook_add(UC_HOOK_CODE, hook)
    stop = None
    try:
        uc.emu_start(begin, fd.SENTINEL)
    except Exception as e:
        stop = "exception: %s" % e
    return uc, trace, stop


def unicorn_until(snap, entry, index):
    """Unicorn bis kurz vor die Instruktion mit Trace-Index ``index``."""
    from unicorn import UC_HOOK_CODE
    import unicorn.arm_const as U

    uc, U, begin = _uc_start(snap, entry)
    uc.reg_write(U.UC_ARM_REG_LR, fd.SENTINEL)
    state = {"n": 0}

    def hook(uc_, address, size, user):
        if state["n"] == index:
            uc_.emu_stop()
            return
        state["n"] += 1

    uc.hook_add(UC_HOOK_CODE, hook)
    try:
        uc.emu_start(begin, fd.SENTINEL)
    except Exception:
        pass
    return uc


def disasm(root, addr, count, thumb):
    try:
        import capstone
        data = open(os.path.join(root, "out", "exefs", "code_decomp.bin"),
                    "rb").read()
        md = capstone.Cs(capstone.CS_ARCH_ARM,
                         capstone.CS_MODE_THUMB if thumb
                         else capstone.CS_MODE_ARM)
        print("Disassemblierung Vorgaenger 0x%08X:" % addr)
        for ins in md.disasm(data[addr - fd.CODE_BASE:
                                   addr - fd.CODE_BASE + count * 4], addr):
            print("  %08X  %-7s %s" % (ins.address, ins.mnemonic, ins.op_str))
    except Exception as e:
        print("(Disassemblierung nicht moeglich: %s)" % e)


def cmd_snapshot(a):
    fd.ROOT = _root(a.root)
    fd.OUT = os.path.join(fd.ROOT, "out", "cosim")
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    env = {k: v for k, v in os.environ.items()
           if not (k.startswith("HLE_") or k.startswith("ARM_") or
                   k.startswith("PORT_"))}
    env["HLE_LOG"] = "1"
    env["ARM_DUMP"] = "0x%08X:%s" % (a.watch, os.path.abspath(a.out))
    log = a.out + ".log"
    with open(log, "wb") as f:
        try:
            subprocess.run([a.exe, os.path.join(fd.ROOT, "out", "romfs"), "--renderer", "soft"],
                           cwd=fd.ROOT, env=env, stdout=f,
                           stderr=subprocess.STDOUT, timeout=a.timeout)
        except subprocess.TimeoutExpired:
            pass
    if not os.path.exists(a.out):
        print("Schnappschuss fehlt: 0x%08X in %d s nicht erreicht (%s)"
              % (a.watch, a.timeout, log))
        return 1
    snap = fd.read_snapshot(a.out)
    print("Schnappschuss %s: pc=0x%08X r0=0x%08X sp=0x%08X regions=%d"
          % (a.out, snap["pc"], snap["regs"][0], snap["regs"][13],
             len(snap["regions"])))
    return 0


def block_set(root, func):
    """Blockadressen einer Funktion aus den Labels in build/gen_boot.c."""
    path = os.path.join(root, "build", "gen_boot.c")
    if not os.path.exists(path):
        return None
    import re
    pat = re.compile(r"^L_%08X_([0-9A-F]{8}):" % func, re.M)
    data = open(path, "r", encoding="utf-8", errors="replace").read()
    out = set(int(m, 16) for m in pat.findall(data))
    return out or None


def cmd_run(a):
    fd.ROOT = _root(a.root)
    fd.OUT = os.path.join(fd.ROOT, "out", "cosim")
    snap = fd.read_snapshot(a.snapshot)
    entry = a.entry or snap["pc"]
    func_addr = a.func_addr or entry
    print("Co-Simulation: Funktion 0x%08X (%s), r0=0x%08X r1=0x%08X "
          "sp=0x%08X"
          % (entry, "Thumb" if snap["flags"]["t"] else "ARM",
             snap["regs"][0], snap["regs"][1], snap["regs"][13]))

    u = fd.run_unicorn(snap, entry, a.unicorn_limit, 600)
    utrace = u["trace"]
    print("unicorn: %d Instruktionen%s"
          % (u["insns"], "" if u["stop"] is None else " (Stopp: %s)" % u["stop"]))
    port = fd.run_port(entry, os.path.abspath(a.snapshot), a.limit_blocks)
    ptrace = fd.read_port_trace(port["trace"])
    print("port:    %s, %d Bloecke" % (port["status"], len(ptrace)))
    if port["stderr"].strip():
        print("port stderr: %s" % port["stderr"].strip().splitlines()[-1])
    if not ptrace:
        print("Keine Port-Blocktrace - Abbruch")
        return 2

    d = fd.first_trace_diff(utrace, ptrace)
    if d is not None:
        kind, pi, pc, r, uv, pv = d
        prev = ptrace[pi - 1][0] if pi > 0 else entry
        print("ERSTE ABWEICHUNG (%s) an Block 0x%08X (#%d), Vorgaenger 0x%08X"
              % (kind, pc, pi, prev))
        if kind == "Register":
            print("  %s unicorn=0x%08X port=0x%08X"
                  % (("r%d" % r), uv & 0xFFFFFFFF, pv & 0xFFFFFFFF))
        elif kind == "Flags":
            printbefund("  Flags unicorn=%s port=%s" % (uv, pv))
        disasm(fd.ROOT, prev, a.context, snap["flags"]["t"])
        return 1

    # Blockfolge stimmt (Unicorn-Teilfolge). Jetzt den Endzustand pruefen:
    # kehrt der Port zu frueh zurueck, fehlen Bloecke/Instruktionen.
    if port["post"] is None:
        print("Port lieferte keinen Endzustand - Abbruch")
        return 2
    post = fd.read_snapshot(port["post"])
    pre_pages = fd.pages_of(snap["regions"])
    port_pages = fd.pages_of(post["regions"])
    uni_pages = fd.unicorn_pages(u["uc"], set(pre_pages) | set(port_pages))
    diffs, npages = fd.compare(u["regs"], u["flags"], uni_pages,
                               post["regs"], post["flags"], port_pages,
                               limit=a.mem_limit)
    if diffs:
        print("ERSTE ABWEICHUNG (Endzustand): Port %d Bloecke, Unicorn %d "
              "Instruktionen" % (len(ptrace), u["insns"]))
        for line in diffs[:16]:
            print("  " + line)
        print("  abweichende Speicherseiten: %d" % npages)
        print("  Hinweis: gleiche Blockfolge, aber Port kehrt zu frueh/"
              "mit falschem Zustand zurueck (Vorgaenger 0x%08X)."
              % ptrace[-1][0])
        disasm(fd.ROOT, ptrace[-1][0], a.context, snap["flags"]["t"])
        return 1
    print("OK: Blockfolge, Endzustand, Register und Speicher identisch.")
    return 0


def printbefund(msg):
    print(msg)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("snapshot", help="Port-Lauf mit ARM_DUMP-Schnappschuss")
    s.add_argument("--root", default=".")
    s.add_argument("--watch", type=lambda x: int(x, 0), required=True)
    s.add_argument("--out", required=True)
    s.add_argument("--timeout", type=int, default=60)
    s.add_argument("--exe", default=None)
    s.set_defaults(func=cmd_snapshot)

    r = sub.add_parser("run", help="Schnappschuss im Blockgleichschritt vergleichen")
    r.add_argument("--root", default=".")
    r.add_argument("--snapshot", required=True)
    r.add_argument("--entry", type=lambda x: int(x, 0), default=None)
    r.add_argument("--limit-blocks", type=int, default=20000000)
    r.add_argument("--unicorn-limit", type=int, default=50000000)
    r.add_argument("--mem-limit", type=int, default=12)
    r.add_argument("--func-addr", type=lambda x: int(x, 0), default=None)
    r.add_argument("--context", type=int, default=24)
    r.set_defaults(func=cmd_run)
    a = ap.parse_args(argv)
    if a.cmd == "snapshot" and not a.exe:
        a.exe = os.path.join(os.path.abspath(a.root), "out", "port.exe")
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
