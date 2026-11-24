"""Codegen fuer ARMv6-SIMD-, DSP- und einige Sonderformen.

Jede Funktion liefert eine Liste von C-Zeilen oder None (nicht unterstuetzt).
Die Opcodes entsprechen den Enums in runtime/arm_cpu.h.
"""
from . import isa

SIMD8 = {"uadd8": 0, "sadd8": 1, "usub8": 2, "ssub8": 3,
         "uqadd8": 4, "sqadd8": 5, "uqsub8": 6, "sqsub8": 7,
         "uhadd8": 8, "shadd8": 9, "uhsub8": 10, "shsub8": 11}

SIMD16 = {"uadd16": 0, "sadd16": 1, "usub16": 2, "ssub16": 3,
          "uqadd16": 4, "sqadd16": 5, "uqsub16": 6, "sqsub16": 7,
          "uhadd16": 8, "shadd16": 9, "uhsub16": 10, "shsub16": 11}

SMLA = {"smlabb": (0, 0), "smlabt": (0, 1), "smlatb": (1, 0), "smlatt": (1, 1)}
SMUL = {"smulbb": (0, 0), "smulbt": (0, 1), "smultb": (1, 0), "smultt": (1, 1)}

QADD = {"qadd": 0, "qsub": 1, "qdadd": 2, "qdsub": 3}

NAMES = set(SIMD8) | set(SIMD16) | set(SMLA) | set(SMUL) | set(QADD) | {
    "sel", "uxtb16", "sxtb16", "pkhbt", "pkhtb", "usat16", "ssat16",
    "usad8", "usada8", "ldrexd", "strexd", "adr",
}


def handles(base):
    return base in NAMES


def _regs(ops, count):
    if len(ops) < count:
        return None
    regs = [isa.reg_num(o) for o in ops[:count]]
    if any(r is None for r in regs):
        return None
    return regs


def _mem_base(text):
    t = text.strip()
    wb = t.endswith("!")
    if wb:
        t = t[:-1]
    if not (t.startswith("[") and t.endswith("]")):
        return None
    inner = isa.split_operands(t[1:-1])
    if not inner:
        return None
    return isa.reg_num(inner[0])


def emit(emitter, ins, base, ops):
    if base in SIMD8:
        r = _regs(ops, 3)
        if r is None:
            return None
        d, n, m = r
        return ["c->r[%d] = arm_par8(c, %d, c->r[%d], c->r[%d]);" % (d, SIMD8[base], n, m)]

    if base in SIMD16:
        r = _regs(ops, 3)
        if r is None:
            return None
        d, n, m = r
        return ["c->r[%d] = arm_par16(c, %d, c->r[%d], c->r[%d]);" % (d, SIMD16[base], n, m)]

    if base == "sel":
        r = _regs(ops, 3)
        if r is None:
            return None
        d, n, m = r
        return ["c->r[%d] = arm_sel(c, c->r[%d], c->r[%d]);" % (d, n, m)]

    if base in ("uxtb16", "sxtb16"):
        r = _regs(ops, 2)
        if r is None:
            return None
        d, m = r
        rot = 0
        if len(ops) > 2:
            amt = isa.imm_value(ops[2].split()[-1]) or 0
            rot = amt // 8  # arm_ext16 erwartet Anzahl 8-Bit-Rotationen
        return ["c->r[%d] = arm_ext16(%d, c->r[%d], %d);"
                % (d, 1 if base == "sxtb16" else 0, m, rot)]

    if base in ("pkhbt", "pkhtb"):
        r = _regs(ops, 3)
        if r is None:
            return None
        d, n, m = r
        shift = 0
        if len(ops) > 3:
            shift = isa.imm_value(ops[3].split()[-1]) or 0
        return ["c->r[%d] = arm_pack16(%d, c->r[%d], c->r[%d], %d, %d);"
                % (d, 1 if base == "pkhbt" else 0, n, m, shift, 0)]

    if base in ("usat16", "ssat16"):
        d = isa.reg_num(ops[0]) if len(ops) > 0 else None
        if d is None or len(ops) < 3:
            return None
        bits = isa.imm_value(ops[1])
        m = isa.reg_num(ops[2])
        if bits is None or m is None:
            return None
        return ["c->r[%d] = arm_sat16(c, %d, c->r[%d], %d);"
                % (d, 1 if base == "ssat16" else 0, m, bits)]

    if base in ("usad8", "usada8"):
        r = _regs(ops, 3)
        if r is None:
            return None
        d, n, m = r
        if base == "usada8":
            if len(ops) < 4:
                return None
            a = isa.reg_num(ops[3])
            if a is None:
                return None
            return ["c->r[%d] = arm_usad(c, c->r[%d], c->r[%d], 1, c->r[%d]);" % (d, n, m, a)]
        return ["c->r[%d] = arm_usad(c, c->r[%d], c->r[%d], 0, 0);" % (d, n, m)]

    if base in QADD:
        r = _regs(ops, 3)
        if r is None:
            return None
        d, m, n = r
        return ["c->r[%d] = arm_qadd32(c, %d, c->r[%d], c->r[%d]);" % (d, QADD[base], m, n)]

    if base in SMLA:
        r = _regs(ops, 4)
        if r is None:
            return None
        d, n, m, a = r
        nt, mt = SMLA[base]
        return ["c->r[%d] = arm_smla16(c, c->r[%d], c->r[%d], c->r[%d], %d, %d, 1);"
                % (d, n, m, a, nt, mt)]

    if base in SMUL:
        r = _regs(ops, 3)
        if r is None:
            return None
        d, n, m = r
        nt, mt = SMUL[base]
        return ["c->r[%d] = arm_smla16(c, c->r[%d], c->r[%d], 0, %d, %d, 0);"
                % (d, n, m, nt, mt)]

    if base == "ldrexd":
        r = _regs(ops, 1)
        if r is None or len(ops) < 2:
            return None
        d = r[0]
        rn = _mem_base(ops[2])
        if rn is None:
            return None
        return ["{ uint64_t v = arm_excl_load64(c, c->r[%d]);" % rn,
                "  c->r[%d] = (uint32_t)v;" % d,
                "  c->r[%d] = (uint32_t)(v >> 32); }" % (d + 1)]

    if base == "strexd":
        r = _regs(ops, 2)
        if r is None or len(ops) < 3:
            return None
        d, m = r
        rn = _mem_base(ops[3])
        if rn is None:
            return None
        return ["c->r[%d] = arm_excl_store64(c, c->r[%d], c->r[%d], c->r[%d]);"
                % (d, rn, m, m + 1)]

    if base == "adr":
        d = isa.reg_num(ops[0]) if len(ops) > 1 else None
        v = isa.imm_value(ops[1]) if len(ops) > 1 else None
        if d is None or v is None:
            return None
        if emitter.thumb:
            target = (emitter.pc_value(ins.address) & ~3) + v
        else:
            target = ins.address + 8 + v
        return ["c->r[%d] = 0x%08Xu;" % (d, target & 0xFFFFFFFF)]

    return None
