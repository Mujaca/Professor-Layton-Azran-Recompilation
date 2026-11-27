"""Baut das Builder-Paket fuer Endnutzer (Arbeitspaket 6) als ZIP.

  python tools/make_builder.py                         # Artefakte aus scratch/builder-cache/
  python tools/make_builder.py --cache D --out O       # anderer Cache-/Zielordner
  python tools/make_builder.py --print-artefacts       # Liste der noetigen Downloads
  python tools/make_builder.py --standin               # NUR LOKALER TEST, siehe unten

Das Skript laedt nichts herunter. Die Artefakte in ARTEFACTS (eingebettetes
Python von python.org, Wheels von PyPI) muessen vorher im Cache-Ordner
liegen; ihre SHA-256 werden geprueft (Eintrag sha256 = None: noch nicht
festgelegt -> Abbruch, ausser --allow-unpinned, das die gemessenen Werte
zum Eintragen ausgibt).

Paketinhalt (ZIP-Wurzel LaytonBuilder-<version>/):
  LaytonBuilder.exe        Starter (tools/starter/starter.c, mit dem
                           gebuendelten zig gebaut)
  README.txt               Anleitung fuer Nutzer (englisch)
  LICENSES/                Lizenztexte aller gebuendelten Teile + THIRD_PARTY.txt
  BUILDINFO.txt            Commit, Artefakte mit SHA-256
  python/                  eingebettetes Python, Lib/site-packages mit
                           pyctr, pycryptodomex, capstone, ziglang;
                           pythonXY._pth: Suchpfad inkl. ..\\src\\tools
  src/                     LICENSE, tools/ (nur Baupfad), runtime/, third_party/

Reproduzierbar: feste Reihenfolge, Zeitstempel = Commitzeit von HEAD,
deflate Stufe 9; der Starter wird mit festen Flags gebaut (zig/lld schreibt
keinen Zeitstempel). Gleicher Commit + gleiche Artefakte = gleiches ZIP.

--standin (nur fuer Tests ohne Downloads): statt der Artefakte wird eine
lokale CPython-Installation (--standin-python, Standard: die laufende) in
das Layout der Embeddable-Distribution umgebaut und die installierten
Pakete aus deren site-packages (per RECORD) kopiert. Das Ergebnis heisst
*-STANDIN.zip und ist nicht zur Veroeffentlichung bestimmt.
"""
import argparse
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NAME = "LaytonBuilder"

# --- Artefakte (Downloads, vom Menschen freizugeben) --------------------------
# Wheels sind versionsunabhaengig (py3-none / cp37-abi3), passen also zu jeder
# Python-3-Version der Embeddable-Distribution.
ARTEFACTS = [
    {"kind": "python", "name": "python-3.10.11-embed-amd64.zip", "version": "3.10.11",
     "url": "https://www.python.org/ftp/python/3.10.11/python-3.10.11-embed-amd64.zip",
     "sha256": "608619f8619075629c9c69f361352a0da6ed7e62f83a0e19c63e0ea32eb7629d", "license": "PSF-2.0 (LICENSE.txt im Archiv)"},
    {"kind": "wheel", "name": "pyctr-0.7.6-py3-none-any.whl", "project": "pyctr",
     "version": "0.7.6",
     "url": "https://files.pythonhosted.org/packages/py3/p/pyctr/pyctr-0.7.6-py3-none-any.whl",
     "sha256": "60fe1dfa2af9fc518a2e4b38e39f42e48eae1f27c594e677836fd04726bb2323", "license": "MIT"},
    {"kind": "wheel", "name": "pycryptodomex-3.24.2-cp37-abi3-win_amd64.whl",
     "project": "pycryptodomex", "version": "3.24.2",
     "url": "https://files.pythonhosted.org/packages/cp37/p/pycryptodomex/"
            "pycryptodomex-3.24.2-cp37-abi3-win_amd64.whl",
     "sha256": "6e912e3a21061bf8f4ce89223cb46f5e98a6b32420d08d770605891a2be266c6", "license": "BSD-2-Clause / Public Domain"},
    {"kind": "wheel", "name": "capstone-5.0.9-py3-none-win_amd64.whl", "project": "capstone",
     "version": "5.0.9",
     "url": "https://files.pythonhosted.org/packages/py3/c/capstone/"
            "capstone-5.0.9-py3-none-win_amd64.whl",
     "sha256": "732cedbbb56d42e723f14d7af6387f1454194a820b4b96b56d1e53f865ef85d0", "license": "BSD-3-Clause"},
    {"kind": "wheel", "name": "ziglang-0.17.1-py3-none-win_amd64.whl", "project": "ziglang",
     "version": "0.17.1",
     "url": "https://files.pythonhosted.org/packages/py3/z/ziglang/"
            "ziglang-0.17.1-py3-none-win_amd64.whl",
     "sha256": "69c3dfc3aff6a297ec4017aa904782c0e450eb988982d7f675d83edce27526fb", "license": "MIT (+ Lizenzen der mitgelieferten libc/libc++)"},
]

# Quellen im Paket (git ls-files unter diesen Pfaden). Nur der Baupfad:
# build.py, extract*.py, romfs_image.py, Recompiler, Workaround-Tabellen.
SRC_PATHS = [
    "LICENSE",
    "tools/builder.py", "tools/build.py", "tools/extract.py", "tools/extract_romfs.py",
    "tools/romfs_image.py", "tools/nokeys.py",
    "tools/recomp/", "tools/workarounds/",
    "runtime/", "third_party/",
]
# Schutz gegen Spielinhalte im Paket (siehe .gitignore).
FORBIDDEN_EXT = {".3ds", ".cia", ".cci", ".cxi", ".cfa", ".bin", ".fa", ".bcstm",
                 ".bcsar", ".moflex", ".bmp", ".png", ".wav"}

STARTER_FLAGS = ["-target", "x86_64-windows-gnu", "-Os", "-s", "-std=c11", "-municode",
                 "-Wall", "-Wextra", "-Wno-unused-parameter"]
STARTER_LIBS = ["-lcomctl32", "-lcomdlg32", "-lshell32", "-lole32", "-luser32", "-lgdi32",
                "-Wl,--subsystem,windows"]

# Teile der Standardbibliothek, die die Embeddable-Distribution nicht enthaelt
# (nur fuer --standin).
STANDIN_LIB_SKIP = {"site-packages", "test", "idlelib", "tkinter", "turtledemo",
                    "ensurepip", "lib2to3", "venv", "__pycache__", "distutils", "pydoc_data"}
STANDIN_DLL_SKIP_PREFIX = ("_test", "_tkinter", "tcl", "tk", "py.ico", "pyc.ico", "pyd.ico")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git(*args):
    return subprocess.run(["git"] + list(args), cwd=ROOT, capture_output=True, text=True,
                          check=True).stdout.strip()


def print_artefacts():
    print("Benoetigte Artefakte (in den Cache-Ordner legen):")
    for a in ARTEFACTS:
        print("  %-48s %s\n      %s\n      sha256: %s" % (
            a["name"], a["license"], a["url"], a["sha256"] or "(noch festzulegen)"))
    print("Alternative fuer die Wheels:\n  pip download --no-deps --only-binary=:all: "
          "--platform win_amd64 --python-version 3.10 -d <cache> "
          + " ".join("%s==%s" % (a["project"], a["version"])
                     for a in ARTEFACTS if a["kind"] == "wheel"))


# --- Staging -----------------------------------------------------------------

class Stage:
    """Sammelt Dateien (Paketpfad -> Bytes oder Quelldatei)."""

    def __init__(self):
        self.files = {}

    def add_bytes(self, rel, data):
        self.files[rel.replace("\\", "/")] = ("b", data)

    def add_file(self, rel, src):
        self.files[rel.replace("\\", "/")] = ("f", src)

    def read(self, rel):
        kind, v = self.files[rel]
        if kind == "b":
            return v
        with open(v, "rb") as f:
            return f.read()


def stage_python_embed(st, zip_path):
    pth = None
    with zipfile.ZipFile(zip_path) as z:
        for info in sorted(z.infolist(), key=lambda i: i.filename):
            if info.is_dir():
                continue
            data = z.read(info)
            if info.filename.endswith("._pth"):
                pth = info.filename
            st.add_bytes("python/" + info.filename, data)
    if pth is None:
        raise SystemExit("FEHLER: keine ._pth-Datei in %s" % zip_path)
    return pth[:-len("._pth")]           # z. B. python310


def stage_python_standin(st, pydir):
    ver = "python%d%d" % sys.version_info[:2]
    if os.path.normcase(os.path.abspath(pydir)) != os.path.normcase(sys.base_prefix):
        raise SystemExit("FEHLER: --standin-python muss die laufende Installation sein "
                         "(%s)" % sys.base_prefix)
    for fn in ["python.exe", "pythonw.exe", "python3.dll", ver + ".dll", "vcruntime140.dll",
               "vcruntime140_1.dll", "LICENSE.txt"]:
        p = os.path.join(pydir, fn)
        if os.path.exists(p):
            st.add_file("python/" + fn, p)
    dlls = os.path.join(pydir, "DLLs")
    for fn in sorted(os.listdir(dlls)):
        if fn.endswith((".pyd", ".dll")) and not fn.startswith(STANDIN_DLL_SKIP_PREFIX):
            st.add_file("python/" + fn, os.path.join(dlls, fn))
    buf = io.BytesIO()
    lib = os.path.join(pydir, "Lib")
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for dp, dn, fns in os.walk(lib):
            dn[:] = sorted(d for d in dn if d not in STANDIN_LIB_SKIP)
            for fn in sorted(fns):
                if fn.endswith(".py"):
                    full = os.path.join(dp, fn)
                    rel = os.path.relpath(full, lib).replace("\\", "/")
                    zi = zipfile.ZipInfo(rel, (1980, 1, 1, 0, 0, 0))
                    zi.compress_type = zipfile.ZIP_DEFLATED
                    with open(full, "rb") as f:
                        z.writestr(zi, f.read())
    st.add_bytes("python/%s.zip" % ver, buf.getvalue())
    return ver


def stage_wheel(st, whl):
    with zipfile.ZipFile(whl) as z:
        for info in sorted(z.infolist(), key=lambda i: i.filename):
            if info.is_dir():
                continue
            name = info.filename
            parts = name.split("/")
            if parts[0].endswith(".data"):
                if len(parts) > 2 and parts[1] in ("purelib", "platlib"):
                    name = "/".join(parts[2:])
                else:
                    continue                # scripts/headers/data: nicht noetig
            st.add_bytes("python/Lib/site-packages/" + name, z.read(info))


def stage_installed(st, sp, project, version):
    """--standin: installiertes Paket per RECORD kopieren (wie aus dem Wheel)."""
    di = os.path.join(sp, "%s-%s.dist-info" % (project, version))
    if not os.path.isdir(di):
        raise SystemExit("FEHLER: %s %s nicht installiert (%s)" % (project, version, di))
    n = 0
    with open(os.path.join(di, "RECORD"), encoding="utf-8") as f:
        for line in f:
            rel = line.rsplit(",", 2)[0]
            if (not rel or rel.startswith("..") or "__pycache__" in rel or rel.endswith(".pyc")
                    or rel.endswith(("/INSTALLER", "/REQUESTED", "/direct_url.json"))):
                continue
            src = os.path.join(sp, rel)
            if os.path.isfile(src):
                st.add_file("python/Lib/site-packages/" + rel, src)
                n += 1
    return n


def head_blobs(files):
    """Inhalt aus HEAD (wie im Repo gespeichert, LF) fuer unveraenderte
    Dateien - unabhaengig von core.autocrlf des Arbeitsbaums."""
    changed = set(git("diff", "--name-only", "HEAD", "--").splitlines())
    tracked = [f for f in files if f not in changed]
    known = set(git("ls-tree", "-r", "--name-only", "HEAD", "--", *SRC_PATHS).splitlines())
    tracked = [f for f in tracked if f in known]
    if not tracked:
        return {}
    req = "".join("HEAD:%s\n" % f for f in tracked).encode("utf-8")
    out = subprocess.run(["git", "cat-file", "--batch"], cwd=ROOT, input=req,
                         capture_output=True, check=True).stdout
    blobs, pos = {}, 0
    for f in tracked:
        nl = out.index(b"\n", pos)
        size = int(out[pos:nl].split()[2])
        blobs[f] = out[nl + 1:nl + 1 + size]
        pos = nl + 1 + size + 1
    return blobs


def stage_sources(st):
    # versionierte + neue, nicht ignorierte Dateien (Test vor dem Commit);
    # unveraenderte aus HEAD, geaenderte/neue aus dem Arbeitsbaum
    files = git("ls-files", "--cached", "--others", "--exclude-standard", "--",
                *SRC_PATHS).splitlines()
    blobs = head_blobs(files)
    for rel in sorted(files):
        ext = os.path.splitext(rel)[1].lower()
        if ext in FORBIDDEN_EXT:
            raise SystemExit("FEHLER: %s darf nicht ins Paket (Spielinhalt?)" % rel)
        if rel in blobs:
            data = blobs[rel]
        else:
            with open(os.path.join(ROOT, rel), "rb") as f:
                data = f.read()
        if len(data) > 4 << 20:
            raise SystemExit("FEHLER: %s ist unerwartet gross" % rel)
        st.add_bytes("src/" + rel, data)
    return len(files)


def write_pth(st, ver):
    # Eingebettetes Python ignoriert PYTHONPATH; der Suchpfad steht hier.
    # ..\src\tools: recomp-Paket fuer "python -m recomp.cli" (tools/build.py).
    st.add_bytes("python/%s._pth" % ver,
                 ("%s.zip\r\n.\r\nLib\\site-packages\r\n..\\src\\tools\r\n" % ver).encode())


def stage_licenses(st, wheels, ver_info):
    lic = {}
    lic["LICENSES/LaytonPort-GPL-3.0.txt"] = st.read("src/LICENSE")
    if "python/LICENSE.txt" in st.files:
        lic["LICENSES/Python/LICENSE.txt"] = st.read("python/LICENSE.txt")
    lic["LICENSES/Lua/LICENSE"] = st.read("src/third_party/lua/LICENSE")
    for project, version in wheels:
        prefix = "python/Lib/site-packages/%s-%s.dist-info/" % (project, version)
        for rel in sorted(st.files):
            if not rel.startswith(prefix):
                continue
            tail = rel[len(prefix):]
            base = tail.split("/")[-1].upper()
            if tail.startswith("licenses/") or base.startswith(("LICENSE", "COPYING", "AUTHORS",
                                                                   "COPYRIGHT", "NOTICE")):
                lic["LICENSES/%s/%s" % (project, tail)] = st.read(rel)
    # zig: LICENSE liegt auch im Paketordner (gleicher Text wie dist-info)
    zl = "python/Lib/site-packages/ziglang/LICENSE"
    if zl in st.files:
        lic["LICENSES/ziglang/LICENSE"] = st.read(zl)
    for k, v in lic.items():
        st.add_bytes(k, v)
    txt = THIRD_PARTY % ver_info
    st.add_bytes("LICENSES/THIRD_PARTY.txt", txt.replace("\n", "\r\n").encode())


THIRD_PARTY = """Third-party components in this package
=======================================

This package contains the source code of the Layton port (GPL-3.0, see
LaytonPort-GPL-3.0.txt) and the following third-party software. Each
component keeps its own license; the full texts are in this folder.

* Python %(python)s (embeddable distribution) - folder python\\
  Python Software Foundation License; see Python\\LICENSE.txt, which also
  lists the licenses of the libraries included in the Windows build
  (OpenSSL, libffi, bzip2, xz, zlib, SQLite, the Microsoft C runtime ...).
  Source: https://www.python.org/

* pyctr %(pyctr)s - MIT License; see pyctr\\. https://github.com/ihaveamac/pyctr
* pycryptodomex %(pycryptodomex)s - BSD-2-Clause / Public Domain; see
  pycryptodomex\\. https://www.pycryptodome.org
* capstone %(capstone)s (Python binding + capstone.dll) - BSD-3-Clause; see
  capstone\\. https://www.capstone-engine.org
* ziglang %(ziglang)s (Zig compiler, used as C compiler) - MIT License; see
  ziglang\\LICENSE. Zig ships libc/libc++ sources and headers with their own
  licenses (mingw-w64, musl, glibc, wasi-libc, LLVM libc++/libc++abi/
  libunwind ...), see ziglang\\licenses\\. https://ziglang.org
  LaytonBuilder.exe itself is compiled with this Zig and links parts of the
  mingw-w64 runtime (see ziglang\\licenses\\ziglang\\lib\\libc\\mingw\\COPYING).
* Lua 5.4.7 (compiled into port.exe) - MIT License; see Lua\\LICENSE.
  https://www.lua.org

No game content (code, graphics, sound, text) and no keys are included.
The game is built on your computer from your own dump.
"""

README = """Professor Layton and the Azran Legacy - native port: builder
=============================================================

This package builds the native Windows port of "Professor Layton and the
Azran Legacy" from YOUR OWN dump of the game. It contains no game content.
Everything needed to build is included (Python, the Zig C compiler, the
recompiler and runtime sources); nothing is installed and nothing is
downloaded.

What you need
-------------
* Windows 10/11 (64-bit) with a Direct3D 11 graphics card
* about 3 GB of free disk space
* a DECRYPTED dump of the European cartridge (product code CTR-P-AL6P),
  as a .3ds file, e.g. made with GodMode9 on your own console
  ("Dump cart", then "Decrypt file" on the .3ds).

How to build
------------
1. Extract this ZIP to a folder you can write to (not "Program Files").
2. Start LaytonBuilder.exe and click "Choose dump..." (or drop the .3ds
   onto the window or onto LaytonBuilder.exe).
3. The builder checks the dump (European release, decrypted, intact) and
   then builds the game. The first build takes several minutes.
4. Click "Play", or use "Play Professor Layton" in the "game" folder
   (or start game\\out\\port.exe directly).

Your dump is only read; it is never copied or changed. The "game" folder
contains the derived files (port.exe, extracted game data); do not share
it. Save games are kept in game\\out\\savedata, settings in
game\\out\\port.ini, logs in game\\out\\logs. Building again keeps them.

Command line: LaytonBuilder.exe <dump.3ds> [--out <folder>] [--headless]
(--headless builds without a window; the log is in <folder>\\builder.log.)

Licenses
--------
The port is free software under the GNU GPL v3.0; its source code is in
src\\. Third-party components and their licenses: LICENSES\\THIRD_PARTY.txt.
This project is not affiliated with or endorsed by Nintendo or Level-5.
"""


def build_starter(st, workdir):
    """Starter mit dem gebuendelten zig bauen (aus dem Staging entpackt)."""
    zdir = os.path.join(workdir, "zig")
    prefix = "python/Lib/site-packages/ziglang/"
    n = 0
    for rel, (kind, v) in st.files.items():
        if rel.startswith(prefix):
            dst = os.path.join(zdir, rel[len(prefix):].replace("/", os.sep))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if kind == "f":
                shutil.copyfile(v, dst)
            else:
                with open(dst, "wb") as f:
                    f.write(v)
            n += 1
    if n == 0:
        raise SystemExit("FEHLER: ziglang fehlt im Staging")
    src = os.path.join(ROOT, "tools", "starter")
    out = os.path.join(workdir, NAME + ".exe")
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=os.path.join(workdir, "zc"),
               ZIG_LOCAL_CACHE_DIR=os.path.join(workdir, "zc"))
    p = subprocess.run([os.path.join(zdir, "zig.exe"), "cc"] + STARTER_FLAGS +
                       ["starter.c", "starter.rc", "-o", out] + STARTER_LIBS,
                       cwd=src, env=env, capture_output=True, text=True)
    if p.returncode != 0 or not os.path.exists(out):
        sys.stdout.write(p.stdout + p.stderr)
        raise SystemExit("FEHLER: Starter nicht gebaut")
    with open(out, "rb") as f:
        st.add_bytes(NAME + ".exe", f.read())


def write_zip(st, path, top, epoch):
    t = time.gmtime(max(epoch, 315532800))
    stamp = (t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec - t.tm_sec % 2)
    tmp = path + ".tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for rel in sorted(st.files):
            zi = zipfile.ZipInfo(top + "/" + rel, stamp)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            zi.create_system = 0
            z.writestr(zi, st.read(rel), compresslevel=9)
    os.replace(tmp, path)


def fetch_artefacts(cache):
    """Nur auf ausdruecklichen Wunsch (--fetch): fehlende Artefakte laden.
    Die Pruefung gegen die festgelegten SHA-256 bleibt beim Paketbau; eine
    veraenderte Datei bricht dort ab."""
    import urllib.request
    os.makedirs(cache, exist_ok=True)
    for art in ARTEFACTS:
        p = os.path.join(cache, art["name"])
        if os.path.isfile(p):
            continue
        print("lade %s" % art["url"])
        tmp = p + ".part"
        with urllib.request.urlopen(art["url"], timeout=300) as r, open(tmp, "wb") as f:
            shutil.copyfileobj(r, f, 1 << 20)
        os.replace(tmp, p)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cache", default=os.path.join(ROOT, "scratch", "builder-cache"))
    ap.add_argument("--out", default=os.path.join(ROOT, "scratch", "builder-dist"))
    ap.add_argument("--print-artefacts", action="store_true")
    ap.add_argument("--allow-unpinned", action="store_true",
                    help="Artefakte ohne festgelegten SHA-256 zulassen (Hashes werden ausgegeben)")
    ap.add_argument("--standin", action="store_true",
                    help="lokale Python-Installation statt Artefakten (nur Test)")
    ap.add_argument("--standin-python", default=sys.base_prefix)
    ap.add_argument("--fetch", action="store_true",
                    help="fehlende Artefakte von ihrer URL in --cache laden (danach wie "
                         "immer per SHA-256 geprueft; fuer CI und einen leeren Cache)")
    a = ap.parse_args()
    if a.print_artefacts:
        print_artefacts()
        return 0
    if a.fetch:
        fetch_artefacts(a.cache)

    t0 = time.time()
    st = Stage()
    commit = git("rev-parse", "HEAD")
    dirty = bool(git("status", "--porcelain", "--", *SRC_PATHS,
                     "tools/starter/", "tools/make_builder.py"))
    epoch = int(git("log", "-1", "--format=%ct"))
    version = git("rev-parse", "--short=10", "HEAD") + ("-dirty" if dirty else "")
    if dirty:
        print("WARNUNG: Arbeitsbaum hat Aenderungen im Paketinhalt - nicht reproduzierbar")
    versions = {"python": "?"}
    used = []
    wheels = [(x["project"], x["version"]) for x in ARTEFACTS if x["kind"] == "wheel"]

    if a.standin:
        print("STAND-IN: Python aus %s, Pakete aus dessen site-packages" % a.standin_python)
        ver = stage_python_standin(st, a.standin_python)
        versions["python"] = "%d.%d.%d (stand-in)" % sys.version_info[:3]
        sp = os.path.join(a.standin_python, "Lib", "site-packages")
        for project, version_ in wheels:
            n = stage_installed(st, sp, project, version_)
            used.append(("%s %s (installiert, stand-in)" % (project, version_), "-", n))
    else:
        missing, bad = [], []
        for art in ARTEFACTS:
            p = os.path.join(a.cache, art["name"])
            if not os.path.isfile(p):
                missing.append(art["name"])
                continue
            h = sha256_file(p)
            if art["sha256"] is None:
                print("UNGEPINNT %s sha256=%s" % (art["name"], h))
                if not a.allow_unpinned:
                    bad.append(art["name"] + " (sha256 nicht festgelegt)")
            elif h != art["sha256"]:
                bad.append("%s (sha256 %s, erwartet %s)" % (art["name"], h, art["sha256"]))
        if missing or bad:
            for m in missing:
                print("FEHLT:", m)
            for b in bad:
                print("FALSCH:", b)
            print()
            print_artefacts()
            return 1
        for art in ARTEFACTS:
            p = os.path.join(a.cache, art["name"])
            if art["kind"] == "python":
                ver = stage_python_embed(st, p)
                versions["python"] = art["version"]
            else:
                stage_wheel(st, p)
            used.append((art["name"], sha256_file(p), 0))
    for project, version_ in wheels:
        versions[project] = version_
    write_pth(st, ver)
    nsrc = stage_sources(st)
    st.add_bytes("README.txt", README.replace("\n", "\r\n").encode())
    stage_licenses(st, wheels, versions)
    with tempfile.TemporaryDirectory(prefix="mkbuilder-") as wd:
        build_starter(st, wd)
    info = ["LaytonBuilder package", "commit: %s%s" % (commit, " (dirty)" if dirty else ""),
            "stand-in (NOT FOR RELEASE): %s" % ("yes" if a.standin else "no"),
            "python: %s" % versions["python"], "artefacts:"]
    for name, h, _ in used:
        info.append("  %s  %s" % (h, name))
    st.add_bytes("BUILDINFO.txt", ("\r\n".join(info) + "\r\n").encode())

    os.makedirs(a.out, exist_ok=True)
    top = "%s-%s" % (NAME, version)
    zname = top + ("-STANDIN" if a.standin else "") + ".zip"
    zpath = os.path.join(a.out, zname)
    write_zip(st, zpath, top, epoch)
    size = os.path.getsize(zpath)
    print("Quellen: %d Dateien, Paket: %d Dateien" % (nsrc, len(st.files)))
    print("geschrieben: %s (%.1f MiB, sha256 %s) in %.0f s" % (
        zpath, size / 2**20, sha256_file(zpath), time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
