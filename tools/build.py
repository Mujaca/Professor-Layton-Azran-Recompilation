"""Baut out/port.exe aus dem ROM-Code.

  python tools/build.py              # komplett: Patches -> Recompiler -> C -> exe
  python tools/build.py --no-gen     # nur Runtime neu kompilieren (gen_boot.o bleibt)
  python tools/build.py --pristine   # ohne Code-Patches (zeigt, was die Workarounds verdecken)

Schritte:
 1. tools/workarounds/apply.py      code.bin -> out/exefs/code_decomp.bin (+ Patches)
 2. tools/recomp (gen)              -> build/gen_boot.c
 3. tools/workarounds/gen_patches   Textersetzungen in gen_boot.c (Schuld)
 4. zig cc                          gen_boot.c -> build/gen_boot.o (nur wenn geändert)
 5. zig cc                          Runtime + gen_boot.o -> out/port.exe
"""
import argparse
import hashlib
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
BUILD = os.path.join(ROOT, "build")
RUNTIME = os.path.join(ROOT, "runtime")

# Alle Runtime-Quellen. Neue Dateien hier eintragen.
SOURCES = [
    "main.c",
    "cpu/arm_cpu.c",
    "cpu/arm_trace.c",
    "kernel/hle.c",
    "kernel/fs_archive.c",
    "gpu/gpu.c",
    "gpu/imgc.c",
    "audio/audio_codec.c",
    "audio/dsp_hle.c",
    "audio/host_output.c",
    "platform/window.c",
    "workarounds.c",
]
INCLUDES = ["", "cpu", "kernel", "gpu", "audio", "platform"]


def zig():
    import ziglang
    return os.path.join(os.path.dirname(ziglang.__file__), "zig.exe")


def run(cmd, **kw):
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if p.returncode != 0:
        sys.stdout.write(p.stdout[-3000:])
        sys.stdout.write(p.stderr[-3000:])
        raise SystemExit("FEHLER: %s" % " ".join(cmd[:3]))
    return p


class gen_lock:
    """Systemweite Sperre (Temp-Verzeichnis) um die gen_boot.c-Kompilierung."""

    def __enter__(self):
        import tempfile
        import time
        self.f = open(os.path.join(tempfile.gettempdir(), "3ds_gen_build.lock"), "a+")
        if os.name == "nt":
            import msvcrt
            while True:
                try:
                    self.f.seek(0)
                    msvcrt.locking(self.f.fileno(), msvcrt.LK_NBLCK, 1)
                    return self
                except OSError:
                    time.sleep(2)
        import fcntl
        fcntl.flock(self.f, fcntl.LOCK_EX)
        return self

    def __exit__(self, *exc):
        if os.name == "nt":
            import msvcrt
            self.f.seek(0)
            msvcrt.locking(self.f.fileno(), msvcrt.LK_UNLCK, 1)
        self.f.close()


def cflags():
    flags = ["-std=c11", "-O1"]
    for inc in INCLUDES:
        flags += ["-I", os.path.join(RUNTIME, inc)]
    return flags


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-gen", action="store_true")
    ap.add_argument("--pristine", action="store_true")
    a = ap.parse_args()
    os.makedirs(BUILD, exist_ok=True)
    os.makedirs(os.path.join(ROOT, "out"), exist_ok=True)
    gen_c = os.path.join(BUILD, "gen_boot.c")
    gen_o = os.path.join(BUILD, "gen_boot.o")

    if not a.no_gen:
        apply_cmd = [sys.executable, os.path.join(TOOLS, "workarounds", "apply.py")]
        if a.pristine:
            apply_cmd.append("--none")
        print(run(apply_cmd).stdout.strip())

        env = dict(os.environ, PYTHONPATH=TOOLS, PYTHONIOENCODING="utf-8")
        run([sys.executable, "-m", "recomp.cli", "gen",
             "--in", os.path.join(ROOT, "out", "exefs", "code_decomp.bin"),
             "--out", gen_c, "--prologues", "--seed-pointers", "--trace",
             "--seed-file", os.path.join(TOOLS, "recomp", "seeds.txt")],
            cwd=ROOT, env=env)
        if not a.pristine:
            sys.path.insert(0, os.path.join(TOOLS, "workarounds"))
            import gen_patches
            gen_patches.apply(gen_c)

        digest = hashlib.sha256(open(gen_c, "rb").read()).hexdigest()
        stamp = gen_o + ".sha256"
        old = open(stamp).read().strip() if os.path.exists(stamp) else ""
        if digest != old or not os.path.exists(gen_o):
            print("kompiliere gen_boot.c (dauert einige Minuten) ...")
            # Mehrere Worktrees bauen parallel; gleichzeitige gen_boot.c-
            # Kompilierungen scheitern am Speicher. Systemweit serialisieren.
            with gen_lock():
                run([zig(), "cc"] + cflags() + ["-c", gen_c, "-o", gen_o])
            open(stamp, "w").write(digest)
        else:
            print("gen_boot.o aktuell")

    srcs = [os.path.join(RUNTIME, s) for s in SOURCES]
    run([zig(), "cc"] + cflags() + srcs + [gen_o, "-o", os.path.join(ROOT, "out", "port.exe"),
                                           "-luser32", "-lgdi32", "-lwinmm"])
    print("gebaut: out/port.exe")


if __name__ == "__main__":
    main()
