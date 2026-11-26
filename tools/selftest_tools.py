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
    # Eigenes Referenzbild: das Azahar-Bild (reports/azahar_topscreen.png)
    # ist Spielinhalt und liegt nur lokal, nicht im Repo.
    import zlib
    raw = b"".join(b"\x00" + bytes([40, 80, 120]) * w for _ in range(h))
    def chunk(t, d):
        return (struct.pack(">I", len(d)) + t + d
                + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF))
    ref = OUT / "ref.png"
    ref.write_bytes(b"\x89PNG\r\n\x1a\n"
                    + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                    + chunk(b"IDAT", zlib.compress(raw))
                    + chunk(b"IEND", b""))
    out = OUT / "frame.json"
    p = run(["framecheck", "--shot", str(shot), "--ref", str(ref),
             "--json-out", str(out)])
    need(p.returncode == 0, "framecheck Exit %d\n%s" % (p.returncode, p.stdout))
    data = load_json(out)
    need(data["placeholder"] is True, "Platzhalter nicht erkannt")
    # 288054 B = 400x240 (ein Screen), 576054 B = 400x480 (beide Screens).
    need(data["shot_bytes"] in (288054, 576054),
         "Screenshotgroesse: %s" % data["shot_bytes"])


def test_rendercmp():
    """tools/rendercmp.py auf synthetischen Bildern: Rauschen von 1 LSB
    zaehlt nicht als Befund, ein 6x6-Block mit +100 ist ein Cluster mit
    richtiger Groesse/Lage im richtigen Screen."""
    import numpy as np
    from PIL import Image
    import rendercmp
    d = OUT / "rendercmp"
    for sub in ("a", "b", "cmp"):
        (d / sub).mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(1)
    a = rng.integers(0, 150, (480, 400, 3), dtype=np.uint8)
    b = a.copy()
    b[:240:2, ::3, 0] += 1                     # Rauschen oben, 1 LSB
    b[300:306, 100:106] += 100                 # Cluster unten (x 60..65)
    Image.fromarray(a).save(d / "a" / "seq_000050.png")
    Image.fromarray(b).save(d / "b" / "seq_000050.png")
    Image.fromarray(a).save(d / "a" / "ziel.png")
    Image.fromarray(a).save(d / "b" / "ziel.png")
    rc = rendercmp.main([str(d / "a"), str(d / "b"), "--out", str(d / "cmp"),
                         "--fail-cluster", "30"])
    need(rc == 1, "Cluster >= 30 nicht gemeldet (rc %s)" % rc)
    res = load_json(d / "cmp" / "cmp.json")
    top, bot = res["seq_000050"]["top"], res["seq_000050"]["bottom"]
    need(top["max"] == 1 and top["p8"] == 0.0 and not top["clusters"],
         "Rauschen falsch bewertet: %s" % top)
    need(abs(top["mad"][0] - 120 * 134 / 96000.0) < 0.01, "MAD oben: %s" % top["mad"])
    cl = bot["clusters"]
    need(len(cl) == 1 and cl[0]["pixels"] == 36 and
         (cl[0]["x0"], cl[0]["y0"], cl[0]["x1"], cl[0]["y1"]) == (60, 60, 65, 65),
         "Cluster unten: %s" % cl)
    need(bot["max"] == 100 and abs(bot["p32"] - 100.0 * 36 / 76800) < 0.001,
         "Kennzahlen unten: %s" % bot)
    need(res["ziel"]["top"]["max"] == 0, "gleiche Bilder nicht gleich")
    need((d / "cmp" / "sheet_seq_000050.png").is_file(), "Kontaktbogen fehlt")
    need(not (d / "cmp" / "sheet_ziel.png").exists(), "Kontaktbogen ohne Befund")


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


def test_pic_thumb_callback_seed():
    """`ldr r1,[pc,#4]; add r1, pc` bildet einen Thumb-Funktionszeiger
    (Callback, wird nur gespeichert) -> Ziel wird als Funktion geseedet."""
    import struct
    from recomp import isa, scan
    base = isa.ARM_BASE
    code = bytearray(0x20)
    code[0x00:0x02] = struct.pack("<H", 0x4901)   # ldr r1, [pc, #4]  (Literal @0x08)
    code[0x02:0x04] = struct.pack("<H", 0x4479)   # add r1, pc        (pc = 0x02+4)
    code[0x04:0x06] = struct.pack("<H", 0x6001)   # str r1, [r0]
    code[0x06:0x08] = struct.pack("<H", 0x4770)   # bx lr
    code[0x08:0x0C] = struct.pack("<I", 0x11 - 6) # Abstand -> base + 0x11 (Thumb)
    code[0x10:0x16] = bytes.fromhex("00b500207047")
    res = scan.scan(bytes(code), [(base, "entry", True)])
    need((base + 0x10) in res.functions and res.functions[base + 0x10].thumb,
         "PIC-Callback nicht als Thumb-Funktion geseedet: %s"
         % sorted("%08X" % a for a in res.functions))


def test_offset_table_seed():
    """`add pc,rX,rB` ueber eine Offset-Tabelle seedet die Handler.

    Der Bilddecoder im Spiel springt so auf Format-Handler, die VOR dem
    Funktionsanfang liegen (0x16A6F8 ...). Ohne Seed landete der Sprung im
    No-op und SP wurde unbalanciert.
    """
    import struct
    from recomp import isa, scan

    code = bytearray(0x60)
    code[0x00:0x04] = bytes.fromhex("10608fe2")   # add r6, pc, #0x10
    code[0x04:0x08] = bytes.fromhex("055196e7")   # ldr r5,[r6,r5,lsl #2]
    code[0x08:0x0C] = bytes.fromhex("06f085e0")   # add pc, r5, r6
    code[0x18:0x1C] = struct.pack("<I", 0x18)     # Offset -> 0x100030
    code[0x1C:0x20] = struct.pack("<I", 0x0)      # Selbstverweis: Default
    code[0x20:0x24] = struct.pack("<I", 0x0)      # Selbstverweis: Default
    code[0x24:0x28] = struct.pack("<I", 0x38)     # Offset -> 0x100050
    code[0x30:0x34] = bytes.fromhex("00402de9")   # push {lr}
    code[0x34:0x38] = bytes.fromhex("0000a0e3")   # mov r0,#0
    code[0x38:0x3C] = bytes.fromhex("0080bde8")   # pop {pc}
    code[0x50:0x54] = bytes.fromhex("00402de9")   # push {lr}
    code[0x54:0x58] = bytes.fromhex("0000a0e3")   # mov r0,#0
    code[0x58:0x5C] = bytes.fromhex("0080bde8")   # pop {pc}
    res = scan.scan(bytes(code), [(isa.ARM_BASE, "entry", False)])
    need(0x00100030 in res.functions,
         "Offset-Tabellen-Ziel nicht geseedet: %s"
         % sorted("%08X" % a for a in res.functions))
    # Die beiden Selbstverweise duerfen die Suche nicht beenden (im Spiel
    # stehen danach die 8-/16-Bit-Handler 0x16AA08/20 bzw. 0x16AF84/9C).
    need(0x00100050 in res.functions,
         "Ziel hinter Selbstverweisen nicht geseedet: %s"
         % sorted("%08X" % a for a in res.functions))

    # Zweite Form: Index bereits in Bytes (`ldr rX,[rB,rI]`) und negatives
    # Offset auf ein Ziel VOR der Tabelle (wie 0x16CF48 -> 0x16CEE4).
    code = bytearray(0x48)
    code[0x00:0x04] = bytes.fromhex("34608fe2")   # add r6, pc, #0x34
    code[0x04:0x08] = bytes.fromhex("085096e7")   # ldr r5,[r6,r8]
    code[0x08:0x0C] = bytes.fromhex("05f086e0")   # add pc, r6, r5
    code[0x10:0x14] = bytes.fromhex("00402de9")   # push {lr}
    code[0x14:0x18] = bytes.fromhex("0000a0e3")   # mov r0,#0
    code[0x18:0x1C] = bytes.fromhex("0080bde8")   # pop {pc}
    code[0x3C:0x40] = struct.pack("<I", 0xFFFFFFD4)  # -> 0x100010
    code[0x40:0x44] = struct.pack("<I", 0)           # zeigt in die Tabelle: Ende
    res = scan.scan(bytes(code), [(isa.ARM_BASE, "entry", False)])
    need(0x00100010 in res.functions,
         "Offset-Tabelle ohne lsl#2/negativ nicht geseedet: %s"
         % sorted("%08X" % a for a in res.functions))


def test_chained_pc_add_seed():
    """`add rN, pc, #hi` + `add rN, rN, #lo` vor `mov pc, rN` seedet das Ziel.

    Der Bilddecoder bildet die Adresse eines Format-Handlers in zwei Schritten
    (z. B. 0x16C3B0: `add r4, pc, #0x2000; add r4, r4, #0x354;
    movle pc, r4` -> 0x16E70C). Ohne Fortschreibung bleibt 0x16E3B8 stehen und
    der Handler fehlt; ab dem 42. Videoframe endet der Film im No-op.
    """
    from recomp import isa, scan

    code = bytearray(0x38)
    code[0x00:0x04] = bytes.fromhex("10408fe2")   # add r4, pc, #0x10
    code[0x04:0x08] = bytes.fromhex("104084e2")   # add r4, r4, #0x10
    code[0x08:0x0C] = bytes.fromhex("000050e3")   # cmp r0, #0
    code[0x0C:0x10] = bytes.fromhex("04f0a0d1")   # movle pc, r4
    code[0x10:0x14] = bytes.fromhex("1eff2fe1")   # bx lr (Fallthrough)
    code[0x28:0x2C] = bytes.fromhex("00402de9")   # push {lr}
    code[0x2C:0x30] = bytes.fromhex("0000a0e3")   # mov r0,#0
    code[0x30:0x34] = bytes.fromhex("0080bde8")   # pop {pc}
    res = scan.scan(bytes(code), [(isa.ARM_BASE, "entry", False)])
    blocks = {b.addr for f in res.functions.values() for b in f.blocks}
    need(0x00100028 in blocks,
         "Zwei-Schritt-PC-Adresse nicht als Block dekodiert: %s"
         % sorted("%08X" % a for a in blocks))


def test_dynamic_pc_resume():
    """Ruecksprung eines Tabellenziels mitten in die aufrufende Funktion.

    Die IDCT-Helfer des Bilddecoders werden per `add lr, pc, #4; add pc, rN, rM`
    angesprungen und kehren mit `mov pc, lr` in den naechsten Block der
    *aufrufenden* Funktion zurueck (belegt: LR 0x0016A600 aus 0x0016EF38).
    Der Emitter muss diesen Gast-PC wieder in einen lokalen Block aufloesen,
    statt die Funktion zu verlassen (sonst bleiben r0-r6 und der Epilog auf der
    Strecke).
    """
    from recomp import emit

    em = emit.Emitter(0x0016A590, thumb=False)
    em.block_labels = {0x0016A590, 0x0016A600, 0x0016A680}
    em.pc_targets = set(em.block_labels)
    code = "\n".join(em.dynamic_pc("0x0016EF3Cu"))
    need("arm_call_guest(c, t); break;" in code,
         "Fremdziel wird nicht per arm_call_guest aufgeloest:\n%s" % code)
    tail = code.split("break;", 1)[1]
    need("case 0x0016A600u: goto L_0016A590_0016A600;" in tail,
         "Ruecksprung in die Funktionsmitte fehlt:\n%s" % code)
    need("default: return;" in tail, "kein Rueckfall aus dem Resume-Switch:\n%s" % code)
def test_ldr_pc_stack_return():
    """`ldr pc,[sp],#imm` ist ein Epilog (Rueckkehr), kein indirekter Aufruf.

    Der ARM-Epilog `add sp,#n; pop {callee-saved}; ldr pc,[sp],#m` laedt die
    Rueckkehradresse dieses C-Aufrufs. Ein arm_call_guest an dieser Adresse
    startet eine Gastfunktion mitten in der Aufruferfunktion (bzw. landet als
    No-op in out/missing_calls.txt); richtig ist die Rueckkehr (`return`).
    """
    from recomp import isa, emit
    ins = isa.decode(bytes.fromhex("14f09de4"), isa.ARM_BASE)  # ldr pc,[sp],#0x14
    need(ins is not None, "ldr pc,[sp],#4 nicht dekodiert")
    em = emit.Emitter(isa.ARM_BASE)
    em.block_labels = {isa.ARM_BASE}
    lines = em.mem(ins)
    text = "\n".join(lines)
    need("return;" in text, "Epilog ohne Rueckkehr: %s" % text)
    need("arm_call_guest" not in text,
         "Epilog geht ueber arm_call_guest: %s" % text)

    # Rueckkehr aus dem Stack muss die SP-Schreibweise ausfuehren (Epilog
    # popt damit die Argumente); danach folgt `return`.
    need("c->r[13] = a + o" in text, "SP-Writeback fehlt: %s" % text)

    # Gegenprobe: `ldr pc,[sp]` (ohne Writeback) ist ebenfalls Rueckkehr.
    ins2 = isa.decode(bytes.fromhex("00f09de5"), isa.ARM_BASE)  # ldr pc,[sp]
    need(ins2 is not None, "ldr pc,[sp] nicht dekodiert")
    text2 = "\n".join(em.mem(ins2))
    need("arm_call_guest" not in text2,
         "ldr pc,[sp] geht ueber arm_call_guest: %s" % text2)


def test_pc_relative_lr_continuation():
    """`add lr, pc, #imm` markiert Tabellen-Handler als Pseudounterprogramm.

    Der Handler kehrt per `mov pc, lr` mitten in die aufrufende Funktion
    zurueck (0x16A5F4 -> 0x16A600, 0x16EF38). Nur solche Funktionen brauchen
    die Fortsetzungspruefung nach arm_call_guest; das haelt den generierten
    Code klein.
    """
    from recomp import isa, scan, cli
    code = bytearray(0x40)
    code[0x00:0x04] = bytes.fromhex("04e08fe2")   # add lr, pc, #4
    code[0x04:0x08] = bytes.fromhex("010000eb")   # bl 0x100010
    code[0x08:0x0C] = bytes.fromhex("1eff2fe1")   # bx lr
    code[0x10:0x14] = bytes.fromhex("0000a0e3")   # mov r0, #0
    code[0x14:0x18] = bytes.fromhex("1eff2fe1")   # bx lr
    res = scan.scan(bytes(code), [(isa.ARM_BASE, "entry", False)])
    fn = res.functions[isa.ARM_BASE]
    need(cli.has_pc_relative_lr(fn), "add lr,pc nicht erkannt")
    plain = res.functions.get(isa.ARM_BASE + 0x10)
    need(plain is not None and not cli.has_pc_relative_lr(plain),
         "Funktion ohne PC-LR faelschlich markiert")


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
    ("rendercmp SW/HW-Bildvergleich", test_rendercmp),
    ("PICA-Registermap", test_pica_regs),
    ("IMGC ETC1/linear", test_imgc_decoder),
    ("BLX-Zielmodus im Scan", test_call_mode),
    ("ldr-pc-Veneer-Ziel im Scan", test_ldrpc_veneer),
    ("Veneer-Seed Thumb", test_veneer_thumb_seed),
    ("PIC-Callback-Seed Thumb", test_pic_thumb_callback_seed),
    ("Offset-Tabelle Seed", test_offset_table_seed),
    ("Zwei-Schritt-PC-Adresse", test_chained_pc_add_seed),
    ("Ruecksprung in Funktionsmitte", test_dynamic_pc_resume),
    ("ldr pc aus SP = Rueckkehr", test_ldr_pc_stack_return),
    ("pc-relative LR = Fortsetzung", test_pc_relative_lr_continuation),
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
