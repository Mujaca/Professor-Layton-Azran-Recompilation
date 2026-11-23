"""hotpatch: Adresse -> C-Callback ohne Rebuild registrieren.

Runtime: HLE_HOOK=<hexaddr>=<callback>[,<hexaddr>=<callback>]
Callbacks:
  count_fields / count   Treffer zaehlen (logarithmisch)
  regs / dump_regs       Register an der Adresse zeigen
  mem / dump_mem         8 Woerter ab r0 zeigen
  log / pc               pc/lr/r0/r1 zeigen
  set_r0                 r0 auf HLE_HOOK_VALUE setzen

Beispiel:
  python tools/hotpatch.py run --hook 28F284=count_fields --timeout 8
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional

from . import callgraph, common, runner

CALLBACKS = {
    "count_fields": "Treffer zaehlen, erste/2/4/8/1000er loggen",
    "count": "Alias fuer count_fields",
    "regs": "r0-r4/sp/lr an der Adresse (max HLE_HOOK_MAX)",
    "dump_regs": "Alias fuer regs",
    "mem": "8 Woerter ab r0 (max HLE_HOOK_MAX)",
    "dump_mem": "Alias fuer mem",
    "obj": "r0-r3 + [r0] + Vtable-Slots +0x00..0x1C und +0x70..0x8C",
    "log": "pc/lr/r0/r1 (max HLE_HOOK_MAX)",
    "pc": "Alias fuer log",
    "set_r0": "r0 = HLE_HOOK_VALUE (veraendert den Gastzustand!)",
}

_HOOK_RE = re.compile(
    r"^\[hook\]\s+(\S+)\s+addr=([0-9A-Fa-f]{8})\s+hits=(\d+)(.*)$")
_FINAL_RE = re.compile(
    r"^\[hook-final\]\s+(\S+)\s+addr=([0-9A-Fa-f]{8})\s+hits=(\d+)")


def parse_hooks(specs) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for item in specs or []:
        for part in item.split(","):
            part = part.strip()
            if not part:
                continue
            if "=" not in part:
                common.fail("--hook erwartet ADRESSE=CALLBACK, bekam %r" % part)
            addr, name = part.split("=", 1)
            if name not in CALLBACKS:
                common.fail("unbekannter Callback %r (siehe hotpatch list)" % name)
            common.parse_int(addr)
            out[addr] = name
    return out


def cmd_list(args) -> int:
    print("hotpatch-Callbacks (HLE_HOOK=ADRESSE=NAME[,ADRESSE=NAME]):")
    for name, desc in CALLBACKS.items():
        print("  %-12s %s" % (name, desc))
    print("\nBeispiele:")
    print("  python tools\\hotpatch.py run --hook 28F284=count_fields --timeout 8")
    print("  python tools\\hotpatch.py run --hook 134818=regs,13485C=regs --timeout 5")
    print("  HLE_HOOK_VALUE=1 python tools\\hotpatch.py run --hook 107A4C=set_r0")
    return 0


def cmd_run(args) -> int:
    hooks = parse_hooks(args.hook)
    if not hooks:
        common.fail("run braucht mindestens ein --hook ADRESSE=CALLBACK")
    hook_env = ",".join("%s=%s" % (a, n) for a, n in hooks.items())
    env = common.env_dict(args.env)
    env["HLE_HOOK"] = hook_env
    if args.max:
        env["HLE_HOOK_MAX"] = str(args.max)
    if args.value:
        env["HLE_HOOK_VALUE"] = args.value
    spec = runner.RunSpec(
        label=args.label or "hotpatch",
        exe=common.ROOT / "out" / ("port.exe" if args.exe == "port" else "boot.exe"),
        args=list(args.arg or ["out/romfs"]), env=env, timeout=args.timeout,
        until=[re.compile(p) for p in (args.until or [])],
        until_text=list(args.until or []))
    result = runner.run_one(spec)
    print("Lauf %s: %s nach %.1fs" % (result.spec.label, result.status,
                                      result.runtime))
    print("Log: %s" % result.out_path)
    found = []
    finals = {}
    for line in result.stdout_lines:
        m = _FINAL_RE.match(line)
        if m:
            finals[(m.group(2).upper(), m.group(1))] = int(m.group(3))
            continue
        m = _HOOK_RE.match(line)
        if m:
            found.append({"callback": m.group(1), "addr": int(m.group(2), 16),
                          "hits": int(m.group(3)), "rest": m.group(4).strip(),
                          "line": line.rstrip()})
    if not found and not finals:
        print("KEINE Hook-Treffer. Adresse falsch oder Funktion nie erreicht.")
        return 1
    graph = None
    try:
        graph = callgraph.load()
    except Exception:
        pass
    print("")
    print("Hook-Ereignisse: %d (Finals: %d)" % (len(found), len(finals)))
    for ev in found[: (len(found) if args.verbose else 20)]:
        fn = graph.label(ev["addr"]) if graph is not None else ""
        print("  %-12s %s  hits=%-6d %s %s" % (
            ev["callback"], common.hexs(ev["addr"]), ev["hits"], fn, ev["rest"]))
    if len(found) > 20 and not args.verbose:
        print("  ... %d weitere (--verbose)" % (len(found) - 20))
        print("")
    for (addr, cb), hits in finals.items():
        fn = graph.label(int(addr, 16)) if graph is not None else ""
        print("FINAL %-12s %s  hits=%d  %s" % (cb, common.hexs(int(addr, 16)),
                                               hits, fn))
    if args.json_out:
        payload = {"env": env, "events": found,
                   "finals": [{"callback": cb, "addr": common.hexs(int(a, 16)),
                               "hits": h} for (a, cb), h in finals.items()],
                   "out": str(result.out_path)}
        Path(args.json_out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json_out).write_text(json.dumps(payload, indent=2,
                                                  ensure_ascii=False),
                                       encoding="utf-8")
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(
        prog="hotpatch",
        description="Adresse->C-Callback per HLE_HOOK ohne Rebuild")
    sub = p.add_subparsers(dest="cmd", required=True)
    l = sub.add_parser("list", help="Callbacks anzeigen")
    l.set_defaults(func=cmd_list)
    r = sub.add_parser("run", help="Port mit Hooks starten")
    r.add_argument("--hook", action="append",
                   help="ADRESSE=CALLBACK (mehrfach oder Komma-getrennt)")
    r.add_argument("--exe", choices=["boot", "port"], default="boot")
    r.add_argument("--arg", action="append")
    r.add_argument("--env", action="append")
    r.add_argument("--timeout", type=float, default=10.0)
    r.add_argument("--until", action="append")
    r.add_argument("--max", type=int, help="HLE_HOOK_MAX")
    r.add_argument("--value", help="HLE_HOOK_VALUE (hex) fuer set_r0")
    r.add_argument("--label")
    r.add_argument("--json-out")
    r.add_argument("--verbose", action="store_true")
    r.set_defaults(func=cmd_run)
    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
