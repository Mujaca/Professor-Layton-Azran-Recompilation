"""Vergleicht die Port-Blockspur (out/btrace.txt) mit Unicorn vom selben
Startzustand (Speicherabbild am ARM_WATCH-Treffer) und meldet die erste
Abweichung von pc/Registern/Flags."""
import sys

from unicorn import UcError, UC_HOOK_CODE
from unicorn.arm_const import *

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
import replay  # noqa: E402

REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4,
        UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9,
        UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR]


def load_port(path, limit):
    seq = []
    for line in open(path):
        f = line.split()
        seq.append((int(f[0], 16), [int(x, 16) for x in f[1:16]], f[16]))
        if len(seq) >= limit:
            break
    return seq


def main():
    dumpdir, btrace = sys.argv[1], sys.argv[2]
    port = load_port(btrace, 60000)
    starts = {p for p, _, _ in port}
    # Unicorn vorbereiten wie replay.run, aber mit Instruktions-Hook
    import os
    os.environ.setdefault("START", "17A6A0")
    mu_holder = {}
    useq = []

    def hook(uc, addr, size, data):
        if addr in starts:
            cpsr = uc.reg_read(UC_ARM_REG_CPSR)
            fl = "%d%d%d%d" % ((cpsr >> 31) & 1, (cpsr >> 30) & 1, (cpsr >> 29) & 1, (cpsr >> 28) & 1)
            useq.append((addr, [uc.reg_read(r) for r in REGS], fl))
            if len(useq) > len(port) + 10:
                uc.emu_stop()

    replay_run = replay.run

    # replay.run baut Unicorn auf; wir haengen unseren Hook ueber einen kleinen Umweg ein
    import unicorn
    orig_start = unicorn.Uc.emu_start

    def patched(self, begin, until, timeout=0, count=0):
        self.hook_add(UC_HOOK_CODE, hook)
        return orig_start(self, begin, until, timeout, count)

    unicorn.Uc.emu_start = patched
    replay_run(dumpdir)
    unicorn.Uc.emu_start = orig_start

    # Ausrichten: ersten Unicorn-Eintrag suchen, der port[0] gleicht
    j0 = next((j for j, u in enumerate(useq) if u[0] == port[0][0] and u[1] == port[0][1]), None)
    if j0 is None:
        print("keine Ausrichtung gefunden; port[0]=%08X, unicorn erste pcs: %s" % (
            port[0][0], " ".join("%08X" % u[0] for u in useq[:10])))
        return
    print("Ausrichtung: unicorn[%d] = port[0] (%08X)" % (j0, port[0][0]))
    names = ["r%d" % i for i in range(13)] + ["sp", "lr"]
    j = j0
    skipped = 0
    for i, p in enumerate(port):
        u = useq[j] if j < len(useq) else None
        if u is None:
            print("Unicorn-Spur zu Ende bei port-Schritt %d (Unicorn kehrte zurueck)" % i)
            print("  port pc=%08X" % p[0])
            return
        if (p[0], p[1], p[2]) == (u[0], u[1], u[2]):
            j += 1
            continue
        # Versatz durch inline uebernommene Bloecke: kurz vorausschauen
        found = None
        for k in range(1, 9):
            if j + k < len(useq) and (useq[j + k][0], useq[j + k][1], useq[j + k][2]) == (p[0], p[1], p[2]):
                found = k
                break
        if found:
            skipped += found
            j += found + 1
            continue
        print("ERSTE ABWEICHUNG bei port-Schritt %d (unicorn %d, %d Versatzschritte uebersprungen)" % (i, j, skipped))
        print("  vorher: port %08X" % port[i - 1][0])
        for k in range(0, 3):
            if j + k < len(useq):
                uu = useq[j + k]
                print("  unicorn[%d] pc=%08X f=%s diff:%s" % (j + k, uu[0], uu[2], " ".join(
                    "%s:%08X/%08X" % (n, p[1][x], uu[1][x]) for x, n in enumerate(names) if p[1][x] != uu[1][x])))
        print("  port    pc=%08X flags=%s" % (p[0], p[2]))
        print("  vorherige Register (port, Schritt %d):" % (i - 1))
        print("   " + " ".join("%s=%08X" % (n, v) for n, v in zip(names, port[i - 1][1])) + " f=" + port[i - 1][2])
        print("  port-Spur: " + " ".join("%08X" % port[x][0] for x in range(max(0, i - 6), min(len(port), i + 4))))
        print("  unicorn  : " + " ".join("%08X" % useq[x][0] for x in range(max(0, j - 6), min(len(useq), j + 4))))
        return
    print("keine Abweichung in %d port-Schritten (%d Versatzschritte)" % (len(port), skipped))


if __name__ == "__main__":
    main()
