"""where: Warteschleifen-/Blocker-Analysator fuer den laufenden Port.

--run startet out/boot.exe mit HLE_PERFMAP, liest das Block-Histogramm und
die Rueckwaertskanten und beantwortet fuer jeden Gast-Thread:

  * aktuelle PC-Region / heisse Funktion
  * erkannte Spin-Schleife (Rueckwaertskante + Loop-Bereich)
  * die in der Schleife gelesenen globalen Felder
  * wer diese Felder schreibt (statisch und optional dynamisch per
    ARM_MEMWATCH)

Ohne --run kann ein vorhandenes Perfmap-File (--perfmap) analysiert werden.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

from . import callgraph, common, disasm, runner, watch

# Projektbekannter Wartepunkt des Resource-Managers. Er wird zusaetzlich
# statisch belegt, weil der aktuelle Lauf laengst weitergelaufen sein kann.
KNOWN_WAIT_FIELDS = [0x0040B4C4, 0x0040B4C8]

# Laufzeitzeiger, die erst hle_init setzt: [0x3D91E8+0x1C] zeigt auf die
# Tick-Task-Tabelle 0x3D7C50 (Beleg: runtime/hle/hle.c, hle_tick-Kommentar).
RUNTIME_TABLE_POINTERS = {0x003D9204: 0x003D7C50}


@dataclass
class Edge:
    thread: int
    frm: int
    to: int
    count: int


@dataclass
class Field:
    addr: int
    site: int
    func: int
    text: str
    depth: int = 0


@dataclass
class PerfMap:
    total: int
    thread_total: Dict[int, int]
    pcs: List[Tuple[int, int, int]]
    edges: List[Edge]


def parse_perfmap(path: Path) -> PerfMap:
    total = 0
    thread_total: Dict[int, int] = {}
    pcs: List[Tuple[int, int, int]] = []
    edges: List[Edge] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("PERFMAP"):
            continue
        if line.startswith("# total "):
            total = int(line.split()[2])
        elif line.startswith("# thread "):
            p = line.split()
            thread_total[int(p[2])] = int(p[3])
        elif line.startswith("# back "):
            p = line.split()
            edges.append(Edge(int(p[2]), int(p[3], 16), int(p[4], 16),
                              int(p[5])))
        elif line.startswith("pc "):
            p = line.split()
            pcs.append((int(p[1], 16), int(p[2]), int(p[3])))
    return PerfMap(total, thread_total, pcs, edges)


def _hot_functions(graph: callgraph.Graph,
                   pcs: Sequence[Tuple[int, int, int]],
                   thread: int) -> List[Tuple[int, int]]:
    agg: Dict[int, int] = {}
    for pc, ti, count in pcs:
        if ti != thread:
            continue
        fn = graph.containing(pc)
        start = fn.start if fn else 0
        agg[start] = agg.get(start, 0) + count
    return sorted(agg.items(), key=lambda kv: -kv[1])


def _read_loop(graph: callgraph.Graph, to: int, frm: int,
               max_depth: int = 1) -> List[Field]:
    fn = graph.containing(to)
    if fn is None:
        return []
    # gen_boot.c enthaelt auch Seed-Labels mitten in Funktionen; die
    # abgeleiteten Grenzen sind dort zu kurz. Fuer die Loop-Analyse grosszuegig
    # bis hinter die Kante dekodieren.
    fend = min(graph.code_base + len(graph.data), max(fn.end, frm + 4,
                                                     to + 0x400))
    insns = disasm.decode_range(graph.data, fn.start, fend)
    lo = max(fn.start, to)
    hi = min(fend, frm + 4)
    fields: List[Field] = []
    seen = set()

    def add(addr: int, site: int, func: int, text: str, depth: int):
        key = (addr, site)
        if key in seen:
            return
        seen.add(key)
        fields.append(Field(addr, site, func, text, depth))

    def scan(ilist, start, end, func_start, depth):
        local = {ins.addr: i for i, ins in enumerate(ilist)}
        for ins in ilist:
            if not (start <= ins.addr < end):
                continue
            mem = ins.mem
            if mem is None:
                continue
            if mem.pc_relative:
                if mem.is_load and mem.lit_addr is not None:
                    val = common.u32(graph.data,
                                     mem.lit_addr - graph.code_base)
                    if 0x003D0000 <= val < 0x20000000:
                        add(val, ins.addr, func_start,
                            ins.text + "  (Literal %s)" % common.hexs(val),
                            depth)
                continue
            if not mem.is_load:
                continue
            i = local.get(ins.addr)
            if i is None:
                continue
            base = disasm.resolve_reg(ilist, i, mem.base)
            if base is None:
                continue
            addend = mem.disp
            if mem.index:
                idxv = disasm.resolve_reg(ilist, i, mem.index)
                if idxv is None:
                    continue
                addend += idxv << (mem.shift or 0)
            ea = (base + addend) & 0xFFFFFFFF
            if ea < 0x00100000 or ea >= 0x20000000:
                continue
            add(ea, ins.addr, func_start, ins.text, depth)

    scan(insns, lo, hi, fn.start, 0)
    if max_depth > 0:
        for ins in insns:
            if not (lo <= ins.addr < hi):
                continue
            if ins.bl_target is None:
                continue
            callee = graph.containing(ins.bl_target)
            if callee is None or callee.start == fn.start:
                continue
            cend = min(graph.code_base + len(graph.data), callee.start + 0x400)
            ci = disasm.decode_range(graph.data, callee.start, cend)
            scan(ci, callee.start, cend, callee.start, 1)
    fields.sort(key=lambda f: (f.depth, f.addr, f.site))
    return fields


def tick_registration(graph: callgraph.Graph, writer: int) -> Optional[dict]:
    """Statisch belegen, ob *writer* als Tick-Task registriert wird.

    Sucht im Aufrufer ein LDR-Literal auf die Writer-Adresse, direkt gefolgt
    von ``mov r1,#slot`` und ``bl registrar``. Im Registrar wird die
    Tabellenbasis ueber ein Literal + ``ldr [base,#0x1c]`` und der
    Callback-Slot ueber ``str [base,#0x10+slot*4]`` nachgewiesen.
    """
    for lit in graph.literals_to(writer & ~1):
        caller = graph.containing(lit.site)
        if caller is None:
            continue
        insns = disasm.decode_range(graph.data, caller.start, caller.end)
        idx = next((i for i, x in enumerate(insns)
                    if x.addr == lit.site), None)
        if idx is None:
            continue
        slot = None
        reg_target = None
        for j in range(idx + 1, min(len(insns), idx + 6)):
            ins = insns[j]
            if ins.mnemonic == "mov" and ins.text.startswith("mov r1"):
                m = re.search(r"#(0x[0-9a-fA-F]+|\d+)", ins.text)
                if m:
                    slot = int(m.group(1), 0)
            if ins.bl_target is not None:
                reg_target = ins.bl_target
                break
        if slot is None or reg_target is None:
            continue
        reg = graph.containing(reg_target)
        if reg is None:
            continue
        rend = min(graph.code_base + len(graph.data), reg.start + 0x200)
        rins = disasm.decode_range(graph.data, reg.start, rend)
        table = None
        store_disp = None
        for i, ins in enumerate(rins):
            if ins.mem and ins.mem.pc_relative and ins.mem.is_load:
                val = common.u32(graph.data,
                                 ins.mem.lit_addr - graph.code_base)
                if val < 0x00100000:
                    continue
                for k in range(i + 1, min(len(rins), i + 6)):
                    nxt = rins[k]
                    if (nxt.mem and nxt.mem.is_load
                            and not nxt.mem.pc_relative
                            and nxt.mem.disp == 0x1C):
                        table = (val + 0x1C) & 0xFFFFFFFF
                        break
            if (table is not None and ins.mem and ins.mem.is_store
                    and not ins.mem.pc_relative and 0x10 <= ins.mem.disp < 0x30):
                store_disp = ins.mem.disp
                break
        if table is not None:
            base = RUNTIME_TABLE_POINTERS.get(table, table)
            return {"site": lit.site, "slot": slot,
                    "registrar": reg.start, "table": base,
                    "table_pointer": table,
                    "store_disp": store_disp, "caller": caller.start}
    return None


def _fn_name(graph: callgraph.Graph, start: int) -> str:
    fn = graph.containing(start)
    return fn.name if fn is not None else common.hexs(start)


def _writer_text(graph: callgraph.Graph, field: int) -> Tuple[str, List[dict]]:
    stores = graph.accesses_at(field, "store")
    out = []
    for a in stores:
        out.append({"site": common.hexs(a.site),
                    "function": _fn_name(graph, a.caller),
                    "function_start": a.caller})
    if not out:
        return "kein statischer Schreiber (Basis nicht aufloesbar)", out
    fn = _fn_name(graph, stores[0].caller)
    return "%s @%s" % (fn, common.hexs(stores[0].site)), out


def _dynamic_writer(field: int, exe: str, timeout: float) -> Optional[dict]:
    env = {"ARM_MEMWATCH": "%X" % field, "HLE_SRV_NAMES": "1"}
    spec = runner.RunSpec(
        label="where_watch",
        exe=common.ROOT / "out" / ("port.exe" if exe == "port" else "boot.exe"),
        args=["out/romfs"], env=env, timeout=timeout,
        until=[], until_text=[])
    result = runner.run_one(spec)
    events = [e for e in watch.parse_events(result.stdout_lines)
              if e.kind == "write" and e.addr == field]
    if not events:
        return None
    last = events[-1]
    return {"pc": common.hexs(last.pc or 0),
            "value": common.hexs(last.value or 0),
            "hits": len(events),
            "log": str(result.out_path)}


def _known_wait(graph: callgraph.Graph, dynamic: bool, exe: str,
                timeout: float, thread: int = 2) -> List[str]:
    lines = []
    for field in KNOWN_WAIT_FIELDS:
        readers = graph.accesses_at(field, "load")
        if not readers:
            continue
        wtext, wlist = _writer_text(graph, field)
        if not wlist:
            continue
        writer_start = wlist[0]["function_start"]
        # Der Schreiber liest das Feld fuer Read-Modify-Write selbst; der
        # eigentliche Waiter ist ein anderer Leser.
        reader_fn = next((graph.containing(a.site) for a in readers
                          if a.caller != writer_start
                          and graph.containing(a.site)), None)
        if reader_fn is None:
            continue
        tick = tick_registration(graph, writer_start)
        dyn = _dynamic_writer(field, exe, timeout) if dynamic else None
        text = ("Bekannter Wartepunkt (statisch): %s liest Feld %s; "
                "Schreiber: %s" % (reader_fn.name, common.hexs(field), wtext))
        if tick is not None:
            text += " (Tick-Task Slot %d, Tabelle %s)" % (
                tick["slot"], common.hexs(tick["table"]))
        if dyn is not None:
            text += ("; dynamisch letzter Schreiber: pc=%s (%s) nach %d Treffern"
                     % (dyn["pc"], graph.label(int(dyn["pc"], 16)), dyn["hits"]))
        lines.append(text)
        if tick is not None:
            writer_name = _fn_name(graph, int(wlist[0]["function_start"]))
            lines.append(
                "  => Thread %d wartet in %s auf Feld %s; letzter Schreiber: "
                "%s (Tick-Task Slot %d der Tabelle %s)" % (
                    thread, reader_fn.name, common.hexs(field), writer_name,
                    tick["slot"], common.hexs(tick["table"])))
    return lines


def analyze(graph: callgraph.Graph, pm: PerfMap, thread: Optional[int],
            dynamic: bool, exe: str, watch_timeout: float) -> List[str]:
    lines = []
    if pm.total:
        lines.append("Perfmap: %d Bloecke, %d Threads mit Aktivitaet" % (
            pm.total, sum(1 for v in pm.thread_total.values() if v)))
    if thread is None:
        best_t, best_score = 0, -1.0
        for ti, total in pm.thread_total.items():
            if total < 1000:
                continue
            ec = max((e.count for e in pm.edges if e.thread == ti), default=0)
            score = ec / float(total)
            if ti == 2 and score > 0.02:
                score += 0.5
            if score > best_score:
                best_t, best_score = ti, score
        thread = best_t
    total = pm.thread_total.get(thread, 0)
    lines.append("")
    lines.append("Thread %d: %d Bloecke" % (thread, total))
    hot = _hot_functions(graph, pm.pcs, thread)
    if hot:
        lines.append("  Heisse Funktionen:")
        for start, _count in hot[:3]:
            cnt = sum(c for pc, ti, c in pm.pcs
                      if ti == thread and graph.containing(pc) and
                      graph.containing(pc).start == start)
            pct = (100.0 * cnt / total) if total else 0.0
            name = graph.label(start) if start else "(ausserhalb)"
            lines.append("    %-28s %7d  %5.1f%%" % (name, cnt, pct))
    edges = sorted((e for e in pm.edges if e.thread == thread),
                   key=lambda e: -e.count)
    if edges:
        e = edges[0]
        pct = (100.0 * e.count / total) if total else 0.0
        lines.append("  Spin-Kante: %s -> %s  %d x (%.1f%%)" % (
            common.hexs(e.frm), common.hexs(e.to), e.count, pct))
        loop_fn = graph.containing(e.to)
        if loop_fn is not None:
            lines.append("  Schleife in %s [%s..%s]" % (
                loop_fn.name, common.hexs(e.to), common.hexs(e.frm + 4)))
        fields = _read_loop(graph, e.to, e.frm)
        if fields:
            lines.append("  Gelesene Felder in der Schleife:")
            for f in fields[:6]:
                fn = graph.label(f.func)
                lines.append("    %s  @%s  %s" % (
                    common.hexs(f.addr), common.hexs(f.site), fn))
                wtext, wlist = _writer_text(graph, f.addr)
                lines.append("      Schreiber statisch: %s" % wtext)
                for w in wlist[:2]:
                    tick = tick_registration(graph, w["function_start"])
                    if tick is not None:
                        lines.append("      Tick-Registrierung: %s -> Slot %d, "
                                     "Tabelle %s (via %s)" % (
                                         graph.label(tick["caller"]),
                                         tick["slot"], common.hexs(tick["table"]),
                                         graph.label(tick["registrar"])))
        else:
            lines.append("  Keine globalen Felder in der Loop statisch "
                         "aufloesbar.")
    else:
        lines.append("  Keine Rueckwaertskante (kein Spin in diesem Sample).")

    known = _known_wait(graph, dynamic, exe, watch_timeout, thread)
    if known:
        lines.append("")
        lines.extend(known)
    return lines


def _default_perfmap() -> Path:
    return common.OUT / "where_perfmap.txt"


def _run_perfmap(args) -> Path:
    path = Path(args.perfmap_out) if args.perfmap_out else _default_perfmap()
    env = common.env_dict(args.env)
    env["HLE_PERFMAP_FILE"] = str(path)
    env.setdefault("HLE_PERFMAP_DUMP_MS", "1500")
    env.setdefault("HLE_SRV_NAMES", "1")
    spec = runner.RunSpec(
        label="where",
        exe=common.ROOT / "out" / ("port.exe" if args.exe == "port" else "boot.exe"),
        args=["out/romfs"], env=env, timeout=args.timeout,
        until=[re.compile(r"\[perfmap\] dump")], until_text=["perfmap"])
    result = runner.run_one(spec)
    print("Lauf %s: %s nach %.1fs, Log %s" % (
        result.spec.label, result.status, result.runtime, result.out_path))
    if not path.is_file():
        common.fail("Perfmap wurde nicht geschrieben: %s" % path)
    return path


def main(argv: Optional[List[str]] = None) -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    p = argparse.ArgumentParser(
        prog="where",
        description="Warteschleifen, gelesene Felder und Schreiber lokalisieren")
    p.add_argument("--run", action="store_true", help="Port mit Perfmap starten")
    p.add_argument("--perfmap", help="vorhandenes Perfmap-File")
    p.add_argument("--perfmap-out", help="Zielpfad fuer --run")
    p.add_argument("--thread", type=int, help="nur diesen Gast-Thread analysieren")
    p.add_argument("--exe", choices=["boot", "port"], default="boot")
    p.add_argument("--env", action="append")
    p.add_argument("--timeout", type=float, default=12.0)
    p.add_argument("--dynamic-timeout", type=float, default=8.0)
    p.add_argument("--no-dynamic", action="store_true",
                   help="kein ARM_MEMWATCH-Nachlauf")
    p.add_argument("--json-out")
    p.add_argument("--verbose", action="store_true")
    args = p.parse_args(argv)

    graph = callgraph.load(verbose=args.verbose)
    if args.perfmap:
        pm_path = Path(args.perfmap)
    elif args.run:
        pm_path = _run_perfmap(args)
    else:
        pm_path = _default_perfmap()
        if not pm_path.is_file():
            pm_path = _run_perfmap(args)
    if not pm_path.is_file():
        common.fail("Perfmap fehlt: %s" % pm_path)
    pm = parse_perfmap(pm_path)
    lines = analyze(graph, pm, args.thread, not args.no_dynamic,
                    args.exe, args.dynamic_timeout)
    common.print_summary(lines, 60 if not args.verbose else 200)
    if args.json_out:
        payload = {"perfmap": str(pm_path), "total": pm.total,
                   "thread_total": pm.thread_total,
                   "edges": [{"thread": e.thread, "from": common.hexs(e.frm),
                              "to": common.hexs(e.to), "count": e.count}
                             for e in pm.edges],
                   "text": lines}
        Path(args.json_out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json_out).write_text(json.dumps(payload, indent=2,
                                                  ensure_ascii=False),
                                       encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
