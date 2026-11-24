"""callgraph: statische Programmdatenbank fuer den recompilierten Gastcode.

Einmal indexieren, danach in Millisekunden abfragen:

  * Funktionen aus build/gen_boot.c
  * ARM-BL/BLX-Aufrufer
  * LDR-Literal-Pools (Literal -> Code-Stelle)
  * globale Datenadressen -> Reader/Writer (basis-symbolisch aufgeloest)
  * String-Literale -> Verwender

Der Index liegt in tools/out/callgraph.json und wird bei Aenderungen an
code_decomp.bin bzw. gen_boot.c automatisch neu gebaut. Es werden nur
Informationen gespeichert, die der lineare Register-Tracker sicher
aufloesen kann; unloesbare Basen werden bewusst nicht geraten.
"""
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Tuple

from . import common, disasm, genmap

CACHE_VERSION = 3
CACHE_NAME = "callgraph.json"


@dataclass
class CallSite:
    site: int
    target: int
    caller: int


@dataclass
class Literal:
    site: int
    lit_addr: int
    value: int
    caller: int


@dataclass
class Access:
    site: int
    ea: int
    width: int
    kind: str          # "load" | "store"
    caller: int
    base: int
    disp: int


@dataclass
class Graph:
    data: bytes
    code_base: int
    functions: genmap.FunctionMap
    calls: List[CallSite]
    literals: List[Literal]
    accesses: List[Access]

    def label(self, addr: int) -> str:
        return self.functions.label(addr)

    def containing(self, addr: int):
        return self.functions.containing(addr)

    def callers(self, target: int) -> List[CallSite]:
        t = target & ~1
        return [c for c in self.calls if (c.target & ~1) == t]

    def callees(self, func_start: int) -> List[CallSite]:
        return [c for c in self.calls if c.caller == func_start]

    def literals_to(self, value: int) -> List[Literal]:
        return [l for l in self.literals if l.value == value]

    def literals_near(self, value: int, size: int = 1) -> List[Literal]:
        return [l for l in self.literals
                if value <= l.value < value + max(1, size)]

    def accesses_at(self, addr: int, kind: Optional[str] = None) -> List[Access]:
        out = []
        for a in self.accesses:
            if a.ea <= addr < a.ea + max(1, a.width):
                if kind is None or a.kind == kind:
                    out.append(a)
        return out

    def function_events(self, func_start: int):
        calls = [c for c in self.calls if c.caller == func_start]
        acc = [a for a in self.accesses if a.caller == func_start]
        lits = [l for l in self.literals if l.caller == func_start]
        return calls, acc, lits


# ---------------------------------------------------------------- Aufbau

def _cache_path() -> Path:
    return common.ensure_out() / CACHE_NAME


def _cache_key(code_path: Path, gen_path: Path) -> dict:
    def stat(p: Path):
        try:
            s = p.stat()
            return {"size": s.st_size, "mtime": int(s.st_mtime)}
        except OSError:
            return {"size": 0, "mtime": 0}
    return {"version": CACHE_VERSION, "code": stat(code_path),
            "gen": stat(gen_path)}


def _scan_function(data: bytes, fn: genmap.FuncInfo,
                   calls: List[CallSite], literals: List[Literal],
                   accesses: List[Access]) -> int:
    insns = disasm.decode_range(data, fn.start, fn.end)
    n = 0
    for i, ins in enumerate(insns):
        n += 1
        if ins.bl_target is not None:
            t = ins.bl_target
            if common.ARM_BASE <= (t & ~1) < common.ARM_BASE + len(data):
                calls.append(CallSite(ins.addr, t, fn.start))
        mem = ins.mem
        if mem is None:
            continue
        if mem.pc_relative and mem.is_load and not mem.index:
            lit = mem.lit_addr
            if lit is None:
                continue
            value = common.u32(data, lit - common.ARM_BASE)
            literals.append(Literal(ins.addr, lit, value, fn.start))
            continue
        if mem.pc_relative:
            continue
        base_val = disasm.resolve_reg(insns, i, mem.base)
        if base_val is None:
            continue
        addend = mem.disp
        if mem.index:
            idx_val = disasm.resolve_reg(insns, i, mem.index)
            if idx_val is None:
                continue
            addend += idx_val << (mem.shift or 0)
        kind = "store" if mem.is_store else "load"
        if ins.reglist:
            for k in range(len(ins.reglist)):
                ea = (base_val + addend + k * 4) & 0xFFFFFFFF
                accesses.append(Access(ins.addr, ea, 4, kind, fn.start,
                                       base_val, addend + k * 4))
        else:
            ea = (base_val + addend) & 0xFFFFFFFF
            accesses.append(Access(ins.addr, ea, mem.width, kind, fn.start,
                                   base_val, addend))
    return n


def build(code_path: Optional[Path] = None, gen_path: Optional[Path] = None,
          verbose: bool = False) -> Graph:
    code_path = Path(code_path) if code_path else common.CODE_BIN
    gen_path = Path(gen_path) if gen_path else common.GEN_BOOT
    common.check_file(code_path, "Code-Dump", "python tools/decomp.py")
    common.check_file(gen_path, "Generierter Code", "python tools/build.py")
    data = code_path.read_bytes()
    fmap = genmap.load_functions(gen_path, code_hi=common.ARM_BASE + len(data))
    calls: List[CallSite] = []
    literals: List[Literal] = []
    accesses: List[Access] = []
    total_insns = 0
    for idx, fn in enumerate(fmap.functions):
        total_insns += _scan_function(data, fn, calls, literals, accesses)
        if verbose and idx % 2000 == 0:
            print("  ... %d/%d Funktionen" % (idx, len(fmap)), file=sys.stderr)
    calls.sort(key=lambda c: (c.site, c.target))
    literals.sort(key=lambda l: (l.value, l.site))
    accesses.sort(key=lambda a: (a.ea, a.site))
    if verbose:
        print("  Index: %d Funktionen, %d Calls, %d Literale, %d Zugriffe, "
              "%d Instruktionen" % (len(fmap), len(calls), len(literals),
                                    len(accesses), total_insns),
              file=sys.stderr)
    return Graph(data, common.ARM_BASE, fmap, calls, literals, accesses)


def save(graph: Graph, code_path: Path, gen_path: Path,
         path: Optional[Path] = None) -> Path:
    path = Path(path) if path else _cache_path()
    payload = {
        "key": _cache_key(code_path, gen_path),
        "functions": [[f.start, f.end, f.name] for f in graph.functions.functions],
        "calls": [[c.site, c.target, c.caller] for c in graph.calls],
        "literals": [[l.site, l.lit_addr, l.value, l.caller]
                     for l in graph.literals],
        "accesses": [[a.site, a.ea, a.width, 1 if a.kind == "store" else 0,
                      a.caller, a.base, a.disp] for a in graph.accesses],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, separators=(",", ":")), encoding="utf-8")
    return path


def _load(path: Path) -> Graph:
    raw = json.loads(path.read_text(encoding="utf-8"))
    funcs = [genmap.FuncInfo(int(a), int(b), str(n))
             for a, b, n in raw["functions"]]
    code_path = common.CODE_BIN
    data = code_path.read_bytes() if code_path.is_file() else b""
    return Graph(
        data=data, code_base=common.ARM_BASE,
        functions=genmap.FunctionMap(funcs),
        calls=[CallSite(int(a), int(b), int(c)) for a, b, c in raw["calls"]],
        literals=[Literal(int(a), int(b), int(c), int(d))
                  for a, b, c, d in raw["literals"]],
        accesses=[Access(int(s), int(e), int(w),
                         "store" if int(k) else "load", int(c), int(b), int(d))
                  for s, e, w, k, c, b, d in raw["accesses"]],
    )


def load(code_path: Optional[Path] = None, gen_path: Optional[Path] = None,
         force: bool = False, verbose: bool = False) -> Graph:
    code_path = Path(code_path) if code_path else common.CODE_BIN
    gen_path = Path(gen_path) if gen_path else common.GEN_BOOT
    path = _cache_path()
    key = _cache_key(code_path, gen_path)
    if not force and path.is_file():
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
            if raw.get("key") == key:
                graph = _load(path)
                if verbose:
                    print("  Cache: %s" % path, file=sys.stderr)
                return graph
        except Exception:
            pass
    graph = build(code_path, gen_path, verbose=verbose)
    save(graph, code_path, gen_path, path)
    return graph


# ---------------------------------------------------------------- Strings

def extract_strings(data: bytes, base: int = common.ARM_BASE,
                    min_len: int = 4) -> List[Tuple[int, str]]:
    out: List[Tuple[int, str]] = []
    n = len(data)
    i = 0
    while i < n:
        c = data[i]
        if 32 <= c < 127:
            j = i
            while j < n and 32 <= data[j] < 127:
                j += 1
            if j - i >= min_len:
                out.append((base + i, data[i:j].decode("ascii", "replace")))
            i = j
        else:
            i += 1
    # UTF-16LE (typisch fuer 3DS-Dateinamen)
    i = 0
    while i + 1 < n:
        if 32 <= data[i] < 127 and data[i + 1] == 0:
            j = i
            chars = []
            while j + 1 < n and 32 <= data[j] < 127 and data[j + 1] == 0:
                chars.append(chr(data[j]))
                j += 2
            if len(chars) >= min_len:
                out.append((base + i, "".join(chars)))
            i = j
        else:
            i += 1
    return out


def string_users(graph: Graph, addr: int, text: str) -> List[Literal]:
    return graph.literals_near(addr, max(1, len(text)))


# ---------------------------------------------------------------- Ausgabe

def _write_json(path: str, payload) -> None:
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(payload, indent=2, ensure_ascii=False),
                 encoding="utf-8")


def _cmd_callers(graph: Graph, args) -> int:
    target = common.parse_int(args.address)
    rows = graph.callers(target)
    lines = ["callers %s: %d" % (common.hexs(target), len(rows))]
    if rows:
        t = common.Table(["STELLE", "FUNKTION", "ZIEL"])
        for c in rows[: (len(rows) if args.verbose else args.limit)]:
            t.add(common.hexs(c.site), graph.label(c.caller), common.hexs(c.target))
        lines.append(t.render())
        if len(rows) > args.limit and not args.verbose:
            lines.append("... %d weitere (--verbose)" % (len(rows) - args.limit))
    else:
        lines.append("KEIN Aufrufer gefunden.")
    common.print_summary(lines, 40 if args.verbose else 20)
    if args.json_out:
        _write_json(args.json_out, [{"site": common.hexs(c.site),
                                     "function": graph.label(c.caller),
                                     "target": common.hexs(c.target)}
                                    for c in rows])
    return 0 if rows else 1


def _cmd_literals(graph: Graph, args) -> int:
    value = common.parse_int(args.value)
    rows = graph.literals_to(value)
    lines = ["literals %s: %d LDR-Literal-Verwender" % (common.hexs(value), len(rows))]
    if rows:
        t = common.Table(["STELLE", "POOL", "FUNKTION"])
        for l in rows[: (len(rows) if args.verbose else args.limit)]:
            t.add(common.hexs(l.site), common.hexs(l.lit_addr), graph.label(l.caller))
        lines.append(t.render())
        if len(rows) > args.limit and not args.verbose:
            lines.append("... %d weitere (--verbose)" % (len(rows) - args.limit))
    else:
        lines.append("KEIN Literal zeigt auf %s." % common.hexs(value))
    common.print_summary(lines, 40 if args.verbose else 20)
    if args.json_out:
        _write_json(args.json_out, [{"site": common.hexs(l.site),
                                     "pool": common.hexs(l.lit_addr),
                                     "value": common.hexs(l.value),
                                     "function": graph.label(l.caller)}
                                    for l in rows])
    return 0 if rows else 1


def _cmd_access(graph: Graph, args, kind: str) -> int:
    target = common.parse_int(args.address)
    rows = graph.accesses_at(target, kind)
    word = "Schreiber" if kind == "store" else "Leser"
    lines = ["%s %s: %d" % (word, common.hexs(target), len(rows))]
    if rows:
        t = common.Table(["STELLE", "FUNKTION", "EA", "BREITE"])
        for a in rows[: (len(rows) if args.verbose else args.limit)]:
            t.add(common.hexs(a.site), graph.label(a.caller),
                  common.hexs(a.ea), a.width)
        lines.append(t.render())
        if len(rows) > args.limit and not args.verbose:
            lines.append("... %d weitere (--verbose)" % (len(rows) - args.limit))
        funcs = []
        for a in rows:
            name = graph.label(a.caller)
            if name not in funcs:
                funcs.append(name)
        lines.append("Funktionen: " + ", ".join(funcs[:10]) +
                     ("" if len(funcs) <= 10 else ", ..."))
    else:
        lines.append("KEIN %s gefunden (nur aufloesbare Basis-Register)." % word)
    common.print_summary(lines, 40 if args.verbose else 20)
    if args.json_out:
        _write_json(args.json_out, [{"site": common.hexs(a.site),
                                     "function": graph.label(a.caller),
                                     "ea": common.hexs(a.ea),
                                     "width": a.width}
                                    for a in rows])
    return 0 if rows else 1


def _cmd_strings(graph: Graph, args) -> int:
    needle = args.text
    found = [(a, s) for a, s in extract_strings(graph.data, graph.code_base)
             if needle in s]
    lines = ["strings %r: %d Treffer" % (needle, len(found))]
    for addr, text in found[: (len(found) if args.verbose else args.limit)]:
        users = string_users(graph, addr, text)
        lines.append("  %s  %r" % (common.hexs(addr), text[:80]))
        for l in users[:6]:
            lines.append("      <- %s in %s (Pool %s)" % (
                common.hexs(l.site), graph.label(l.caller),
                common.hexs(l.lit_addr)))
        if len(users) > 6:
            lines.append("      ... %d weitere Verwender" % (len(users) - 6))
    if not found:
        lines.append("KEIN String-Treffer.")
    common.print_summary(lines, 40 if args.verbose else 20)
    if args.json_out:
        _write_json(args.json_out, [{"addr": common.hexs(a), "text": s,
                                     "users": [common.hexs(l.site) for l in
                                               string_users(graph, a, s)]}
                                    for a, s in found])
    return 0 if found else 1


def _cmd_fn(graph: Graph, args) -> int:
    addr = common.parse_int(args.address)
    fn = graph.containing(addr)
    if fn is None:
        common.fail("%s liegt in keiner bekannten Funktion" % common.hexs(addr))
    calls, acc, lits = graph.function_events(fn.start)
    lines = ["fn %s  [%s..%s)" % (
        fn.name, common.hexs(fn.start), common.hexs(fn.end))]
    if addr != fn.start:
        lines.append("Eingabe liegt bei %s (+0x%X)" %
                     (common.hexs(addr), addr - fn.start))
    callers = graph.callers(fn.start)
    lines.append("Aufrufer: %d" % len(callers))
    for c in callers[: (len(callers) if args.verbose else 8)]:
        lines.append("  %s  in %s" % (common.hexs(c.site), graph.label(c.caller)))
    lines.append("Aufrufe (BL): %d" % len(calls))
    for c in calls[: (len(calls) if args.verbose else 8)]:
        lines.append("  %s -> %s" % (common.hexs(c.site), graph.label(c.target)))
    stores = [a for a in acc if a.kind == "store"]
    loads = [a for a in acc if a.kind == "load"]
    lines.append("Globale Stores: %d, Loads: %d, Literale: %d" %
                 (len(stores), len(loads), len(lits)))
    for a in stores[: (len(stores) if args.verbose else 6)]:
        lines.append("  STORE %s  @%s" % (common.hexs(a.ea), common.hexs(a.site)))
    for l in lits[: (len(lits) if args.verbose else 6)]:
        lines.append("  LIT %s  -> %s  @%s" % (
            common.hexs(l.lit_addr), common.hexs(l.value), common.hexs(l.site)))
    common.print_summary(lines, 60 if args.verbose else 30)
    return 0


def _cmd_stats(graph: Graph, args) -> int:
    lines = ["callgraph: %d Funktionen, %d Calls, %d Literale, %d Zugriffe" % (
        len(graph.functions), len(graph.calls), len(graph.literals),
        len(graph.accesses))]
    per_func = {}
    for a in graph.accesses:
        per_func[a.caller] = per_func.get(a.caller, 0) + 1
    top = sorted(per_func.items(), key=lambda kv: -kv[1])[:10]
    lines.append("Zugriffe je Funktion (Top 10):")
    for start, n in top:
        lines.append("  %-14s %d" % (graph.label(start), n))
    common.print_summary(lines)
    return 0


# ---------------------------------------------------------------- CLI

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="callgraph",
        description="Statische Programmdatenbank (Funktionen, Calls, Literale, "
                    "Reader/Writer, Strings)")
    p.add_argument("--bin", help="Default: out/exefs/code_decomp.bin")
    p.add_argument("--gen", help="Default: build/gen_boot.c")
    p.add_argument("--rebuild", action="store_true", help="Cache neu bauen")
    p.add_argument("--verbose", action="store_true")
    sub = p.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("callers", help="BL-Aufrufer einer Funktion")
    c.add_argument("address")
    c.add_argument("--limit", type=int, default=20)
    c.add_argument("--json-out")
    c.set_defaults(func=_cmd_callers)

    l = sub.add_parser("literals", help="LDR-Literale, die auf einen Wert zeigen")
    l.add_argument("value")
    l.add_argument("--limit", type=int, default=20)
    l.add_argument("--json-out")
    l.set_defaults(func=_cmd_literals)

    w = sub.add_parser("writers", help="Wer schreibt eine globale Adresse?")
    w.add_argument("address")
    w.add_argument("--limit", type=int, default=20)
    w.add_argument("--json-out")
    w.set_defaults(func=lambda g, a: _cmd_access(g, a, "store"))

    r = sub.add_parser("readers", help="Wer liest eine globale Adresse?")
    r.add_argument("address")
    r.add_argument("--limit", type=int, default=20)
    r.add_argument("--json-out")
    r.set_defaults(func=lambda g, a: _cmd_access(g, a, "load"))

    s = sub.add_parser("strings", help="String-Literal und seine Verwender")
    s.add_argument("text")
    s.add_argument("--limit", type=int, default=20)
    s.add_argument("--json-out")
    s.set_defaults(func=_cmd_strings)

    f = sub.add_parser("fn", help="Funktionsprofil")
    f.add_argument("address")
    f.set_defaults(func=_cmd_fn)

    st = sub.add_parser("stats", help="Index-Uebersicht")
    st.set_defaults(func=_cmd_stats)
    return p


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    graph = load(Path(args.bin) if args.bin else None,
                 Path(args.gen) if args.gen else None,
                 force=args.rebuild, verbose=args.verbose)
    return args.func(graph, args)


if __name__ == "__main__":
    sys.exit(main())
