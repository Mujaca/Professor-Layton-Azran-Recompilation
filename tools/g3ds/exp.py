"""guest-exp: kleine Experiment-Registry (JSON) fuer A/B-Messungen."""
from __future__ import annotations

import hashlib
import json
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from . import common, runner

REGISTRY = common.OUT / "experiments.json"


def load(path: Optional[Path] = None) -> dict:
    path = Path(path) if path else REGISTRY
    if not path.is_file():
        return {"version": 1, "experiments": []}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        common.fail("Registry kaputt: %s (%s)" % (path, exc))
    if "experiments" not in data:
        data["experiments"] = []
    return data


def save(data: dict, path: Optional[Path] = None) -> Path:
    path = Path(path) if path else REGISTRY
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")
    return path


def signature(spec: runner.RunSpec) -> str:
    payload = {
        "exe": str(spec.exe),
        "args": list(spec.args),
        "env": dict(sorted(spec.env.items())),
        "timeout": round(float(spec.timeout), 3),
        "until": list(spec.until_text),
    }
    raw = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha1(raw).hexdigest()[:16]


def metrics(result: runner.RunResult) -> dict:
    return {
        "status": result.status,
        "until_hits": dict(sorted(result.until_hits.items())),
        "ipc_total": sum(result.ipc.values()),
        "srvnames": list(result.srvnames),
        "fatal": result.fatal,
        "perf_rate": result.perf_rate,
    }


def _metric_key(m: dict) -> str:
    return json.dumps(m, sort_keys=True, separators=(",", ":"))


def _stable_metrics(m: dict) -> dict:
    """Nur reproduzierbare Felder: IPC-Zaehler haengen vom Kill-Zeitpunkt ab."""
    return {
        "status": m.get("status"),
        "until_hits": m.get("until_hits"),
        "srvnames": m.get("srvnames"),
        "fatal": m.get("fatal"),
    }


def record(data: dict, spec: runner.RunSpec, hypothesis: str,
           repeat: int = 1, note: str = "", force: bool = False,
           path: Optional[Path] = None) -> Tuple[dict, bool]:
    sig = signature(spec)
    for old in data["experiments"]:
        if old.get("signature") == sig and not force:
            return old, False
    runs = []
    for i in range(max(1, repeat)):
        one = runner.RunSpec(
            label="%s_r%d" % (spec.label, i + 1),
            exe=spec.exe, args=list(spec.args), env=dict(spec.env),
            timeout=spec.timeout, until=list(spec.until),
            until_text=list(spec.until_text), cwd=spec.cwd)
        result = runner.run_one(one)
        runs.append({
            "label": one.label,
            "status": result.status,
            "runtime": round(result.runtime, 3),
            "metrics": metrics(result),
            "out": str(result.out_path),
            "err": str(result.err_path),
        })
    keys = [_metric_key(_stable_metrics(r["metrics"])) for r in runs]
    deterministic = len(set(keys)) <= 1
    ipc_values = [r["metrics"]["ipc_total"] for r in runs]
    ipc_stable = len(set(ipc_values)) <= 1
    entry = {
        "id": "exp-%s-%s" % (datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S"),
                             sig[:6]),
        "ts": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "commit": common.git_commit(),
        "hypothesis": hypothesis,
        "note": note,
        "signature": sig,
        "spec": {
            "exe": str(spec.exe),
            "args": list(spec.args),
            "env": dict(sorted(spec.env.items())),
            "timeout": spec.timeout,
            "until": list(spec.until_text),
        },
        "repeat": repeat,
        "deterministic": deterministic,
        "deterministic_fields": ["status", "until_hits", "srvnames", "fatal"],
        "ipc_identical": ipc_stable,
        "runs": runs,
    }
    data["experiments"].append(entry)
    save(data, path)
    return entry, True


def find(data: dict, spec: runner.RunSpec) -> Optional[dict]:
    sig = signature(spec)
    for old in data["experiments"]:
        if old.get("signature") == sig:
            return old
    return None


def brief(entry: dict) -> dict:
    runs = entry.get("runs") or []
    m = runs[0]["metrics"] if runs else {}
    return {
        "id": entry.get("id"),
        "ts": entry.get("ts"),
        "hypothesis": entry.get("hypothesis"),
        "deterministic": entry.get("deterministic"),
        "status": m.get("status"),
        "ipc_total": m.get("ipc_total"),
        "srv_count": len(m.get("srvnames") or []),
        "until_hits": m.get("until_hits"),
        "fatal": m.get("fatal"),
    }
