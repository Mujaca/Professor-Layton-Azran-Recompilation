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
    need(docs.is_file(), "docs/TOOLS.md fehlt")
    need(skill.is_file(), ".opencode/skills/3ds-hle/SKILL.md fehlt")
    text = docs.read_text(encoding="utf-8")
    for name in ("trace-diff", "guest-refs", "guest-run", "guest-watch", "guest-exp"):
        need(name in text, "%s fehlt in docs/TOOLS.md" % name)
    st = skill.read_text(encoding="utf-8")
    for needle in ("cmdbuf[0]", "0x10", "0x18", "0x20", "ReceiveParameter",
                   "InterruptRelayQueue", "ndspInit", "aptMainLoop"):
        need(needle in st, "Skill ohne %r" % needle)


TESTS = [
    ("guest-refs 0x40B4C4", test_guest_refs),
    ("trace-diff boss:U", test_trace_diff),
    ("guest-run --until reproduzierbar", test_guest_run_repeat),
    ("guest-watch 0x40B4C4", test_guest_watch),
    ("guest-exp --repeat 2", test_guest_exp),
    ("klare Fehlermeldungen", test_failures),
    ("docs/TOOLS.md + Skill 3ds-hle", test_docs_skill),
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
