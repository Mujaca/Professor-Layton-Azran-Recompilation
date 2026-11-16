"""C-Codegenerator fuer ARM11-Instruktionen (MVP)."""
import capstone.arm as A

from . import isa


class Unsupported(Exception):
    pass


# id -> (DP-Code, nutzt rn, schreibt rd)
DP_OPS = {
    A.ARM_INS_AND: ("DP_AND", True, True),
    A.ARM_INS_EOR: ("DP_EOR", True, True),
    A.ARM_INS_ORR: ("DP_ORR", True, True),
    A.ARM_INS_BIC: ("DP_BIC", True, True),
    A.ARM_INS_SUB: ("DP_SUB", True, True),
    A.ARM_INS_RSB: ("DP_RSB", True, True),
    A.ARM_INS_ADD: ("DP_ADD", True, True),
    A.ARM_INS_ADC: ("DP_ADC", True, True),
    A.ARM_INS_SBC: ("DP_SBC", True, True),
    A.ARM_INS_RSC: ("DP_RSC", True, True),
    A.ARM_INS_TST: ("DP_TST", True, False),
    A.ARM_INS_TEQ: ("DP_TEQ", True, False),
    A.ARM_INS_CMP: ("DP_CMP", True, False),
    A.ARM_INS_CMN: ("DP_CMN", True, False),
    A.ARM_INS_MOV: ("DP_MOV", False, True),
    A.ARM_INS_MVN: ("DP_MVN", False, True),
}

MEM_OPS = {
    A.ARM_INS_LDR: (4, 0, True),
    A.ARM_INS_STR: (4, 0, False),
    A.ARM_INS_LDRB: (1, 0, True),
    A.ARM_INS_STRB: (1, 0, False),
    A.ARM_INS_LDRH: (2, 0, True),
    A.ARM_INS_STRH: (2, 0, False),
    A.ARM_INS_LDRSB: (1, 1, True),
    A.ARM_INS_LDRSH: (2, 1, True),
}

LDM_IDS = (A.ARM_INS_LDM, A.ARM_INS_LDMDA, A.ARM_INS_LDMDB, A.ARM_INS_LDMIB)
STM_IDS = (A.ARM_INS_STM, A.ARM_INS_STMDA, A.ARM_INS_STMDB, A.ARM_INS_STMIB)

SHIFT = {"lsl": "SH_LSL", "lsr": "SH_LSR", "asr": "SH_ASR", "ror": "SH_ROR"}

VFP_ARITH = {"vadd": 0, "vsub": 1, "vmul": 2, "vdiv": 3}
VFP_UNARY = {"vneg": 4, "vabs": 5, "vsqrt": 6}

COND_SUFFIXES = ("eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl",
                 "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le")
VFP_BASES = ("vldr", "vstr", "vmov", "vadd", "vsub", "vmul", "vdiv", "vneg",
             "vabs", "vsqrt", "vcmp", "vcmpe", "vmrs", "vmsr", "vpush", "vpop",
             "vmla", "vmls", "vnmul", "vcvt", "vldmia", "vldmdb", "vstmia", "vstmdb")


CORE_BASES = {"nop", "hint", "pld", "pldw", "clrex", "lsl", "lsr", "asr", "ror",
              "rrx", "uxth", "uxtb", "sxth", "sxtb", "uxtah", "sxtah", "ldrd", "strd",
              "ldrex", "strex", "mrc", "mcr", "ldm", "stm", "push", "pop", "rev",
              "rev16", "revsh", "usat", "ssat", "qadd", "qsub", "sdiv", "udiv"}


def strip_cc(mnemonic):
    """Entfernt eine Bedingungs-Endung (z.B. 'vmovls' -> 'vmov', 'ldrdeq' -> 'ldrd')."""
    m = mnemonic.split(".")[0].lower()
    for cc in COND_SUFFIXES:
        if m.endswith(cc):
            core = m[:-2]
            if core in VFP_BASES or core in CORE_BASES:
                return core
    return m


def u32(v):
    return v & 0xFFFFFFFF


class Emitter:
    def __init__(self, func_addr):
        self.func = func_addr

    def label(self, addr):
        return "L_%08X_%08X" % (self.func, u32(addr))

    # ---------- operands ----------
    def value(self, tok, pc):
        t = tok.strip()
        if t.startswith("#"):
            v = isa.imm_value(t)
            if v is None:
                raise Unsupported(t)
            return "0x%08Xu" % u32(v)
        n = isa.reg_num(t)
        if n is None:
            raise Unsupported(t)
        if n == 15:
            return "0x%08Xu" % u32(pc + 8)
        return "c->r[%d]" % n

    def operand2(self, toks, pc):
        if not toks:
            raise Unsupported("leerer Operand")
        base = self.value(toks[0], pc)
        if len(toks) == 1:
            return base, None
        st = toks[1].strip()
        if st.lower() == "rrx":
            return "arm_shift_imm(SH_ROR, 0u, %s, &sh)" % base, "sh"
        parts = st.split()
        if len(parts) != 2:
            raise Unsupported(st)
        kind = SHIFT.get(parts[0].lower())
        if kind is None:
            raise Unsupported(st)
        if parts[1].startswith("#"):
            amt = isa.imm_value(parts[1])
            return "arm_shift_imm(%s, %du, %s, &sh)" % (kind, amt, base), "sh"
        raddr = self.value(parts[1], pc)
        return "arm_shift_reg(%s, %s, %s, &sh)" % (kind, raddr, base), "sh"

    # ---------- instruction classes ----------
    def dp(self, ins):
        code, uses_rn, writes = DP_OPS[ins.id]
        ops = isa.split_operands(ins.op_str)
        pc = ins.address
        if writes:
            rd = isa.reg_num(ops[0])
            if rd is None:
                raise Unsupported(ins.op_str)
            if uses_rn:
                rn = self.value(ops[1], pc)
                rest = ops[2:]
            else:
                rn = "0"
                rest = ops[1:]
        else:
            rd = -1
            rn = self.value(ops[0], pc)
            rest = ops[1:]
        o2, sh = self.operand2(rest, pc)
        sf = 1 if ins.update_flags else 0
        if sh:
            return ["{", "  int sh = -1;",
                    "  arm_dp(c, %s, %d, %s, %s, %d, sh);" % (code, rd, rn, o2, sf),
                    "}"]
        return ["arm_dp(c, %s, %d, %s, %s, %d, -1);" % (code, rd, rn, o2, sf)]

    def mem(self, ins):
        size, signed, load = MEM_OPS[ins.id]
        ops = isa.split_operands(ins.op_str)
        pc = ins.address
        rd = isa.reg_num(ops[0])
        if rd is None:
            raise Unsupported(ins.op_str)
        if len(ops) < 2 or not ops[1].startswith("["):
            raise Unsupported(ins.op_str)
        mem_txt = ops[1]
        wb = mem_txt.endswith("!")
        if wb:
            mem_txt = mem_txt[:-1]
        if not mem_txt.endswith("]"):
            raise Unsupported(ins.op_str)
        inner = isa.split_operands(mem_txt[1:-1])
        base_reg = isa.reg_num(inner[0])
        if base_reg is None:
            raise Unsupported(ins.op_str)
        basev = "0x%08Xu" % u32(pc + 8) if base_reg == 15 else "c->r[%d]" % base_reg
        post = len(ops) > 2
        if post:
            off, sh = self.operand2([ops[2]], pc)
        elif len(inner) > 1:
            off, sh = self.operand2(inner[1:], pc)
        else:
            off, sh = "0", None

        lines = ["{"]
        if sh:
            lines.append("  int sh = -1;")
        if post:
            lines.append("  uint32_t a = %s;" % basev)
        else:
            lines.append("  uint32_t a = %s + %s;" % (basev, off))
        if load:
            lines.append("  c->r[%d] = arm_ld(c, a, %d, %d);" % (rd, size, signed))
        else:
            lines.append("  arm_st(c, a, %d, c->r[%d]);" % (size, rd))
        if wb:
            if post:
                lines.append("  c->r[%d] = a + %s;" % (base_reg, off))
            else:
                lines.append("  c->r[%d] = a;" % base_reg)
        lines.append("}")
        if load and rd == 15:
            lines.append("return;")
        return lines

    def multi(self, ins):
        m = strip_cc(ins.mnemonic.lower())
        ops = isa.split_operands(ins.op_str)
        if ins.id in (A.ARM_INS_PUSH, A.ARM_INS_POP) or m in ("push", "pop"):
            regs = isa.reg_list(ops[-1])
            mask = 0
            for r in regs:
                mask |= 1 << r
            if m == "push":
                return ["{ int w = arm_ldm_stm(c, 13, 0, 1, 3, 0x%08Xu); (void)w; }" % mask]
            return ["if (arm_ldm_stm(c, 13, 1, 1, 0, 0x%08Xu)) return;" % mask]
        base_tok = ops[0].rstrip("!")
        wb = ops[0].endswith("!")
        base_reg = isa.reg_num(base_tok)
        if base_reg is None:
            raise Unsupported(ins.op_str)
        regs = isa.reg_list(ops[-1])
        mask = 0
        for r in regs:
            mask |= 1 << r
        load = ins.id in LDM_IDS
        mode = 0
        if m.endswith("ib"):
            mode = 1
        elif m.endswith("da"):
            mode = 2
        elif m.endswith("db"):
            mode = 3
        call = "arm_ldm_stm(c, %d, %d, %d, %d, 0x%08Xu)" % (
            base_reg, 1 if load else 0, 1 if wb else 0, mode, mask)
        if load:
            return ["if (%s) return;" % call]
        return ["{ int w = %s; (void)w; }" % call]

    def branch(self, ins):
        ops = isa.split_operands(ins.op_str)
        pc = ins.address
        if ins.id == A.ARM_INS_B:
            if not ins.operands or ins.operands[0].type != A.ARM_OP_IMM:
                raise Unsupported(ins.op_str)
            tgt = ins.operands[0].imm
            return ["goto %s;" % self.label(tgt)]
        if ins.id == A.ARM_INS_BL:
            if not ins.operands or ins.operands[0].type != A.ARM_OP_IMM:
                raise Unsupported(ins.op_str)
            tgt = ins.operands[0].imm
            nxt = pc + 4
            return ["c->r[14] = 0x%08Xu;" % u32(nxt),
                    "fn_%08X(c);" % u32(tgt),
                    "if (c->aborted) return;"]
        if ins.id == A.ARM_INS_BLX:
            if ins.operands and ins.operands[0].type == A.ARM_OP_IMM:
                tgt = ins.operands[0].imm
                return ["c->r[14] = 0x%08Xu;" % u32(pc + 4),
                        "c->t = 1u;",
                        "fn_%08X(c);" % u32(tgt),
                        "if (c->aborted) return;"]
        if ins.id in (A.ARM_INS_BX, A.ARM_INS_BLX):
            reg = isa.reg_num(ops[0])
            if reg is None:
                raise Unsupported(ins.op_str)
            lines = []
            if ins.id == A.ARM_INS_BLX:
                lines.append("c->r[14] = 0x%08Xu;" % u32(pc + 4))
            lines.append("{ uint32_t t = c->r[%d]; c->t = t & 1u; c->r[15] = t & ~1u; }" % reg)
            lines.append("return;")
            return lines
        raise Unsupported(ins.op_str)

    def vfp(self, ins):
        m = ins.mnemonic.lower()
        base = strip_cc(m)
        ops = isa.split_operands(ins.op_str)
        pc = ins.address

        if base in ("vldr", "vstr"):
            reg = isa.vfp_reg(ops[0])
            if reg is None or len(ops) < 2:
                raise Unsupported(ins.op_str)
            kind, idx = reg
            mem_txt = ops[1]
            wb = mem_txt.endswith("!")
            if wb:
                mem_txt = mem_txt[:-1]
            inner = isa.split_operands(mem_txt[1:-1])
            base_reg = isa.reg_num(inner[0])
            if base_reg is None:
                raise Unsupported(ins.op_str)
            basev = "c->r[%d]" % base_reg
            off = "0"
            if len(inner) > 1:
                off = self.value(inner[1], pc)
            lines = ["{"]
            lines.append("  uint32_t a = %s + %s;" % (basev, off))
            if base == "vldr":
                if kind == "s":
                    lines.append("  c->sreg[%d] = arm_mem_read(c, a, 4);" % idx)
                else:
                    lines.append("  c->sreg[%d] = arm_mem_read(c, a, 4);" % (idx * 2))
                    lines.append("  c->sreg[%d] = arm_mem_read(c, a + 4, 4);" % (idx * 2 + 1))
            else:
                if kind == "s":
                    lines.append("  arm_mem_write(c, a, 4, c->sreg[%d]);" % idx)
                else:
                    lines.append("  arm_mem_write(c, a, 4, c->sreg[%d]);" % (idx * 2))
                    lines.append("  arm_mem_write(c, a + 4, 4, c->sreg[%d]);" % (idx * 2 + 1))
            if wb:
                lines.append("  c->r[%d] = a;" % base_reg)
            lines.append("}")
            return lines

        if base == "vmov":
            if len(ops) == 2 and "#" in ops[1]:
                reg = isa.vfp_reg(ops[0])
                v = isa.imm_value(ops[1])
                if reg is None or v is None:
                    raise Unsupported(ins.op_str)
                kind, idx = reg
                if kind == "s":
                    return ["vfp_set_s(c, %d, %df);" % (idx, float(v))]
                return ["vfp_set_d(c, %d, %s);" % (idx, repr(float(v)))]
            if len(ops) == 2:
                a = isa.vfp_reg(ops[0])
                b = isa.vfp_reg(ops[1])
                if a and b:
                    if a[0] != b[0]:
                        raise Unsupported(ins.op_str)
                    if a[0] == "s":
                        return ["c->sreg[%d] = c->sreg[%d];" % (a[1], b[1])]
                    return ["c->sreg[%d] = c->sreg[%d];" % (a[1] * 2, b[1] * 2),
                            "c->sreg[%d] = c->sreg[%d];" % (a[1] * 2 + 1, b[1] * 2 + 1)]
                if a and b is None:
                    rn = isa.reg_num(ops[1])
                    if rn is None:
                        raise Unsupported(ins.op_str)
                    if a[0] == "s":
                        return ["c->sreg[%d] = c->r[%d];" % (a[1], rn)]
                    return ["c->sreg[%d] = c->r[%d];" % (a[1] * 2, rn),
                            "c->sreg[%d] = 0;" % (a[1] * 2 + 1)]
                if b and a is None:
                    rd = isa.reg_num(ops[0])
                    if rd is None:
                        raise Unsupported(ins.op_str)
                    if b[0] == "s":
                        return ["c->r[%d] = c->sreg[%d];" % (rd, b[1])]
                    return ["c->r[%d] = c->sreg[%d];" % (rd, b[1] * 2),
                            "c->r[%d] = 0;" % (rd,)]
            raise Unsupported(ins.op_str)

        if base in VFP_ARITH:
            op = VFP_ARITH[base]
            regs = [isa.vfp_reg(o) for o in ops]
            if len(regs) != 3 or any(r is None for r in regs):
                raise Unsupported(ins.op_str)
            dbl = 1 if regs[0][0] == "d" else 0
            return ["vfp_op(c, %d, %d, %d, %d, %d);" % (
                op, dbl, regs[0][1], regs[1][1], regs[2][1])]

        if base in VFP_UNARY:
            op = VFP_UNARY[base]
            regs = [isa.vfp_reg(o) for o in ops]
            if len(regs) != 2 or any(r is None for r in regs):
                raise Unsupported(ins.op_str)
            dbl = 1 if regs[0][0] == "d" else 0
            return ["vfp_op(c, %d, %d, %d, %d, %d);" % (
                op, dbl, regs[0][1], regs[1][1], regs[1][1])]

        if base in ("vcmp", "vcmpe"):
            a = isa.vfp_reg(ops[0])
            if a is None:
                raise Unsupported(ins.op_str)
            dbl = 1 if a[0] == "d" else 0
            if len(ops) > 1 and ops[1].startswith("#"):
                return ["vfp_cmp(c, %d, %d, 0, 1);" % (dbl, a[1])]
            b = isa.vfp_reg(ops[1])
            if b is None:
                raise Unsupported(ins.op_str)
            return ["vfp_cmp(c, %d, %d, %d, 0);" % (dbl, a[1], b[1])]

        if base == "vmrs":
            return ["vfp_fpscr_to_flags(c);"]
        if base == "vmsr":
            rd = isa.reg_num(ops[1])
            if rd is None:
                raise Unsupported(ins.op_str)
            return ["c->fpscr = c->r[%d];" % rd]

        if base in ("vpush", "vpop"):
            regs = isa.vfp_reg_list(ops[0])
            words = []
            for kind, idx in regs:
                if kind == "s":
                    words.append(idx)
                else:
                    words.append(idx * 2)
                    words.append(idx * 2 + 1)
            lines = []
            if base == "vpush":
                lines.append("c->r[13] -= %du;" % (4 * len(words)))
                for k, w in enumerate(words):
                    lines.append("arm_mem_write(c, c->r[13] + %du, 4, c->sreg[%d]);" % (4 * k, w))
            else:
                for k, w in enumerate(words):
                    lines.append("c->sreg[%d] = arm_mem_read(c, c->r[13] + %du, 4);" % (w, 4 * k))
                lines.append("c->r[13] += %du;" % (4 * len(words)))
            return lines

        if base in ("vldm", "vldmia", "vldmdb", "vstm", "vstmia", "vstmdb"):
            raise Unsupported(ins.op_str)

        raise Unsupported(ins.op_str)

    # ---------- dispatch per mnemonic (Fallback fuer nicht-per-id erkannte Formen) ----------
    def by_mnemonic(self, ins):
        m = ins.mnemonic.lower()
        base = strip_cc(m)
        ops = isa.split_operands(ins.op_str)
        pc = ins.address

        if base in ("nop", "hint", "pld", "pldw", "clrex", "dbg", "sev", "wfe", "wfi"):
            return []

        if base.startswith(("lsl", "lsr", "asr", "ror", "rrx")):
            kind = {"lsl": "SH_LSL", "lsr": "SH_LSR", "asr": "SH_ASR",
                    "ror": "SH_ROR", "rrx": "SH_ROR"}[base[:3]]
            rd = isa.reg_num(ops[0])
            if rd is None:
                raise Unsupported(ins.op_str)
            if base == "rrx":
                return ["c->r[%d] = arm_shift_imm(SH_ROR, 0u, c->r[%d], 0);"
                        % (rd, isa.reg_num(ops[1]))]
            src = self.value(ops[1], pc)
            if len(ops) >= 3:
                amt = ops[2]
                if amt.startswith("#"):
                    return ["c->r[%d] = arm_shift_imm(%s, %du, %s, 0);"
                            % (rd, kind, isa.imm_value(amt), src)]
                ra = self.value(amt, pc)
                return ["c->r[%d] = arm_shift_reg(%s, %s, %s, 0);" % (rd, kind, ra, src)]
            return ["c->r[%d] = %s;" % (rd, src)]

        if base in ("uxth", "uxtb", "uxth.w", "uxtb.w", "sxth", "sxtb", "sxth.w", "sxtb.w"):
            rd = isa.reg_num(ops[0])
            rm = isa.reg_num(ops[1])
            if rd is None or rm is None:
                raise Unsupported(ins.op_str)
            size = 2 if "th" in base else 1
            signed = base.startswith("s")
            expr = "c->r[%d]" % rm
            if len(ops) >= 3 and ops[2].lower().startswith("ror"):
                rot = ops[2].split()[-1]
                expr = "arm_shift_imm(SH_ROR, %du, %s, 0)" % (isa.imm_value(rot), expr)
            if size == 1:
                if signed:
                    return ["c->r[%d] = (uint32_t)(int32_t)(int8_t)(%s);" % (rd, expr)]
                return ["c->r[%d] = (%s) & 0xFFu;" % (rd, expr)]
            if signed:
                return ["c->r[%d] = (uint32_t)(int32_t)(int16_t)(%s);" % (rd, expr)]
            return ["c->r[%d] = (%s) & 0xFFFFu;" % (rd, expr)]

        if base in ("ldrd", "strd"):
            rd = isa.reg_num(ops[0])
            rd2 = isa.reg_num(ops[1])
            if rd is None or rd2 is None:
                raise Unsupported(ins.op_str)
            mem_txt = ops[2]
            wb = mem_txt.endswith("!")
            if wb:
                mem_txt = mem_txt[:-1]
            inner = isa.split_operands(mem_txt[1:-1])
            base_reg = isa.reg_num(inner[0])
            if base_reg is None:
                raise Unsupported(ins.op_str)
            basev = "c->r[%d]" % base_reg
            off = "0"
            if len(inner) > 1:
                off = self.value(inner[1], pc)
            lines = ["{", "  uint32_t a = %s + %s;" % (basev, off)]
            if base == "ldrd":
                if rd2 != (rd + 1) % 16:
                    raise Unsupported(ins.op_str)
                lines.append("  c->r[%d] = arm_mem_read(c, a, 4);" % rd)
                lines.append("  c->r[%d] = arm_mem_read(c, a + 4, 4);" % rd2)
            else:
                lines.append("  arm_mem_write(c, a, 4, c->r[%d]);" % rd)
                lines.append("  arm_mem_write(c, a + 4, 4, c->r[%d]);" % rd2)
            if wb:
                lines.append("  c->r[%d] = a;" % base_reg)
            lines.append("}")
            return lines

        if base in ("uxtah", "sxtah", "uxtab", "sxtab"):
            rd = isa.reg_num(ops[0])
            rn = isa.reg_num(ops[1])
            rm = isa.reg_num(ops[2])
            if None in (rd, rn, rm):
                raise Unsupported(ins.op_str)
            ext = "(int32_t)(int16_t)c->r[%d]" if base.startswith("s") else "(uint32_t)(c->r[%d] & 0xFFFFu)"
            ext = ext % rm
            return ["c->r[%d] = c->r[%d] + %s;" % (rd, rn, ext)]

        if base in ("usat", "ssat"):
            rd = isa.reg_num(ops[0])
            bits = isa.imm_value(ops[1])
            rm = isa.reg_num(ops[2])
            shift = 0
            if len(ops) > 3:
                shift = isa.imm_value(ops[3].split()[-1])
            if rd is None or rm is None:
                raise Unsupported(ins.op_str)
            src = "c->r[%d]" % rm
            if shift:
                src = "(%s << %du)" % (src, shift)
            if base == "usat":
                return ["{ int32_t v = (int32_t)(%s); if (v < 0) v = 0; "
                        "if (v > %d) v = %d; c->r[%d] = (uint32_t)v; }"
                        % (src, (1 << bits) - 1, (1 << bits) - 1, rd)]
            hi = (1 << (bits - 1)) - 1
            lo = -(1 << (bits - 1))
            return ["{ int32_t v = (int32_t)(%s); if (v > %d) v = %d; "
                    "if (v < %d) v = %d; c->r[%d] = (uint32_t)v; }"
                    % (src, hi, hi, lo, lo, rd)]

        if base in ("rev", "rev16", "revsh"):
            rd = isa.reg_num(ops[0])
            rm = isa.reg_num(ops[1])
            if rd is None or rm is None:
                raise Unsupported(ins.op_str)
            if base == "rev":
                expr = "__builtin_bswap32(c->r[%d])" % rm
            elif base == "rev16":
                expr = "((__builtin_bswap32(c->r[%d]) >> 16) | (__builtin_bswap32(c->r[%d]) << 16))" % (rm, rm)
            else:
                expr = "(uint32_t)(int32_t)(int16_t)(((c->r[%d] & 0xFFu) << 8) | ((c->r[%d] >> 8) & 0xFFu))" % (rm, rm)
            return ["c->r[%d] = %s;" % (rd, expr)]

        if base in ("ldrex", "ldrexb", "ldrexh"):
            rd = isa.reg_num(ops[0])
            mem_txt = ops[1]
            inner = isa.split_operands(mem_txt[1:-1])
            rn = isa.reg_num(inner[0])
            if rd is None or rn is None:
                raise Unsupported(ins.op_str)
            size = 1 if base.endswith("b") else (2 if base.endswith("h") else 4)
            return ["c->r[%d] = arm_mem_read(c, c->r[%d], %d);" % (rd, rn, size)]

        if base in ("strex", "strexb", "strexh"):
            rd = isa.reg_num(ops[0])
            rm = isa.reg_num(ops[1])
            mem_txt = ops[2]
            inner = isa.split_operands(mem_txt[1:-1])
            rn = isa.reg_num(inner[0])
            if None in (rd, rm, rn):
                raise Unsupported(ins.op_str)
            size = 1 if base.endswith("b") else (2 if base.endswith("h") else 4)
            return ["arm_mem_write(c, c->r[%d], %d, c->r[%d]);" % (rn, size, rm),
                    "c->r[%d] = 0;" % rd]

        if base in ("mrc", "mcr"):
            # p15,0,rX,c13,c0,3 = TPIDRURO (TLS-Zeiger)
            if "c13" in ins.op_str and "c0" in ins.op_str:
                rd = isa.reg_num(ops[2])
                if rd is None:
                    raise Unsupported(ins.op_str)
                if base == "mrc":
                    return ["c->r[%d] = c->tpidruro;" % rd]
                return ["c->tpidruro = c->r[%d];" % rd]
                rd = isa.reg_num(ops[2]) if len(ops) > 2 else None
            if base == "mrc" and rd is not None:
                return ["c->r[%d] = 0;" % rd]
            return []

        if base in ("vmla", "vmls", "vnmul"):
            regs = [isa.vfp_reg(o) for o in ops]
            if len(regs) != 3 or any(r is None for r in regs):
                raise Unsupported(ins.op_str)
            dbl = 1 if regs[0][0] == "d" else 0
            if dbl:
                raise Unsupported(ins.op_str)
            d, n, mm = regs[0][1], regs[1][1], regs[2][1]
            prod = "vfp_get_s(c, %d) * vfp_get_s(c, %d)" % (n, mm)
            if base == "vmla":
                return ["vfp_set_s(c, %d, vfp_get_s(c, %d) + %s);" % (d, d, prod)]
            if base == "vmls":
                return ["vfp_set_s(c, %d, vfp_get_s(c, %d) - %s);" % (d, d, prod)]
            return ["vfp_set_s(c, %d, -(%s));" % (d, prod)]

        if base == "vcvt":
            parts = m.split(".")
            dst = isa.vfp_reg(ops[0])
            src = isa.vfp_reg(ops[1])
            if dst is None or src is None or len(parts) < 3:
                raise Unsupported(ins.op_str)
            to, from_ = parts[1], parts[2]
            d, s = dst[1], src[1]
            if to == "f32" and from_ == "s32":
                return ["vfp_set_s(c, %d, (float)(int32_t)c->sreg[%d]);" % (d, s)]
            if to == "f32" and from_ == "u32":
                return ["vfp_set_s(c, %d, (float)c->sreg[%d]);" % (d, s)]
            if to == "s32" and from_ == "f32":
                return ["c->sreg[%d] = (uint32_t)(int32_t)vfp_get_s(c, %d);" % (d, s)]
            if to == "u32" and from_ == "f32":
                return ["c->sreg[%d] = (uint32_t)vfp_get_s(c, %d);" % (d, s)]
            if to == "f64" and from_ == "s32":
                return ["vfp_set_d(c, %d, (double)(int32_t)c->sreg[%d]);" % (d, s)]
            if to == "f32" and from_ == "f64":
                return ["vfp_set_s(c, %d, (float)vfp_get_d(c, %d));" % (d, s)]
            if to == "f64" and from_ == "f32":
                return ["vfp_set_d(c, %d, (double)vfp_get_s(c, %d));" % (d, s)]
            if to == "s32" and from_ == "f64":
                return ["c->sreg[%d] = (uint32_t)(int32_t)vfp_get_d(c, %d);" % (d, s)]
            raise Unsupported(ins.op_str)

        if base in ("vldmia", "vldmdb", "vstmia", "vstmdb"):
            regs = isa.vfp_reg_list(ops[1])
            words = []
            for kind, idx in regs:
                if kind == "s":
                    words.append(idx)
                else:
                    words.append(idx * 2)
                    words.append(idx * 2 + 1)
            base_tok = ops[0]
            wb = base_tok.endswith("!")
            rn = isa.reg_num(base_tok.rstrip("!"))
            if rn is None:
                raise Unsupported(ins.op_str)
            load = base.startswith("vldm")
            dec = base.endswith("db")
            lines = ["{"]
            if dec and load:
                lines.append("  c->r[%d] -= %du;" % (rn, 4 * len(words)))
            for k, w in enumerate(words):
                if load:
                    lines.append("  c->sreg[%d] = arm_mem_read(c, c->r[%d] + %du, 4);"
                                 % (w, rn, 4 * k))
                else:
                    lines.append("  arm_mem_write(c, c->r[%d] + %du, 4, c->sreg[%d]);"
                                 % (rn, 4 * k, w))
            if wb:
                lines.append("  c->r[%d] += %du;" % (rn, 4 * len(words)))
            lines.append("}")
            return lines

        if base == "vmrs":
            return ["vfp_fpscr_to_flags(c);"]
        if base == "vmsr":
            rd = isa.reg_num(ops[1])
            if rd is None:
                raise Unsupported(ins.op_str)
            return ["c->fpscr = c->r[%d];" % rd]

        raise Unsupported(ins.mnemonic)

    # ---------- dispatch ----------
    def emit(self, ins):
        pc = ins.address
        cond = None if ins.cc == A.ARM_CC_AL else ins.cc
        if ins.id in DP_OPS:
            lines = self.dp(ins)
        elif ins.id in MEM_OPS:
            lines = self.mem(ins)
        elif ins.id in (A.ARM_INS_PUSH, A.ARM_INS_POP) or ins.id in LDM_IDS or ins.id in STM_IDS:
            lines = self.multi(ins)
        elif ins.id in (A.ARM_INS_B, A.ARM_INS_BL, A.ARM_INS_BX, A.ARM_INS_BLX):
            lines = self.branch(ins)
        elif ins.id == A.ARM_INS_NOP:
            lines = []
        elif ins.id == A.ARM_INS_SVC:
            imm = ins.operands[0].imm if ins.operands else 0
            lines = ["arm_svc(c, %du);" % u32(imm)]
        elif ins.id == A.ARM_INS_MRS:
            ops = isa.split_operands(ins.op_str)
            rd = isa.reg_num(ops[0])
            if rd is None:
                raise Unsupported(ins.op_str)
            lines = ["c->r[%d] = (c->n << 31) | (c->z << 30) | (c->c << 29) | (c->v << 28);" % rd]
        elif ins.id == A.ARM_INS_MSR:
            ops = isa.split_operands(ins.op_str)
            if len(ops) < 2 or ops[1].startswith("#"):
                raise Unsupported(ins.op_str)
            rn = isa.reg_num(ops[1])
            if rn is None:
                raise Unsupported(ins.op_str)
            lines = ["{ uint32_t v = c->r[%d]; c->n = (v >> 31) & 1u; c->z = (v >> 30) & 1u; "
                     "c->c = (v >> 29) & 1u; c->v = (v >> 28) & 1u; }" % rn]
        elif ins.id == A.ARM_INS_CLZ:
            ops = isa.split_operands(ins.op_str)
            rd = isa.reg_num(ops[0])
            rm = isa.reg_num(ops[1])
            if rd is None or rm is None:
                raise Unsupported(ins.op_str)
            lines = ["c->r[%d] = c->r[%d] ? (uint32_t)__builtin_clz(c->r[%d]) : 32u;" % (rd, rm, rm)]
        elif ins.id in (A.ARM_INS_MOVW, A.ARM_INS_MOVT):
            ops = isa.split_operands(ins.op_str)
            rd = isa.reg_num(ops[0])
            val = self.value(ops[1], pc)
            if rd is None:
                raise Unsupported(ins.op_str)
            if ins.id == A.ARM_INS_MOVW:
                lines = ["c->r[%d] = %s & 0xFFFFu;" % (rd, val)]
            else:
                lines = ["c->r[%d] = (c->r[%d] & 0xFFFFu) | ((%s & 0xFFFFu) << 16);" % (rd, rd, val)]
        elif ins.id in (A.ARM_INS_MUL, A.ARM_INS_MLA):
            ops = isa.split_operands(ins.op_str)
            rd = isa.reg_num(ops[0])
            if rd is None or len(ops) < 2:
                raise Unsupported(ins.op_str)
            sf = 1 if ins.update_flags else 0
            if ins.id == A.ARM_INS_MUL:
                rn = isa.reg_num(ops[1])
                rm = isa.reg_num(ops[2]) if len(ops) > 2 else None
                if rn is None or rm is None:
                    raise Unsupported(ins.op_str)
                lines = ["c->r[%d] = arm_mul(c, c->r[%d], c->r[%d], %d);" % (rd, rm, rn, sf)]
            else:
                rn = isa.reg_num(ops[1])
                rm = isa.reg_num(ops[2])
                ra = isa.reg_num(ops[3])
                if None in (rn, rm, ra):
                    raise Unsupported(ins.op_str)
                lines = ["arm_mla(c, %d, c->r[%d], c->r[%d], c->r[%d], %d);" % (rd, rn, rm, ra, sf)]
        elif ins.id in (A.ARM_INS_UMULL, A.ARM_INS_UMLAL, A.ARM_INS_SMULL, A.ARM_INS_SMLAL):
            ops = isa.split_operands(ins.op_str)
            if len(ops) < 4:
                raise Unsupported(ins.op_str)
            rdlo = isa.reg_num(ops[0])
            rdhi = isa.reg_num(ops[1])
            rn = isa.reg_num(ops[2])
            rm = isa.reg_num(ops[3])
            if None in (rdlo, rdhi, rn, rm):
                raise Unsupported(ins.op_str)
            signed = 1 if ins.id in (A.ARM_INS_SMULL, A.ARM_INS_SMLAL) else 0
            acc = 1 if ins.id in (A.ARM_INS_UMLAL, A.ARM_INS_SMLAL) else 0
            sf = 1 if ins.update_flags else 0
            lines = ["arm_mull(c, %d, %d, c->r[%d], c->r[%d], %d, %d, %d);" % (
                rdlo, rdhi, rn, rm, signed, acc, sf)]
        elif ins.id in (A.ARM_INS_VLDR, A.ARM_INS_VSTR, A.ARM_INS_VMOV, A.ARM_INS_VADD,
                        A.ARM_INS_VSUB, A.ARM_INS_VMUL, A.ARM_INS_VDIV, A.ARM_INS_VNEG,
                        A.ARM_INS_VABS, A.ARM_INS_VSQRT, A.ARM_INS_VCMP, A.ARM_INS_VCMPE,
                        A.ARM_INS_VMRS, A.ARM_INS_VMSR, A.ARM_INS_VPUSH, A.ARM_INS_VPOP):
            lines = self.vfp(ins)
        else:
            lines = self.by_mnemonic(ins)

        if cond is not None:
            wrapped = ["if (arm_cond(c, %d)) {" % (cond - 1)]
            wrapped += ["  " + ln for ln in lines]
            wrapped.append("}")
            return wrapped
        return lines


def fallback(ins):
    try:
        raw = int.from_bytes(ins.bytes, "little")
    except Exception:
        raw = 0
    return ['arm_unimplemented(c, 0x%08Xu, 0x%08Xu, "%s"); return;' % (
        u32(ins.address), u32(raw), ins.mnemonic)]
