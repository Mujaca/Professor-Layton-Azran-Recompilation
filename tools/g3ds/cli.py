"""CLI-Dispatcher fuer g3ds: trace-diff, guest-refs, guest-run, guest-watch, guest-exp."""
from __future__ import annotations

import argparse
import json
import re
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import List, Optional

from . import common

# Module werden in den Unterkommandos importiert, damit --help schnell bleibt.


def _exe(value: str) -> Path:
    if value in ("boot", "out/boot.exe", "out\\boot.exe"):
        return common.ROOT / "out" / "boot.exe"
    if value in ("port", "out/port.exe", "out\\port.exe"):
        return common.ROOT / "out" / "port.exe"
    p = Path(value)
    return p if p.is_absolute() else (common.ROOT / p)


def _watch_spec(args) -> "runner.RunSpec":
    from . import runner, watch
    plan = watch.WatchPlan()
    if args.write:
        try:
            plan.write_addr = common.parse_int(args.write)
        except Exception as exc:
            common.fail("--write: %s (%s)" % (args.write, exc))
    if args.read:
        plan.read_spec = args.read
    if args.range:
        lo, hi = watch.parse_spec(args.range, "--range")
        plan.range_spec = "%X,%X" % (lo, hi - lo)
    if args.pc:
        try:
            plan.pc_addr = common.parse_int(args.pc)
        except Exception as exc:
            common.fail("--pc: %s (%s)" % (args.pc, exc))
    if args.mwval:
        try:
            plan.mwval = common.parse_int(args.mwval)
        except Exception as exc:
            common.fail("--mwval: %s (%s)" % (args.mwval, exc))
    plan.perf = bool(args.perf)
    env = watch.apply_plan(plan, common.env_dict(args.env))
    if args.write:
        plan.expect_writes = [plan.write_addr]
    spec = runner.RunSpec(
        label=args.label or "watch",
        exe=_exe(args.exe), args=list(args.arg or ["out/romfs"]),
        env=env, timeout=args.timeout,
        until=[re.compile(p) for p in (args.until or [])],
        until_text=list(args.until or []))
    return spec, plan


def cmd_trace_diff(args) -> int:
    from . import trace_diff
    ref_path = Path(args.reference)
    port_path = Path(args.port)
    ref = trace_diff.parse_reference(ref_path)
    port = trace_diff.parse_port(port_path)
    result = trace_diff.compare(ref, port, args.kind)
    lines = trace_diff.render(result, ref, port)
    if args.table:
        lines.extend(trace_diff.render_table(ref, port))
    if args.verbose:
        detail = [e for e in port.events if e.cmd is not None and e.params]
        if detail:
            lines.append("Port-IPC mit Parametern (erste 20):")
            for e in detail[:20]:
                lines.append("  " + e.display + "  " + " ".join(e.params))
    common.print_summary(lines, max_lines=20 if not args.verbose else 200)
    if args.json_out:
        payload = {
            "reference": str(ref_path),
            "port": str(port_path),
            "kind": args.kind,
            "mode": result["mode"],
            "first": result["first"],
            "missing": result["missing"],
            "extra": result["extra"],
            "ref_events": len(ref.events),
            "port_events": len(port.events),
            "ref_services": ref.services,
            "port_services": port.services,
        }
        Path(args.json_out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json_out).write_text(json.dumps(payload, indent=2,
                                                  ensure_ascii=False),
                                       encoding="utf-8")
    if result["mode"] == "equal":
        return 0
    return 0 if result["mode"] != "missing-service" else 0


def cmd_guest_refs(args) -> int:
    from . import disasm, genmap, refs
    try:
        target = common.parse_int(args.address)
    except Exception as exc:
        common.fail("Adresse: %s (%s)" % (args.address, exc))
    code_path = Path(args.bin) if args.bin else common.CODE_BIN
    common.check_file(code_path, "Code-Dump",
                      "python tools\\decomp.py")
    data = code_path.read_bytes()
    fmap = genmap.load_functions(Path(args.gen) if args.gen else None,
                                 code_hi=common.ARM_BASE + len(data))
    result = refs.find_refs(target, args.kind, data=data, fmap=fmap)
    lines = []
    lines.append("guest-refs %s: %d Daten-Treffer, %d BL-Aufrufer "
                 "(%d Funktionen / %d Instruktionen durchsucht)"
                 % (common.hexs(target), len(result.events),
                    len(result.callers), result.functions_scanned,
                    result.instructions_scanned))
    if result.target_func is not None:
        lines.append("Ziel liegt im Code: %s [%s..%s)" % (
            result.target_func.name, common.hexs(result.target_func.start),
            common.hexs(result.target_func.end)))
    else:
        lines.append("Ziel ist eine Datenadresse (nicht im Codebereich).")
    if result.events:
        lines.append("  ADRESSE   FUNKTION       ART      INSTRUKTION                    ZUSATZ")
        limit = len(result.events) if args.verbose else min(args.limit,
                                                            len(result.events))
        for e in result.events[:limit]:
            extra = ""
            if e.base is not None:
                sign = "+" if e.disp >= 0 else "-"
                extra = "Basis %s %s0x%X" % (common.hexs(e.base), sign,
                                             abs(e.disp))
            elif e.lit_addr is not None:
                extra = "Pool %s = %s" % (common.hexs(e.lit_addr),
                                          common.hexs(e.lit_value or 0))
            lines.append("  %s  %-13s  %-8s %-30s %s" % (
                common.hexs(e.addr), e.func_name, e.kind,
                e.text[:30], extra))
        if len(result.events) > limit:
            lines.append("  ... %d weitere (--verbose)" %
                         (len(result.events) - limit))
    if result.callers:
        lines.append("BL-Aufrufer: %d" % len(result.callers))
        for e in result.callers[: (len(result.callers) if args.verbose else 10)]:
            lines.append("  %s  in %s" % (common.hexs(e.addr), e.func_name))
    if args.verbose:
        lines.append("")
        for e in result.events:
            lines.append("Kontext %s (%s):" % (common.hexs(e.addr), e.func_name))
            lines.extend("  " + c for c in refs.context_for(e, data))
    if not result.events and not result.callers:
        print("\n".join(lines))
        print("KEIN Treffer fuer %s." % common.hexs(target))
        print("Geprueft: %d Funktionen, %d LDR-Literale, %d aufgeloeste "
                 "Speicherzugriffe." % (result.functions_scanned,
                                        result.ldr_literals,
                                        result.resolved_accesses))
        print("Hinweis: --kind=lite|store|load|all; Basis-Register "
                      "werden nur innerhalb einer Funktion verfolgt.")
        return 1
    common.print_summary(lines, max_lines=20 if not args.verbose else 400)
    if args.json_out:
        payload = {
            "target": common.hexs(target),
            "kind": args.kind,
            "events": [{
                "addr": common.hexs(e.addr), "kind": e.kind,
                "function": e.func_name,
                "text": e.text,
                "base": None if e.base is None else common.hexs(e.base),
                "disp": e.disp,
                "lit_addr": None if e.lit_addr is None else common.hexs(e.lit_addr),
                "lit_value": None if e.lit_value is None else common.hexs(e.lit_value),
            } for e in result.events],
            "callers": [{"addr": common.hexs(e.addr), "function": e.func_name}
                        for e in result.callers],
        }
        Path(args.json_out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json_out).write_text(json.dumps(payload, indent=2),
                                       encoding="utf-8")
    return 0


def _make_run_specs(args, for_exp=False):
    from . import runner
    env = common.env_dict(args.env)
    combos = runner.parse_matrix(args.matrix or [])
    specs = []
    base_label = args.label or ("exp" if for_exp else "run")
    for i, combo in enumerate(combos):
        merged = dict(env)
        merged.update(combo)
        label = base_label if len(combos) == 1 else "%s_m%02d" % (base_label, i)
        spec = runner.RunSpec(
            label=label,
            exe=_exe(args.exe),
            args=list(args.arg or ["out/romfs"]),
            env=merged,
            timeout=args.timeout,
            until=[re.compile(p) for p in (args.until or [])],
            until_text=list(args.until or []))
        specs.append(spec)
    return specs


def cmd_guest_run(args) -> int:
    from . import runner
    specs = _make_run_specs(args)
    results = []
    if args.jobs > 1 and len(specs) > 1:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(runner.run_one, specs))
    else:
        results = [runner.run_one(s) for s in specs]
    results.sort(key=lambda r: r.spec.label)
    if args.matrix:
        print(runner.matrix_table(results))
    else:
        for r in results:
            for line in r.summary_lines(args.verbose):
                print(line)
            print("")
    if args.json_out:
        payload = []
        for r in results:
            payload.append({
                "label": r.spec.label,
                "env": r.spec.env,
                "status": r.status,
                "runtime": round(r.runtime, 3),
                "until_hits": r.until_hits,
                "ipc_total": sum(r.ipc.values()),
                "srvnames": r.srvnames,
                "threads": {k: list(v) for k, v in r.threads.items()},
                "perf_rate": r.perf_rate,
                "fatal": r.fatal,
                "out": str(r.out_path),
                "err": str(r.err_path),
            })
        Path(args.json_out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json_out).write_text(json.dumps(payload, indent=2),
                                       encoding="utf-8")
    return 0


def cmd_guest_watch(args) -> int:
    from . import genmap, runner, watch
    if not any([args.write, args.read, args.range, args.pc, args.mwval,
                args.perf]):
        common.fail("guest-watch braucht --write/--read/--range/--pc/--mwval/--perf")
    spec, plan = _watch_spec(args)
    result = runner.run_one(spec)
    events = watch.parse_events(result.stdout_lines)
    data = common.CODE_BIN.read_bytes() if common.CODE_BIN.is_file() else b""
    fmap = genmap.load_functions(code_hi=common.ARM_BASE + len(data)) if data else genmap.FunctionMap([])
    lines, ok = watch.summarize(plan, events, fmap, result.runtime, args.verbose)
    if plan.perf:
        if result.perf_rate is not None:
            lines.append("Blockrate: %.0f Bloecke/s (%s Bloecke)"
                         % (result.perf_rate,
                            ("%.1fM" % (result.perf_blocks / 1e6))
                            if result.perf_blocks else "?"))
        else:
            lines.append("Blockrate: n/a (<1M Bloecke in der Laufzeit)")
    common.print_summary(lines, max_lines=20 if not args.verbose else 400)
    if args.json_out:
        Path(args.json_out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json_out).write_text(json.dumps({
            "watch": {
                "write": None if plan.write_addr is None else common.hexs(plan.write_addr),
                "read": plan.read_spec, "range": plan.range_spec,
                "pc": None if plan.pc_addr is None else common.hexs(plan.pc_addr),
            },
            "events": [{
                "kind": e.kind, "addr": common.hexs(e.addr),
                "value": None if e.value is None else common.hexs(e.value),
                "pc": None if e.pc is None else common.hexs(e.pc),
                "lr": None if e.lr is None else common.hexs(e.lr),
                "thread": e.thread,
            } for e in events],
            "ok": ok,
        }, indent=2), encoding="utf-8")
    return 0 if ok else 1


def _exp_spec(args):
    specs = _make_run_specs(args, for_exp=True)
    if len(specs) != 1:
        common.fail("guest-exp record/check erwartet genau eine Spezifikation "
                    "(keine --matrix); nutze guest-run fuer Matrizen")
    return specs[0], args


def cmd_guest_exp(args) -> int:
    from . import exp, runner
    if args.action == "list":
        data = exp.load(Path(args.registry) if args.registry else None)
        rows = [exp.brief(e) for e in data["experiments"]]
        if not rows:
            print("Registry leer: %s" % (args.registry or exp.REGISTRY))
            return 0
        cols = ["ID", "TS", "HYPOTHESE", "DET", "STATUS", "IPC", "SRV", "UNTIL", "FATAL"]
        rows = rows[-args.limit:]
        t = common.Table(cols)
        for r in rows:
            t.add(r["id"], r["ts"], (r["hypothesis"] or "")[:40],
                  "ja" if r["deterministic"] else "NEIN",
                  r["status"], r["ipc_total"], r["srv_count"],
                  json.dumps(r["until_hits"], separators=(",", ":")),
                  r["fatal"])
        print(t.render())
        return 0
    if args.action == "show":
        data = exp.load(Path(args.registry) if args.registry else None)
        for e in data["experiments"]:
            if e.get("id") == args.id:
                print(json.dumps(e, indent=2, ensure_ascii=False))
                return 0
        common.fail("Experiment nicht gefunden: %s" % args.id, code=1)
    if args.action == "check":
        spec, _ = _exp_spec(args)
        data = exp.load(Path(args.registry) if args.registry else None)
        old = exp.find(data, spec)
        if old:
            print("Bereits gemessen: %s  %s" % (old["id"], old["hypothesis"]))
            return 0
        print("Noch nicht gemessen (Signatur %s)." % exp.signature(spec))
        return 1
    if args.action == "record":
        spec, _ = _exp_spec(args)
        data = exp.load(Path(args.registry) if args.registry else None)
        entry, created = exp.record(data, spec, args.hypothesis,
                                    args.repeat, args.note, args.force,
                                    Path(args.registry) if args.registry else None)
        if not created:
            print("Bereits gemessen: %s  (--force fuer neuen Lauf)" % entry["id"])
            return 0
        print("Experiment %s gespeichert (%s, repeat=%d, deterministisch=%s)"
              % (entry["id"], entry["commit"][:12], entry["repeat"],
                 "ja" if entry["deterministic"] else "NEIN"))
        print("Hypothese: %s" % entry["hypothesis"])
        for r in entry["runs"]:
            m = r["metrics"]
            print("  %-8s %-8s %5.1fs  ipc=%-4d srv=%-2d fatal=%d  until=%s"
                  % (r["label"], r["status"], r["runtime"], m["ipc_total"],
                     len(m["srvnames"]), m["fatal"],
                     json.dumps(m["until_hits"], separators=(",", ":"))))
        return 0
    common.fail("unbekannte guest-exp Aktion: %s" % args.action)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="g3ds", description="Werkzeuge fuer den statischen 3DS-Port")
    sub = p.add_subparsers(dest="command", required=True)

    t = sub.add_parser("trace-diff", help="Azahar-Log vs. Port-Log vergleichen")
    t.add_argument("reference")
    t.add_argument("port")
    t.add_argument("--kind", choices=["srv", "ipc", "all"], default="srv")
    t.add_argument("--table", action="store_true",
                   help="Dienst-Praesenz-Tabelle ausgeben")
    t.add_argument("--json-out")
    t.add_argument("--verbose", action="store_true")
    t.set_defaults(func=cmd_trace_diff)

    g = sub.add_parser("guest-refs", help="Referenzen auf Gastadresse finden")
    g.add_argument("address")
    g.add_argument("--kind", choices=["lite", "store", "load", "all"], default="all")
    g.add_argument("--bin", help="Default: out/exefs/code_decomp.bin")
    g.add_argument("--gen", help="Default: build/gen_boot.c")
    g.add_argument("--limit", type=int, default=20)
    g.add_argument("--json-out")
    g.add_argument("--verbose", action="store_true")
    g.set_defaults(func=cmd_guest_refs)

    r = sub.add_parser("guest-run", help="Boot/Port im Hintergrund messen")
    _add_run_args(r)
    r.set_defaults(func=cmd_guest_run)

    w = sub.add_parser("guest-watch", help="ARM_WATCH/MEMWATCH einheitlich auswerten")
    w.add_argument("--write", help="ARM_MEMWATCH: exakte Adresse")
    w.add_argument("--read", help="ARM_MEMWATCHRD: ADDR[,SIZE]")
    w.add_argument("--range", help="ARM_MEMWATCHRANGE: LO,HI")
    w.add_argument("--pc", help="ARM_WATCH: PC-Adresse")
    w.add_argument("--mwval", help="HLE_MEMWATCHVAL: Wert")
    w.add_argument("--perf", action="store_true", help="HLE_PERF=1 setzen")
    w.add_argument("--exe", default="boot")
    w.add_argument("--arg", action="append")
    w.add_argument("--env", action="append")
    w.add_argument("--timeout", type=float, default=15.0)
    w.add_argument("--until", action="append")
    w.add_argument("--label")
    w.add_argument("--json-out")
    w.add_argument("--verbose", action="store_true")
    w.set_defaults(func=cmd_guest_watch)

    e = sub.add_parser("guest-exp", help="Experiment-Registry")
    e.add_argument("action", choices=["record", "list", "show", "check"])
    e.add_argument("id", nargs="?")
    e.add_argument("--hypothesis", default="")
    e.add_argument("--note", default="")
    e.add_argument("--repeat", type=int, default=1)
    e.add_argument("--force", action="store_true")
    e.add_argument("--registry")
    e.add_argument("--limit", type=int, default=30)
    _add_run_args(e)
    e.set_defaults(func=cmd_guest_exp)
    return p


def _add_run_args(sp):
    sp.add_argument("--exe", default="boot",
                    help="boot | port | Pfad zu einer exe")
    sp.add_argument("--arg", action="append",
                    help="Argument fuer das Binary (mehrfach); Default out/romfs")
    sp.add_argument("--env", action="append", help="NAME=WERT (mehrfach)")
    sp.add_argument("--timeout", type=float, default=60.0)
    sp.add_argument("--until", action="append",
                    help="Regex; Lauf stoppt beim ersten Treffer")
    sp.add_argument("--matrix", action="append",
                    help="Env-Matrix K=V1|V2,K2=V3 (kartesisch)")
    sp.add_argument("--jobs", type=int, default=1)
    sp.add_argument("--label")
    sp.add_argument("--json-out")
    sp.add_argument("--verbose", action="store_true")


def main(argv: Optional[List[str]] = None) -> int:
    argv = list(argv) if argv is not None else sys.argv[1:]
    # Neue Werkzeuge haben eigene Parser (inkl. --help); g3ds reicht nur durch.
    if argv:
        name = argv[0].replace("_", "-")
        if name in ("callgraph", "diff-trace", "milestones", "matrix",
                    "where", "perfmap", "framecheck"):
            from importlib import import_module
            mod = import_module("g3ds." + name.replace("-", "_"))
            return mod.main(argv[1:])
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
