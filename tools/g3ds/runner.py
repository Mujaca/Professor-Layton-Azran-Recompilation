"""guest-run: Port-/Boot-Binary im Hintergrund starten und zusammenfassen."""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import threading
import time
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Sequence

from . import common

DEFAULT_ENV = {
    "HLE_SRV_NAMES": "1",
    "HLE_PERF": "1",
}

_IPC_RE = re.compile(
    r"^\[ipc\]\s+th=(\S+)\s+blk=\S+\s+(\S+)\s+cmd=0x([0-9A-Fa-f]{4})")
_SRV_RE = re.compile(r'^\[srvname\]\s+"([^"]+)"')
_WD_RE = re.compile(r"^\[wd\]\s+thread\s+([0-9A-Fa-f]+):\s+pc=0x([0-9A-Fa-f]+)\s+lr=0x([0-9A-Fa-f]+)")
_TD_RE = re.compile(r"^\[tdump\]\s+\d+\s+pc=([0-9A-Fa-f]+)\s+lr=([0-9A-Fa-f]+)")
_PERF_RE = re.compile(r"^\[perf\]\s+(\d+)\s+blocks in\s+(\d+)\s+ms\s+\(([0-9.]+)/s\)")
_QUEUE_RE = re.compile(r"^\[queue\]\s+elems=(\d+)")
_FATAL_RE = re.compile(r"fatal|FATAL|FEHLER")


@dataclass
class RunSpec:
    label: str
    exe: Path
    args: List[str]
    env: Dict[str, str]
    timeout: float
    until: List[re.Pattern]
    until_text: List[str] = field(default_factory=list)
    cwd: Path = common.ROOT
    run_dir: Optional[Path] = None


@dataclass
class RunResult:
    spec: RunSpec
    returncode: Optional[int]
    status: str
    runtime: float
    until_hits: Dict[str, int]
    until_lines: List[str]
    out_path: Path
    err_path: Path
    ipc: Counter
    srvnames: List[str]
    threads: Dict[str, tuple]
    perf_rate: Optional[float]
    perf_blocks: Optional[int]
    queue_max: int
    fatal: int
    stdout_lines: List[str]
    meta_path: Path

    def summary_lines(self, verbose: bool = False) -> List[str]:
        s = self.spec
        lines = []
        lines.append("Lauf %s: %s%s" % (
            s.label, s.exe.name,
            (" " + " ".join(s.args)) if s.args else ""))
        lines.append("Status: %s nach %.1fs (Timeout %.0fs)"
                     % (self.status, self.runtime, s.timeout))
        if s.until_text:
            total = sum(self.until_hits.values())
            lines.append("Until: %d Treffer  %s" % (total, ", ".join(
                "%s=%d" % (k, v) for k, v in self.until_hits.items())))
            if verbose:
                for h in self.until_lines[:8]:
                    lines.append("  " + h.rstrip()[:160])
        if self.ipc:
            total = sum(self.ipc.values())
            top = self.ipc.most_common(6 if not verbose else 30)
            lines.append("IPC: %d Ereignisse; top: %s" % (
                total, ", ".join("%s cmd=0x%04X x%d" % (svc, cmd, n)
                                 for (svc, cmd), n in top)))
        else:
            lines.append("IPC: keine Ereignisse")
        if self.srvnames:
            shown = self.srvnames if verbose else self.srvnames[:14]
            lines.append("srvnames: %d  %s%s" % (
                len(self.srvnames), ", ".join(shown),
                "" if verbose or len(self.srvnames) == len(shown) else ", ..."))
        else:
            lines.append("srvnames: keine")
        if self.threads:
            shown = list(self.threads.items())
            if not verbose:
                shown = shown[:8]
            lines.append("Threads: %d  %s" % (len(self.threads), "  ".join(
                "t%s=0x%s" % (t, pc) for t, (pc, lr) in shown)))
        if self.perf_rate is not None:
            lines.append("Blockrate: %.0f Bloecke/s (%s Bloecke)"
                         % (self.perf_rate,
                            ("%.1fM" % (self.perf_blocks / 1e6))
                            if self.perf_blocks else "?"))
        else:
            lines.append("Blockrate: n/a (HLE_PERF braucht >= 1M Bloecke)")
        if self.queue_max:
            lines.append("Queue-Max: %d" % self.queue_max)
        lines.append("Fatal: %d" % self.fatal)
        lines.append("Logs: %s  |  %s" % (
            self.out_path.relative_to(common.ROOT) if self.out_path.is_relative_to(common.ROOT) else self.out_path,
            self.err_path.relative_to(common.ROOT) if self.err_path.is_relative_to(common.ROOT) else self.err_path))
        return lines


def _reader(pipe, path: Path, lines: List[str], lock: threading.Lock,
            spec: RunSpec, hits: Dict[str, int], evidence: List[str],
            hit_event: threading.Event):
    try:
        with path.open("w", encoding="utf-8", errors="replace", newline="") as f:
            for line in iter(pipe.readline, ""):
                f.write(line)
                with lock:
                    lines.append(line)
                    for pat, text in zip(spec.until, spec.until_text):
                        if pat.search(line):
                            hits[text] = hits.get(text, 0) + 1
                            if len(evidence) < 20:
                                evidence.append(line)
                            hit_event.set()
    except Exception as exc:
        with lock:
            lines.append("[g3ds] Lesefehler: %s\n" % exc)
    finally:
        try:
            pipe.close()
        except Exception:
            pass


def _kill_tree(proc: subprocess.Popen):
    try:
        if proc.poll() is not None:
            return
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2)
    except Exception:
        pass


def make_run_dir(label: str) -> Path:
    base = common.ensure_out() / "runs"
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", label)[:40] or "run"
    path = base / ("%s_%s_%d" % (stamp, safe, os.getpid()))
    n = 1
    while path.exists():
        path = base / ("%s_%s_%d_%d" % (stamp, safe, os.getpid(), n))
        n += 1
    path.mkdir(parents=True, exist_ok=True)
    return path


def run_one(spec: RunSpec) -> RunResult:
    if not spec.exe.is_file():
        common.fail("Binary fehlt: %s  (python tools\\build.py)" % spec.exe)
    run_dir = spec.run_dir or make_run_dir(spec.label)
    run_dir.mkdir(parents=True, exist_ok=True)
    out_path = run_dir / (spec.label + ".out.log")
    err_path = run_dir / (spec.label + ".err.log")
    meta_path = run_dir / "meta.json"

    env = dict(os.environ)
    env.update(DEFAULT_ENV)
    env.update(spec.env)
    cmd = [str(spec.exe)] + list(spec.args)
    start = time.monotonic()
    try:
        proc = subprocess.Popen(
            cmd, cwd=str(spec.cwd), env=env, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding="utf-8",
            errors="replace", bufsize=1)
    except OSError as exc:
        common.fail("Start fehlgeschlagen: %s (%s)" % (spec.exe, exc))

    out_lines: List[str] = []
    err_lines: List[str] = []
    lock = threading.Lock()
    hits: Dict[str, int] = {t: 0 for t in spec.until_text}
    evidence: List[str] = []
    hit_event = threading.Event()
    t_out = threading.Thread(target=_reader,
                             args=(proc.stdout, out_path, out_lines, lock,
                                   spec, hits, evidence, hit_event), daemon=True)
    t_err = threading.Thread(target=_reader,
                             args=(proc.stderr, err_path, err_lines, lock,
                                   spec, hits, evidence, hit_event), daemon=True)
    t_out.start()
    t_err.start()

    timed_out = False
    until_hit = False
    deadline = start + spec.timeout
    while proc.poll() is None:
        if hit_event.wait(0.05):
            until_hit = True
            _kill_tree(proc)
            break
        if time.monotonic() >= deadline:
            timed_out = True
            _kill_tree(proc)
            break
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        _kill_tree(proc)
    t_out.join(timeout=3)
    t_err.join(timeout=3)
    runtime = time.monotonic() - start
    if until_hit:
        status = "until"
    elif timed_out:
        status = "timeout"
    elif proc.returncode == 0:
        status = "exit0"
    else:
        status = "exit%d" % (proc.returncode or 0)

    all_lines = out_lines + err_lines
    ipc: Counter = Counter()
    srvnames: List[str] = []
    threads: Dict[str, tuple] = {}
    perf_rate = None
    perf_blocks = None
    queue_max = 0
    fatal = 0
    for line in all_lines:
        m = _IPC_RE.match(line)
        if m:
            ipc[(m.group(2), int(m.group(3), 16))] += 1
            continue
        m = _SRV_RE.match(line)
        if m:
            name = m.group(1)
            if name not in srvnames:
                srvnames.append(name)
            continue
        m = _WD_RE.match(line)
        if m:
            threads[m.group(1)] = (m.group(2), m.group(3))
            continue
        m = _PERF_RE.match(line)
        if m:
            perf_blocks = int(m.group(1))
            perf_rate = float(m.group(3))
            continue
        m = _QUEUE_RE.match(line)
        if m:
            queue_max = max(queue_max, int(m.group(1)))
            continue
        if _FATAL_RE.search(line):
            fatal += 1

    result = RunResult(
        spec=spec, returncode=proc.returncode, status=status, runtime=runtime,
        until_hits=hits, until_lines=evidence, out_path=out_path,
        err_path=err_path, ipc=ipc, srvnames=srvnames, threads=threads,
        perf_rate=perf_rate, perf_blocks=perf_blocks, queue_max=queue_max,
        fatal=fatal, stdout_lines=out_lines + err_lines, meta_path=meta_path)

    meta = {
        "label": spec.label,
        "exe": str(spec.exe),
        "args": spec.args,
        "env": spec.env,
        "timeout": spec.timeout,
        "until": spec.until_text,
        "status": status,
        "returncode": proc.returncode,
        "runtime": round(runtime, 3),
        "until_hits": hits,
        "ipc": {"%s|0x%04X" % k: v for k, v in ipc.items()},
        "srvnames": srvnames,
        "threads": {k: list(v) for k, v in threads.items()},
        "perf_rate": perf_rate,
        "perf_blocks": perf_blocks,
        "fatal": fatal,
        "out": str(out_path),
        "err": str(err_path),
    }
    try:
        meta_path.write_text(json.dumps(meta, indent=2), encoding="utf-8")
    except OSError:
        pass
    return result


def parse_matrix(items: Sequence[str]) -> List[Dict[str, str]]:
    """--matrix "K=V1|V2,K2=V3" -> Liste von Env-Dicts (kartesisch)."""
    if not items:
        return [{}]
    combos: List[Dict[str, str]] = [{}]
    for item in items:
        axes = []
        for part in item.split(","):
            part = part.strip()
            if not part:
                continue
            if "=" not in part:
                common.fail("--matrix erwartet K=V1|V2, bekam %r" % part)
            key, values = part.split("=", 1)
            axes.append((key.strip(), values.split("|")))
        new = []
        for base in combos:
            for key, values in axes:
                for value in values:
                    d = dict(base)
                    d[key] = value
                    new.append(d)
        combos = new
    return combos


def matrix_table(results: Sequence[RunResult]) -> str:
    cols = ["LABEL", "ENV", "STATUS", "UNTIL", "SRV", "IPC", "TIME"]
    rows = []
    for r in results:
        env = ",".join("%s=%s" % (k, v) for k, v in sorted(r.spec.env.items())
                       if k not in DEFAULT_ENV)
        rows.append([
            r.spec.label,
            env or "-",
            r.status,
            str(sum(r.until_hits.values())) if r.spec.until_text else "-",
            str(len(r.srvnames)),
            str(sum(r.ipc.values())),
            "%.1fs" % r.runtime,
        ])
    return common.Table(cols).render() if not rows else _render_rows(cols, rows)


def _render_rows(cols, rows):
    t = common.Table(cols)
    for row in rows:
        t.add(*row)
    return t.render()
