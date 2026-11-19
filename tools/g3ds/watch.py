"""guest-watch: ARM_WATCH / ARM_MEMWATCH* einheitlich auswerten."""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from . import common, genmap, runner

_MW_RE = re.compile(
    r"^\[mw\]\s+([0-9A-Fa-f]{8})\s+<-\s+([0-9A-Fa-f]{8})\s+"
    r"size=(\d+)\s+pc=([0-9A-Fa-f]{8})\s+lr=([0-9A-Fa-f]{8})"
    r"(?:\s+th=([0-9A-Fa-f]+))?")
_MRD_RE = re.compile(
    r"^\[mrd\]\s+([0-9A-Fa-f]{8})\s+size=(\d+)\s+blk=([0-9A-Fa-f]{8})"
    r"\s+lr=([0-9A-Fa-f]{8})(?:\s+th=([0-9A-Fa-f]+))?")
_MWVAL_RE = re.compile(
    r"^\[mwval\]\s+([0-9A-Fa-f]{8})\s+<-\s+([0-9A-Fa-f]{8})"
    r"\s+pc=([0-9A-Fa-f]{8})")
_WATCH_RE = re.compile(
    r"^\[watch\]\s+0x([0-9A-Fa-f]{8})\s+r0=([0-9A-Fa-f]{8}).*?"
    r"\blr=([0-9A-Fa-f]{8})")


@dataclass
class WatchEvent:
    kind: str                    # write / read / pc / mwval
    addr: int
    value: Optional[int] = None
    pc: Optional[int] = None
    lr: Optional[int] = None
    thread: Optional[str] = None
    raw: str = ""


@dataclass
class WatchPlan:
    write_addr: Optional[int] = None
    read_spec: Optional[str] = None
    range_spec: Optional[str] = None
    pc_addr: Optional[int] = None
    mwval: Optional[int] = None
    perf: bool = False
    expect_writes: List[int] = field(default_factory=list)


def apply_plan(plan: WatchPlan, env: Dict[str, str]) -> Dict[str, str]:
    out = dict(env)
    if plan.write_addr is not None:
        out["ARM_MEMWATCH"] = "%X" % plan.write_addr
    if plan.read_spec:
        out["ARM_MEMWATCHRD"] = plan.read_spec
    if plan.range_spec:
        out["ARM_MEMWATCHRANGE"] = plan.range_spec
    if plan.pc_addr is not None:
        out["ARM_WATCH"] = "%X" % plan.pc_addr
    if plan.mwval is not None:
        out["HLE_MEMWATCHVAL"] = "%X" % plan.mwval
    if plan.perf:
        out["HLE_PERF"] = "1"
    return out


def parse_events(lines: List[str]) -> List[WatchEvent]:
    events: List[WatchEvent] = []
    for line in lines:
        m = _MW_RE.match(line)
        if m:
            events.append(WatchEvent("write", int(m.group(1), 16),
                                     int(m.group(2), 16),
                                     int(m.group(4), 16), int(m.group(5), 16),
                                     m.group(6), line))
            continue
        m = _MRD_RE.match(line)
        if m:
            events.append(WatchEvent("read", int(m.group(1), 16), None,
                                     int(m.group(3), 16), int(m.group(4), 16),
                                     m.group(5), line))
            continue
        m = _MWVAL_RE.match(line)
        if m:
            events.append(WatchEvent("mwval", int(m.group(1), 16),
                                     int(m.group(2), 16), int(m.group(3), 16),
                                     None, None, line))
            continue
        m = _WATCH_RE.match(line)
        if m:
            events.append(WatchEvent("pc", int(m.group(1), 16),
                                     int(m.group(2), 16), int(m.group(1), 16),
                                     int(m.group(3), 16), None, line))
    return events


def annotate(events: List[WatchEvent], fmap: genmap.FunctionMap) -> List[Tuple]:
    rows = []
    for e in events:
        func = fmap.label(e.pc) if e.pc is not None else "-"
        caller = fmap.label(e.lr) if e.lr is not None else "-"
        rows.append((e.kind, common.hexs(e.addr),
                     "-" if e.value is None else common.hexs(e.value),
                     "-" if e.pc is None else common.hexs(e.pc),
                     "-" if e.lr is None else common.hexs(e.lr),
                     func, caller, e.thread or "-"))
    return rows


def summarize(plan: WatchPlan, events: List[WatchEvent],
              fmap: genmap.FunctionMap, runtime: float,
              verbose: bool = False) -> Tuple[List[str], bool]:
    writes = [e for e in events if e.kind in ("write", "mwval")]
    reads = [e for e in events if e.kind == "read"]
    pcs = [e for e in events if e.kind == "pc"]
    ok = True
    lines = []
    lines.append("guest-watch: %d Ereignisse in %.1fs" %
                 (len(events), runtime))
    if plan.expect_writes:
        for addr in plan.expect_writes:
            hits = [e for e in writes if e.addr == addr]
            if hits:
                first = hits[0]
                lines.append("Schreibzugriff %s: %d x, erster Wert %s @ pc=%s (%s)" % (
                    common.hexs(addr), len(hits), common.hexs(first.value or 0),
                    common.hexs(first.pc or 0),
                    fmap.label(first.pc or 0)))
            else:
                ok = False
                lines.append("NIE GESCHRIEBEN: %s (0 Ereignisse)" % common.hexs(addr))
    elif writes:
        by_addr: Dict[int, int] = {}
        for e in writes:
            by_addr[e.addr] = by_addr.get(e.addr, 0) + 1
        lines.append("Schreibzugriffe: %d auf %d Adressen" % (len(writes), len(by_addr)))
        for addr, n in list(by_addr.items())[:6]:
            hits = [e for e in writes if e.addr == addr]
            lines.append("  %s: %d x (erster Wert %s @ pc=%s)" % (
                common.hexs(addr), n, common.hexs(hits[0].value or 0),
                common.hexs(hits[0].pc or 0)))
    if plan.read_spec:
        lines.append("Lesezugriffe: %d (%s)" % (len(reads), plan.read_spec))
    if plan.pc_addr is not None:
        lines.append("PC-Watch %s: %d Treffer" %
                     (common.hexs(plan.pc_addr), len(pcs)))
    threads = sorted({e.thread for e in events if e.thread})
    if threads:
        lines.append("Threads: " + ", ".join(threads))

    rows = annotate(events, fmap)
    if rows:
        limit = len(rows) if verbose else min(12, len(rows))
        lines.append("  ART    ADRESSE   WERT      PC        LR        FUNKTION@PC          AUFRUFER@LR          TH")
        for row in rows[:limit]:
            lines.append("  %-5s  %s  %s  %s  %s  %-20s %-20s %s" % row)
        if len(rows) > limit:
            lines.append("  ... %d weitere (--verbose)" % (len(rows) - limit))
    return lines, ok


def parse_spec(text: str, what: str) -> Tuple[int, int]:
    parts = [p.strip() for p in text.split(",")]
    try:
        lo = common.parse_int(parts[0])
        hi = common.parse_int(parts[1]) if len(parts) > 1 else lo + 4
    except Exception as exc:
        common.fail("%s: %s (%s)" % (what, text, exc))
    if hi <= lo:
        common.fail("%s: Ende muss > Anfang sein (%s)" % (what, text))
    return lo, hi
