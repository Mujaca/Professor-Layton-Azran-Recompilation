"""matrix: A/B-Runner fuer HLE_*-Kombinationen.

Fuehrt out/boot.exe (oder port.exe) fuer jede Env-Kombination aus, wertet
die Meilenstein-Kette aus und gibt eine Tabelle plus den besten Lauf aus.
Laeufe sind parallelisierbar; Logs bleiben je Lauf erhalten.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Dict, List, Optional, Sequence

from . import common, milestones, runner


def _specs(args) -> List[runner.RunSpec]:
    base_env = common.env_dict(args.env)
    if args.deterministic:
        base_env.setdefault("HLE_TICK_MS", "2")
    combos = runner.parse_matrix(args.matrix or [])
    specs = []
    for ci, combo in enumerate(combos):
        env = dict(base_env)
        env.update(combo)
        label = args.label or "matrix"
        if len(combos) > 1:
            label = "%s_m%02d" % (label, ci)
        for rep in range(args.repeat):
            rlabel = label if args.repeat == 1 else "%s_r%02d" % (label, rep)
            specs.append(runner.RunSpec(
                label=rlabel,
                exe=(common.ROOT / "out" /
                     ("port.exe" if args.exe == "port" else "boot.exe")),
                args=list(args.arg or ["out/romfs"]),
                env=dict(env),
                timeout=args.timeout,
                until=[re.compile(p) for p in (args.until or [])],
                until_text=list(args.until or [])))
    return specs


def _score(checks: Sequence[milestones.Check]) -> tuple:
    passed = sum(1 for c in checks if c.ok)
    # Letzte erreichte Stufe zaehlt staerker als die reine Anzahl.
    last = 0
    for i, c in enumerate(checks):
        if c.ok:
            last = i + 1
    return (passed, last)


def _run_all(specs: Sequence[runner.RunSpec], jobs: int,
             shot: Optional[Path]):
    def one(spec):
        result = runner.run_one(spec)
        checks = milestones.evaluate(result.stdout_lines, shot)
        return result, checks

    if jobs > 1 and len(specs) > 1:
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            return list(pool.map(one, specs))
    return [one(s) for s in specs]


def _env_text(env: Dict[str, str]) -> str:
    skip = {"HLE_SRV_NAMES", "HLE_PERF"}
    return ",".join("%s=%s" % (k, v) for k, v in sorted(env.items())
                    if k not in skip) or "-"


def main(argv: Optional[List[str]] = None) -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    p = argparse.ArgumentParser(
        prog="matrix",
        description="A/B-Matrix fuer HLE_*-Schalter mit Meilenstein-Auswertung")
    p.add_argument("--matrix", action="append",
                   help="K=V1|V2,K2=V3 (kartesisch), mehrfach")
    p.add_argument("--env", action="append", help="feste Env NAME=WERT")
    p.add_argument("--repeat", type=int, default=1)
    p.add_argument("--jobs", type=int, default=1)
    p.add_argument("--timeout", type=float, default=20.0)
    p.add_argument("--exe", choices=["boot", "port"], default="boot")
    p.add_argument("--arg", action="append")
    p.add_argument("--until", action="append")
    p.add_argument("--label")
    p.add_argument("--deterministic", action="store_true",
                   help="HLE_TICK_MS=2 setzen")
    p.add_argument("--shot", default=str(common.ROOT / "out" / "shot.bmp"))
    p.add_argument("--json-out")
    p.add_argument("--verbose", action="store_true")
    args = p.parse_args(argv)

    specs = _specs(args)
    results = _run_all(specs, args.jobs, Path(args.shot) if args.shot else None)

    rows = []
    best = None
    best_score = (-1, -1)
    for result, checks in results:
        score = _score(checks)
        if score > best_score:
            best = (result, checks)
            best_score = score
        marks = "".join("+" if c.ok else "." for c in checks)
        rows.append((result.spec.label, _env_text(result.spec.env),
                     result.status, "%.1fs" % result.runtime,
                     str(len(result.srvnames)), str(sum(result.ipc.values())),
                     marks, str(result.fatal)))

    cols = ["LABEL", "ENV", "STATUS", "TIME", "SRV", "IPC", "M1..M9", "FATAL"]
    t = common.Table(cols, max_width=48)
    for row in rows:
        t.add(*row)
    print("Meilenstein-Legende: " + " ".join(
        "%d=%s" % (i + 1, c.id) for i, c in enumerate(
            milestones.evaluate([], None))))
    print(t.render())
    if best is not None:
        result, checks = best
        print("")
        print("Bester Lauf: %s  (%d/%d Meilensteine, %s)" % (
            result.spec.label, sum(1 for c in checks if c.ok), len(checks),
            result.out_path))
        for line in milestones.render(checks):
            print(line)

    # Determinismus: gleiche Meilenstein-Signatur bei Wiederholungen?
    if args.repeat > 1:
        groups: Dict[str, List[str]] = {}
        for result, checks in results:
            base = result.spec.label.rsplit("_r", 1)[0]
            sig = "".join("1" if c.ok else "0" for c in checks)
            groups.setdefault(base, []).append(sig)
        for base, sigs in groups.items():
            print("Determinismus %s: %s" % (
                base, "identisch" if len(set(sigs)) == 1 else "ABWEICHUNG %s" % sigs))

    if args.json_out:
        payload = []
        for result, checks in results:
            payload.append({
                "label": result.spec.label,
                "env": result.spec.env,
                "status": result.status,
                "runtime": round(result.runtime, 3),
                "out": str(result.out_path),
                "srvnames": result.srvnames,
                "ipc_total": sum(result.ipc.values()),
                "fatal": result.fatal,
                "milestones": [{"id": c.id, "ok": c.ok,
                                "evidence": c.evidence, "line": c.line_no,
                                "detail": c.detail} for c in checks],
                "passed": sum(1 for c in checks if c.ok),
            })
        Path(args.json_out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json_out).write_text(json.dumps(payload, indent=2,
                                                  ensure_ascii=False),
                                       encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
