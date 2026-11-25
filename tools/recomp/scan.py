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


def _code_ok(data, addr, thumb=False):
    addr &= ~1
    if addr < isa.ARM_BASE or addr >= isa.ARM_BASE + len(data):
        return False
    return isa.decode(data, addr, thumb=thumb) is not None


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


def offset_table_targets(data, base, thumb):
    """Ziele einer Tabelle aus relativen Offsets: `ldr rX,[rB,rI,lsl #2]`
    mit `rB = pc - imm` direkt danach `add pc, rX, rB`.

    Solche Tabellen (z. B. Format-Handler eines Bilddecoders) stehen im
    Image als 32-Bit-Offsets relativ zum Tabellenanfang; die Eintraege sind
    oft Code-Fragmente VOR dem Funktionsanfang. Ohne eigenen Funktions-Seed
    landet der dynamische Sprung im No-op bzw. in arm_call_guest ohne Ziel.
    Gelesen wird bis zum ersten Eintrag, der auf keinen dekodierbaren Code
    zeigt.
    """
    out = []
    for i in range(128):
        off = base + 4 * i
        v = _rd32(data, off)
        if v is None:
            break
        tgt = (base + v) & 0xFFFFFFFF
        if tgt < isa.ARM_BASE or not _code_ok(data, tgt, thumb):
            break
        # Eintraege, die in die Tabelle selbst zeigen, sind Daten (z. B. zwei
        # Default-Slots vor den eigentlichen Handlern). Sie duerfen die Suche
        # nicht beenden: im Bilddecoder folgen nach den beiden Selbstverweisen
        # erst die 8-/16-Bit-Handler 0x16AA08/0x16AA20 bzw. 0x16AF84/0x16AF9C.
        if base <= tgt < base + 4 * i + 4:
            continue
        out.append((tgt & ~1, thumb))
    return out


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
        if not _code_ok(data, tgt, thumb=True):
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


def lr_const_after(ins, lr_const, thumb):
    """Fortschreibung eines statisch bekannten LR-Werts ueber eine Instruktion.

    `add lr, pc, #imm` / `sub lr, pc, #imm` (auch `adr lr`) setzen LR auf eine
    Konstante; jede andere Instruktion, die LR schreibt (mov, ldr, pop, bl ...),
    macht den Wert unbekannt (None).
    """
    try:
        ops = isa.split_operands(ins.op_str)
    except Exception:
        ops = []
    if (ins.id in (A.ARM_INS_ADD, A.ARM_INS_SUB) and len(ops) >= 3
            and isa.reg_num(ops[0]) == 14 and isa.reg_num(ops[1]) == 15
            and ins.cc == A.ARM_CC_AL):
        imm = isa.imm_value(ops[2])
        if imm is not None:
            base = ((ins.address + 4) & ~3) if thumb else (ins.address + 8)
            return (base + (imm if ins.id == A.ARM_INS_ADD else -imm)) & 0xFFFFFFFF
    if (lr_const is not None and ins.id in (A.ARM_INS_ADD, A.ARM_INS_SUB)
            and len(ops) >= 3 and isa.reg_num(ops[0]) == 14
            and isa.reg_num(ops[1]) == 14 and ins.cc == A.ARM_CC_AL):
        # Zweischritt-Adressbildung: `add lr, pc, #hi; add lr, lr, #lo`.
        imm = isa.imm_value(ops[2])
        if imm is not None:
            return (lr_const + (imm if ins.id == A.ARM_INS_ADD else -imm)) & 0xFFFFFFFF
    try:
        _, written = ins.regs_access()
    except Exception:
        written = ()
    if A.ARM_REG_LR in written or ins.id in (A.ARM_INS_BL, A.ARM_INS_BLX):
        return None
    return lr_const


def is_pseudo_call(ins, lr_const):
    """`add lr, pc, #k ... b<cond> ziel` mit LR = nahe Fortsetzung hinter dem `b`.

    Handgeschriebener Assembler (z. B. der Mobiclip-Videodecoder) baut so
    Aufrufe: LR wird auf eine Fortsetzung gesetzt (oft zusaetzlich mit einem
    eigenen Stack-Frame), dann wird mit `b` (auch bedingt) zu einer Routine
    gesprungen, die mit `pop {..., pc}` bzw. `bx lr` zur Fortsetzung
    zurueckkehrt. Semantisch ist das ein (bedingter) Aufruf; als Sprung
    behandelt wuerde die Rueckkehr des Ziels die rekompilierte C-Funktion
    verlassen und die Fortsetzung ueberspringen. Die Fortsetzung muss hinter
    dem `b` liegen (hoechstens 0x100 Byte), sonst ist LR nur ein Datenregister.
    """
    if ins.id != A.ARM_INS_B or lr_const is None:
        return False
    if direct_target(ins) is None:
        return False
    nxt = ins.address + ins.size
    return nxt <= (lr_const & ~1) <= nxt + 0x100


def pseudo_call_info(blk, thumb):
    """{adresse: fortsetzung} fuer Pseudo-Aufrufe (`b`) und berechnete
    Spruenge (`add/ldr/mov pc, ...`) mit bekannter LR-Fortsetzung im Block."""
    out = {}
    lr = None
    for ins in blk.insns:
        if is_pseudo_call(ins, lr):
            out[ins.address] = lr & ~1
            if ins.cc == A.ARM_CC_AL:
                lr = None
            continue
        if lr is not None and ins.id == A.ARM_INS_BX:
            # `add lr, pc, #k; bx rN` (Mobiclip 0x16BD84/0x165784/0x15F184):
            # berechneter Aufruf, das Ziel kehrt per LR hinter den Sprung zurueck.
            try:
                reg = isa.reg_num(isa.split_operands(ins.op_str)[0])
            except Exception:
                reg = None
            nxt = ins.address + ins.size
            if reg is not None and reg != 14 and nxt <= (lr & ~1) <= nxt + 0x100:
                out[ins.address] = lr & ~1
        elif (lr is not None and op0_is_pc(ins)
                and ins.id in (A.ARM_INS_ADD, A.ARM_INS_SUB, A.ARM_INS_LDR, A.ARM_INS_MOV)):
            src = None
            try:
                ops = isa.split_operands(ins.op_str)
                src = isa.reg_num(ops[1]) if len(ops) > 1 else None
            except Exception:
                pass
            if src != 14:
                nxt = ins.address + ins.size
                if nxt <= (lr & ~1) <= nxt + 0x100:
                    out[ins.address] = lr & ~1
        lr = lr_const_after(ins, lr, thumb)
    return out


def pseudo_calls_in_block(blk, thumb):
    """Adressen der `b`-Instruktionen eines Blocks, die Pseudo-Aufrufe sind."""
    return {a for a, _ in pseudo_call_info(blk, thumb).items()}


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
        # IT-Zustand (Thumb-2) wie in emit.it_conditions mitfuehren: ein
        # Terminator in einem IT-Block ist bedingt ausgefuehrt, auch wenn
        # seine Kodierung selbst kein Bedingungsfeld hat.
        itstate = 0
        # PC-relative Konstanten im Block (Interworking-Veneer):
        #   add rN, pc, #imm  /  adr rN, #imm   ->  rN = Zieladresse
        pc_const = {}
        # `ldr rX,[rB,rI,lsl #2]` mit pc-relativer Basis rB: Offset-Tabelle.
        off_tbl = {}
        lr_known = None
        while True:
            ins = isa.decode(data, cur, thumb=fn.thumb)
            if ins is None:
                break
            blk.insns.append(ins)
            visited[cur] = ins.size
            in_it = False
            if itstate:
                in_it = True
                if (itstate & 7) == 0:
                    itstate = 0
                else:
                    itstate = (itstate & 0xE0) | ((itstate << 1) & 0x1F)
            elif fn.thumb:
                m = ins.mnemonic.lower().split(".")[0]
                if (m.startswith("it") and len(m) <= 5
                        and set(m[1:]) <= set("te")):
                    b = ins.bytes
                    if len(b) >= 2:
                        hw = int.from_bytes(b[:2], "little")
                        firstcond = (hw >> 4) & 0xF
                        mask = hw & 0xF
                        if mask:
                            itstate = (firstcond << 4) | mask
            if is_pseudo_call(ins, lr_known):
                # `add lr, pc, #k; ...; b<cond> ziel` = Aufruf: Ziel als
                # Funktion seeden und hinter dem `b` weiter dekodieren; die
                # Fortsetzung (LR) ist bereits als Block vorgemerkt.
                fn.calls.add((direct_target(ins), fn.thumb))
                if ins.cc == A.ARM_CC_AL:
                    lr_known = None
                cur += ins.size
                continue
            new_lr = lr_const_after(ins, lr_known, fn.thumb)
            if new_lr is not None and new_lr != lr_known:
                # `add lr, pc, #k` setzt eine Rueckkehradresse fuer einen
                # folgenden (oft berechneten) Aufruf. Die Fortsetzung muss ein
                # Blockanfang sein, sonst findet der Dispatch nach der
                # Rueckkehr kein Ziel und die C-Funktion endet vorzeitig.
                tgt = new_lr & ~1
                if tgt not in seen and isa.ARM_BASE <= tgt < isa.ARM_BASE + len(data):
                    todo.append(tgt)
            lr_known = new_lr
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
            elif (ins.id in (A.ARM_INS_ADD, A.ARM_INS_SUB) and len(ops) >= 3
                    and isa.reg_num(ops[0]) is not None
                    and isa.reg_num(ops[0]) == isa.reg_num(ops[1])):
                # Adressbildung in zwei Schritten: `add r4, pc, #hi`
                # gefolgt von `add r4, r4, #lo` (z. B. 0x16C3B0 ->
                # `movle pc, r4`). Ohne Fortschreibung bleibt das Ziel
                # 0x16E3B8 statt 0x16E70C und der Handler wird nie dekodiert.
                rd = isa.reg_num(ops[0])
                imm = isa.imm_value(ops[2])
                if rd in const_reg and imm is not None:
                    d = imm if ins.id == A.ARM_INS_ADD else -imm
                    const_reg[rd] = (const_reg[rd] + d) & 0xFFFFFFFF
            if ins.id == A.ARM_INS_MOV and len(ops) >= 2 and isa.is_pc(ops[0]):
                src = isa.reg_num(ops[1])
                if src in const_reg and const_reg[src] not in seen:
                    todo.append(const_reg[src])
            value = pc_relative_const(ins, cur, fn.thumb)
            if value is not None:
                pc_const[value[0]] = value[1]
            if (ins.id == A.ARM_INS_LDR and len(ins.operands) >= 2
                    and ins.operands[0].type == A.ARM_OP_REG
                    and ins.operands[1].type == A.ARM_OP_MEM):
                mem = ins.operands[1].mem
                if (mem.base in pc_const and mem.index != 0):
                    opn = ins.op_str.replace(" ", "")
                    # Wortindex (lsl #2) oder bereits in Bytes gerechneter
                    # Index (`ldr rX,[rB,rI]`); beides sind Offset-Tabellen.
                    if "lsl#2" in opn or "lsl" not in opn:
                        off_tbl[ins.operands[0].reg] = pc_const[mem.base]
            if (ins.id == A.ARM_INS_ADD and op0_is_pc(ins)
                    and len(ins.operands) >= 3
                    and ins.operands[1].type == A.ARM_OP_REG
                    and ins.operands[2].type == A.ARM_OP_REG):
                # `add pc, rX, rB` mit Offset-Tabelle: Eintraege als
                # Funktions-Seeds, damit der dynamische Sprung ein Ziel hat.
                # Basis und Offset koennen in beliebiger Reihenfolge stehen.
                a = ins.operands[1].reg
                b = ins.operands[2].reg
                base = pc_const.get(b)
                if base is None or off_tbl.get(a) != base:
                    base = pc_const.get(a)
                    if base is None or off_tbl.get(b) != base:
                        base = None
                if base is not None:
                    for tgt, tthumb in offset_table_targets(data, base, fn.thumb):
                        fn.calls.add((tgt, tthumb))
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
                        or ins.cc != A.ARM_CC_AL or in_it)
                fall = nxt
                extra = []
                if ins.id == A.ARM_INS_LDR and op0_is_pc(ins):
                    # Sprungtabelle ab pc+8 (ARM) bzw. Align(pc+4) (Thumb).
                    # Beim bedingten ARM-Switch `ldrls pc, [pc, rN, lsl #2]`
                    # steht bei +4 der Default-Zweig (meist `b default`), der
                    # bei falscher Bedingung ausgefuehrt wird: dort weiter
                    # dekodieren, nicht hinter der Tabelle.
                    extra, table_end = ldr_pc_table(data, ins, fn.thumb)
                    if table_end is not None and not (cond and not fn.thumb):
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
    """Alle Woerter im Image, die auf Code zeigen, als Funktionskandidaten.

    Ein gesetztes Bit 0 bedeutet nach der ARM/Thumb-Interworking-Regel
    Thumb-Code; der Kandidat wird in genau diesem Modus geprueft. Der
    gesamte Codebereich (auch die ersten 4 KiB mit crt0/libc) wird
    durchsucht, da Init-/Konstruktor-Tabellen dort stehen koennen.
    """
    out = []
    base = isa.ARM_BASE
    end = base + len(data)
    for off in range(0, len(data) - 4, 4):
        v = int.from_bytes(data[off:off + 4], "little")
        addr = v & ~1
        if addr < base or addr >= end:
            continue
        thumb = bool(v & 1)
        if not _code_ok(data, addr, thumb=thumb):
            continue
        out.append((addr, "pointer@%06X" % (off + base), thumb))
        if len(out) >= limit:
            break
    return out


def looks_like_entry(data, addr, thumb):
    """Grober Funktionsanfang: Prolog, der LR sichert.

    Nur fuer schwache Seeds (Zeiger/Prolog-Kandidaten) gedacht: liegt ein
    solcher Kandidat in bereits dekodiertem Code, ist er trotzdem ein echter
    Funktionsanfang, wenn dort ein Prolog steht (ein linear dekodierter
    Block kann ueber den Anfang der naechsten Funktion gelaufen sein).
    """
    ins = isa.decode(data, addr, thumb=thumb)
    if ins is None:
        return False
    return ins.id in (A.ARM_INS_PUSH, A.ARM_INS_STMDB) and "lr" in ins.op_str


def scan(data, entries, follow_prologues=False, seed_pointers=False):
    functions = {}
    visited = {}
    queue = list(entries)

    def drain():
        while queue:
            addr, source, thumb = queue.pop(0)
            if addr in functions or addr < isa.ARM_BASE:
                continue
            # Zeiger-/Prolog-Seeds sind nur Kandidaten: liegt die Adresse
            # mitten in bereits dekodiertem Code und beginnt dort kein
            # Prolog, ist sie kein Funktionsanfang (falscher Seed). Direkte
            # Call-Ziele (`bl@`) bleiben dagegen autoritativ.
            weak = source.startswith("pointer@") or source == "prologue"
            if weak and addr in visited and not looks_like_entry(data, addr, thumb):
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
            if addr in functions or addr in visited:
                continue
            ins = isa.decode(data, addr)
            if ins is None:
                continue
            m = ins.mnemonic
            if (m == "push" or m == "stmdb") and "lr" in ins.op_str:
                seeds.append(addr)
        for addr in seeds:
            if addr not in functions and addr not in visited:
                queue.append((addr, "prologue", False))
        drain()

    return ScanResult(functions=functions, visited=visited)
