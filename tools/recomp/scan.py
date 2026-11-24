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
TBB_IDS = tuple(getattr(A, n) for n in ("ARM_INS_TBB", "ARM_INS_TBH")
                if getattr(A, n, None) is not None)


def _rd32(data, addr):
    off = addr - isa.ARM_BASE
    if off < 0 or off + 4 > len(data):
        return None
    return int.from_bytes(data[off:off + 4], "little")


def _code_ok(data, addr):
    addr &= ~1
    if addr < isa.ARM_BASE or addr >= isa.ARM_BASE + len(data):
        return False
    return isa.decode(data, addr) is not None


def ldr_pc_table(data, ins, thumb):
    """Ziele und Tabellenende fuer `ldr pc, [pc, rN, lsl #2]`.

    Die Tabelle steht direkt hinter der Instruktion (ARM: pc+8,
    Thumb: Align(pc+4,4)). Eintraege sind absolute Codezeiger; die Tabelle
    endet beim ersten Wort, das auf keinen dekodierbaren Code zeigt.
    """
    op = ins.op_str.replace(" ", "").lower()
    if "pc" not in op or "lsl#2" not in op:
        return [], None
    base = (ins.address + 8) if not thumb else ((ins.address + 4) & ~3)
    targets = []
    end = base
    for i in range(256):
        v = _rd32(data, base + 4 * i)
        if v is None or not _code_ok(data, v):
            break
        targets.append((v & ~1, bool(v & 1) if thumb else thumb))
        end = base + 4 * (i + 1)
    return (targets, end) if targets else ([], None)


def tbb_table(data, ins):
    """Ziele und Tabellenende fuer Thumb tbb/tbh [pc, rN]."""
    base = (ins.address + 4) & ~3
    half = ins.id == getattr(A, "ARM_INS_TBH", -1)
    unit = 2 if half else 1
    targets = []
    end = base
    for i in range(512):
        off = base + unit * i
        o = off - isa.ARM_BASE
        if o < 0 or o + unit > len(data):
            break
        raw = int.from_bytes(data[o:o + unit], "little")
        tgt = base + 2 * raw
        if not _code_ok(data, tgt):
            break
        targets.append((tgt & ~1, True))
        end = off + unit
    return (targets, end) if targets else ([], None)



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
    if ins.id in TBB_IDS:
        return True
    return False


def direct_target(ins):
    if ins.id in (A.ARM_INS_B, A.ARM_INS_BL, A.ARM_INS_BLX, A.ARM_INS_CBZ, A.ARM_INS_CBNZ):
        try:
            # CBZ/CBNZ: Operand 0 ist das Register, Operand 1 das Ziel.
            idx = 1 if ins.id in (A.ARM_INS_CBZ, A.ARM_INS_CBNZ) else 0
            if len(ins.operands) > idx and ins.operands[idx].type == A.ARM_OP_IMM:
                return ins.operands[idx].imm
        except Exception:
            return None
    return None


def ldrpc_literal_target(ins, caller_thumb, data):
    """Statisches Ziel eines `ldr pc, [pc, #imm]` (Veneer/Tail-Branch).

    Die geladene Adresse ist ein Sprungziel; Bit 0 waehlt wie bei bx den
    Instruktionssatz (z. B. ARM-Veneer 0x24CCEC -> Thumb 0x100373).
    Rueckgabe: (ziel, thumb) oder None.
    """
    if ins.id != A.ARM_INS_LDR or not ins.operands:
        return None
    if (ins.operands[0].type != A.ARM_OP_REG
            or ins.operands[0].reg != A.ARM_REG_PC):
        return None
    if len(ins.operands) < 2 or ins.operands[1].type != A.ARM_OP_MEM:
        return None
    mem = ins.operands[1].mem
    if mem.base != A.ARM_REG_PC or mem.index != 0:
        return None
    pc = (ins.address + 4) & ~3 if caller_thumb else ins.address + 8
    off = pc + mem.disp - isa.ARM_BASE
    if off < 0 or off + 4 > len(data):
        return None
    value = int.from_bytes(data[off:off + 4], "little")
    target = value & ~1
    thumb = bool(value & 1)
    if target < isa.ARM_BASE or target - isa.ARM_BASE >= len(data):
        return None
    if isa.decode(data, target, thumb=thumb) is None:
        return None
    return target, thumb


def call_target_thumb(ins, caller_thumb):
    """Modus des Aufrufziels: BL behaelt den Modus, BLX wechselt ihn.

    (Thumb-BLX springt in ARM-Code und umgekehrt; emit.py setzt c->t
    entsprechend. Vorher wurde jedes BLX-Ziel als Thumb geseedet, wodurch
    ARM-Funktionen im 0x29xxxx-Bereich als Thumb dekodiert wurden.)
    """
    if ins.id == A.ARM_INS_BLX:
        return not caller_thumb
    return caller_thumb


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
        const_reg = {}
        while True:
            ins = isa.decode(data, cur, thumb=fn.thumb)
            if ins is None:
                break
            blk.insns.append(ins)
            visited[cur] = ins.size
            ldrpc = ldrpc_literal_target(ins, fn.thumb, data)
            if ldrpc is not None:
                fn.calls.add(ldrpc)
            try:
                ops = isa.split_operands(ins.op_str)
            except Exception:
                ops = []
            # `adr rX, ziel` dekodiert capstone je nach Modus als
            # `add rX, pc, #imm`; fuer `mov pc, rX` ist das Ziel statisch.
            if (ins.id in (A.ARM_INS_ADD, A.ARM_INS_SUB) and len(ops) >= 3
                    and isa.reg_num(ops[1]) == 15):
                rd = isa.reg_num(ops[0])
                imm = isa.imm_value(ops[2])
                if rd is not None and imm is not None:
                    d = imm if ins.id == A.ARM_INS_ADD else -imm
                    pcbase = ((cur + 4) & ~3) if fn.thumb else (cur + 8)
                    const_reg[rd] = pcbase + d
            if ins.id == A.ARM_INS_MOV and len(ops) >= 2 and isa.is_pc(ops[0]):
                src = isa.reg_num(ops[1])
                if src in const_reg and const_reg[src] not in seen:
                    todo.append(const_reg[src])
            if ins.id in (A.ARM_INS_BL, A.ARM_INS_BLX):
                tgt = direct_target(ins)
                if tgt is not None:
                    fn.calls.add((tgt, call_target_thumb(ins, fn.thumb)))
            nxt = cur + ins.size
            if is_terminator(ins):
                tgt = direct_target(ins)
                if tgt is not None and tgt not in seen:
                    todo.append(tgt)
                # Bedingt ausgefuehrte Terminatoren (bxeq lr, popeq {pc},
                # ldrne pc, bne, cbz ...) haben immer einen Fallthrough; sonst
                # fehlt der Folgeblock und die Funktion endet vorzeitig.
                cond = (ins.id in (A.ARM_INS_CBZ, A.ARM_INS_CBNZ)
                        or ins.cc != A.ARM_CC_AL)
                fall = nxt
                extra = []
                if ins.id == A.ARM_INS_LDR and op0_is_pc(ins):
                    # Sprungtabelle steht direkt hinter der Instruktion.
                    extra, table_end = ldr_pc_table(data, ins, fn.thumb)
                    if table_end is not None:
                        fall = table_end
                elif ins.id in TBB_IDS:
                    extra, table_end = tbb_table(data, ins)
                    if table_end is not None:
                        fall = table_end
                elif (ins.id in (A.ARM_INS_ADD, A.ARM_INS_SUB)
                      and op0_is_pc(ins)):
                    # `add pc, pc, rN, lsl #2`: ab pc+8 folgt eine Tabelle
                    # aus `b`-Instruktionen (4-Byte-Slots). Jeden Slot als
                    # eigenen Block anlegen, damit der dynamische Sprung ein
                    # Ziel per switch/goto findet.
                    if nxt not in seen:
                        todo.append(nxt)
                    slot = nxt + 4
                    for _ in range(64):
                        s = isa.decode(data, slot, fn.thumb)
                        if s is None or s.id != A.ARM_INS_B:
                            break
                        if direct_target(s) is None:
                            break
                        if slot not in seen:
                            todo.append(slot)
                        slot += 4
                for e, ethumb in extra:
                    if e not in seen:
                        todo.append(e)
                if cond and fall not in seen:
                    todo.append(fall)
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
