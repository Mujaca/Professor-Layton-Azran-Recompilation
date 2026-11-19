"""milestones: Fortschrittskette des Ports als Single Source of Truth.

Jede Stufe wird mit einem oder mehreren Regex-Beweisen geprueft. Die
Ausgabe enthaelt immer die konkrete Beweiszeile (oder die Information,
dass sie fehlt). Damit ersetzt der Checker freie Statusberichte.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence

from . import common, runner

# Kette in Projektreihenfolge. "all" verlangt alle Muster, "any" eines.
CHAIN = [
    {"id": "apt", "label": "APT-Parameter (ReceiveParameter)",
     "any": [r"ReceiveParameter", r"APT:U\s+cmd=0x000D", r"APT:U\s+cmd=0x000E"]},
    {"id": "fs", "label": "FS/RomFS geoeffnet",
     "any": [r"\[fs\]", r"OpenFileDirectly", r"file\s+cmd=0x080[23]",
             r"fs:USER\s+cmd=0x08(?:03|0C)"]},
    {"id": "dsp", "label": "DSP initialisiert",
     "any": [r"dsp::DSP", r"\[dsp\]", r"ndsp"]},
    {"id": "gsp_init", "label": "GSP-Init (0x0013 + 0x001E)",
     "all": [r"gsp::Gpu\s+cmd=0x0013|RegisterInterruptRelayQueue",
             r"gsp::Gpu\s+cmd=0x001E|SetInternalPriorities"]},
    {"id": "boss", "label": "boss:U akquiriert",
     "any": [r"srvname.*boss:U", r"srv:GetServiceHandle\(boss:U\)"]},
    {"id": "savedata", "label": "SaveData / head.lt6 geoeffnet",
     "any": [r"head\.lt6", r"SaveDataArchive", r"savedata"]},
    {"id": "gsp_000c", "label": "gsp::Gpu 0x000C (TriggerCmdReqQueue)",
     "any": [r"gsp::Gpu\s+cmd=0x000C", r"TriggerCmdReqQueue"]},
    {"id": "vram", "label": "VRAM-Writes (mw] 1F...)",
     "any": [r"\[mw\]\s+1F", r"VRAM"]},
    {"id": "frame", "label": "echter Frame (SetBufferSwap + VRAM)",
     "custom": "frame"},
]


@dataclass
class Check:
    id: str
    label: str
    ok: bool
    evidence: str = ""
    line_no: int = 0
    detail: str = ""


def _find_any(lines: Sequence[str], patterns: Sequence[str]):
    for no, line in enumerate(lines, 1):
        for pat in patterns:
            if re.search(pat, line, re.IGNORECASE):
                return no, line.rstrip()
    return 0, ""


def _find_all(lines: Sequence[str], patterns: Sequence[str]):
    hits = []
    for pat in patterns:
        no, line = _find_any(lines, [pat])
        if no == 0:
            return 0, "", "fehlt: %s" % pat
        hits.append((no, line))
    return hits[0][0], hits[0][1], ""


def _frame_check(lines: Sequence[str], shot: Optional[Path]) -> Check:
    no1, ev1 = _find_any(lines, [r"gsp::Gpu\s+cmd=0x0005|SetBufferSwap"])
    no2, ev2 = _find_any(lines, [r"\[mw\]\s+1F", r"VRAM"])
    if no1 == 0 or no2 == 0:
        missing = []
        if no1 == 0:
            missing.append("SetBufferSwap")
        if no2 == 0:
            missing.append("VRAM-Write")
        return Check("frame", "echter Frame (SetBufferSwap + VRAM)", False,
                     "", 0, "fehlt: " + ", ".join(missing))
    detail = ""
    if shot is not None and shot.is_file():
        size = shot.stat().st_size
        if size == 288054:
            detail = "Screenshot %d B = Port-Platzhalter" % size
        else:
            detail = "Screenshot %d B" % size
    # SetBufferSwap ohne VRAM-Write ist kein echter Frame; VRAM ohne Swap
    # ebenfalls nicht. Beides muss vorkommen.
    return Check("frame", "echter Frame (SetBufferSwap + VRAM)", True,
                 ev2 or ev1, no2 or no1, detail)


def evaluate(lines: Sequence[str], shot: Optional[Path] = None) -> List[Check]:
    out: List[Check] = []
    for item in CHAIN:
        if item.get("custom") == "frame":
            out.append(_frame_check(lines, shot))
            continue
        patterns = item.get("all") or item.get("any") or []
        if item.get("all"):
            no, ev, detail = _find_all(lines, patterns)
            out.append(Check(item["id"], item["label"], no > 0, ev, no, detail))
        else:
            no, ev = _find_any(lines, patterns)
            out.append(Check(item["id"], item["label"], no > 0, ev, no))
    return out


def _read_logs(paths: Sequence[Path]) -> List[str]:
    lines: List[str] = []
    for p in paths:
        if p.is_dir():
            for f in sorted(p.glob("*.log")):
                lines.extend(common.read_text_auto(f).splitlines())
        elif p.is_file():
            lines.extend(common.read_text_auto(p).splitlines())
    return lines


def render(checks: Sequence[Check]) -> List[str]:
    out = []
    passed = sum(1 for c in checks if c.ok)
    out.append("Meilensteine: %d/%d" % (passed, len(checks)))
    for c in checks:
        mark = "OK  " if c.ok else "FEHLT"
        icon = "✅" if c.ok else "❌"
        out.append("%s %-7s %s" % (icon, mark, c.label))
        if c.evidence:
            out.append("        Beweis (Zeile %d): %s" % (
                c.line_no, c.evidence[:180]))
        if c.detail:
            out.append("        %s" % c.detail)
    return out


def _default_log() -> Path:
    runs = common.OUT / "runs"
    if runs.is_dir():
        cands = sorted(runs.glob("*/*.out.log"),
                       key=lambda p: p.stat().st_mtime, reverse=True)
        if cands:
            return cands[0]
    return common.ROOT / "out" / "boot_log.txt"


def _run_latest(args) -> Path:
    spec = runner.RunSpec(
        label=args.label or "milestones",
        exe=(common.ROOT / "out" / ("port.exe" if args.exe == "port" else "boot.exe")),
        args=list(args.arg or ["out/romfs"]),
        env=common.env_dict(args.env),
        timeout=args.timeout,
        until=[re.compile(p) for p in (args.until or [])],
        until_text=list(args.until or []))
    result = runner.run_one(spec)
    print("Lauf %s: %s nach %.1fs" % (result.spec.label, result.status,
                                      result.runtime))
    print("Log: %s" % result.out_path)
    return result.out_path


def main(argv: Optional[List[str]] = None) -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    p = argparse.ArgumentParser(
        prog="milestones",
        description="Fortschrittskette eines Port-Laufs mit Beweiszeilen")
    p.add_argument("--log", action="append",
                   help="Logdatei oder Lauf-Verzeichnis (mehrfach)")
    p.add_argument("--run", action="store_true", help="erst einen Lauf starten")
    p.add_argument("--exe", choices=["boot", "port"], default="boot")
    p.add_argument("--arg", action="append")
    p.add_argument("--env", action="append")
    p.add_argument("--timeout", type=float, default=20.0)
    p.add_argument("--until", action="append")
    p.add_argument("--label")
    p.add_argument("--shot", default=str(common.ROOT / "out" / "shot.bmp"))
    p.add_argument("--json-out")
    p.add_argument("--verbose", action="store_true")
    args = p.parse_args(argv)

    paths = [Path(x) for x in args.log] if args.log else []
    if args.run:
        paths.append(_run_latest(args))
    if not paths:
        paths = [_default_log()]
    for path in paths:
        if not path.exists():
            common.fail("Log fehlt: %s" % path)
    lines = _read_logs(paths)
    if not lines:
        common.fail("Log ist leer: %s" % ", ".join(str(x) for x in paths))
    checks = evaluate(lines, Path(args.shot) if args.shot else None)
    common.print_summary(render(checks), 40 if not args.verbose else 200)
    if args.json_out:
        payload = {"logs": [str(x) for x in paths],
                   "passed": sum(1 for c in checks if c.ok),
                   "total": len(checks),
                   "checks": [{"id": c.id, "label": c.label, "ok": c.ok,
                               "evidence": c.evidence, "line": c.line_no,
                               "detail": c.detail} for c in checks]}
        Path(args.json_out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json_out).write_text(json.dumps(payload, indent=2,
                                                  ensure_ascii=False),
                                       encoding="utf-8")
    return 0 if all(c.ok for c in checks) else 1


if __name__ == "__main__":
    sys.exit(main())
