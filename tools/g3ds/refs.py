"""guest-refs: Referenzen auf eine Gastadresse finden.

Unterstuetzt:
  * LDR-Literal (pc+8+imm12) und die zugehoerigen Literal-Pools
  * STR/LDR mit Immediate-Offset, wenn die Basis-Registerkonstante
    innerhalb der Funktion symbolisch aufloesbar ist
    (z. B. str r0,[r4,#0x154] mit r4=0x40B370)
  * LDM/STM mit aufloesbarer Basis
  * ARM-BL/BLX-Aufrufer
  * Funktionsgrenzen aus build/gen_boot.c
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, List, Optional

from . import common, disasm, genmap


KIND_ORDER = {"ldr-lit": 0, "pool": 1, "store": 2, "load": 3, "call": 4}


@dataclass
class RefEvent:
    addr: int
    kind: str
    target: int
    text: str
    func_start: int
    func_name: str
    base: Optional[int] = None
    disp: int = 0
    width: int = 4
    lit_addr: Optional[int] = None
    lit_value: Optional[int] = None
    value: Optional[int] = None          # gelesener/geschriebener Wert, wenn bekannt
    caller: Optional[str] = None


@dataclass
class RefResult:
    target: int
    events: List[RefEvent]
    callers: List[RefEvent]
    functions_scanned: int
    instructions_scanned: int
    ldr_literals: int
    resolved_accesses: int
    target_func: Optional[genmap.FuncInfo]


def _classify(mem) -> str:
    if mem.is_store:
        return "store"
    return "load"


def _make_event(ins, fn, kind, target, base=None, disp=0, width=4):
    return RefEvent(addr=ins.addr, kind=kind, target=target, text=ins.text,
                    func_start=fn.start, func_name=fn.name, base=base,
                    disp=disp, width=width,
                    lit_addr=getattr(ins.mem, "lit_addr", None),
                    lit_value=getattr(ins.mem, "lit_value", None))


def _scan_function(data: bytes, fn: genmap.FuncInfo, target: int,
                   stats: Dict[str, int]) -> List[RefEvent]:
    insns = disasm.decode_range(data, fn.start, fn.end)
    events: List[RefEvent] = []
    last_end = fn.start
    for i, ins in enumerate(insns):
        last_end = max(last_end, ins.addr + ins.size)
        stats["instructions"] += 1
        mem = ins.mem

        if mem is not None and mem.pc_relative and mem.is_load and not mem.index:
            stats["ldr_literals"] += 1
            lit_addr = (ins.addr + 8 + mem.disp) & 0xFFFFFFFF
            lit_val = common.u32(data, lit_addr - common.ARM_BASE)
            # Literal-Pool-Wort zeigt direkt auf das Ziel
            if lit_val == target or (lit_val & ~1) == (target & ~1):
                ev = _make_event(ins, fn, "ldr-lit", target)
                ev.lit_addr = lit_addr
                ev.lit_value = lit_val
                events.append(ev)
            # LDR greift direkt auf die Zieladresse zu (pc-relativ)
            if lit_addr == target:
                ev = _make_event(ins, fn, "pool", target)
                ev.lit_addr = lit_addr
                ev.lit_value = lit_val
                events.append(ev)

        if mem is not None and not mem.pc_relative:
            stats["resolved_accesses"] += 1
            base_val = disasm.resolve_reg(insns, i, mem.base)
            if base_val is None:
                continue
            addend = mem.disp
            if mem.index:
                idx_val = disasm.resolve_reg(insns, i, mem.index)
                if idx_val is None:
                    continue
                addend += idx_val << (mem.shift or 0)
            if (ins.reglist and mem.is_load) or (ins.reglist and mem.is_store):
                for k, _reg in enumerate(ins.reglist):
                    ea = (base_val + addend + k * 4) & 0xFFFFFFFF
                    if ea == target:
                        ev = _make_event(ins, fn, _classify(mem), target,
                                         base=base_val, disp=addend + k * 4,
                                         width=4)
                        events.append(ev)
            else:
                ea = (base_val + addend) & 0xFFFFFFFF
                if ea == target:
                    ev = _make_event(ins, fn, _classify(mem), target,
                                     base=base_val, disp=addend, width=mem.width)
                    events.append(ev)

        if ins.bl_target is not None and (ins.bl_target & ~1) == (target & ~1):
            ev = RefEvent(addr=ins.addr, kind="call", target=ins.bl_target,
                          text=ins.text, func_start=fn.start,
                          func_name=fn.name, caller=fn.name)
            events.append(ev)

    # Trailing Literal-Pool: Woerter nach der letzten Instruktion.
    pool_start = (last_end + 3) & ~3
    for a in range(pool_start, fn.end, 4):
        value = common.u32(data, a - common.ARM_BASE)
        if value == target or (value & ~1) == (target & ~1):
            ev = RefEvent(addr=a, kind="pool", target=target,
                          text=".word 0x%08X" % value,
                          func_start=fn.start, func_name=fn.name,
                          lit_addr=a, lit_value=value)
            events.append(ev)
    return events


def find_refs(target: int, kind: str = "all", data: Optional[bytes] = None,
              fmap: Optional[genmap.FunctionMap] = None,
              progress=None) -> RefResult:
    data = data if data is not None else common.CODE_BIN.read_bytes()
    code_hi = common.ARM_BASE + len(data)
    fmap = fmap or genmap.load_functions(code_hi=code_hi)
    stats = {"instructions": 0, "ldr_literals": 0, "resolved_accesses": 0}
    all_events: List[RefEvent] = []
    for n, fn in enumerate(fmap.functions):
        all_events.extend(_scan_function(data, fn, target, stats))
        if progress is not None and (n % 1000 == 0):
            progress(n, len(fmap.functions))

    # Doppelte Ereignisse (z. B. pool + ldr-lit am selben Wort) entfernen.
    seen = set()
    uniq: List[RefEvent] = []
    for ev in all_events:
        key = (ev.addr, ev.kind, ev.disp)
        if key in seen:
            continue
        seen.add(key)
        uniq.append(ev)
    uniq.sort(key=lambda e: (e.addr, e.kind))

    callers = [e for e in uniq if e.kind == "call"]
    if kind == "lite":
        events = [e for e in uniq if e.kind in ("ldr-lit", "pool")]
    elif kind == "store":
        events = [e for e in uniq if e.kind == "store"]
    elif kind == "load":
        events = [e for e in uniq if e.kind == "load"]
    else:
        events = [e for e in uniq if e.kind != "call"]
    target_func = fmap.containing(target)
    return RefResult(target=target, events=events, callers=callers,
                     functions_scanned=len(fmap),
                     instructions_scanned=stats["instructions"],
                     ldr_literals=stats["ldr_literals"],
                     resolved_accesses=stats["resolved_accesses"],
                     target_func=target_func)


def context_for(event: RefEvent, data: bytes, radius: int = 3) -> List[str]:
    """Drei Zeilen Kontext um ein Ereignis (Instruktionsadressen)."""
    lo = max(event.func_start, event.addr - radius * 4)
    hi = event.addr + (radius + 1) * 4
    insns = disasm.decode_range(data, lo, hi)
    lines = []
    for ins in insns:
        mark = ">>" if ins.addr == event.addr else "  "
        lines.append("%s %08X  %s" % (mark, ins.addr, ins.text))
    return lines
