"""diff_trace: Port-Log und Azahar-Referenz auf normierte Ereignisse
abbilden und die erste fehlende Referenzaktion nennen.

Unterschied zu trace-diff: Dieses Werkzeug vergleicht ab einem Anker
(Default: ``gsp 0x001E`` / SetInternalPriorities), fasst Wiederholungen
zusammen und listet *alle* Referenzereignisse, die im Port-Fenster fehlen.
Ausserdem werden die Azahar-Zeitstempel entfernt, sodass auch die
Service-Methodenzeilen (GSP/DSP/FS/Y2R/...) wirklich geparst werden.
"""
from __future__ import annotations

import argparse
import difflib
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

from . import common, trace_diff

DEFAULT_AFTER = r"gsp.*(?:0x)?001e|setinternalpriorities"


@dataclass
class NormEvent:
    index: int
    key: str
    display: str
    line: int
    raw: str
    count: int = 1


def _default_port_log() -> Path:
    runs = common.OUT / "runs"
    candidates = []
    if runs.is_dir():
        candidates = sorted(runs.glob("*/*.out.log"),
                            key=lambda p: p.stat().st_mtime, reverse=True)
    if candidates:
        return candidates[0]
    fixture = common.ROOT / "tools" / "fixtures" / "port_boot_20261119.log"
    return fixture


def _canonical(event: trace_diff.Event, kind: str) -> str:
    key = event.key
    if key.startswith("srv."):
        return key
    if "." in key:
        service, rest = key.split(".", 1)
        if rest.startswith("cmd=0x"):
            cmd = rest.split("=", 1)[1]
            method = trace_diff._CMD_MAP.get(
                (event.service if event.service else service, int(cmd, 16)))
            # _CMD_MAP nutzt die Schreibweise aus dem Port-Log.
            if method is None:
                for (svc, cid), name in trace_diff._CMD_MAP.items():
                    if svc.lower() == service.lower() and cid == int(cmd, 16):
                        method = name
                        break
            if method:
                return "%s.%s" % (service, method)
    return key


def _norm(events: Sequence[trace_diff.Event], kind: str,
          collapse: bool = True) -> List[NormEvent]:
    out: List[NormEvent] = []
    for e in events:
        if kind == "ipc" and e.key.startswith("srv."):
            continue
        key = _canonical(e, kind)
        ne = NormEvent(e.index, key, e.display, e.line_no, e.raw)
        if collapse and out and out[-1].key == ne.key:
            out[-1].count += 1
            out[-1].display = ne.display
            continue
        out.append(ne)
    return out


def _find_anchor(events: Sequence[NormEvent], pattern: re.Pattern) -> int:
    for i, e in enumerate(events):
        if pattern.search(e.key) or pattern.search(e.display):
            return i
    return -1


def _primary_set(events: Sequence[NormEvent], start: int) -> set:
    return {e.key for e in events[start:]}


def _match_port_anchor(port_events: Sequence[NormEvent], ref_key: str,
                       start: int) -> int:
    for i in range(start, len(port_events)):
        if port_events[i].key == ref_key:
            return i
    return -1


def _fmt_ctx(events: Sequence[NormEvent], idx: int, radius: int = 2) -> List[str]:
    lo = max(0, idx - radius)
    hi = min(len(events), idx + radius + 1)
    lines = []
    for i in range(lo, hi):
        mark = ">>" if i == idx else "  "
        e = events[i]
        rep = (" x%d" % e.count) if e.count > 1 else ""
        lines.append("%s [%d] %s%s  (Zeile %d)" % (
            mark, i, e.display, rep, e.line))
    return lines


def _limit_events(events: Sequence[NormEvent], n: int = 30) -> List[str]:
    return ["  %s%s" % (e.display, (" x%d" % e.count) if e.count > 1 else "")
            for e in events[:n]]


def compare(ref: trace_diff.Trace, port: trace_diff.Trace, kind: str,
            after: Optional[str], collapse: bool = True) -> dict:
    if kind == "srv":
        base = trace_diff.compare(ref, port, "srv")
        return {
            "mode": base.get("mode"),
            "anchor": after,
            "first": base.get("first"),
            "missing": base.get("missing", []),
            "extra": base.get("extra", []),
            "ref_events": len(ref.events),
            "port_events": len(port.events),
            "context": [],
        }

    ref_events = _norm(ref.events, kind, collapse)
    port_events = _norm(port.events, kind, collapse)
    pattern = re.compile(after, re.IGNORECASE) if after else None
    ref_start = 0
    port_start = 0
    anchor_display = None
    if pattern is not None:
        ri = _find_anchor(ref_events, pattern)
        pi = _find_anchor(port_events, pattern)
        if ri >= 0:
            anchor_display = ref_events[ri].display
            ref_start = ri + 1
            pi = _match_port_anchor(port_events, ref_events[ri].key,
                                    pi if pi >= 0 else 0)
            port_start = (pi + 1) if pi >= 0 else len(port_events)
        elif pi >= 0:
            anchor_display = port_events[pi].display
            port_start = pi + 1

    ref_suffix = ref_events[ref_start:]
    port_suffix = port_events[port_start:]
    ref_keys = [e.key for e in ref_suffix]
    port_keys = [e.key for e in port_suffix]
    ref_set = set(ref_keys)
    port_set = set(port_keys)
    # Fehlende Ereignisse pro Key nur einmal listen (mit Anzahl), damit
    # Referenzschleifen (z. B. Y2R) die Ausgabe nicht fluten.
    missing_counts: Dict[str, int] = {}
    missing: List[NormEvent] = []
    for e in ref_suffix:
        if e.key in port_set:
            continue
        if e.key not in missing_counts:
            missing_counts[e.key] = 0
            missing.append(e)
        missing_counts[e.key] += e.count
    extra_counts: Dict[str, int] = {}
    extra: List[NormEvent] = []
    for e in port_suffix:
        if e.key in ref_set:
            continue
        if e.key not in extra_counts:
            extra_counts[e.key] = 0
            extra.append(e)
        extra_counts[e.key] += e.count

    sm = difflib.SequenceMatcher(a=ref_keys, b=port_keys, autojunk=False)
    first = None
    context: List[str] = []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue
        if tag in ("delete", "replace"):
            first = ref_suffix[i1]
            context = _fmt_ctx(ref_suffix, i1)
            break
        if tag == "insert":
            first = port_suffix[j1]
            context = _fmt_ctx(port_suffix, j1)
            break

    return {
        "mode": "equal" if (not missing and not extra) else
                ("missing-event" if missing else "extra-event"),
        "anchor": anchor_display,
        "anchor_pattern": after,
        "first": None if first is None else first.display,
        "first_key": None if first is None else first.key,
        "first_line": None if first is None else first.line,
        "first_side": ("port" if first is not None and first in port_suffix
                       else "reference"),
        "missing": [e.display for e in missing],
        "missing_total": sum(missing_counts.values()),
        "missing_detail": [{"display": e.display, "key": e.key,
                            "line": e.line, "count": missing_counts[e.key]}
                           for e in missing],
        "extra": [e.display for e in extra],
        "context": context,
        "ref_events": len(ref.events),
        "port_events": len(port.events),
        "ref_after": len(ref_suffix),
        "port_after": len(port_suffix),
    }


def render(result: dict, limit: int = 30) -> List[str]:
    lines = ["diff_trace: Referenz %d Ereignisse, Port %d Ereignisse" % (
        result["ref_events"], result["port_events"])]
    anchor = result.get("anchor")
    if result.get("anchor_pattern"):
        lines.append("Anker: %s" % (anchor or
                                    "nicht gefunden (%s)" %
                                    result.get("anchor_pattern")))
    lines.append("Verglichen: %d Referenz- / %d Port-Ereignisse nach dem Anker"
                 % (result.get("ref_after", 0), result.get("port_after", 0)))
    details = result.get("missing_detail") or []
    if details:
        d = details[0]
        text = "Erste fehlende Referenzaktion%s: %s (Referenzzeile %d)" % (
            " nach Anker" if anchor else "", d["display"], d["line"])
        lines.append(text)
    first = result.get("first")
    if first is None:
        lines.append("Erste Sequenzabweichung: keine")
    else:
        side = "Port" if result.get("first_side") == "port" else "Referenz"
        lines.append("Erste Sequenzabweichung (%s): %s" % (side, first))
        if result.get("first_line"):
            lines.append("  Referenzzeile: %d" % result["first_line"])
        for c in result.get("context", []):
            lines.append("  " + c)
    missing = result.get("missing", [])
    if missing:
        lines.append("Fehlende Ereignisse aus der Referenz: %d unique "
                     "(%d Vorkommen)" % (len(missing),
                                         result.get("missing_total", 0)))
        details = result.get("missing_detail") or []
        shown = details[:limit]
        for d in shown:
            suffix = (" x%d" % d["count"]) if d.get("count", 1) > 1 else ""
            lines.append("  " + d["display"] + suffix)
        if len(details) > limit:
            lines.append("  ... %d weitere (--verbose)" % (len(details) - limit))
    if result.get("extra"):
        lines.append("Nur im Port: %d" % len(result["extra"]))
        for m in _limit_lines(result["extra"], min(10, limit)):
            lines.append("  " + m)
    return lines


def _limit_lines(items: Sequence[str], n: int) -> List[str]:
    out = list(items[:n])
    if len(items) > n:
        out.append("... %d weitere (--verbose)" % (len(items) - n))
    return out


def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(
        prog="diff_trace",
        description="Port-Log gegen Azahar-Referenz ab einem Anker vergleichen")
    p.add_argument("--ref", default=str(common.ROOT / "reports" / "azahar_trace.txt"))
    p.add_argument("--port", help="Default: neuester tools/out/runs-Lauf")
    p.add_argument("--kind", choices=["srv", "ipc", "all"], default="all")
    p.add_argument("--after", default=DEFAULT_AFTER,
                   help="Regex-Anker (Default: gsp 0x001E); '' = kein Anker")
    p.add_argument("--no-collapse", action="store_true",
                   help="wiederholte identische Ereignisse nicht zusammenfassen")
    p.add_argument("--limit", type=int, default=30)
    p.add_argument("--json-out")
    p.add_argument("--verbose", action="store_true")
    args = p.parse_args(argv)

    ref_path = Path(args.ref)
    port_path = Path(args.port) if args.port else _default_port_log()
    if not port_path.is_file():
        common.fail("Port-Log fehlt: %s (guest-run erzeugt eins)" % port_path)
    ref = trace_diff.parse_reference(ref_path)
    port = trace_diff.parse_port(port_path)
    after = args.after if args.after != "" else None
    result = compare(ref, port, args.kind, after,
                     collapse=not args.no_collapse)
    common.print_summary(render(result, args.limit if not args.verbose else 200),
                         40 if not args.verbose else 400)
    if args.json_out:
        Path(args.json_out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json_out).write_text(
            json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    return 0 if not result["missing"] else 1


if __name__ == "__main__":
    sys.exit(main())
