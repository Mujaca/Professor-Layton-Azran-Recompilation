"""Funktions- und Block-Discovery fuer ARM11-Code (ARM + Thumb)."""
from dataclasses import dataclass, field
import capstone.arm as A

from . import isa


@dataclass
class Block:
    addr: int
    insns: list = field(default_factory=list)


@dataclass
class Function:
    addr: int
    thumb: bool = False
    source: str = ""
    blocks: list = field(default_factory=list)
    calls: set = field(default_factory=set)      # (addr, thumb)


@dataclass
class ScanResult:
    functions: dict
    visited: set


LDM_IDS = (A.ARM_INS_LDM, A.ARM_INS_LDMDA, A.ARM_INS_LDMDB, A.ARM_INS_LDMIB)
STUB_IDS = (A.ARM_INS_B, A.ARM_INS_BX, A.ARM_INS_BLX, A.ARM_INS_CBZ, A.ARM_INS_CBNZ)


def op0_is_pc(ins) -> bool:
    try:
        ops = isa.split_operands(ins.op_str)
        return bool(ops) and isa.is_pc(ops[0])
    except Exception:
        return False


def reg_operand(ins, idx=0) -> bool:
    try:
        return ins.operands[idx].type == A.ARM_OP_REG
    except Exception:
        return False


def is_terminator(ins) -> bool:
    if ins.id == A.ARM_INS_B:
        return True
    if ins.id in (A.ARM_INS_BX, A.ARM_INS_CBZ, A.ARM_INS_CBNZ):
        return True
    # BLX (Register und Immediate) ist immer ein Aufruf (setzt LR), kein Blockende.
    if ins.id == A.ARM_INS_POP:
        return "pc" in ins.op_str
    if ins.id in LDM_IDS:
        return "pc" in ins.op_str
    if ins.id in (A.ARM_INS_LDR, A.ARM_INS_MOV) and op0_is_pc(ins):
        return True
    if ins.id in (A.ARM_INS_ADD, A.ARM_INS_SUB) and op0_is_pc(ins):
        return True
    return False


def direct_target(ins):
    if ins.id in (A.ARM_INS_B, A.ARM_INS_BL, A.ARM_INS_BLX, A.ARM_INS_CBZ, A.ARM_INS_CBNZ):
        try:
            if ins.operands and ins.operands[0].type == A.ARM_OP_IMM:
                return ins.operands[0].imm
        except Exception:
            return None
    return None


def decode_function(data, fn, visited):
    # Modus automatisch erkennen, falls der Seed falsch geraten war
    if isa.decode(data, fn.addr, thumb=fn.thumb) is None:
        if isa.decode(data, fn.addr, thumb=not fn.thumb) is not None:
            fn.thumb = not fn.thumb
    todo = [fn.addr]
    seen = set()
    while todo:
        baddr = todo.pop(0)
        if baddr in seen:
            continue
        seen.add(baddr)
        blk = Block(addr=baddr)
        fn.blocks.append(blk)
        cur = baddr
        while True:
            ins = isa.decode(data, cur, thumb=fn.thumb)
            if ins is None:
                break
            blk.insns.append(ins)
            visited[cur] = ins.size
            if ins.id in (A.ARM_INS_BL, A.ARM_INS_BLX):
                tgt = direct_target(ins)
                if tgt is not None:
                    fn.calls.add((tgt, ins.id == A.ARM_INS_BLX or fn.thumb))
            nxt = cur + ins.size
            if is_terminator(ins):
                tgt = direct_target(ins)
                if tgt is not None and tgt not in seen:
                    todo.append(tgt)
                cond = ins.id in (A.ARM_INS_CBZ, A.ARM_INS_CBNZ)
                if (cond or (ins.id == A.ARM_INS_B and ins.cc != A.ARM_CC_AL)) and nxt not in seen:
                    todo.append(nxt)
                break
            cur = nxt
            if len(blk.insns) > 20000:
                break


def pointer_seeds(data, limit=40000):
    """Alle Woerter im Image, die auf Code zeigen, als Funktionskandidaten."""
    out = []
    base = isa.ARM_BASE
    end = base + len(data)
    for off in range(0, len(data) - 4, 4):
        v = int.from_bytes(data[off:off + 4], "little")
        addr = v & ~1
        if addr < base + 0x1000 or addr >= end:
            continue
        if isa.decode(data, addr) is None:
            continue
        out.append((addr, "pointer@%06X" % off, False))
        if len(out) >= limit:
            break
    return out


def scan(data, entries, follow_prologues=False, seed_pointers=False):
    functions = {}
    visited = {}
    queue = list(entries)

    def drain():
        while queue:
            addr, source, thumb = queue.pop(0)
            if addr in functions or addr < isa.ARM_BASE:
                continue
            fn = Function(addr=addr, thumb=thumb, source=source)
            functions[addr] = fn
            decode_function(data, fn, visited)
            for call, is_thumb in sorted(fn.calls):
                if call not in functions and call >= isa.ARM_BASE:
                    queue.append((call, f"bl@{addr:08X}", is_thumb))

    drain()

    if seed_pointers:
        extra = pointer_seeds(data)
        for e in extra:
            if e[0] not in functions:
                queue.append(e)
        drain()

    if follow_prologues:
        seeds = []
        for off in range(0, len(data) - 4, 4):
            addr = isa.ARM_BASE + off
            if addr in functions:
                continue
            ins = isa.decode(data, addr)
            if ins is None:
                continue
            m = ins.mnemonic
            if (m == "push" or m == "stmdb") and "lr" in ins.op_str:
                seeds.append(addr)
        for addr in seeds:
            if addr not in functions:
                queue.append((addr, "prologue", False))
        drain()

    return ScanResult(functions=functions, visited=visited)
