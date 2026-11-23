"""Selbsttest fuer die g3ds-Werkzeuge gegen verifizierte Repo-Fakten.

  python tools/selftest_tools.py
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))
OUT = ROOT / "tools" / "out" / "selftest_tools"


def run(args, timeout=180):
    return subprocess.run(
        [sys.executable, str(ROOT / "tools" / "g3ds.py")] + list(args),
        cwd=str(ROOT), capture_output=True, text=True, encoding="utf-8",
        errors="replace", timeout=timeout)


def need(condition, message):
    if not condition:
        raise AssertionError(message)


def load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def test_guest_refs():
    out = OUT / "refs.json"
    p = run(["guest-refs", "0x0040B4C4", "--json-out", str(out)])
    need(p.returncode == 0, "guest-refs Exit %d\n%s\n%s"
         % (p.returncode, p.stdout[-1500:], p.stderr[-1500:]))
    data = load_json(out)
    addrs = {e["addr"] for e in data["events"]}
    need("0x00134850" in addrs, "STR 0x00134850 fehlt: %s" % sorted(addrs))
    need("0x0028F284" in addrs, "LDR 0x0028F284 fehlt: %s" % sorted(addrs))
    kinds = {e["kind"] for e in data["events"] if e["addr"] in
             ("0x00134850", "0x0028F284")}
    need(kinds == {"store", "load"}, "Arten falsch: %s" % kinds)
    need("0x00134850" in p.stdout and "0x0028F284" in p.stdout,
         "Adressen fehlen in der Standardausgabe")


def test_trace_diff():
    out = OUT / "diff.json"
    fixture = ROOT / "tools" / "fixtures" / "port_boot_20261119.log"
    need(fixture.is_file(), "Port-Fixture fehlt: %s" % fixture)
    p = run(["trace-diff", "reports/azahar_trace.txt", str(fixture),
             "--json-out", str(out)])
    need(p.returncode == 0, "trace-diff Exit %d\n%s" % (p.returncode, p.stderr))
    data = load_json(out)
    need(data["mode"] == "missing-service", "Modus: %s" % data["mode"])
    need(data["first"] == "boss:U", "erste Divergenz: %s" % data["first"])
    need("boss:U" in p.stdout, "boss:U fehlt in der Ausgabe")


def test_guest_run_repeat():
    a = OUT / "run_a.json"
    b = OUT / "run_b.json"
    args = ["guest-run", "--until", "srvname.*gsp::Gpu",
            "--timeout", "15", "--json-out"]
    pa = run(args + [str(a)])
    pb = run(args + [str(b)])
    need(pa.returncode == 0, "guest-run A: %s" % pa.stderr[-1000:])
    need(pb.returncode == 0, "guest-run B: %s" % pb.stderr[-1000:])
    da = load_json(a)[0]
    db = load_json(b)[0]
    ha = sum(da["until_hits"].values())
    hb = sum(db["until_hits"].values())
    need(ha == 1 and hb == 1, "Until-Treffer nicht reproduzierbar: %d/%d" % (ha, hb))
    need(da["status"] == "until" and db["status"] == "until",
         "Status: %s/%s" % (da["status"], db["status"]))
    lines = [l for l in pa.stdout.splitlines() if l.strip()]
    need(len(lines) <= 25, "guest-run Ausgabe zu lang: %d Zeilen" % len(lines))


def test_guest_watch():
    out = OUT / "watch.json"
    p = run(["guest-watch", "--write", "0x40B4C4",
             "--until", r"\[mw\] 0040B4C4", "--timeout", "10",
             "--json-out", str(out)])
    need(p.returncode == 0, "guest-watch Exit %d\n%s\n%s"
         % (p.returncode, p.stdout[-1500:], p.stderr[-1500:]))
    data = load_json(out)
    need(data["ok"] is True, "Watch nicht ok")
    writes = [e for e in data["events"] if e["kind"] == "write"]
    need(writes, "keine Schreibereignisse")
    need(writes[0]["addr"] == "0x0040B4C4", "Adresse: %s" % writes[0]["addr"])
    need(writes[0]["thread"] == "2",
         "Thread nicht erkannt: %s" % writes[0]["thread"])


def test_guest_exp():
    reg = OUT / "exp.json"
    if reg.exists():
        reg.unlink()
    p = run(["guest-exp", "record", "--hypothesis",
             "Selbsttest: GSP-Acquisition reproduzierbar",
             "--until", "srvname.*gsp::Gpu", "--timeout", "15",
             "--repeat", "2", "--registry", str(reg)])
    need(p.returncode == 0, "guest-exp Exit %d\n%s" % (p.returncode, p.stderr[-1500:]))
    data = load_json(reg)
    need(len(data["experiments"]) == 1,
         "Registry-Eintraege: %d" % len(data["experiments"]))
    e = data["experiments"][0]
    need(e["deterministic"] is True, "nicht deterministisch")
    need(len(e["runs"]) == 2, "Runs: %d" % len(e["runs"]))
    q = run(["guest-exp", "check", "--until", "srvname.*gsp::Gpu",
             "--timeout", "15", "--registry", str(reg)])
    need(q.returncode == 0, "check fand den Lauf nicht: %s" % q.stdout)


def test_failures():
    p = run(["guest-refs", "0x0040B4C4", "--bin", "tools/out/does_not_exist.bin"])
    need(p.returncode == 2, "Exit bei fehlender Datei: %d" % p.returncode)
    need("FEHLER" in (p.stderr + p.stdout), "keine klare Fehlermeldung")
    p = run(["guest-run", "--exe", "out/does_not_exist.exe", "--timeout", "1"])
    need(p.returncode == 2, "Exit bei fehlender exe: %d" % p.returncode)
    need("FEHLER" in (p.stderr + p.stdout), "keine klare exe-Fehlermeldung")


def test_docs_skill():
    docs = ROOT / "docs" / "TOOLS.md"
    skill = ROOT / ".opencode" / "skills" / "3ds-hle" / "SKILL.md"
    new_skill = ROOT / ".opencode" / "skills" / "3ds-port" / "SKILL.md"
    need(docs.is_file(), "docs/TOOLS.md fehlt")
    need(skill.is_file(), ".opencode/skills/3ds-hle/SKILL.md fehlt")
    need(new_skill.is_file(), ".opencode/skills/3ds-port/SKILL.md fehlt")
    need((ROOT / "docs" / "SKILL.md").is_file(), "docs/SKILL.md fehlt")
    text = docs.read_text(encoding="utf-8")
    for name in ("trace-diff", "guest-refs", "guest-run", "guest-watch",
                 "guest-exp", "where", "callgraph", "diff_trace",
                 "milestones", "matrix", "perfmap", "hotpatch", "framecheck"):
        need(name in text, "%s fehlt in docs/TOOLS.md" % name)
    st = skill.read_text(encoding="utf-8")
    for needle in ("cmdbuf[0]", "0x10", "0x18", "0x20", "ReceiveParameter",
                   "InterruptRelayQueue", "ndspInit", "aptMainLoop"):
        need(needle in st, "Skill ohne %r" % needle)


def test_callgraph():
    out = OUT / "cg.json"
    p = run(["callgraph", "writers", "0x40B4C4", "--json-out", str(out)])
    need(p.returncode == 0, "callgraph writers: %s\n%s" % (p.stdout, p.stderr))
    data = load_json(out)
    sites = {e["site"] for e in data}
    need("0x00134850" in sites, "writer 0x00134850 fehlt: %s" % sorted(sites))
    p = run(["callgraph", "literals", "0x134818"])
    need("0x00128934" in p.stdout and "fn_001287E0" in p.stdout,
         "Tick-Registrierungs-Literal fehlt: %s" % p.stdout[:500])


def test_diff_trace():
    out = OUT / "dt.json"
    p = run(["diff-trace", "--ref", "reports/azahar_trace.txt", "--port",
             "tools/fixtures/port_boot_20261119.log", "--kind", "all",
             "--json-out", str(out)])
    need(p.returncode == 1, "diff-trace Exit %d\n%s" % (p.returncode, p.stdout))
    data = load_json(out)
    need(data["anchor"] and "SetInternalPriorities" in data["anchor"],
         "Anker nicht erkannt: %s" % data.get("anchor"))
    need(data["missing"], "keine fehlenden Referenzereignisse")


def test_milestones():
    out = OUT / "ms.json"
    p = run(["milestones", "--log", "tools/fixtures/port_boot_20261119.log",
             "--json-out", str(out)])
    need(p.returncode == 1, "milestones Exit %d" % p.returncode)
    data = load_json(out)
    got = {c["id"]: c["ok"] for c in data["checks"]}
    need(got.get("apt") and got.get("fs") and got.get("dsp")
         and got.get("gsp_init"), "fruehe Meilensteine fehlen: %s" % got)
    need(not got.get("boss") and not got.get("frame"),
         "spaete Meilensteine falsch: %s" % got)


def test_framecheck():
    import struct
    OUT.mkdir(parents=True, exist_ok=True)
    shot = OUT / "placeholder.bmp"
    w, h = 400, 240
    row = w * 3
    data_size = row * h
    hdr = bytearray(54)
    hdr[0:2] = b"BM"
    struct.pack_into("<I", hdr, 2, 54 + data_size)
    struct.pack_into("<I", hdr, 10, 54)
    struct.pack_into("<I", hdr, 14, 40)
    struct.pack_into("<i", hdr, 18, w)
    struct.pack_into("<i", hdr, 22, h)
    struct.pack_into("<H", hdr, 26, 1)
    struct.pack_into("<H", hdr, 28, 24)
    struct.pack_into("<I", hdr, 34, data_size)
    shot.write_bytes(bytes(hdr) + b"\x00" * data_size)
    out = OUT / "frame.json"
    p = run(["framecheck", "--shot", str(shot), "--json-out", str(out)])
    need(p.returncode == 0, "framecheck Exit %d\n%s" % (p.returncode, p.stdout))
    data = load_json(out)
    need(data["placeholder"] is True, "Platzhalter nicht erkannt")
    # 288054 B = 400x240 (ein Screen), 576054 B = 400x480 (beide Screens).
    need(data["shot_bytes"] in (288054, 576054),
         "Screenshotgroesse: %s" % data["shot_bytes"])


def test_hotpatch_list():
    p = run(["hotpatch", "list"])
    need(p.returncode == 0, "hotpatch list Exit %d" % p.returncode)
    for name in ("count_fields", "regs", "mem", "log", "set_r0"):
        need(name in p.stdout, "Callback %s fehlt" % name)


def test_canonical_shot():
    """Shot-Hash des kanonischen Laufs: zwei Laeufe muessen byte-identisch
    sein (Screenshot wartet auf stabile Engine-Submits), und die
    Referenzmessung darf nicht schlechter werden."""
    import hashlib
    exe = ROOT / "out" / "port.exe"
    need(exe.is_file(), "out/port.exe fehlt (python tools\\build_port.py)")
    hashes = []
    tries = 0
    while tries < 6:
        tries += 1
        shot = OUT / "canon_try.bmp"
        p = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "run_chain_direct.py"),
             "--profile", str(ROOT / "tools" / "profiles" /
                             "canonical_shot.json"),
             "--shot", str(shot)],
            cwd=str(ROOT), capture_output=True, text=True, timeout=300)
        need(p.returncode == 0, "run_chain_direct Exit %d\n%s"
             % (p.returncode, p.stdout[-800:]))
        need(shot.is_file(), "Shot fehlt: %s" % shot)
        hashes.append(hashlib.sha256(shot.read_bytes()).hexdigest())
        if len(hashes) >= 2 and hashes[-1] == hashes[-2]:
            break
    need(len(hashes) >= 2 and hashes[-1] == hashes[-2],
         "Shot nicht deterministisch: %s" % hashes)
    log = (ROOT / "out" / "logs" / "chain_direct.txt").read_text(
        encoding="utf-8", errors="replace")
    need("[shot] frames=" in log and "lists=" in log,
         "kein [shot]-Beleg im Log")
    m = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "measure_ref.py"),
         str(OUT / "canon_try.bmp")],
        cwd=str(ROOT), capture_output=True, text=True, timeout=120)
    need(m.returncode == 0, "measure_ref Exit %d" % m.returncode)
    bottom = None
    for line in m.stdout.splitlines():
        if line.startswith("BOTTOM sichtbar"):
            bottom = int(line.split()[4].split("/")[0])
    need(bottom is not None, "BOTTOM-Zeile fehlt:\n%s" % m.stdout)
    need(bottom <= 16000, "BOTTOM diff zu hoch: %d (linear 14752)" % bottom)
    print("     shot-sha256 %s  BOTTOM %d/71360" % (hashes[0][:16], bottom))


def test_classa_record_block():
    """Record-Kette 0xD2-Loader -> Records -> Klasse-A-Block (sprites=3)."""
    logp = ROOT / "out" / "logs" / "chain_direct.txt"

    def run_profile(name):
        p = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "run_chain_direct.py"),
             "--profile", str(ROOT / "tools" / "profiles" / name),
             "--shot", str(OUT / (name + ".bmp"))],
            cwd=str(ROOT), capture_output=True, text=True, timeout=300)
        need(p.returncode == 0, "run_chain_direct %s Exit %d"
             % (name, p.returncode))
        return logp.read_text(encoding="utf-8", errors="replace")

    log = run_profile("classa_drive.json")
    need("284A48(" in log and "0E905000,3" in log,
         "Loader-Alloc-Kette (284A48 0E905000,3) fehlt")
    need("[classa] eye0" in log and "hdr=" in log,
         "Klasse-A-Block fehlt")
    log = run_profile("record_quads.json")
    need("sprites=3 " in log, "Record-Liste sprites=3 fehlt")
    print("     sprites=3, Klasse-A-Block vorhanden, 284A48 0E905000")


def test_pica_regs():
    p = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "test_pica_c.py")],
        cwd=str(ROOT), capture_output=True, text=True, timeout=300)
    need(p.returncode == 0, "test_pica_c Exit %d\n%s"
         % (p.returncode, p.stdout[-1200:]))
    need("ERGEBNIS: OK" in p.stdout, "PICA-Checks fehlgeschlagen")


def test_imgc_decoder():
    p = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "test_imgc_c.py")],
        cwd=str(ROOT), capture_output=True, text=True, timeout=300)
    need(p.returncode == 0, "test_imgc_c Exit %d\n%s"
         % (p.returncode, p.stdout[-1500:]))
    need("ERGEBNIS: OK" in p.stdout, "IMGC-Checks fehlgeschlagen")


TESTS = [
    ("guest-refs 0x40B4C4", test_guest_refs),
    ("trace-diff boss:U", test_trace_diff),
    ("guest-run --until reproduzierbar", test_guest_run_repeat),
    ("guest-watch 0x40B4C4", test_guest_watch),
    ("guest-exp --repeat 2", test_guest_exp),
    ("klare Fehlermeldungen", test_failures),
    ("docs/TOOLS.md + Skills", test_docs_skill),
    ("callgraph writers/literals", test_callgraph),
    ("diff-trace Anker + Missing", test_diff_trace),
    ("milestones Fixture", test_milestones),
    ("framecheck Platzhalter", test_framecheck),
    ("hotpatch list", test_hotpatch_list),
    ("kanonischer Shot deterministisch", test_canonical_shot),
    ("Record->Block sprites=3", test_classa_record_block),
    ("PICA-Registermap", test_pica_regs),
    ("IMGC ETC1/linear", test_imgc_decoder),
]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    failed = 0
    t0 = time.time()
    for name, fn in TESTS:
        start = time.time()
        try:
            fn()
            print("[OK]   %-40s %5.1fs" % (name, time.time() - start))
        except Exception as exc:
            failed += 1
            print("[FAIL] %-40s %5.1fs  %s"
                  % (name, time.time() - start, exc))
    total = len(TESTS)
    print("\n%d/%d Tests bestanden (%.1fs)"
          % (total - failed, total, time.time() - t0))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
