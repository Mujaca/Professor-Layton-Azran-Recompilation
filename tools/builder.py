"""Builder fuer Endnutzer (Arbeitspaket 6): prueft den Dump und baut port.exe.

  python tools/builder.py check <dump.3ds>
  python tools/builder.py build <dump.3ds> --game <ordner> [--keep-build]

Wird im Builder-Paket von LaytonBuilder.exe (tools/builder/starter.c) mit
dem eingebetteten Python aufgerufen, funktioniert aber genauso im Repo.
`build` legt unter <ordner> dasselbe Layout an wie das Repo
(<ordner>/out/port.exe, out/exefs/, out/romfs/, out/romfs_image.bin), damit
port.exe unveraendert laeuft (Datenpfade relativ zu <ordner>, siehe
runtime/main.c data_root_fallback). Zwischenstaende (gen_boot.c/.o,
zig-Cache) liegen in <ordner>/build und werden nach Erfolg geloescht.

Der Dump wird nur gelesen, nie kopiert oder veraendert.

Ausgabeprotokoll fuer den Starter (eine Zeile je Ereignis, UTF-8):
  @step <n>/<m> <Text>   neuer Schritt
  @progress <0..100>     Gesamtfortschritt
  @error <Text>          Fehlermeldung fuer den Nutzer (englisch), danach Exit != 0
  @done <port.exe>       fertig
Alle anderen Zeilen sind Log.

Meldungen fuer den Nutzer sind englisch (Paket fuer Endnutzer).
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))

# --- Kennung der unterstuetzten Ausgabe (EUR-Modul) ---------------------------
# Programm-ID und Produktcode stehen unverschluesselt im NCCH-Kopf (0x118,
# 0x150). Der SHA-256 des (komprimierten) ExeFS-.code ist der des Dumps, aus
# dem tools/recomp/seeds.txt und alle Adressen im Repo stammen; er steht auch
# im ExeFS-Kopf des Dumps selbst. Kein Spielinhalt, nur Pruefsummen.
EUR_PROGRAM_ID = 0x00040000000F3000
EUR_PRODUCT = "CTR-P-AL6P"
GAME_PREFIX = "CTR-P-AL6"
CODE_SHA256 = "d662bc3649ad7d6781b0fb9f93e5649e84211342a7a65e02770128c8bf07fad4"
REGION_NAMES = {"E": "North American", "J": "Japanese", "K": "Korean",
                "C": "Chinese", "W": "Taiwanese", "P": "European"}
NOCRYPTO = 0x04          # NCCH-Flag 7, Bit 2
MIN_FREE = 3 << 30       # RomFS-Dateien + Abbild + Zwischenstaende (~2.3 GiB)


class DumpError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


def _u32(b, off):
    return struct.unpack_from("<I", b, off)[0]


def check_dump(path, code_sha256=CODE_SHA256):
    """Prueft einen Dump. Rueckgabe: dict mit Kennwerten; sonst DumpError
    (code: not_found, not_3ds, cia, ncch, truncated, wrong_region, wrong_game,
    encrypted, code_mismatch)."""
    if not os.path.isfile(path):
        raise DumpError("not_found", "The file does not exist: %s" % path)
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        head = f.read(0x200)
        if len(head) < 0x200:
            raise DumpError("not_3ds", "The file is too small to be a 3DS cartridge dump.")
        if head[0x100:0x104] != b"NCSD":
            if head[0x100:0x104] == b"NCCH":
                raise DumpError("ncch", "This is a single NCCH/CXI partition, not a full "
                                "cartridge dump. Please dump the whole cartridge as a "
                                "decrypted .3ds file (e.g. with GodMode9).")
            if _u32(head, 0) == 0x2020:
                raise DumpError("cia", "This is a CIA file (installable title), not a "
                                "cartridge dump. Please dump the cartridge as a decrypted "
                                ".3ds file (e.g. with GodMode9).")
            raise DumpError("not_3ds", "This is not a 3DS cartridge dump (.3ds). "
                            "No NCSD header found.")
        mu_ncsd = 0x200 << head[0x188 + 6]
        p0_off, p0_size = struct.unpack_from("<II", head, 0x120)
        p0_off *= mu_ncsd
        p0_size *= mu_ncsd
        if p0_size == 0 or p0_off < 0x200:
            raise DumpError("not_3ds", "The cartridge dump has no game partition.")
        if p0_off + 0x200 > size:
            raise DumpError("truncated", "The dump is incomplete (file ends before the "
                            "game partition).")
        f.seek(p0_off)
        ncch = f.read(0x200)
        if ncch[0x100:0x104] != b"NCCH":
            raise DumpError("not_3ds", "Partition 0 of the dump is not a valid NCCH "
                            "(bad or corrupted dump).")
        program_id = struct.unpack_from("<Q", ncch, 0x118)[0]
        product = ncch[0x150:0x160].split(b"\0")[0].decode("ascii", "replace")
        if product != EUR_PRODUCT or program_id != EUR_PROGRAM_ID:
            if (product != EUR_PRODUCT and product.startswith(GAME_PREFIX)
                    and len(product) == len(EUR_PRODUCT)):
                region = REGION_NAMES.get(product[-1], "non-European")
                raise DumpError("wrong_region", "This is the %s release of the game "
                                "(%s). Only the European release (%s) is supported."
                                % (region, product, EUR_PRODUCT))
            raise DumpError("wrong_game", "This is not Professor Layton and the Azran "
                            "Legacy (found product code '%s', title ID %016X; expected "
                            "%s, %016X)." % (product, program_id, EUR_PRODUCT,
                                             EUR_PROGRAM_ID))
        mu = 0x200 << ncch[0x188 + 6]
        flags7 = ncch[0x188 + 7]
        exefs_off = _u32(ncch, 0x1A0) * mu
        romfs_off, romfs_size = struct.unpack_from("<II", ncch, 0x1B0)
        romfs_off *= mu
        romfs_size *= mu
        if exefs_off == 0 or romfs_off == 0:
            raise DumpError("not_3ds", "The game partition has no ExeFS/RomFS "
                            "(bad or corrupted dump).")
        if p0_off + romfs_off + romfs_size > size:
            raise DumpError("truncated", "The dump is incomplete (file ends inside the "
                            "game data). Please dump the cartridge again.")
        f.seek(p0_off + exefs_off)
        exefs = f.read(0x200)
        f.seek(p0_off + romfs_off)
        romfs_magic = f.read(4)
        plain = exefs[0:8] == b".code\0\0\0" and romfs_magic == b"IVFC"
        if not plain:
            raise DumpError("encrypted", "The dump is encrypted. Please create a "
                            "decrypted dump (GodMode9: dump the cartridge, then use "
                            "'Decrypt file (...)' on the .3ds) and select that file.")
        code_off, code_size = struct.unpack_from("<II", exefs, 8)
        f.seek(p0_off + exefs_off + 0x200 + code_off)
        digest = hashlib.sha256(f.read(code_size)).hexdigest()
        if digest != code_sha256:
            raise DumpError("code_mismatch", "The game code in this dump does not match "
                            "the known European release (bad, modified or patched dump?). "
                            "Please dump the cartridge again.")
    return {"size": size, "program_id": program_id, "product": product,
            "nocrypto_flag": bool(flags7 & NOCRYPTO), "code_sha256": digest}


# --- Bau -------------------------------------------------------------------

STEPS = [  # (Titel, Gewicht in % der Gesamtzeit, grob gemessen)
    ("Checking the dump", 2),
    ("Extracting ExeFS", 2),
    ("Extracting RomFS files", 16),
    ("Extracting RomFS image", 8),
    ("Recompiling and compiling (takes several minutes)", 70),
    ("Cleaning up", 2),
]


class Progress:
    def __init__(self):
        self.base = 0
        self.idx = 0

    def step(self, i):
        self.idx = i
        self.base = sum(w for _, w in STEPS[:i])
        emit("@step %d/%d %s" % (i + 1, len(STEPS), STEPS[i][0]))
        emit("@progress %d" % self.base)

    def part(self, frac):
        emit("@progress %d" % (self.base + int(STEPS[self.idx][1] * max(0.0, min(1.0, frac)))))


def emit(line):
    sys.stdout.write(line + "\n")
    sys.stdout.flush()


def fail(message, rc=2):
    emit("@error " + message.replace("\n", " "))
    sys.exit(rc)


def run_step(cmd, env=None, on_line=None):
    """Startet ein Python-Tool, reicht die Ausgabe zeilenweise durch."""
    full = [sys.executable, "-u"] + cmd
    emit("> " + " ".join(os.path.basename(c) if i < 2 else c for i, c in enumerate(full)))
    p = subprocess.Popen(full, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         env=env, cwd=os.path.dirname(TOOLS),
                         creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    for raw in p.stdout:
        line = raw.decode("utf-8", "replace").rstrip()
        emit("  " + line)
        if on_line:
            on_line(line)
    if p.wait() != 0:
        return False
    return True


def cmd_check(a):
    try:
        info = check_dump(a.rom)
    except DumpError as e:
        fail(e.message)
    emit("OK: %s, title ID %016X, %.1f MiB, code SHA-256 %s"
         % (info["product"], info["program_id"], info["size"] / 2**20, info["code_sha256"]))
    return 0


def cmd_build(a):
    t0 = time.time()
    prog = Progress()
    game = os.path.abspath(a.game)
    out = os.path.join(game, "out")
    build = os.path.join(game, "build")
    rom = os.path.abspath(a.rom)

    prog.step(0)
    try:
        info = check_dump(rom)
    except DumpError as e:
        fail(e.message)
    emit("dump OK: %s (%016X)" % (info["product"], info["program_id"]))
    if os.path.normcase(os.path.dirname(rom)).startswith(os.path.normcase(out)):
        fail("Please keep the dump outside the output folder.")
    try:
        os.makedirs(out, exist_ok=True)
    except OSError as e:
        fail("Cannot create the output folder %s (%s). Choose a folder you can "
             "write to." % (out, e.strerror))
    free = shutil.disk_usage(game).free
    if free < MIN_FREE:
        fail("Not enough disk space in %s: %.1f GB free, about %.1f GB needed."
             % (game, free / 1e9, MIN_FREE / 1e9))
    exe = os.path.join(out, "port.exe")
    if os.path.exists(exe):
        try:
            open(exe, "ab").close()       # schlaegt fehl, solange port.exe laeuft
        except OSError:
            fail("port.exe is still running. Close the game and try again.")

    env = dict(os.environ, PYTHONIOENCODING="utf-8",
               ZIG_GLOBAL_CACHE_DIR=os.path.join(build, "zig-cache"),
               ZIG_LOCAL_CACHE_DIR=os.path.join(build, "zig-cache"))
    tools = TOOLS

    prog.step(1)
    if not run_step([os.path.join(tools, "extract.py"), "--rom", rom, "--out", out,
                     "--reports", os.path.join(build, "reports")], env):
        fail("Extracting the ExeFS failed (see log).")

    prog.step(2)
    pat = re.compile(r"^\s*(\d+)/(\d+) ")

    def romfs_line(line):
        m = pat.match(line)
        if m and int(m.group(2)):
            prog.part(int(m.group(1)) / int(m.group(2)))
    if not run_step([os.path.join(tools, "extract_romfs.py"), "--rom", rom, "--out", out],
                    env, romfs_line):
        fail("Extracting the RomFS files failed (see log).")

    prog.step(3)
    if not run_step([os.path.join(tools, "romfs_image.py"), rom,
                     os.path.join(out, "romfs_image.bin")], env):
        fail("Extracting the RomFS image failed (see log).")

    prog.step(4)
    def build_line(line):
        if "kompiliere gen_boot.c" in line:
            prog.part(0.15)
        elif "gen_boot.o aktuell" in line:
            prog.part(0.9)
    t_build = time.time()
    if not run_step([os.path.join(tools, "build.py"), "--out-dir", out,
                     "--build-dir", build], env, build_line):
        fail("Building port.exe failed (see log).")
    if not os.path.isfile(exe):
        fail("Building port.exe failed: %s is missing." % exe)
    emit("build step: %.0f s" % (time.time() - t_build))

    prog.step(5)
    if not a.keep_build:
        shutil.rmtree(build, ignore_errors=True)
    emit("@progress 100")
    emit("finished in %.0f s" % (time.time() - t0))
    emit("@done " + exe)
    return 0


def main(argv=None):
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("rom")
    b = sub.add_parser("build")
    b.add_argument("rom")
    b.add_argument("--game", required=True, help="Zielordner (enthaelt danach out/port.exe)")
    b.add_argument("--keep-build", action="store_true",
                   help="gen_boot.c/.o und zig-Cache in <game>/build behalten")
    a = ap.parse_args(argv)
    return cmd_check(a) if a.cmd == "check" else cmd_build(a)


if __name__ == "__main__":
    sys.exit(main())
