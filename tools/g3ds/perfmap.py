"""perfmap: Block-Histogramm pro Funktion/Thread und Geschwindigkeit.

Die Runtime (arm_trace.c) zaehlt bei HLE_PERFMAP_FILE=<pfad> jeden Block in
einem offenen Hash (Thread+PC) und schreibt das Histogramm periodisch,
ohne printf pro Block. Dieses Werkzeug aggregiert nach Funktion, zeigt die
heissesten Rueckwaertskanten und schaetzt emulierte Sekunden pro
Wandsekunde (268,111 MHz Gast-CPU).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

from . import callgraph, common, runner, where

CPU_HZ = 268111000.0
DEFAULT_PERFMAP = "tools/out/perfmap.txt"


def _block_words(data: bytes, pc: int, base: int, limit: int = 24) -> int:
    """Konservative Blocklaenge in 4-Byte-Woertern bis zum naechsten Sprung."""
    off = pc - base
    if off < 0 or off + 4 > len(data):
        return 1
    n = 0
    while n < limit and off + 4 <= len(data):
        w = int.from_bytes(data[off:off + 4], "little")
        n += 1
        # B/BL/BLX immediate: bits 27..25 == 101
        if (w & 0x0E000000) == 0x0A000000:
            break
        # BX/BLX register, mov pc, ldr pc, pop {...,pc}
        if (w & 0x0FFFFFF0) == 0x012FFF10:
            break
        if (w & 0x0FFFFFF0) == 0x012FFF30:
            break
        if (w & 0x0FFF0000) == 0x01A0F000:      # mov pc, ...
            break
        if (w & 0x0FF00000) == 0x0590F000:      # ldr pc, [...]
            break
        if (w & 0x0E000000) == 0x08000000 and (w & 0x8000):
            # LDM mit PC in der Reglist
            if w & (1 << 15):
                break
        off += 4
    return max(1, n)


def _report(pm: where.PerfMap, graph: callgraph.Graph,
            seconds: float, top: int, json_out: Optional[str]) -> int:
    lines = ["perfmap: %d Bloecke, %d aktive Threads, %.2fs" % (
        pm.total, sum(1 for v in pm.thread_total.values() if v), seconds)]
    # Pro Thread aggregieren.
    per_thread: Dict[int, Dict[int, int]] = {}
    for pc, ti, count in pm.pcs:
        fn = graph.containing(pc)
        start = fn.start if fn else 0
        d = per_thread.setdefault(ti, {})
        d[start] = d.get(start, 0) + count

    weighted: Dict[int, int] = {}
    block_cost: Dict[int, int] = {}
    for pc, ti, count in pm.pcs:
        if pc not in block_cost:
            block_cost[pc] = _block_words(graph.data, pc, graph.code_base)
        weighted[pc] = weighted.get(pc, 0) + count * block_cost[pc]
    total_insns = sum(weighted.values())
    rate = (total_insns / seconds / CPU_HZ) if seconds > 0 else 0.0

    for ti in sorted(pm.thread_total):
        total = pm.thread_total[ti]
        if total == 0:
            continue
        lines.append("")
        lines.append("Thread %d: %d Bloecke (%.1f%%)" % (
            ti, total, 100.0 * total / pm.total if pm.total else 0.0))
        funcs = sorted(per_thread.get(ti, {}).items(), key=lambda kv: -kv[1])
        for start, cnt in funcs[:top]:
            name = graph.label(start) if start else "(ausserhalb)"
            lines.append("  %-28s %8d  %5.1f%%" % (
                name, cnt, 100.0 * cnt / total if total else 0.0))

    # Globale Top-Funktionen.
    func_total: Dict[int, int] = {}
    for ti, d in per_thread.items():
        for start, cnt in d.items():
            func_total[start] = func_total.get(start, 0) + cnt
    lines.append("")
    lines.append("Top-Funktionen (alle Threads):")
    for start, cnt in sorted(func_total.items(), key=lambda kv: -kv[1])[:top]:
        name = graph.label(start) if start else "(ausserhalb)"
        lines.append("  %-28s %8d  %5.1f%%" % (
            name, cnt, 100.0 * cnt / pm.total if pm.total else 0.0))

    edges = sorted(pm.edges, key=lambda e: -e.count)[:top]
    if edges:
        lines.append("")
        lines.append("Top-Rueckwaertskanten (Spin-Kandidaten):")
        for e in edges:
            lines.append("  T%-2d %s -> %s  %8d x  %s" % (
                e.thread, common.hexs(e.frm), common.hexs(e.to), e.count,
                graph.label(e.to)))

    lines.append("")
    lines.append("Instruktionsschaetzung: %d Instruktionen, "
                 "Emuliert/Wandsekunde: %.3fx" % (total_insns, rate))
    lines.append("  (268,111 MHz Gast-CPU, Blocklaenge statisch bis zum "
                 "naechsten Sprung; Schaetzung)")
    common.print_summary(lines, 80)
    if json_out:
        payload = {
            "total_blocks": pm.total,
            "seconds": seconds,
            "block_rate": (pm.total / seconds) if seconds > 0 else 0,
            "estimated_instructions": total_insns,
            "emulated_seconds_per_wall_second": rate,
            "threads": pm.thread_total,
            "functions": [{"function": graph.label(k), "blocks": v}
                          for k, v in sorted(func_total.items(),
                                             key=lambda kv: -kv[1])],
            "edges": [{"thread": e.thread, "from": common.hexs(e.frm),
                       "to": common.hexs(e.to), "count": e.count}
                      for e in edges],
        }
        Path(json_out).parent.mkdir(parents=True, exist_ok=True)
        Path(json_out).write_text(json.dumps(payload, indent=2,
                                             ensure_ascii=False),
                                  encoding="utf-8")
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(
        prog="perfmap",
        description="Block-Histogramm und Emulationsgeschwindigkeit")
    sub = p.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("run", help="Port mit Perfmap starten und auswerten")
    r.add_argument("--exe", choices=["boot", "port"], default="boot")
    r.add_argument("--arg", action="append")
    r.add_argument("--env", action="append")
    r.add_argument("--timeout", type=float, default=12.0)
    r.add_argument("--perfmap-out", default=DEFAULT_PERFMAP)
    r.add_argument("--top", type=int, default=10)
    r.add_argument("--json-out")
    r.add_argument("--label")

    q = sub.add_parser("report", help="vorhandenes Perfmap auswerten")
    q.add_argument("--perfmap", default=DEFAULT_PERFMAP)
    q.add_argument("--seconds", type=float, required=True,
                   help="Wandzeit des Laufs (fuer die Rate)")
    q.add_argument("--top", type=int, default=10)
    q.add_argument("--json-out")

    args = p.parse_args(argv)
    if args.cmd == "run":
        path = Path(args.perfmap_out)
        env = common.env_dict(args.env)
        env["HLE_PERFMAP_FILE"] = str(path)
        env.setdefault("HLE_PERFMAP_DUMP_MS", "1500")
        env.setdefault("HLE_SRV_NAMES", "1")
        spec = runner.RunSpec(
            label=args.label or "perfmap",
            exe=common.ROOT / "out" / (
                "port.exe" if args.exe == "port" else "boot.exe"),
            args=list(args.arg or ["out/romfs"]), env=env,
            timeout=args.timeout,
            until=[re.compile(r"\[perfmap\] dump")],
            until_text=["perfmap"])
        result = runner.run_one(spec)
        print("Lauf %s: %s nach %.2fs, Log %s" % (
            result.spec.label, result.status, result.runtime,
            result.out_path))
        if not path.is_file():
            common.fail("Perfmap wurde nicht geschrieben: %s" % path)
        seconds = max(0.1, result.runtime)
        pm = where.parse_perfmap(path)
        graph = callgraph.load()
        return _report(pm, graph, seconds, args.top, args.json_out)

    path = Path(args.perfmap)
    if not path.is_file():
        common.fail("Perfmap fehlt: %s" % path)
    pm = where.parse_perfmap(path)
    graph = callgraph.load()
    return _report(pm, graph, args.seconds, args.top, args.json_out)


if __name__ == "__main__":
    sys.exit(main())
