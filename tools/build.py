"""Baut out/port.exe aus dem ROM-Code.

  python tools/build.py              # komplett: Patches -> Recompiler -> C -> exe
  python tools/build.py --no-gen     # nur Runtime neu kompilieren (gen_boot.o bleibt)
  python tools/build.py --pristine   # ohne Code-Patches (zeigt, was die Workarounds verdecken)
  python tools/build.py --out-dir O --build-dir B   # Builder-Paket (tools/builder.py):
                                     # O statt out/, B statt build/

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
    "kernel/romfs_overlay.c",
    "modding/lua_host.c",
    "gpu/gpu.c",
    "gpu/hw_d3d11.c",
    "gpu/hw_present.c",
    "gpu/imgc.c",
    "audio/audio_codec.c",
    "audio/dsp_hle.c",
    "audio/host_output.c",
    "platform/window.c",
    "platform/logtee.c",
    "workarounds.c",
]
INCLUDES = ["", "cpu", "kernel", "gpu", "audio", "platform", "modding"]

# Lua 5.4.7 (third_party/README.md): nur Kern + Sandbox-Bibliotheken, Quellen
# unveraendert. Kein lua.c/luac.c/linit.c/liolib.c/loslib.c/ldblib.c/loadlib.c.
LUA_DIR = os.path.join(ROOT, "third_party", "lua")
LUA_SOURCES = [
    "lapi.c", "lcode.c", "lctype.c", "ldebug.c", "ldo.c", "ldump.c", "lfunc.c",
    "lgc.c", "llex.c", "lmem.c", "lobject.c", "lopcodes.c", "lparser.c",
    "lstate.c", "lstring.c", "ltable.c", "ltm.c", "lundump.c", "lvm.c", "lzio.c",
    "lauxlib.c", "lbaselib.c", "lcorolib.c", "lmathlib.c", "lstrlib.c",
    "ltablib.c", "lutf8lib.c",
]
# Feste String-Hash-Saat (lstate.c nimmt sonst Uhrzeit und Adressen): Mods
# laufen in jedem Start gleich (Reihenfolge von pairs ueber String-Schluessel).
LUA_DEFINES = ["-Dluai_makeseed(L)=0x4C756121u"]


def lua_flags():
    return ["-I", LUA_DIR] + LUA_DEFINES


def lua_sources():
    return [os.path.join(LUA_DIR, s) for s in LUA_SOURCES]


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
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "out"),
                    help="erwartet exefs/code.bin, schreibt exefs/code_decomp.bin + port.exe")
    ap.add_argument("--build-dir", default=BUILD)
    a = ap.parse_args()
    out_dir = os.path.abspath(a.out_dir)
    build_dir = os.path.abspath(a.build_dir)
    os.makedirs(build_dir, exist_ok=True)
    os.makedirs(out_dir, exist_ok=True)
    gen_c = os.path.join(build_dir, "gen_boot.c")
    gen_o = os.path.join(build_dir, "gen_boot.o")

    if not a.no_gen:
        apply_cmd = [sys.executable, os.path.join(TOOLS, "workarounds", "apply.py"),
                     "--code", os.path.join(out_dir, "exefs", "code.bin"),
                     "--out", os.path.join(out_dir, "exefs", "code_decomp.bin")]
        if a.pristine:
            apply_cmd.append("--none")
        print(run(apply_cmd).stdout.strip())

        env = dict(os.environ, PYTHONPATH=TOOLS, PYTHONIOENCODING="utf-8")
        # Erzeugen und Kompilieren unter derselben systemweiten Sperre: der
        # Generator schreibt build/gen_boot.c neu, und ein parallel laufendes
        # zig haelt die Datei gemappt - das Truncaten scheitert dann mit
        # OSError EINVAL (beobachtet bei parallelen Laeufen im selben Baum).
        with gen_lock():
            run([sys.executable, "-m", "recomp.cli", "gen",
                 "--in", os.path.join(out_dir, "exefs", "code_decomp.bin"),
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
                run([zig(), "cc"] + cflags() + ["-c", gen_c, "-o", gen_o])
                open(stamp, "w").write(digest)
            else:
                print("gen_boot.o aktuell")

    srcs = [os.path.join(RUNTIME, s) for s in SOURCES] + lua_sources()
    run([zig(), "cc"] + cflags() + lua_flags() + srcs + [gen_o, "-o", os.path.join(out_dir, "port.exe"),
                                           "-luser32", "-lgdi32", "-lwinmm",
                                           # Arbeitspaket 7: Windows-Programm ohne
                                           # Konsole (--console zeigt sie; Pipes
                                           # der Tools funktionieren weiter).
                                           "-Wl,--subsystem,windows"])
    print("gebaut: %s" % os.path.join(out_dir, "port.exe"))


if __name__ == "__main__":
    main()
