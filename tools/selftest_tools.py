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
    args = ["guest-run", "--until", "srvname.*ndm:u",
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
             "Selbsttest: hid-Acquisition reproduzierbar",
             "--until", "srvname.*ndm:u", "--timeout", "15",
             "--repeat", "2", "--registry", str(reg)])
    need(p.returncode == 0, "guest-exp Exit %d\n%s" % (p.returncode, p.stderr[-1500:]))
    data = load_json(reg)
    need(len(data["experiments"]) == 1,
         "Registry-Eintraege: %d" % len(data["experiments"]))
    e = data["experiments"][0]
    need(e["deterministic"] is True, "nicht deterministisch")
    need(len(e["runs"]) == 2, "Runs: %d" % len(e["runs"]))
    q = run(["guest-exp", "check", "--until", "srvname.*ndm:u",
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
    for rel in ("AGENTS.md", "docs/ROADMAP.md", "docs/REFERENCE.md",
                "docs/WORKAROUNDS.md", "docs/TOOLS.md",
                ".opencode/skills/3ds-port/SKILL.md",
                ".opencode/skills/3ds-hle/SKILL.md"):
        need((ROOT / rel).is_file(), "%s fehlt" % rel)
    text = (ROOT / "docs" / "TOOLS.md").read_text(encoding="utf-8")
    for name in ("frontier", "trace-diff", "guest-refs", "guest-run",
                 "guest-watch", "callgraph", "workarounds"):
        need(name in text, "%s fehlt in docs/TOOLS.md" % name)
    ref = (ROOT / "docs" / "REFERENCE.md").read_text(encoding="utf-8")
    for needle in ("cmdbuf[0]", "InterruptRelayQueue", "IVFC", "0x22F"):
        need(needle in ref, "REFERENCE.md ohne %r" % needle)


def test_callgraph():
    out = OUT / "cg.json"
    p = run(["callgraph", "writers", "0x40B4C4", "--json-out", str(out)])
    need(p.returncode == 0, "callgraph writers: %s\n%s" % (p.stdout, p.stderr))
    data = load_json(out)
    sites = {e["site"] for e in data}
    need("0x00134850" in sites, "writer 0x00134850 fehlt: %s" % sorted(sites))
    p = run(["callgraph", "literals", "0x134818"])
    need("0x00128934" in p.stdout
         and ("fn_001287E0" in p.stdout or "fn_001286C0" in p.stdout),
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


def test_call_mode():
    """BLX wechselt den Instruktionssatz, BL behaelt ihn (Scan-Seeds)."""
    import capstone.arm as A
    from recomp import isa, scan
    cases = [
        ("000000eb", False, A.ARM_INS_BL, False),   # ARM  BL  -> ARM
        ("000000fa", False, A.ARM_INS_BLX, True),   # ARM  BLX -> Thumb
        ("00f000f8", True, A.ARM_INS_BL, True),     # Thumb BL -> Thumb
        ("00f040e8", True, A.ARM_INS_BLX, False),   # Thumb BLX -> ARM
    ]
    for raw, caller_thumb, ins_id, want_thumb in cases:
        ins = isa.decode(bytes.fromhex(raw), isa.ARM_BASE, thumb=caller_thumb)
        need(ins is not None and ins.id == ins_id,
             "%s nicht als erwartete Instruktion dekodiert" % raw)
        got = scan.call_target_thumb(ins, caller_thumb)
        need(got is want_thumb,
             "%s: Zielmodus %s statt %s" % (raw, got, want_thumb))


def test_ldrpc_veneer():
    """ARM-Veneer `ldr pc,[pc,#-4]` + Thumb-Ziel wird als Thumb-Funktion geseedet."""
    import struct
    from recomp import isa, scan
    base = isa.ARM_BASE
    data = bytearray(0x40)
    struct.pack_into("<I", data, 0x00, 0xE51FF004)   # ldr pc, [pc, #-4]
    struct.pack_into("<I", data, 0x04, base + 0x11)  # Thumb-Ziel 0x100010 | 1
    data[0x10:0x12] = b"\x70\x47"                    # bx lr
    res = scan.scan(bytes(data), [(base, "entry", False)])
    fn = res.functions.get(base)
    need(fn is not None, "Einstiegsfunktion fehlt")
    need((base + 0x10, True) in fn.calls,
         "Veneer-Ziel fehlt/Modus falsch: %s" % sorted(fn.calls))
    target = res.functions.get(base + 0x10)
    need(target is not None and target.thumb,
         "Thumb-Ziel nicht als Thumb-Funktion angelegt")


def test_veneer_thumb_seed():
    """Veneer (`add r12,pc,#1; bx r12`) und `ldr pc`-Stub seeden Thumb-Ziele."""
    import struct
    from recomp import isa, scan

    code = bytearray(0x40)
    code[0:8] = bytes.fromhex("01c08fe21cff2fe1")   # add r12,pc,#1; bx r12
    code[8:14] = bytes.fromhex("00b500207047")      # push {lr}; movs r0,#0; bx lr
    res = scan.scan(bytes(code), [(isa.ARM_BASE, "entry", False)])
    need(0x00100008 in res.functions,
         "Thumb-Ziel des Veneers nicht geseedet: %s"
         % sorted("%08X" % a for a in res.functions))
    need(res.functions[0x00100008].thumb,
         "Veneer-Ziel als ARM statt Thumb dekodiert")

    code = bytearray(0x60)
    code[0:4] = struct.pack("<I", 0xEA000006)       # b 0x100020
    code[0x20:0x24] = bytes.fromhex("04f01fe5")     # ldr pc,[pc,#-4]
    code[0x24:0x28] = struct.pack("<I", 0x00100039) # Literal 0x100039 (Thumb)
    code[0x38:0x3E] = bytes.fromhex("00b500207047")
    res = scan.scan(bytes(code), [(isa.ARM_BASE, "entry", False)])
    need(0x00100038 in res.functions and res.functions[0x00100038].thumb,
         "ldr-pc-Stub-Ziel nicht als Thumb-Funktion geseedet: %s"
         % sorted("%08X" % a for a in res.functions))


TESTS = [
    ("guest-refs 0x40B4C4", test_guest_refs),
    ("trace-diff boss:U", test_trace_diff),
    ("guest-run --until reproduzierbar", test_guest_run_repeat),
    ("guest-watch 0x40B4C4", test_guest_watch),
    ("guest-exp --repeat 2", test_guest_exp),
    ("klare Fehlermeldungen", test_failures),
    ("Doku + Skills vorhanden", test_docs_skill),
    ("callgraph writers/literals", test_callgraph),
    ("diff-trace Anker + Missing", test_diff_trace),
    ("milestones Fixture", test_milestones),
    ("framecheck Platzhalter", test_framecheck),
    ("PICA-Registermap", test_pica_regs),
    ("IMGC ETC1/linear", test_imgc_decoder),
    ("BLX-Zielmodus im Scan", test_call_mode),
    ("ldr-pc-Veneer-Ziel im Scan", test_ldrpc_veneer),
    ("Veneer-Seed Thumb", test_veneer_thumb_seed),
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
