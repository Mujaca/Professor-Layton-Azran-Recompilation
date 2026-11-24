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


def pc_relative_const(ins, addr, thumb):
    """(Zielregister, Wert) fuer `add/sub rN, pc, #imm` bzw. `adr rN, #imm`.

    ARM nutzt PC = addr + 8; Thumb Align(addr + 4, 4) (REFERENCE.md, ADR).
    """
    try:
        if ins.id == A.ARM_INS_ADR and len(ins.operands) >= 2:
            d, i = ins.operands[0], ins.operands[1]
            if d.type == A.ARM_OP_REG and i.type == A.ARM_OP_IMM:
                return d.reg, i.imm & 0xFFFFFFFF
        if ins.id in (A.ARM_INS_ADD, A.ARM_INS_SUB) and len(ins.operands) >= 3:
            d, s, i = ins.operands[0], ins.operands[1], ins.operands[2]
            if (d.type == A.ARM_OP_REG and s.type == A.ARM_OP_REG
                    and s.reg == A.ARM_REG_PC and i.type == A.ARM_OP_IMM):
                base = addr + (4 if thumb else 8)
                if thumb:
                    base &= ~3
                if ins.id == A.ARM_INS_SUB:
                    return d.reg, (base - i.imm) & 0xFFFFFFFF
                return d.reg, (base + i.imm) & 0xFFFFFFFF
    except Exception:
        return None
    return None


def pc_literal_target(ins, addr, thumb, data, image_base=0x100000):
    """Wert eines statischen `ldr pc, [pc, #imm]`-Stubs (ARM/Thumb).

    Solche Stubs legt der Linker als Sprungtabelle an (z. B.
    0x24CCEC: `ldr pc,[pc,#-4]`, Literal 0x00100373). Nur der reine
    PC-Basis-Fall ohne Indexregister; `ldr pc,[pc,rN,lsl#2]` behandelt
    cli.py als Switch-Tabelle.
    """
    try:
        if ins.id != A.ARM_INS_LDR or len(ins.operands) < 2:
            return None
        d, m = ins.operands[0], ins.operands[1]
        if d.type != A.ARM_OP_REG or d.reg != A.ARM_REG_PC:
            return None
        if m.type != A.ARM_OP_MEM or m.mem.base != A.ARM_REG_PC or m.mem.index != 0:
            return None
        base = addr + (4 if thumb else 8)
        if thumb:
            base &= ~3
        off = base + m.mem.disp - image_base
        if off < 0 or off + 4 > len(data):
            return None
        return int.from_bytes(data[off:off + 4], "little")
    except Exception:
        return None


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
        # PC-relative Konstanten im Block (Interworking-Veneer):
        #   add rN, pc, #imm  /  adr rN, #imm   ->  rN = Zieladresse
        pc_const = {}
        while True:
            ins = isa.decode(data, cur, thumb=fn.thumb)
            if ins is None:
                break
            blk.insns.append(ins)
            visited[cur] = ins.size
            value = pc_relative_const(ins, cur, fn.thumb)
            if value is not None:
                pc_const[value[0]] = value[1]
            if ins.id in (A.ARM_INS_BL, A.ARM_INS_BLX):
                tgt = direct_target(ins)
                if tgt is not None:
                    fn.calls.add((tgt, call_target_thumb(ins, fn.thumb)))
            if ins.id in (A.ARM_INS_BX, A.ARM_INS_BLX) and reg_operand(ins):
                # Sprung ueber eine berechnete PC-Konstante: Die Adresse steht
                # statisch fest. Der Linker baut so ARM->Thumb-Veneeren
                # (`add r12, pc, #1; bx r12`); das Thumb-Ziel braucht einen
                # eigenen Funktions-Seed, sonst landet der Aufruf im No-op.
                val = pc_const.get(ins.operands[0].reg)
                if val is not None and (val & ~1) >= isa.ARM_BASE:
                    fn.calls.add((val & ~1, bool(val & 1)))
            if ins.id == A.ARM_INS_LDR and op0_is_pc(ins):
                # Linker-Stub `ldr pc, [pc, #imm]`: Literal steht daneben.
                # Ein ungerades Ziel ist Thumb-Code (0x24CCEC -> 0x100373) und
                # braucht einen eigenen Funktions-Seed, weil der Emitter es
                # ueber arm_call_guest ausfuehrt.
                lit = pc_literal_target(ins, cur, fn.thumb, data)
                if lit is not None and (lit & 1) and (lit & ~1) >= isa.ARM_BASE:
                    fn.calls.add((lit & ~1, True))
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
                if cond and nxt not in seen:
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
