"""Standard-Szenen reproduzierbar anfahren (ohne Durchklicken).

  python tools/scene.py titel
  python tools/scene.py erste-szene --renderer soft --wav --profile
  python tools/scene.py froenborg --every 300
  python tools/scene.py froenborg raetsel1 --repro     # zwei Laeufe, Bildfolgen gleich?
  python tools/scene.py --list

Ablauf je Szene:
 1. Fixture-Spielstand (out/fixtures/saves/<fixture>/, Layout wie
    out/savedata) in einen temporaeren Ordner kopieren - die Fixture selbst
    wird nie beschrieben.
 2. out/port.exe mit --savedata <kopie>, --renderer hw (oder soft) und den
    Eingaben der Szene in Gast-Frames (--press taste@g<frame>) starten;
    der Lauf endet per --frames-guest am Ziel-Gast-Frame (Host-Frame-Grenze
    nur als Notbremse).
 3. Ergebnis nach scratch/scene/<name>/: ziel.png (Endbild), optional
    seq_<frame>.png (--every N Gast- bzw. --every-host N Host-Frames),
    port.log, optional audio.wav (HLE_AUDIO_WAV) und profil.txt
    (Sampling-Profiler, --profile), spielstand/ (Kopie nach dem Lauf, z. B.
    um eine neue Fixture abzuleiten).

port.exe laeuft dabei mit --deterministic (virtuelle Zeit, VBlank im
Gleichschritt mit dem Host, runtime/kernel/vclock.h): zwei Laeufe liefern
dieselbe Bildfolge, unabhaengig von Renderer und Rechnerlast. --realtime
schaltet das ab (Wanduhr; dann gelten die Szenen-Zeiten nicht).

Alle Eingaben gehen ueber die normale HID-Eingabe des Ports (--press); es
wird kein Gastzustand gesetzt. Die Fixtures liegen unter out/ und werden nicht
eingecheckt (Nutzer-Spielstaende); `--list` zeigt Szenen und Fixtures.
"""
import argparse
import collections
import ctypes
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "out" / "port.exe"
FIXTURES = ROOT / "out" / "fixtures" / "saves"
OUTBASE = ROOT / "scratch" / "scene"

# Touch-Ziele auf dem unteren Screen (320x240).
# Mit vorhandenem Spielstand: New Game oben, Continue in der Mitte (ohne
# Spielstand steht New Game mittig bei 160,118 - GAME_NOTES).
NEW_GAME = (160, 64)
CONTINUE = (160, 118)
KB_OK = (160, 225)


def touch(g, xy, dur=12):
    return "touch@g%d:%d:%d:%d" % (g, dur, xy[0], xy[1])


# Szenen: fixture, Eingaben (Gast-Frames), Ziel-Gast-Frame, Beschreibung.
# Zeiten wurden mit --every an Bildfolgen ausgemessen (docs/TOOLS.md) und
# gelten fuer den deterministischen Modus (port.exe --deterministic, Standard
# hier): dort ist ein Lauf bildgleich wiederholbar, unabhaengig von Renderer
# und Hostlast.
#
# Das Titelmenue ist ab ~g1900 sichtbar, nimmt Tipps aber erst ab ~g2450 an
# (gemessen: g2400 ignoriert, g2500 angenommen). Im Echtzeitbetrieb haengt
# diese Schwelle an der Wanduhr, nicht an Gast-Frames (docs/GAME_NOTES.md),
# deshalb griffen fruehere Zeiten (g1800) nach dem schnelleren HW-Renderer
# nicht mehr. Die Folgen nach "New Game" wurden um MENU_SHIFT gegenueber den
# alten Messungen (Menue-Tipp bei g1800) verschoben.
MENU = 2550
MENU_SHIFT = MENU - 1800
NAME_KEYS = [(55, 111), (170, 135), (112, 135)]   # drei Buchstaben


def new_game_presses(t_new, t_kb):
    p = [touch(t_new, NEW_GAME, 20)]
    for i, xy in enumerate(NAME_KEYS):
        p.append(touch(t_kb + 60 * i, xy))
    p.append(touch(t_kb + 60 * len(NAME_KEYS) + 120, KB_OK, 15))  # OK!
    return p


# Slot-Auswahl (Laden/Speichern): Slot 1/2/3 untereinander.
SLOT = {1: (160, 35), 2: (160, 106), 3: (160, 175)}

# Raetsel 001 "Airship's Destination" (4x4-Raster, Start oben links). Einzige
# Route durch alle Staedte/Tankstellen mit 4 Tankeinheiten (ausgerechnet):
PUZZLE1_PATH = [(1, 0), (2, 0), (3, 0), (3, 1), (2, 1), (2, 2), (1, 2), (1, 1),
                (0, 1), (0, 2), (0, 3), (1, 3), (2, 3), (3, 3), (3, 2)]
PUZZLE1_X = [97, 138, 180, 223]
PUZZLE1_Y = [57, 98, 140, 182]


def puzzle1_presses():
    """Ab dem Raetsel-Titel "001": Titel antippen, dann Tutorial-Hinweis
    wegtippen (der Gast-Zaehler steht dort, daher g<frame>+<host>), Tipps auf
    das Luftschiff sind harmlos; danach die Route im Abstand von 25
    Gast-Frames (schnellere Tipps fallen in die Zuganimation und gehen
    verloren)."""
    ship = (PUZZLE1_X[0], PUZZLE1_Y[0])
    d = MENU_SHIFT
    p = ["touch@g%d+30:12:160:120" % (5540 + d)]
    p += ["touch@g%d+%d:8:%d:%d" % (5950 + d, h, ship[0], ship[1]) for h in (60, 120, 180, 240)]
    p += ["touch@g%d+%d:8:%d:%d" % (6030 + d, h, ship[0], ship[1]) for h in (60, 120)]
    p += [touch(g + d, ship, 6) for g in (6070, 6100)]
    # Im deterministischen Modus liegt der "TOUCH"-Hinweis bis ~g6920 ueber
    # dem Raster; die Route beginnt deshalb 70 Gast-Frames spaeter als frueher.
    p += [touch(6210 + d + 25 * i, (PUZZLE1_X[c], PUZZLE1_Y[r]), 6)
          for i, (r, c) in enumerate(PUZZLE1_PATH)]
    return p


def continue_presses(slot):
    """Continue -> Slot -> "The Story So Far..." weitertippen -> Froenborg."""
    p = [touch(MENU, CONTINUE, 20), touch(MENU + 300, SLOT[slot], 10)]
    p += [touch(g, (160, 120), 8) for g in range(3300, 4250, 150)]
    return p


SCENES = collections.OrderedDict()
SCENES["titel"] = dict(
    fixture="froenborg", target=MENU, presses=[],
    desc="Titelbild mit Menue (New Game / Continue / Bonuses)")
SCENES["namenseingabe"] = dict(
    fixture="froenborg", target=MENU + 300, presses=[touch(MENU, NEW_GAME, 20)],
    desc="Bildschirmtastatur nach 'New Game'")
SCENES["erste-szene"] = dict(
    fixture="froenborg", target=3300 + MENU_SHIFT, presses=new_game_presses(MENU, MENU + 300),
    desc="Neues Spiel (Name 'abc'): erste Spielszene des Prologs")
SCENES["raetsel1-neu"] = dict(
    fixture="froenborg", target=5520 + MENU_SHIFT, presses=new_game_presses(MENU, MENU + 300),
    desc="Neues Spiel bis zur Titelkarte von Raetsel 001 (ohne Speichern)")
SCENES["fixture-raetsel1"] = dict(
    fixture="froenborg", target=12000 + MENU_SHIFT,
    presses=new_game_presses(MENU, MENU + 300) + puzzle1_presses() +
    [touch(g + MENU_SHIFT, (160, 120), 8) for g in (7200, 7350, 7550, 7750, 7950)] +   # Erklaerung/Zwischenszenen
    [touch(11000 + MENU_SHIFT, KB_OK, 10),            # Mysteries: OK!
     touch(11400 + MENU_SHIFT, (100, 147), 10),       # "Save your progress?" -> OK!
     touch(11700 + MENU_SHIFT, SLOT[2], 10)],         # Slot 2
    desc="Neues Spiel, Raetsel 001 loesen, in Slot 2 speichern; "
         "spielstand/ ist dann die Fixture raetsel1 (~13 min)")
SCENES["froenborg"] = dict(
    fixture="froenborg", target=4450, presses=continue_presses(1),
    desc="Continue Slot 1 (Prologue End): 3D-Dialogszene in Froenborg")
SCENES["raetsel1"] = dict(
    fixture="raetsel1", target=4450, presses=continue_presses(2),
    desc="Continue Slot 2: Spielstand direkt nach Raetsel 001 (erster Speicherpunkt) "
         "-> Kapitel 1, Froenborg")


# --------------------------------------------------------------------------
# Sampling-Profiler (Windows x64): haelt alle Threads ~1000x/s kurz an, liest
# RIP und loest am Ende per dbghelp Funktionsnamen auf (PDB neben port.exe).


def _profiler_types():
    from ctypes import wintypes as W

    class THREADENTRY32(ctypes.Structure):
        _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD), ("th32ThreadID", W.DWORD),
                    ("th32OwnerProcessID", W.DWORD), ("tpBasePri", W.LONG),
                    ("tpDeltaPri", W.LONG), ("dwFlags", W.DWORD)]

    regs = ["P1Home", "P2Home", "P3Home", "P4Home", "P5Home", "P6Home"]

    class CONTEXT(ctypes.Structure):
        _fields_ = ([(r, ctypes.c_uint64) for r in regs] +
                    [("ContextFlags", W.DWORD), ("MxCsr", W.DWORD),
                     ("SegCs", W.WORD), ("SegDs", W.WORD), ("SegEs", W.WORD), ("SegFs", W.WORD),
                     ("SegGs", W.WORD), ("SegSs", W.WORD), ("EFlags", W.DWORD)] +
                    [(r, ctypes.c_uint64) for r in
                     ["Dr0", "Dr1", "Dr2", "Dr3", "Dr6", "Dr7", "Rax", "Rcx", "Rdx", "Rbx",
                      "Rsp", "Rbp", "Rsi", "Rdi", "R8", "R9", "R10", "R11", "R12", "R13",
                      "R14", "R15", "Rip"]] +
                    [("rest", ctypes.c_byte * 1024)])

    class SYMBOL_INFO(ctypes.Structure):
        _fields_ = [("SizeOfStruct", W.ULONG), ("TypeIndex", W.ULONG),
                    ("Reserved", ctypes.c_uint64 * 2), ("Index", W.ULONG), ("Size", W.ULONG),
                    ("ModBase", ctypes.c_uint64), ("Flags", W.ULONG), ("Value", ctypes.c_uint64),
                    ("Address", ctypes.c_uint64), ("Register", W.ULONG), ("Scope", W.ULONG),
                    ("Tag", W.ULONG), ("NameLen", W.ULONG), ("MaxNameLen", W.ULONG),
                    ("Name", ctypes.c_char * 256)]
    return W, THREADENTRY32, CONTEXT, SYMBOL_INFO


class Profiler(threading.Thread):
    def __init__(self, proc, delay_s):
        super().__init__(daemon=True)
        self.proc = proc
        self.delay_s = delay_s
        self.stop = threading.Event()
        self.report = "Profil: keine Samples\n"

    def run(self):
        W, THREADENTRY32, CONTEXT, SYMBOL_INFO = _profiler_types()
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        dbg = ctypes.WinDLL("dbghelp", use_last_error=True)
        k32.OpenThread.restype = W.HANDLE
        k32.OpenProcess.restype = W.HANDLE
        k32.CreateToolhelp32Snapshot.restype = W.HANDLE
        if self.stop.wait(self.delay_s):
            return
        pid = self.proc.pid
        hproc = k32.OpenProcess(0x1F0FFF, False, pid)
        # Symbole jetzt laden, solange der Prozess lebt (Modulliste); am Ende
        # ist port.exe beendet und dbghelp faende keine Module mehr.
        time.sleep(0.5)
        dbg.SymSetOptions(0x2 | 0x10)
        dbg.SymInitialize(W.HANDLE(hproc), str(PORT.parent).encode(), True)
        buf = ctypes.create_string_buffer(ctypes.sizeof(CONTEXT) + 16)
        addr = (ctypes.addressof(buf) + 15) & ~15
        ctx = CONTEXT.from_address(addr)
        samples = collections.Counter()
        tsamples = collections.Counter()
        handles = {}
        rounds = 0
        t0 = time.time()

        def threads():
            snap = k32.CreateToolhelp32Snapshot(0x4, 0)
            te = THREADENTRY32()
            te.dwSize = ctypes.sizeof(te)
            out = []
            ok = k32.Thread32First(snap, ctypes.byref(te))
            while ok:
                if te.th32OwnerProcessID == pid:
                    out.append(te.th32ThreadID)
                ok = k32.Thread32Next(snap, ctypes.byref(te))
            k32.CloseHandle(snap)
            return out

        while not self.stop.is_set() and self.proc.poll() is None:
            for tid in threads():
                h = handles.get(tid)
                if h is None:
                    h = handles[tid] = k32.OpenThread(0x001F03FF, False, tid)
                if not h or k32.SuspendThread(h) == 0xFFFFFFFF:
                    continue
                ctx.ContextFlags = 0x00100001  # CONTEXT_CONTROL (AMD64)
                if k32.GetThreadContext(h, ctypes.c_void_p(addr)):
                    samples[ctx.Rip] += 1
                    tsamples[(tid, ctx.Rip)] += 1
                k32.ResumeThread(h)
            rounds += 1
            time.sleep(0.001)
        dur = time.time() - t0
        si = SYMBOL_INFO()
        si.SizeOfStruct = 88
        si.MaxNameLen = 255
        disp = ctypes.c_uint64()
        names = {}
        funcs = collections.Counter()
        for rip, cnt in samples.items():
            if dbg.SymFromAddr(W.HANDLE(hproc), ctypes.c_uint64(rip), ctypes.byref(disp),
                               ctypes.byref(si)):
                names[rip] = si.Name.decode(errors="replace")
            else:
                names[rip] = "?%X" % rip
            funcs[names[rip]] += cnt
        per = collections.defaultdict(collections.Counter)
        for (tid, rip), cnt in tsamples.items():
            per[tid][names[rip]] += cnt
        wait = ("Wait", "Delay", "Alert", "Sleep")
        lines = ["== Profil: %.1f s, %d Runden, %d Samples" % (dur, rounds, sum(funcs.values())),
                 "== je Thread (aktiver Anteil, Top 3)"]

        def active(ctr):
            return sum(v for k, v in ctr.items() if not any(w in k for w in wait))
        for tid, ctr in sorted(per.items(), key=lambda kv: -active(kv[1])):
            t = sum(ctr.values())
            lines.append("tid %6d aktiv %5.1f%%  %s" % (
                tid, 100.0 * active(ctr) / t,
                ", ".join("%s %d%%" % (k, 100 * v // t) for k, v in ctr.most_common(3))))
        tot = max(1, sum(funcs.values()))
        lines.append("== Funktionen (Top 40)")
        for name, cnt in funcs.most_common(40):
            lines.append("%6.2f%%  %s" % (100.0 * cnt / tot, name))
        self.report = "\n".join(lines) + "\n"


# --------------------------------------------------------------------------


def bmp_to_png(bmp, png):
    from PIL import Image
    with Image.open(bmp) as im:
        im.save(png)
    os.remove(bmp)


def fixtures():
    return sorted(p.name for p in FIXTURES.iterdir()) if FIXTURES.is_dir() else []


def run_scene(name, a):
    sc = SCENES[name]
    fix = FIXTURES / (a.fixture or sc["fixture"])
    if not fix.is_dir():
        raise SystemExit("Fixture fehlt: %s (siehe docs/TOOLS.md, Abschnitt Szenen)" % fix)
    if not PORT.is_file():
        raise SystemExit("out/port.exe fehlt (python tools/build.py)")
    out = OUTBASE / (name + ("-" + a.tag if a.tag else ""))
    out.mkdir(parents=True, exist_ok=True)
    for f in out.glob("*"):          # nur Dateien der letzten Szene, keine Ordner-Rekursion
        if f.is_file():
            f.unlink()
    tmp = Path(tempfile.mkdtemp(prefix="scene_%s_" % name))
    save = tmp / "savedata"
    shutil.copytree(fix, save)
    target = (a.target or sc["target"]) + a.extra
    presses = list(sc["presses"]) + ([a.press] if a.press else [])
    shot = out / "ziel.bmp"
    cmd = [str(PORT), "--renderer", a.renderer, "--savedata", str(save),
           "--frames-guest", str(target), "--frames", str(a.max_host or target * 4 + 3600),
           "--shot", str(shot)]
    if not a.realtime:
        cmd.append("--deterministic")
    # Szenen laufen ohne Mods, ausser --mods DIR (docs/MODDING.md).
    cmd += ["--mods", "--mods-dir", a.mods] if a.mods else ["--no-mods"]
    if presses:
        cmd += ["--press", ",".join(presses)]
    if a.every:
        cmd += ["--shot-every", str(a.every), "--shot-guest", "--shot-from", str(a.every_from)]
    elif a.every_host:
        cmd += ["--shot-every", str(a.every_host), "--shot-from", str(a.every_from)]
    env = dict(os.environ)
    if a.wav:
        env["HLE_AUDIO_WAV"] = str(out / "audio.wav")
    print("== %s: %s" % (name, sc["desc"]))
    print("   " + " ".join(cmd))
    t0 = time.time()
    with open(out / "port.log", "w", encoding="utf-8", errors="replace") as log:
        log.write(" ".join(cmd) + "\n")
        log.flush()
        p = subprocess.Popen(cmd, cwd=str(ROOT), stdout=log, stderr=subprocess.STDOUT, env=env)
        prof = None
        if a.profile:
            prof = Profiler(p, a.profile_after)
            prof.start()
        try:
            rc = p.wait(timeout=a.timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            rc = "Zeitlimit"
        if prof is not None:
            prof.stop.set()
            prof.join(60)
            (out / "profil.txt").write_text(prof.report, encoding="utf-8")
    dt = time.time() - t0
    if shot.exists():
        bmp_to_png(shot, out / "ziel.png")
    for b in sorted(out.glob("ziel_*.bmp")):
        bmp_to_png(b, out / ("seq_" + b.stem.split("_")[-1] + ".png"))
    dst = out / "spielstand"
    if dst.exists():
        shutil.rmtree(dst)            # nur eigener Ausgabeordner, keine Junctions darin
    shutil.copytree(save, dst)
    shutil.rmtree(tmp, ignore_errors=True)
    last = ""
    for line in (out / "port.log").read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("[port] frames="):
            last = line
    print("   rc=%s  %.1f s  %s" % (rc, dt, last[:60]))
    print("   Bild: %s" % (out / "ziel.png"))
    return rc == 0


def frame_hashes(name):
    """SHA-1 der Bildfolge (seq_*.png, Pixel ohne PNG-Metadaten) und des
    Endbilds einer Szene."""
    import hashlib
    from PIL import Image
    out = collections.OrderedDict()
    d = OUTBASE / name
    for f in sorted(d.glob("seq_*.png")) + [d / "ziel.png"]:
        if f.exists():
            with Image.open(f) as im:
                out[f.name] = hashlib.sha1(im.convert("RGB").tobytes()).hexdigest()
    return out


def check_repro(name, a):
    """Szene zweimal fahren und die Bildfolgen vergleichen (M18)."""
    if not a.every:
        a.every = 50
    runs = []
    for i in range(2):
        if not run_scene(name, a):
            print("   Lauf %d fehlgeschlagen" % (i + 1))
            return False
        runs.append(frame_hashes(name + ("-" + a.tag if a.tag else "")))
    keys = list(runs[0])
    same = [k for k in keys if runs[1].get(k) == runs[0][k]]
    diff = [k for k in keys if k not in same] + [k for k in runs[1] if k not in runs[0]]
    print("== %s: %d/%d Bilder identisch (alle %d Gast-Frames ab g%d, plus Endbild)" % (
        name, len(same), len(keys), a.every, a.every_from))
    for k in diff[:10]:
        print("   verschieden: %s" % k)
    return bool(keys) and not diff


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scene", nargs="*", help="Szene(n): " + ", ".join(SCENES))
    ap.add_argument("--list", action="store_true", help="Szenen und Fixtures auflisten")
    ap.add_argument("--renderer", choices=["hw", "soft"], default="hw")
    ap.add_argument("--tag", default="",
                    help="Ausgabe nach scratch/scene/<szene>-<tag>/ (z. B. soft/hw fuer tools/imgcmp.py)")
    ap.add_argument("--every", type=int, default=0, help="zusaetzlich alle N Gast-Frames ein Bild")
    ap.add_argument("--every-host", type=int, default=0,
                    help="Bildfolge alle N Host-Frames (Standbilder: Gast-Zaehler steht)")
    ap.add_argument("--every-from", type=int, default=0, help="Bildfolge erst ab Frame M (Gast bzw. Host)")
    ap.add_argument("--extra", type=int, default=0, help="Gast-Frames ueber das Ziel hinaus laufen")
    ap.add_argument("--target", type=int, default=0, help="Ziel-Gast-Frame statt Szenen-Standard")
    ap.add_argument("--press", default="", help="zusaetzliche --press-Ereignisse (z. B. touch@g5600:12:160:120)")
    ap.add_argument("--fixture", default="", help="anderer Fixture-Ordner (Name unter out/fixtures/saves)")
    ap.add_argument("--wav", action="store_true", help="Ton als audio.wav (HLE_AUDIO_WAV)")
    ap.add_argument("--profile", action="store_true", help="Sampling-Profiler -> profil.txt")
    ap.add_argument("--profile-after", type=float, default=0.0,
                    help="Profiler erst nach S Sekunden starten")
    ap.add_argument("--max-host", type=int, default=0, help="Notbremse in Host-Frames")
    ap.add_argument("--repro", action="store_true",
                    help="Szene zweimal fahren und die Bildfolgen (--every, Standard 50) "
                         "vergleichen; Exit 1 bei Abweichung")
    ap.add_argument("--mods", default="",
                    help="Mods aus diesem Ordner laden (Standard: --no-mods)")
    ap.add_argument("--realtime", action="store_true",
                    help="ohne --deterministic (Wanduhr wie im Spiel; Laeufe nicht reproduzierbar, "
                         "Szenen-Zeiten gelten dann nicht)")
    ap.add_argument("--timeout", type=float, default=1800.0, help="Sekunden bis Abbruch")
    a = ap.parse_args()
    if a.list or not a.scene:
        for k, v in SCENES.items():
            print("%-14s fixture=%-10s ziel=g%-5d %s" % (k, v["fixture"], v["target"], v["desc"]))
        print("Fixtures in %s: %s" % (FIXTURES, ", ".join(fixtures()) or "(keine)"))
        return 0
    bad = [s for s in a.scene if s not in SCENES]
    if bad:
        raise SystemExit("unbekannte Szene: %s" % ", ".join(bad))
    if a.repro:
        ok = all([check_repro(s, a) for s in a.scene])
    else:
        ok = all([run_scene(s, a) for s in a.scene])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
