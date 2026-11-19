"""ARM-Dekodierung: capstone wenn vorhanden, sonst Minimal-Decoder.

Der Minimal-Decoder deckt genau die fuer guest-refs noetigen Faelle ab:
LDR/STR (Literal + Immediate-Offset), MOV/MOVW/MOVT, ADD/SUB, BL/BLX.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import List, Optional, Tuple

from . import common

try:
    import capstone as _cs
    from capstone import arm as _arm
    HAVE_CAPSTONE = True
except Exception:                                    # pragma: no cover
    _cs = None
    _arm = None
    HAVE_CAPSTONE = False

_MD = None


@dataclass
class MemRef:
    base: str
    disp: int = 0
    index: Optional[str] = None
    shift: int = 0
    is_store: bool = False
    is_load: bool = False
    width: int = 4
    pc_relative: bool = False
    lit_addr: Optional[int] = None
    lit_value: Optional[int] = None


@dataclass
class Ins:
    addr: int
    size: int
    mnemonic: str
    text: str
    writes: Tuple[str, ...] = ()
    const: Optional[int] = None
    copy_from: Optional[str] = None
    alu: Optional[Tuple[str, str, int]] = None          # (add/sub, rn, imm)
    alu_reg: Optional[Tuple[str, str, str]] = None      # (add/sub, rn, rm)
    movt: bool = False
    mem: Optional[MemRef] = None
    reglist: Tuple[str, ...] = ()
    is_branch: bool = False
    bl_target: Optional[int] = None


def _regname(ins, reg_id: int) -> str:
    try:
        return ins.reg_name(reg_id)
    except Exception:
        return "?"


def _decode_capstone(data: bytes, start: int, end: int,
                     base: int = common.ARM_BASE) -> List[Ins]:
    global _MD
    if _MD is None:
        _MD = _cs.Cs(_cs.CS_ARCH_ARM, _cs.CS_MODE_ARM)
        _MD.detail = True
        _MD.skipdata = True
    md = _MD
    blob = data[start - base:end - base]
    out: List[Ins] = []
    for ins in md.disasm(blob, start):
        try:
            ops = list(ins.operands)
        except Exception:
            ops = []
        if ins.id == 0:
            out.append(Ins(addr=ins.address, size=ins.size,
                           mnemonic=".word", text=".word"))
            continue
        writes: List[str] = []
        const = None
        copy_from = None
        alu = None
        alu_reg = None
        movt = False
        mem = None
        reglist: List[str] = []
        is_branch = False
        bl_target = None

        iid = ins.id
        if ops and ops[0].type == _arm.ARM_OP_REG:
            writes.append(_regname(ins, ops[0].reg))

        if iid in (_arm.ARM_INS_MOV, _arm.ARM_INS_MVN) and len(ops) >= 2:
            src = ops[1]
            if src.type == _arm.ARM_OP_IMM:
                const = int(src.imm) & 0xFFFFFFFF
            elif src.type == _arm.ARM_OP_REG:
                copy_from = _regname(ins, src.reg)
        elif iid == _arm.ARM_INS_MOVW and len(ops) >= 2:
            if ops[1].type == _arm.ARM_OP_IMM:
                const = int(ops[1].imm) & 0xFFFF
        elif iid == _arm.ARM_INS_MOVT and len(ops) >= 2:
            movt = True
            if ops[1].type == _arm.ARM_OP_IMM:
                const = int(ops[1].imm) & 0xFFFF
        elif iid in (_arm.ARM_INS_ADD, _arm.ARM_INS_SUB, _arm.ARM_INS_RSB) \
                and len(ops) >= 3:
            if iid == _arm.ARM_INS_ADD:
                opname = "add"
            elif iid == _arm.ARM_INS_SUB:
                opname = "sub"
            else:
                opname = "rsb"
            if ops[1].type == _arm.ARM_OP_REG:
                rn = _regname(ins, ops[1].reg)
                if ops[2].type == _arm.ARM_OP_IMM:
                    alu = (opname, rn, int(ops[2].imm))
                elif ops[2].type == _arm.ARM_OP_REG:
                    alu_reg = (opname, rn, _regname(ins, ops[2].reg))

        for op in ops:
            if op.type != _arm.ARM_OP_MEM:
                continue
            base_name = _regname(ins, op.mem.base) if op.mem.base else ""
            idx_name = _regname(ins, op.mem.index) if op.mem.index else None
            width = 4
            if iid in (_arm.ARM_INS_LDRB, _arm.ARM_INS_STRB,
                       _arm.ARM_INS_LDRSB):
                width = 1
            elif iid in (_arm.ARM_INS_LDRH, _arm.ARM_INS_STRH,
                         _arm.ARM_INS_LDRSH):
                width = 2
            is_load = iid in (_arm.ARM_INS_LDR, _arm.ARM_INS_LDRB,
                              _arm.ARM_INS_LDRH, _arm.ARM_INS_LDRSB,
                              _arm.ARM_INS_LDRSH, _arm.ARM_INS_LDRD)
            is_store = iid in (_arm.ARM_INS_STR, _arm.ARM_INS_STRB,
                               _arm.ARM_INS_STRH, _arm.ARM_INS_STRD)
            mem = MemRef(base=base_name, disp=int(op.mem.disp),
                         index=idx_name,
                         shift=int(getattr(op.mem, "lshift", 0)),
                         is_load=is_load, is_store=is_store, width=width,
                         pc_relative=(base_name in ("pc", "r15")))
            break

        if iid in (_arm.ARM_INS_LDM, _arm.ARM_INS_LDMIB,
                   _arm.ARM_INS_LDMDA, _arm.ARM_INS_LDMDB,
                   _arm.ARM_INS_STM, _arm.ARM_INS_STMIB,
                   _arm.ARM_INS_STMDA, _arm.ARM_INS_STMDB):
            is_load = iid in (_arm.ARM_INS_LDM, _arm.ARM_INS_LDMIB,
                              _arm.ARM_INS_LDMDA, _arm.ARM_INS_LDMDB)
            if ops and ops[0].type == _arm.ARM_OP_REG:
                base_name = _regname(ins, ops[0].reg)
                regs = []
                for op in ops[1:]:
                    if op.type == _arm.ARM_OP_REG:
                        regs.append(_regname(ins, op.reg))
                reglist = tuple(regs)
                mem = MemRef(base=base_name, disp=0, is_load=is_load,
                             is_store=not is_load, width=4)
                if is_load:
                    writes.extend(regs)
        elif iid == _arm.ARM_INS_POP and ops:
            reglist = tuple(_regname(ins, o.reg) for o in ops
                            if o.type == _arm.ARM_OP_REG)
            writes.extend(reglist)

        if iid in (_arm.ARM_INS_BL, _arm.ARM_INS_BLX):
            is_branch = True
            if ops and ops[0].type == _arm.ARM_OP_IMM:
                bl_target = int(ops[0].imm) & 0xFFFFFFFF
        elif iid in (_arm.ARM_INS_B, _arm.ARM_INS_BX):
            is_branch = True

        if mem is not None and mem.pc_relative and mem.is_load \
                and not mem.index:
            lit = (ins.address + 8 + mem.disp) & 0xFFFFFFFF
            mem.lit_addr = lit
            mem.lit_value = common.u32(data, lit - base)
            const = mem.lit_value

        text = ins.mnemonic
        if ins.op_str:
            text += " " + ins.op_str
        out.append(Ins(addr=ins.address, size=ins.size, mnemonic=ins.mnemonic,
                       text=text, writes=tuple(writes), const=const,
                       copy_from=copy_from, alu=alu, alu_reg=alu_reg,
                       movt=movt, mem=mem, reglist=reglist,
                       is_branch=is_branch, bl_target=bl_target))
    return out


# ---------- Minimal-Decoder (Fallback ohne capstone) ----------

def _rotl(value: int, bits: int) -> int:
    bits &= 31
    value &= 0xFFFFFFFF
    if not bits:
        return value
    return ((value << bits) | (value >> (32 - bits))) & 0xFFFFFFFF


def _decode_word(w: int, addr: int) -> Ins:
    writes: List[str] = []
    const = None
    copy_from = None
    alu = None
    alu_reg = None
    movt = False
    mem = None
    reglist: List[str] = []
    is_branch = False
    bl_target = None
    mnem = "?"

    # LDR/STR immediate/register
    if (w & 0x0C000000) == 0x04000000:
        p_bit = (w >> 24) & 1
        u_bit = (w >> 23) & 1
        b_bit = (w >> 22) & 1
        l_bit = (w >> 20) & 1
        rn = (w >> 16) & 0xF
        rt = (w >> 12) & 0xF
        imm = w & 0xFFF
        reg_off = (w >> 25) & 1
        if not reg_off and p_bit:
            base = "pc" if rn == 15 else "r%d" % rn
            disp = imm if u_bit else -imm
            mem = MemRef(base=base, disp=disp, is_load=bool(l_bit),
                         is_store=not bool(l_bit), width=1 if b_bit else 4,
                         pc_relative=(rn == 15))
            if l_bit:
                writes.append("r%d" % rt)
            mnem = ("ldr" if l_bit else "str") + ("b" if b_bit else "")
        elif reg_off:
            rm = w & 0xF
            mem = MemRef(base="r%d" % rn, index="r%d" % rm,
                         is_load=bool(l_bit), is_store=not bool(l_bit),
                         width=1 if b_bit else 4)
            if l_bit:
                writes.append("r%d" % rt)
            mnem = ("ldr" if l_bit else "str") + ("b" if b_bit else "")

    # MOV / MOVW / MOVT
    if (w & 0x0FE00000) == 0x03A00000:
        rd = (w >> 12) & 0xF
        const = _rotl(w & 0xFF, ((w >> 8) & 0xF) * 2)
        writes.append("r%d" % rd)
        mnem = "mov"
    elif (w & 0x0FE0FFF0) == 0x01A00000:
        rd = (w >> 12) & 0xF
        copy_from = "r%d" % (w & 0xF)
        writes.append("r%d" % rd)
        mnem = "mov"
    elif (w & 0x0FF00000) == 0x03000000:
        rd = (w >> 12) & 0xF
        const = ((w >> 4) & 0xF000) | (w & 0xFFF)
        writes.append("r%d" % rd)
        mnem = "movw"
    elif (w & 0x0FF00000) == 0x03400000:
        rd = (w >> 12) & 0xF
        const = ((w >> 4) & 0xF000) | (w & 0xFFF)
        movt = True
        writes.append("r%d" % rd)
        mnem = "movt"

    # ADD/SUB immediate or register
    if (w & 0x0FE00000) in (0x02800000, 0x02400000):
        op = "add" if (w & 0x0FE00000) == 0x02800000 else "sub"
        rn = (w >> 16) & 0xF
        rd = (w >> 12) & 0xF
        writes.append("r%d" % rd)
        alu = (op, "pc" if rn == 15 else "r%d" % rn,
               _rotl(w & 0xFF, ((w >> 8) & 0xF) * 2))
        mnem = op
    elif (w & 0x0FE00FF0) in (0x00800000, 0x00400000):
        op = "add" if (w & 0x0FE00FF0) == 0x00800000 else "sub"
        rn = (w >> 16) & 0xF
        rd = (w >> 12) & 0xF
        rm = w & 0xF
        writes.append("r%d" % rd)
        alu_reg = (op, "pc" if rn == 15 else "r%d" % rn, "r%d" % rm)
        mnem = op

    # BL / BLX
    if (w & 0x0F000000) == 0x0B000000:
        imm = w & 0xFFFFFF
        if imm & 0x800000:
            imm -= 0x1000000
        bl_target = (addr + 8 + imm * 4) & 0xFFFFFFFF
        is_branch = True
        mnem = "bl"
    elif (w & 0xFE000000) == 0xFA000000:
        imm = w & 0xFFFFFF
        h = (w >> 24) & 1
        if imm & 0x800000:
            imm -= 0x1000000
        bl_target = (addr + 8 + imm * 4 + h * 2) & 0xFFFFFFFF
        is_branch = True
        mnem = "blx"

    return Ins(addr=addr, size=4, mnemonic=mnem, text=mnem,
               writes=tuple(writes), const=const, copy_from=copy_from,
               alu=alu, alu_reg=alu_reg, movt=movt, mem=mem,
               reglist=tuple(reglist), is_branch=is_branch,
               bl_target=bl_target)


def _decode_minimal(data: bytes, start: int, end: int,
                    base: int = common.ARM_BASE) -> List[Ins]:
    out = []
    for addr in range(start & ~3, end, 4):
        off = addr - base
        if off < 0 or off + 4 > len(data):
            break
        w = int.from_bytes(data[off:off + 4], "little")
        ins = _decode_word(w, addr)
        if ins.mem is not None and ins.mem.pc_relative and ins.mem.is_load \
                and not ins.mem.index:
            lit = (addr + 8 + ins.mem.disp) & 0xFFFFFFFF
            ins.mem.lit_addr = lit
            ins.mem.lit_value = common.u32(data, lit - base)
            ins.const = ins.mem.lit_value
        out.append(ins)
    return out


def decode_range(data: bytes, start: int, end: int,
                 base: int = common.ARM_BASE) -> List[Ins]:
    if HAVE_CAPSTONE:
        return _decode_capstone(data, start, end, base)
    return _decode_minimal(data, start, end, base)


def resolve_reg(insns: List[Ins], idx: int, reg: str,
                depth: int = 0) -> Optional[int]:
    """Registerwert vor insns[idx] symbolisch bestimmen (linear rueckwaerts)."""
    if depth > 48:
        return None
    for j in range(idx - 1, -1, -1):
        ins = insns[j]
        if reg not in ins.writes:
            continue
        if ins.movt and ins.const is not None:
            low = None
            for k in range(j - 1, -1, -1):
                prev = insns[k]
                if reg in prev.writes:
                    if prev.mnemonic == "movw" and prev.const is not None:
                        low = prev.const
                    break
            if low is None:
                return None
            return ((ins.const << 16) | (low & 0xFFFF)) & 0xFFFFFFFF
        if ins.const is not None:
            return ins.const & 0xFFFFFFFF
        if ins.copy_from is not None:
            return resolve_reg(insns, j, ins.copy_from, depth + 1)
        if ins.alu is not None:
            op, rn, imm = ins.alu
            base_val = (ins.addr + 8) if rn in ("pc", "r15") else \
                resolve_reg(insns, j, rn, depth + 1)
            if base_val is None:
                return None
            return (base_val + imm if op == "add" else base_val - imm) & 0xFFFFFFFF
        if ins.alu_reg is not None:
            op, rn, rm = ins.alu_reg
            b1 = (ins.addr + 8) if rn in ("pc", "r15") else \
                resolve_reg(insns, j, rn, depth + 1)
            b2 = (ins.addr + 8) if rm in ("pc", "r15") else \
                resolve_reg(insns, j, rm, depth + 1)
            if b1 is None or b2 is None:
                return None
            return (b1 + b2 if op == "add" else b1 - b2) & 0xFFFFFFFF
        return None
    return None
