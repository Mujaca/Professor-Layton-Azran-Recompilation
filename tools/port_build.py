"""Baut die Port-Binary, laeuft kurz, sammelt fehlende Call-Ziele und wiederholt.

  python tools/port_build.py [--runs 6] [--seconds 5]
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
MISSING = os.path.join(ROOT, "out", "missing_calls.txt")
SEEDS = os.path.join(ROOT, "out", "seeds.txt")
GEN = os.path.join(ROOT, "build", "gen_boot.c")
EXE = os.path.join(ROOT, "out", "boot.exe")
LOG = os.path.join(ROOT, "out", "boot_log.txt")


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main():
    runs = 6
    seconds = 40
    for i, arg in enumerate(sys.argv):
        if arg == "--runs":
            runs = int(sys.argv[i + 1])
        if arg == "--seconds":
            seconds = int(sys.argv[i + 1])

    env = dict(os.environ)
    env["PYTHONPATH"] = TOOLS
    env["PYTHONIOENCODING"] = "utf-8"

    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), "zig.exe")

    for run_idx in range(runs):
        gen_cmd = [sys.executable, "-m", "recomp.cli", "gen",
                   "--in", os.path.join(ROOT, "out", "exefs", "code_decomp.bin"),
                   "--out", GEN, "--prologues", "--seed-pointers", "--trace",
                   "--seed-file", SEEDS]
        p = run(gen_cmd, cwd=ROOT, env=env)
        if p.returncode != 0:
            print("GEN-FEHLER:\n", p.stdout[-2000:], p.stderr[-2000:])
            return 1

        cmd = [zig, "cc", "-std=c11", "-O1", "-I", os.path.join(ROOT, "runtime"),
               "-I", os.path.join(ROOT, "runtime", "hle"),
               os.path.join(ROOT, "tests", "boot.c"), GEN,
               os.path.join(ROOT, "runtime", "arm_cpu.c"),
               os.path.join(ROOT, "runtime", "arm_trace.c"),
               os.path.join(ROOT, "runtime", "hle", "hle.c"), "-o", EXE]
        p = run(cmd)
        if p.returncode != 0:
            print("BUILD-FEHLER:\n", p.stderr[-3000:])
            return 1
        print("Lauf %d: build ok" % run_idx)

        proc = subprocess.Popen([EXE, os.path.join(ROOT, "out", "romfs")], cwd=ROOT,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            out, _ = proc.communicate(timeout=seconds)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
        with open(LOG, "w", encoding="utf-8") as f:
            f.write(out or "")
        missing = []
        if os.path.exists(MISSING):
            with open(MISSING, encoding="utf-8") as mf:
                missing = [l.strip() for l in mf if l.strip()]
            open(MISSING, "w").close()
        if missing:
            known = set()
            if os.path.exists(SEEDS):
                known = set(l.strip().upper() for l in open(SEEDS) if l.strip())
            with open(SEEDS, "a", encoding="utf-8") as sf:
                for m in missing:
                    v = int(m, 16)
                    if not (0x100000 <= (v & ~1) < 0x3EB000):
                        continue
                    key = "%08X" % v
                    if key not in known:
                        sf.write(key + "\n")
                        known.add(key)
        print("   fehlende Ziele: %d   (Log: %d Zeilen)" % (len(missing), len((out or "").splitlines())))
        tail = [l for l in (out or "").splitlines() if l.startswith("[") or "arm:" in l]
        for l in tail[-4:]:
            print("   ", l)
        if not missing:
            print("keine fehlenden Ziele mehr")
            return 0
        # fehlende Ziele sind beim naechsten Lauf Seeds
    return 0


if __name__ == "__main__":
    sys.exit(main())
