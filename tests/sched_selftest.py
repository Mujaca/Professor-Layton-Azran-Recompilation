"""Deterministischer Selbsttest der Scheduler-Entscheidungen.

  python tests/sched_selftest.py

Baut tests/sched_selftest.c gegen runtime/kernel/kernel_sched.h und fuehrt es
aus. Geprueft werden Prioritaetswahl, Weck-Reihenfolge,
WaitSynchronizationN-Index, die SystemTick-Umrechnung (268111856 Hz) und die
virtuelle Zeitbasis des deterministischen Modus (runtime/kernel/vclock.h).
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "out", "selftest")


def main():
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), "zig.exe")
    os.makedirs(OUT, exist_ok=True)
    exe = os.path.join(OUT, "sched_selftest.exe")
    cmd = [zig, "cc", "-std=c11", "-O1", "-Wall", "-Wextra",
           "-I", os.path.join(ROOT, "runtime", "kernel"),
           os.path.join(ROOT, "tests", "sched_selftest.c"), "-o", exe]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print("COMPILE-FEHLER\n%s\n%s" % (p.stdout, p.stderr))
        return 1
    r = subprocess.run([exe], capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    if r.returncode != 0:
        print("[sched] FEHLER: %s" % out)
        return 1
    print("[sched] %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
