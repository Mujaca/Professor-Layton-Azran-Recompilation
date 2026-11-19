"""Funktionsgrenzen und BL-Aufrufer aus build/gen_boot.c / code_decomp.bin."""
from __future__ import annotations

import bisect
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

from . import common

FN_RE = re.compile(rb"void (fn_([0-9A-F]{8}))\(ArmCpu \*c\) \{")


@dataclass(frozen=True)
class FuncInfo:
    start: int
    end: int
    name: str


class FunctionMap:
    def __init__(self, functions: List[FuncInfo]):
        self.functions = sorted(functions, key=lambda f: f.start)
        self.starts = [f.start for f in self.functions]

    def __len__(self):
        return len(self.functions)

    def containing(self, addr: int) -> Optional[FuncInfo]:
        i = bisect.bisect_right(self.starts, addr) - 1
        if i < 0:
            return None
        fn = self.functions[i]
        if fn.start <= addr < fn.end:
            return fn
        return None

    def name_at(self, addr: int) -> str:
        fn = self.containing(addr)
        return fn.name if fn else ("0x%08X" % addr)

    def label(self, addr: int) -> str:
        fn = self.containing(addr)
        if fn is None:
            return "0x%08X" % addr
        return "%s+0x%X" % (fn.name, addr - fn.start)


def _read_cache(cache_path: Path, key: dict, code_hi: int):
    if not cache_path.is_file():
        return None
    try:
        data = json.loads(cache_path.read_text(encoding="utf-8"))
    except Exception:
        return None
    if data.get("key") != key:
        return None
    try:
        funcs = [FuncInfo(int(a), min(int(b), code_hi), str(n))
                 for a, b, n in data["functions"]]
    except Exception:
        return None
    return funcs


def _load_cached(gen_path: Path, code_hi: int) -> List[FuncInfo]:
    try:
        stat = gen_path.stat()
    except OSError:
        common.fail("build/gen_boot.c fehlt: %s  (python tools/build_port.py)"
                    % gen_path)
    cache_path = common.ensure_out() / "genmap.json"
    key = {"gen_size": stat.st_size, "gen_mtime": int(stat.st_mtime)}
    cached = _read_cache(cache_path, key, code_hi)
    if cached is not None:
        return cached

    raw = gen_path.read_bytes()
    found = []
    for m in FN_RE.finditer(raw):
        addr = int(m.group(2), 16)
        if common.ARM_BASE <= addr < code_hi:
            found.append((addr, m.group(1).decode("ascii")))
    found.sort()
    funcs = []
    for i, (addr, name) in enumerate(found):
        end = found[i + 1][0] if i + 1 < len(found) else code_hi
        if end <= addr:
            continue
        funcs.append(FuncInfo(addr, end, name))
    try:
        cache_path.write_text(json.dumps(
            {"key": key,
             "functions": [[f.start, f.end, f.name] for f in funcs]},
            separators=(",", ":")), encoding="utf-8")
    except OSError:
        pass
    return funcs


def load_functions(gen_path: Optional[Path] = None,
                   code_hi: int = 0x00400000) -> FunctionMap:
    gen_path = Path(gen_path) if gen_path else common.GEN_BOOT
    return FunctionMap(_load_cached(gen_path, code_hi))


def bl_callers(code: bytes, fmap: FunctionMap, target: int,
               code_base: int = common.ARM_BASE) -> List[dict]:
    """Alle ARM-BL/BLX-Immediate-Aufrufer von *target* finden."""
    out = []
    n = len(code) // 4
    for i in range(n):
        off = i * 4
        w = int.from_bytes(code[off:off + 4], "little")
        site = code_base + off
        if (w & 0x0F000000) == 0x0B000000:          # BL
            imm = w & 0xFFFFFF
            if imm & 0x800000:
                imm -= 0x1000000
            dst = (site + 8 + imm * 4) & 0xFFFFFFFF
            if dst == target:
                out.append({"site": site, "target": dst, "func": fmap.containing(site)})
        elif (w & 0xFE000000) == 0xFA000000:        # BLX immediate
            imm = w & 0xFFFFFF
            h = (w >> 24) & 1
            if imm & 0x800000:
                imm -= 0x1000000
            dst = (site + 8 + imm * 4 + h * 2) & 0xFFFFFFFF
            if dst == target:
                out.append({"site": site, "target": dst, "func": fmap.containing(site)})
    return out
