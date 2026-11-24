"""C-Codegenerator fuer ARM11-Instruktionen (MVP)."""
import capstone.arm as A

from . import isa
from . import simd


# Spielspezifische Nach-Aufruf-Hooks (technische Schuld, docs/WORKAROUNDS.md).
def _load_call_hooks():
    import json, os
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "workarounds", "block_hooks.json")
    if not os.path.exists(path):
        return {}
    data = json.load(open(path, encoding="utf-8"))
    return {int(h["addr"], 16): list(h["c"]) for h in data.get("after_call", [])}


CALL_HOOKS = _load_call_hooks()


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
TBB_IDS = tuple(getattr(A, n) for n in ("ARM_INS_TBB", "ARM_INS_TBH")
                if getattr(A, n, None) is not None)

SHIFT = {"lsl": "SH_LSL", "lsr": "SH_LSR", "asr": "SH_ASR", "ror": "SH_ROR"}

VFP_ARITH = {"vadd": 0, "vsub": 1, "vmul": 2, "vdiv": 3}
VFP_UNARY = {"vneg": 4, "vabs": 5, "vsqrt": 6}

COND_SUFFIXES = ("eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl",
                 "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le")
VFP_BASES = ("vldr", "vstr", "vmov", "vadd", "vsub", "vmul", "vdiv", "vneg",
             "vabs", "vsqrt", "vcmp", "vcmpe", "vmrs", "vmsr", "vpush", "vpop",
             "vmla", "vmls", "vnmul", "vcvt", "vldmia", "vldmdb", "vstmia", "vstmdb")


CORE_BASES = {"nop", "hint", "pld", "pldw", "clrex", "cdp", "cdp2",
              "ldrt", "strt", "ldrbt", "strbt", "ldrht", "strht",
              "ldrsbt", "ldrsht", "smlsld", "smlald", "smlsldx", "smlaldx",
              "ldc", "stc", "mcrr", "mrrc", "lsl", "lsr", "asr", "ror",
              "rrx", "uxth", "uxtb", "sxth", "sxtb", "uxtah", "sxtah", "ldrd", "strd",
              "ldrex", "strex", "mrc", "mcr", "ldm", "stm", "push", "pop", "rev",
              "rev16", "revsh", "usat", "ssat", "qadd", "qsub", "sdiv", "udiv"}


def _known(core):
    return (simd.handles(core) or core in VFP_BASES or core in CORE_BASES)


def strip_cc(mnemonic):
    """Entfernt eine Bedingungs-Endung (z.B. 'vmovls' -> 'vmov', 'ldrdeq' -> 'ldrd')."""
    m = mnemonic.split(".")[0].lower()
    # IT-Blocks (Thumb-2): it/itt/ite/... sind Praefixe fuer bedingte
    # Ausfuehrung; die folgenden Instruktionen tragen die Bedingung selbst.
    if m.startswith("it") and len(m) <= 5 and set(m[1:]) <= set("te"):
        return "it"
    if _known(m):
        return m
    for cc in COND_SUFFIXES:
        if m.endswith(cc):
            core = m[:-2]
            if _known(core):
                return core
    return m


def u32(v):
    return v & 0xFFFFFFFF


# 16-Bit-Thumb-Formen, die ihre Flags nur ausserhalb eines IT-Blocks setzen
# (die "S"-Form ist die Kodierung; im IT-Block ist es dieselbe Instruktion
# ohne Flag-Wirkung, ARM ARM A7.7.3).
IT_NO_FLAGS_16 = {"add", "sub", "and", "eor", "lsl", "lsr", "asr", "adc",
                  "sbc", "ror", "rsb", "orr", "mul", "mov", "mvn", "bic"}


def imm_shifter_carry(ins, thumb):
    """Shifter-Carry eines Immediate-Operanden aus der Kodierung (0/1, -1 = C bleibt).

    ARM: Datenverarbeitung mit Immediate (Bit 25), Rotation = Bits 11..8.
    Rotation != 0 -> Carry = Bit 31 des rotierten Werts, sonst unveraendert.
    Thumb-2 (32 Bit, modified immediate): imm12 = i:imm3:imm8; bei
    imm12[11:10] == 0 bleibt C, sonst Carry = Bit 31 des expandierten Werts.
    16-Bit-Thumb-Immediates veraendern C nicht.
    """
    raw = bytes(ins.bytes)
    if not thumb:
        if len(raw) != 4:
            return -1
        w = int.from_bytes(raw, "little")
        if (w >> 26) & 3 != 0 or not (w >> 25) & 1:
            return -1
        rot = ((w >> 8) & 0xF) * 2
        if rot == 0:
            return -1
        imm8 = w & 0xFF
        val = ((imm8 >> rot) | (imm8 << (32 - rot))) & 0xFFFFFFFF
        return (val >> 31) & 1
    if len(raw) != 4:
        return -1
    hw1 = int.from_bytes(raw[0:2], "little")
    hw2 = int.from_bytes(raw[2:4], "little")
    # Data processing (modified immediate): 11110 i 0 op S Rn | 0 imm3 Rd imm8
    if (hw1 & 0xFA00) != 0xF000 or (hw2 & 0x8000):
        return -1
    i = (hw1 >> 10) & 1
    imm3 = (hw2 >> 12) & 7
    imm8 = hw2 & 0xFF
    imm12 = (i << 11) | (imm3 << 8) | imm8
    if (imm12 >> 10) == 0:
        return -1
    unrot = 0x80 | (imm12 & 0x7F)
    r = (imm12 >> 7) & 0x1F
    val = ((unrot >> r) | (unrot << (32 - r))) & 0xFFFFFFFF
    return (val >> 31) & 1


def sets_flags(ins, thumb, in_it=False):
    """S-Bit aus der Kodierung lesen.

    capstone 5.0.7 setzt ``update_flags`` fuer ADC/SBC/RSC immer auf True
    (das C-Flag wird als impliziter Eingang gewertet). Das S-Bit steht bei ARM
    in Bit 20, bei 32-Bit-Thumb in Bit 4 des ersten Halfwords.

    Zusaetzlich gilt fuer 16-Bit-Thumb: Innerhalb eines IT-Blocks setzen die
    "S"-Formen (ADD/SUB/MOV/LSL/... T1) die Flags *nicht*; die Kodierung sagt
    das nicht, sie haengt vom ITSTATE ab (ARM ARM A7.7.3). Ohne diese Regel
    setzte der Recompiler Flags, die das Original unveraendert laesst.
    """
    try:
        b = ins.bytes
    except Exception:
        b = b""
    if thumb and in_it and len(b) == 2:
        m = ins.mnemonic.lower().split(".")[0]
        base = m[:-1] if m.endswith("s") else m
        if base in IT_NO_FLAGS_16:
            return 0
    if not thumb and len(b) >= 4:
        return (int.from_bytes(b[:4], "little") >> 20) & 1
    if thumb:
        if len(b) == 4:
            return (int.from_bytes(b[:2], "little") >> 4) & 1
        return 1 if ins.update_flags else 0
    return 1 if ins.update_flags else 0


def it_conditions(insns):
    """Bedingungen fuer Instruktionen in Thumb-2-IT-Bloecken.

    ``isa.decode`` dekodiert jede Instruktion einzeln; capstone kennt dann den
    IT-Zustand nicht und meldet AL. Ohne diese Zuordnung wuerde eine
    Instruktion in einem IT-Block unbedingt ausgefuehrt (falsche Flags/Register).
    """
    out = {}
    itstate = 0
    for ins in insns:
        if itstate:
            # capstone-Konvention: ARM_CC_EQ == 1, IT liefert 0..14
            out[id(ins)] = (itstate >> 4) + 1
            if (itstate & 7) == 0:
                itstate = 0
            else:
                itstate = (itstate & 0xE0) | ((itstate << 1) & 0x1F)
            continue
        m = ins.mnemonic.lower().split(".")[0]
        if m.startswith("it") and len(m) <= 5 and set(m[1:]) <= set("te"):
            b = ins.bytes
            if len(b) >= 2:
                hw = int.from_bytes(b[:2], "little")
                firstcond = (hw >> 4) & 0xF
                mask = hw & 0xF
                if mask:
                    itstate = (firstcond << 4) | mask
    return out


def _build_aliases():
    out = {}
    for name, target in (("ARM_INS_MOVS", "ARM_INS_MOV"), ("ARM_INS_ADDS", "ARM_INS_ADD"),
                         ("ARM_INS_SUBS", "ARM_INS_SUB"), ("ARM_INS_ANDS", "ARM_INS_AND"),
                         ("ARM_INS_ORRS", "ARM_INS_ORR"), ("ARM_INS_EORS", "ARM_INS_EOR"),
                         ("ARM_INS_BICS", "ARM_INS_BIC"), ("ARM_INS_MVNS", "ARM_INS_MVN"),
                         ("ARM_INS_LSLS", "ARM_INS_LSL"), ("ARM_INS_LSRS", "ARM_INS_LSR"),
                         ("ARM_INS_ASRS", "ARM_INS_ASR"), ("ARM_INS_RORS", "ARM_INS_ROR"),
                         ("ARM_INS_LDRT", "ARM_INS_LDR"), ("ARM_INS_STRT", "ARM_INS_STR"),
                         ("ARM_INS_LDRBT", "ARM_INS_LDRB"), ("ARM_INS_STRBT", "ARM_INS_STRB"),
                         ("ARM_INS_LDRHT", "ARM_INS_LDRH"), ("ARM_INS_STRHT", "ARM_INS_STRH"),
                         ("ARM_INS_LDRSBT", "ARM_INS_LDRSB"), ("ARM_INS_LDRSHT", "ARM_INS_LDRSH"),
                         ("ARM_INS_SMLSLD", "ARM_INS_SMULL")):
        a = getattr(A, name, None)
        b = getattr(A, target, None)
        if a is not None and b is not None:
            out[a] = b
    return out


ID_ALIASES = _build_aliases()


class InsnView(object):
    """Duenne Huelle, die eine Alias-Instruktion auf ihre Basis-ID abbildet."""
    def __init__(self, ins, iid):
        self._ins = ins
        self.id = iid

    def __getattr__(self, name):
        return getattr(self._ins, name)


class Emitter:
    def __init__(self, func_addr, thumb=False):
        self.func = func_addr
        self.thumb = thumb
        self.block_labels = None   # set of local block addresses (for ldr pc)
        self.pc_targets = None     # alle Instruktionsadressen (indirekte PC-Ziele)
        self.ret_bx = set()        # bx-Adressen mit Rueckkehrziel aus dem Stack
        self.pseudo_calls = {}     # {adresse: LR-Fortsetzung} (scan.pseudo_call_info)
        self.ret_mov = set()       # `mov pc, rN` mit rN = unveraenderte lr-Kopie
        self.it_cond = {}          # id(ins) -> Bedingung aus einem IT-Block

    def pc_value(self, pc):
        if self.thumb:
            return (pc + 4) & ~3
        return pc + 8

    def label(self, addr):
        return "L_%08X_%08X" % (self.func, u32(addr))

    def goto_cont(self, cont, nxt=None):
        """Nach einem Pseudo-Aufruf an der LR-Fortsetzung weiterlaufen."""
        if nxt is not None and cont == nxt:
            return []
        if self.block_labels is not None and cont in self.block_labels:
            return ["goto %s;" % self.label(cont)]
        # Fortsetzung kein lokaler Block: ueber den Gast-PC verteilen.
        return ["c->r[15] = 0x%08Xu;" % u32(cont)] + self.dynamic_pc("c->r[15]")

    def dynamic_pc(self, expr, interwork=True, call_cont=None):
        """Schreibt PC aus einem Ausdruck und springt dorthin.

        Ziele innerhalb der Funktion werden per switch/goto angesprungen
        (lokale Bloecke haben keinen Eintrag in g_entries), alles andere ueber
        arm_call_guest. Der Aufruf kehrt nie zurueck: nach einem Sprung ist die
        Funktion beendet (Ruecksprung erfolgt im Ziel ueber dessen LR).
        """
        lines = ["{", "  uint32_t t = (%s);" % expr,
                 "  c->r[15] = t & ~1u;"]
        if interwork:
            lines.append("  c->t = t & 1u;")
        if call_cont is not None:
            # Berechneter Aufruf (LR = Fortsetzung, scan.pseudo_call_info):
            # immer als echter Aufruf ausfuehren, auch bei lokalem Ziel, damit
            # dessen Rueckkehr (pop pc / bx lr) nicht diese C-Funktion beendet.
            lines.append("  arm_call_guest(c, t);")
            lines.append("  if (c->aborted) return;")
            lines.extend("  " + ln for ln in self.goto_cont(call_cont))
            lines.append("}")
            return lines
        labs = []
        src = self.pc_targets if self.pc_targets else self.block_labels
        if src:
            labs = sorted(a for a in src
                          if self.func <= a < self.func + 0x10000)
        if labs:
            lines.append("  switch (c->r[15]) {")
            for a in labs:
                lines.append("    case 0x%08Xu: goto %s;" % (u32(a), self.label(a)))
            lines.append("    default: arm_call_guest(c, t); break;")
            lines.append("  }")
            # Das Ziel kann per `mov pc, lr` mitten in *diese* Funktion
            # zurueckkehren (Tail-Jump auf eine Adresse, die das Ziel als LR
            # erhalten hat, z. B. `add lr, pc, #4; add pc, rN, rM`). Dann muss
            # die Ausfuehrung am gesetzten Gast-PC weiterlaufen, statt die
            # Funktion zu verlassen. Nur echte Blockanfaenge sind anspringbar;
            # alle Rueckkehradressen von `bl` sind Bloecke.
            local = sorted(self.block_labels or ())
            if local:
                lines.append("  switch (c->r[15]) {")
                for a in local:
                    lines.append("    case 0x%08Xu: goto %s;"
                                 % (u32(a), self.label(a)))
                lines.append("    default: return;")
                lines.append("  }")
            else:
                lines.append("  return;")
        else:
            lines.append("  arm_call_guest(c, t); return;")
        lines.append("}")
        lines.append("return;")
        return lines

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
            if self.thumb and getattr(self, "_thumb_pc_unaligned", False):
                # Thumb-Datenverarbeitung mit PC als Registeroperand
                # (`add r7, pc`, `mov r0, pc`): PC = Instruktion + 4, ohne
                # Ausrichtung. Align(PC, 4) gilt nur fuer adr/Literal-Loads.
                return "0x%08Xu" % u32(pc + 4)
            return "0x%08Xu" % u32(self.pc_value(pc))
        return "c->r[%d]" % n

    def operand2(self, toks, pc):
        if not toks:
            raise Unsupported("leerer Operand")
        if len(toks) == 2 and toks[0].startswith("#") and toks[1].startswith("#"):
            imm8 = isa.imm_value(toks[0])
            rot = isa.imm_value(toks[1])
            if imm8 is None or rot is None:
                raise Unsupported(" ".join(toks))
            v = imm8 & 0xFF
            r = rot & 31
            if r:
                v = ((v >> r) | (v << (32 - r))) & 0xFFFFFFFF
            return "0x%08Xu" % v, None
        base = self.value(toks[0], pc)
        if len(toks) == 1:
            return base, None
        st = toks[1].strip()
        if st.lower() == "rrx":
            return "arm_rrx(c, %s, &sh)" % base, "sh"
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
    def dp(self, ins, in_it=False):
        ops = isa.split_operands(ins.op_str)
        # Register-Form (kein Immediate) -> PC unausgerichtet (siehe value()).
        self._thumb_pc_unaligned = bool(self.thumb and ops
                                        and not ops[-1].strip().startswith("#"))
        try:
            return self._dp(ins, in_it)
        finally:
            self._thumb_pc_unaligned = False

    def _dp(self, ins, in_it=False):
        code, uses_rn, writes = DP_OPS[ins.id]
        ops = isa.split_operands(ins.op_str)
        pc = ins.address
        if writes:
            rd = isa.reg_num(ops[0])
            if rd is None:
                raise Unsupported(ins.op_str)
            if uses_rn:
                if self.thumb and len(ops) == 2:
                    rn = "c->r[%d]" % rd
                    rest = ops[1:]
                else:
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
        sf = sets_flags(ins, self.thumb, in_it)
        imm_c = -1
        if not sh and rest and rest[0].strip().startswith("#"):
            imm_c = imm_shifter_carry(ins, self.thumb)
        if not sh and imm_c >= 0:
            # Rotierter Immediate-Operand: Shifter-Carry = Bit 31 des Werts
            # (ARM ARM A5.1.3; Thumb-2 ThumbExpandImm_C). Wirkt nur bei
            # logischen Operationen mit S-Bit (tst/teq/ands/orrs/movs ...).
            lines = ["arm_dp(c, %s, %d, %s, %s, %d, %d);" % (code, rd, rn, o2, sf, imm_c)]
        elif sh:
            lines = ["{", "  int sh = -1;",
                     "  arm_dp(c, %s, %d, %s, %s, %d, sh);" % (code, rd, rn, o2, sf),
                     "}"]
        else:
            lines = ["arm_dp(c, %s, %d, %s, %s, %d, -1);" % (code, rd, rn, o2, sf)]
        if writes and rd == 15:
            src = isa.reg_num(ops[1]) if len(ops) > 1 else None
            if code == "DP_MOV" and src is not None and src != 14 and pc in self.ret_mov:
                # rN ist eine Kopie von lr (cli.lr_copy_returns): Rueckkehr.
                lines.append("return;")
            elif code == "DP_MOV" and src == 14:
                # `mov pc, lr` ist eine Rueckkehr in den C-Aufrufer; ein
                # arm_call_guest am Rueckkehrziel wuerde eine Gastfunktion
                # an einer Adresse mitten in der Aufruferfunktion starten.
                lines.append("c->r[15] = c->r[14];")
                lines.append("return;")
            else:
                # `mov pc, rN` / `add pc, ...` / Sprungtabellen: dynamisch
                # weiterleiten (lokale Bloecke per switch/goto).
                lines.extend(self.dynamic_pc("c->r[15]", call_cont=self.pseudo_calls.get(pc)))
        return lines

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
        basev = "0x%08Xu" % u32(self.pc_value(pc)) if base_reg == 15 else "c->r[%d]" % base_reg
        post = len(ops) > 2
        if post:
            wb = True
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
            lines.append("  uint32_t o = %s;" % off)
        else:
            lines.append("  uint32_t a = %s + %s;" % (basev, off))
        if load:
            lines.append("  c->r[%d] = arm_ld(c, a, %d, %d);" % (rd, size, signed))
        else:
            lines.append("  arm_st(c, a, %d, c->r[%d]);" % (size, rd))
        if wb:
            if post:
                lines.append("  c->r[%d] = a + o;" % base_reg)
            else:
                lines.append("  c->r[%d] = a;" % base_reg)
        lines.append("}")
        if load and rd == 15 and base_reg == 13 and post:
            # `ldr pc, [sp], #imm` = Epilog-Rueckkehr (wie `pop {pc}`): das
            # Ziel ist die Rueckkehradresse im Aufrufer, kein Sprung.
            lines.append("c->t = c->r[15] & 1u; c->r[15] &= ~1u;")
            lines.append("return;")
            return lines
        if load and rd == 15:
            # Dynamischer Sprung (z. B. ldr pc,[pc,rN,lsl#2]).
            # Ziele INNERHALB der Funktion muessen per goto auf das lokale
            # Label springen (sonst wird der Epilog uebersprungen und SP
            # bleibt unbalanciert). Nur echte Fremd-Eintraege via
            # arm_call_guest ausfuehren.
            lines.append("if (arm_ldrpc_log_on()) arm_ldrpc_log(c->r[15]);")
            lines.append("{ uint32_t t = c->r[15]; c->r[15] = t & ~1u;")
            lines.append("  c->t = t & 1u;")
            src = self.pc_targets if self.pc_targets else self.block_labels
            labs = []
            if src:
                labs = sorted(a for a in src
                              if self.func <= a < self.func + 0x10000)
            if labs:
                lines.append("  switch (c->r[15]) {")
                for a in labs:
                    lines.append("    case 0x%08Xu: goto %s;"
                                 % (u32(a), self.label(a)))
                lines.append("    default: arm_call_guest(c, t); return;")
                lines.append("  }")
            else:
                lines.append("  arm_call_guest(c, t); return;")
            lines.append("}")
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
        sfx = m[3:5]
        mode = {"ib": 1, "da": 2, "db": 3}.get(sfx, 0)
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
            if pc in self.pseudo_calls:
                # `add lr, pc, #k; ...; b<cond> ziel` mit LR = Fortsetzung: ein
                # Aufruf (scan.is_pseudo_call). Kein arm_restore_sp: das Ziel
                # raeumt ggf. einen vom Aufrufer selbst gebauten Stack-Frame mit
                # ab, der SP nach der Rueckkehr ist daher bewusst ein anderer.
                return (["fn_%08X(c);" % u32(tgt), "if (c->aborted) return;"]
                        + self.goto_cont(self.pseudo_calls[pc],
                                         nxt=pc + ins.size))
            if self.block_labels is not None and tgt not in self.block_labels:
                # Ziel ausserhalb der Funktion (Tail-Call): als Gastaufruf
                # ausfuehren; das Ziel kehrt ueber LR zum Aufrufer zurueck.
                return ["{ uint32_t sp0 = c->r[13];",
                        "  arm_call_guest(c, 0x%08Xu);" % u32(tgt),
                        "  arm_restore_sp(c, sp0); }",
                        "return;"]
            return ["goto %s;" % self.label(tgt)]
        if ins.id == A.ARM_INS_BL:
            if not ins.operands or ins.operands[0].type != A.ARM_OP_IMM:
                raise Unsupported(ins.op_str)
            tgt = ins.operands[0].imm
            nxt = pc + ins.size + (1 if self.thumb else 0)
            lines = ["{ uint32_t sp0 = c->r[13];",
                     "  c->r[14] = 0x%08Xu;" % u32(nxt),
                     "  fn_%08X(c);" % u32(tgt)]
            post = CALL_HOOKS.get(u32(tgt))
            if post:
                lines.extend("  " + ln for ln in post)
            lines.append("  arm_restore_sp(c, sp0); }")
            lines.append("if (c->aborted) return;")
            return lines
        if ins.id == A.ARM_INS_BLX:
            if ins.operands and ins.operands[0].type == A.ARM_OP_IMM:
                tgt = ins.operands[0].imm
                return ["{ uint32_t sp0 = c->r[13];",
                        "  c->r[14] = 0x%08Xu;" % u32(pc + ins.size + (1 if self.thumb else 0)),
                        "  c->t = %du;" % (0 if self.thumb else 1),
                        "  fn_%08X(c);" % u32(tgt),
                        "  arm_restore_sp(c, sp0); }",
                        "if (c->aborted) return;"]
        if ins.id in (A.ARM_INS_BX, A.ARM_INS_BLX):
            reg = isa.reg_num(ops[0])
            if reg is None:
                raise Unsupported(ins.op_str)
            if ins.id == A.ARM_INS_BX and reg != 14 and pc in self.ret_bx:
                # Rueckkehr-Idiom: Ziel wurde aus dem Stack geladen (pop/ldm).
                # Das ist eine Rueckkehr in den C-Aufrufer; ein Dispatch ueber
                # arm_call_guest wuerde eine Gastfunktion an einer
                # Rueckkehradresse starten (Prolog fehlt -> SP driftet).
                return ["return;"]
            if ins.id == A.ARM_INS_BLX or reg != 14:
                lines = []
                if ins.id == A.ARM_INS_BLX:
                    lines.append("{ uint32_t sp0 = c->r[13];")
                    lines.append("  c->r[14] = 0x%08Xu;"
                                 % u32(pc + ins.size + (1 if self.thumb else 0)))
                    lines.append("  arm_call_guest(c, c->r[%d]);" % reg)
                    lines.append("  arm_restore_sp(c, sp0); }")
                else:
                    lines.append("arm_call_guest(c, c->r[%d]);" % reg)
                lines.append("if (c->aborted) return;")
                if ins.id == A.ARM_INS_BX:
                    lines.append("return;")
                return lines
            lines = []
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
            basev = ("0x%08Xu" % u32(self.pc_value(pc))
                     if base_reg == 15 else "c->r[%d]" % base_reg)
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
                    raise Unsupported(ins.op_str)
                if b and a is None:
                    rd = isa.reg_num(ops[0])
                    if rd is None:
                        raise Unsupported(ins.op_str)
                    if b[0] == "s":
                        return ["c->r[%d] = c->sreg[%d];" % (rd, b[1])]
                    raise Unsupported(ins.op_str)
            if len(ops) == 3:
                # vmov dN, rX, rY  bzw.  vmov rX, rY, dN (64 Bit als zwei Worte)
                a = isa.vfp_reg(ops[0])
                b = isa.vfp_reg(ops[2])
                if a and a[0] == "d":
                    r1, r2 = isa.reg_num(ops[1]), isa.reg_num(ops[2])
                    if r1 is None or r2 is None:
                        raise Unsupported(ins.op_str)
                    return ["c->sreg[%d] = c->r[%d];" % (a[1] * 2, r1),
                            "c->sreg[%d] = c->r[%d];" % (a[1] * 2 + 1, r2)]
                if b and b[0] == "d":
                    r1, r2 = isa.reg_num(ops[0]), isa.reg_num(ops[1])
                    if r1 is None or r2 is None:
                        raise Unsupported(ins.op_str)
                    return ["c->r[%d] = c->sreg[%d];" % (r1, b[1] * 2),
                            "c->r[%d] = c->sreg[%d];" % (r2, b[1] * 2 + 1)]
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
            rd = isa.reg_num(ops[0]) if ops else None
            if rd is not None and "fpscr" in ins.op_str.lower():
                return ["c->r[%d] = c->fpscr;" % rd]
            return ["vfp_fpscr_to_flags(c);"]
        if base == "vmsr":
            rd = isa.reg_num(ops[1])
            if rd is None:
                raise Unsupported(ins.op_str)
            return ["vfp_set_fpscr(c, c->r[%d]);" % rd]

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

        if simd.handles(base):
            lines = simd.emit(self, ins, base, ops)
            if lines is None:
                raise Unsupported(ins.op_str)
            return lines

        if base in ("vnmla", "vnmls"):
            regs = [isa.vfp_reg(o) for o in ops]
            if len(regs) != 3 or any(r is None for r in regs):
                raise Unsupported(ins.op_str)
            dbl = 1 if regs[0][0] == "d" else 0
            if any(r[0] != regs[0][0] for r in regs):
                raise Unsupported(ins.op_str)
            # VNMLA: d = -d - n*m, VNMLS: d = -d + n*m
            mode = 3 if base == "vnmla" else 4
            return ["vfp_muladd(c, %d, %d, %d, %d, %d);"
                    % (dbl, regs[0][1], regs[1][1], regs[2][1], mode)]

        if base == "clrex":
            return ["arm_excl_clear(c);"]

        if base in ("nop", "hint", "pld", "pldw", "dbg", "sev", "wfe", "wfi",
                    "it", "cdp", "cdp2", "ldc", "stc", "mcrr", "mrrc") or base.startswith(("ldc", "stc")):
            return []

        if base.startswith(("lsl", "lsr", "asr", "ror", "rrx")):
            kind = {"lsl": "SH_LSL", "lsr": "SH_LSR", "asr": "SH_ASR",
                    "ror": "SH_ROR", "rrx": "SH_ROR"}[base[:3]]
            rd = isa.reg_num(ops[0])
            if rd is None:
                raise Unsupported(ins.op_str)

            if base == "rrx":
                expr = "arm_rrx(c, %s, &sh)" % self.value(ops[1], pc)
                plain = "arm_rrx(c, %s, 0)" % self.value(ops[1], pc)
            elif len(ops) >= 3:
                src = self.value(ops[1], pc)
                amt = ops[2]
                if amt.startswith("#"):
                    expr = "arm_shift_imm(%s, %du, %s, &sh)" % (kind, isa.imm_value(amt), src)
                    plain = "arm_shift_imm(%s, %du, %s, 0)" % (kind, isa.imm_value(amt), src)
                else:
                    ra = self.value(amt, pc)
                    expr = "arm_shift_reg(%s, %s, %s, &sh)" % (kind, ra, src)
                    plain = "arm_shift_reg(%s, %s, %s, 0)" % (kind, ra, src)
            else:
                src = self.value(ops[1], pc)
                expr = src
                plain = src

            if not ins.update_flags:
                return ["c->r[%d] = %s;" % (rd, plain)]

            return ["{", "  int sh = -1;",
                    "  uint32_t r = %s;" % expr,
                    "  c->r[%d] = r;" % rd,
                    "  c->n = r >> 31; c->z = (r == 0) ? 1u : 0u;",
                    "  if (sh >= 0) c->c = (uint32_t)sh;",
                    "}"]

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
            basev = ("0x%08Xu" % u32(self.pc_value(pc))
                     if base_reg == 15 else "c->r[%d]" % base_reg)
            off = "0"
            post = len(ops) > 3
            if post:
                wb = True
                off = self.value(ops[3], pc)
            elif len(inner) > 1:
                off = self.value(inner[1], pc)
            if post:
                lines = ["{", "  uint32_t a = %s;" % basev,
                         "  uint32_t o = %s;" % off]
            else:
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
                if post:
                    lines.append("  c->r[%d] = a + o;" % base_reg)
                else:
                    lines.append("  c->r[%d] = a;" % base_reg)
            lines.append("}")
            return lines

        if base in ("uxtah", "sxtah", "uxtab", "sxtab"):
            rd = isa.reg_num(ops[0])
            rn = isa.reg_num(ops[1])
            rm = isa.reg_num(ops[2])
            if None in (rd, rn, rm):
                raise Unsupported(ins.op_str)
            src = "c->r[%d]" % rm
            if len(ops) > 3 and ops[3].lower().startswith("ror"):
                sh = isa.imm_value(ops[3].split()[-1]) or 0
                src = "arm_shift_imm(SH_ROR, %du, %s, 0)" % (sh, src)
            bits = 8 if base.endswith("tab") else 16
            if base.startswith("s"):
                ext = "(uint32_t)(int32_t)(int%d_t)(%s)" % (bits, src)
            else:
                ext = "((%s) & 0x%Xu)" % (src, (1 << bits) - 1)
            return ["c->r[%d] = c->r[%d] + %s;" % (rd, rn, ext)]

        if base in ("usat", "ssat"):
            rd = isa.reg_num(ops[0])
            bits = isa.imm_value(ops[1])
            rm = isa.reg_num(ops[2])
            shift = 0
            kind = "lsl"
            if len(ops) > 3:
                parts = ops[3].split()
                if len(parts) == 2:
                    kind = parts[0].lower()
                shift = isa.imm_value(parts[-1]) or 0
            if rd is None or rm is None or bits is None:
                raise Unsupported(ins.op_str)
            src = "c->r[%d]" % rm
            if shift:
                if kind == "asr":
                    src = "((uint32_t)((int32_t)%s >> %du))" % (src, shift)
                else:
                    src = "(%s << %du)" % (src, shift)
            return ["c->r[%d] = arm_sat32(c, %d, %s, %d);"
                    % (rd, 0 if base == "usat" else 1, src, bits)]

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

        if base in ("ubfx", "sbfx"):
            rd = isa.reg_num(ops[0])
            rn = isa.reg_num(ops[1])
            lsb = isa.imm_value(ops[2])
            width = isa.imm_value(ops[3])
            if rd is None or rn is None or lsb is None or width is None:
                raise Unsupported(ins.op_str)
            if width < 1 or lsb + width > 32:
                raise Unsupported(ins.op_str)
            mask = (1 << width) - 1
            if base == "ubfx":
                return ["c->r[%d] = (c->r[%d] >> %d) & 0x%Xu;" % (rd, rn, lsb, mask)]
            sh = 32 - width
            return ["c->r[%d] = (uint32_t)((int32_t)(((c->r[%d] >> %d) & 0x%Xu) << %d) >> %d);"
                    % (rd, rn, lsb, mask, sh, sh)]

        if base in ("bfi", "bfc"):
            rd = isa.reg_num(ops[0])
            if base == "bfi":
                rn = isa.reg_num(ops[1])
                lsb = isa.imm_value(ops[2])
                width = isa.imm_value(ops[3])
            else:
                rn = 0
                lsb = isa.imm_value(ops[1])
                width = isa.imm_value(ops[2])
            if rd is None or lsb is None or width is None or width < 1:
                raise Unsupported(ins.op_str)
            mask = ((1 << width) - 1) << lsb
            if base == "bfc":
                return ["c->r[%d] &= ~0x%08Xu;" % (rd, mask)]
            return ["c->r[%d] = (c->r[%d] & ~0x%08Xu) | ((c->r[%d] << %d) & 0x%08Xu);"
                    % (rd, rd, mask, rn, lsb, mask)]

        if base in ("ldrex", "ldrexb", "ldrexh"):
            rd = isa.reg_num(ops[0])
            mem_txt = ops[1]
            inner = isa.split_operands(mem_txt[1:-1])
            rn = isa.reg_num(inner[0])
            if rd is None or rn is None:
                raise Unsupported(ins.op_str)
            size = 1 if base.endswith("b") else (2 if base.endswith("h") else 4)
            return ["c->r[%d] = arm_excl_load(c, c->r[%d], %d);" % (rd, rn, size)]

        if base in ("strex", "strexb", "strexh"):
            rd = isa.reg_num(ops[0])
            rm = isa.reg_num(ops[1])
            mem_txt = ops[2]
            inner = isa.split_operands(mem_txt[1:-1])
            rn = isa.reg_num(inner[0])
            if None in (rd, rm, rn):
                raise Unsupported(ins.op_str)
            size = 1 if base.endswith("b") else (2 if base.endswith("h") else 4)
            return ["c->r[%d] = arm_excl_store(c, c->r[%d], %d, c->r[%d]);"
                    % (rd, rn, size, rm)]

        if base in ("mrc", "mcr", "mrc2", "mcr2"):
            rd = isa.reg_num(ops[2]) if len(ops) > 2 else None
            if "c13" in ins.op_str and "c0" in ins.op_str and rd is not None:
                if base.startswith("mrc"):
                    return ["c->r[%d] = c->tpidruro;" % rd]
                return ["c->tpidruro = c->r[%d];" % rd]
            if base.startswith("mrc") and rd is not None:
                return ["c->r[%d] = 0;" % rd]
            return []

        if base in ("vmla", "vmls", "vnmul"):
            regs = [isa.vfp_reg(o) for o in ops]
            if len(regs) != 3 or any(r is None for r in regs):
                raise Unsupported(ins.op_str)
            if any(r[0] != regs[0][0] for r in regs):
                raise Unsupported(ins.op_str)
            dbl = 1 if regs[0][0] == "d" else 0
            mode = {"vmla": 0, "vmls": 1, "vnmul": 2}[base]
            return ["vfp_muladd(c, %d, %d, %d, %d, %d);"
                    % (dbl, regs[0][1], regs[1][1], regs[2][1], mode)]

        if base == "vcvt":
            types = [p for p in m.split(".") if p in ("s32", "u32", "f32", "f64")]
            dst = isa.vfp_reg(ops[0])
            src = isa.vfp_reg(ops[1])
            if dst is None or src is None or len(types) != 2:
                raise Unsupported(ins.op_str)
            to, from_ = types
            fbits = None
            if len(ops) >= 3:
                fbits = isa.imm_value(ops[2])
                if fbits is None:
                    raise Unsupported(ins.op_str)
            if to in ("s32", "u32"):
                if dst[0] != "s" or from_ not in ("f32", "f64"):
                    raise Unsupported(ins.op_str)
                signed = 1 if to == "s32" else 0
                src_dbl = 1 if from_ == "f64" else 0
                if fbits is None:
                    return ["vfp_cvt_f2i(c, %d, %d, %d, %d);"
                            % (src_dbl, signed, dst[1], src[1])]
                return ["vfp_cvt_f2i_fixed(c, %d, %d, %d, %d, %d);"
                        % (src_dbl, signed, dst[1], src[1], fbits)]
            if from_ in ("s32", "u32"):
                if src[0] != "s":
                    raise Unsupported(ins.op_str)
                dst_dbl = 1 if to == "f64" else 0
                if dst[0] != ("d" if dst_dbl else "s"):
                    raise Unsupported(ins.op_str)
                signed = 1 if from_ == "s32" else 0
                if fbits is None:
                    return ["vfp_cvt_i2f(c, %d, %d, %d, %d);"
                            % (dst_dbl, signed, dst[1], src[1])]
                return ["vfp_cvt_i2f_fixed(c, %d, %d, %d, %d, %d);"
                        % (dst_dbl, signed, dst[1], src[1], fbits)]
            if fbits is not None:
                raise Unsupported(ins.op_str)
            if dst[0] != ("d" if to == "f64" else "s") or \
               src[0] != ("d" if from_ == "f64" else "s"):
                raise Unsupported(ins.op_str)
            return ["vfp_cvt_fp(c, %d, %d, %d, %d);"
                    % (1 if to == "f64" else 0, dst[1],
                       1 if from_ == "f64" else 0, src[1])]

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
            rd = isa.reg_num(ops[0]) if ops else None
            if rd is not None and "fpscr" in ins.op_str.lower():
                return ["c->r[%d] = c->fpscr;" % rd]
            return ["vfp_fpscr_to_flags(c);"]
        if base == "vmsr":
            rd = isa.reg_num(ops[1])
            if rd is None:
                raise Unsupported(ins.op_str)
            return ["vfp_set_fpscr(c, c->r[%d]);" % rd]

        raise Unsupported(ins.mnemonic)

    def thumb_table_branch(self, ins):
        """Thumb tbb/tbh [pc, rN]: Tabellensprung."""
        ops = isa.split_operands(ins.op_str)
        r = None
        for tok in ops:
            t = tok.strip()
            if t.startswith("["):
                inner = isa.split_operands(t.strip("[]"))
                if len(inner) >= 2:
                    r = isa.reg_num(inner[1])
        if r is None:
            raise Unsupported(ins.op_str)
        base = (ins.address + 4) & ~3
        half = ins.id == getattr(A, "ARM_INS_TBH", -1)
        if half:
            lines = ["{", "  uint32_t b = 0x%08Xu;" % u32(base),
                     "  uint32_t o = arm_mem_read(c, b + c->r[%d] * 2u, 2);" % r,
                     "  uint32_t t = b + 2u * o;"]
        else:
            lines = ["{", "  uint32_t b = 0x%08Xu;" % u32(base),
                     "  uint32_t o = arm_mem_read(c, b + c->r[%d], 1);" % r,
                     "  uint32_t t = b + 2u * o;"]
        labs = []
        src = self.pc_targets if self.pc_targets else self.block_labels
        if src:
            labs = sorted(a for a in src
                          if self.func <= a < self.func + 0x10000)
        lines.append("  c->r[15] = t & ~1u; c->t = 1u;")
        if labs:
            lines.append("  switch (c->r[15]) {")
            for a in labs:
                lines.append("    case 0x%08Xu: goto %s;" % (u32(a), self.label(a)))
            lines.append("    default: arm_call_guest(c, t); return;")
            lines.append("  }")
        else:
            lines.append("  arm_call_guest(c, t); return;")
        lines.append("}")
        lines.append("return;")
        return lines

    # ---------- dispatch ----------
    def emit(self, ins):
        in_it = id(ins) in self.it_cond
        cc0 = self.it_cond.get(id(ins), ins.cc)
        iid = ID_ALIASES.get(ins.id)
        if iid is not None:
            ins = InsnView(ins, iid)
        pc = ins.address
        cond = None if cc0 == A.ARM_CC_AL else cc0
        if ins.id in DP_OPS:
            lines = self.dp(ins, in_it)
        elif ins.id in MEM_OPS:
            lines = self.mem(ins)
        elif ins.id in (A.ARM_INS_PUSH, A.ARM_INS_POP) or ins.id in LDM_IDS or ins.id in STM_IDS:
            lines = self.multi(ins)
        elif ins.id in (A.ARM_INS_B, A.ARM_INS_BL, A.ARM_INS_BX, A.ARM_INS_BLX):
            lines = self.branch(ins)
        elif ins.id == A.ARM_INS_NOP:
            lines = []
        elif ins.id in TBB_IDS:
            lines = self.thumb_table_branch(ins)
        elif ins.id in (A.ARM_INS_CBZ, A.ARM_INS_CBNZ):
            ops = isa.split_operands(ins.op_str)
            r = isa.reg_num(ops[0]) if ops else None
            tgt = None
            if len(ins.operands) > 1 and ins.operands[1].type == A.ARM_OP_IMM:
                tgt = ins.operands[1].imm
            if r is None or tgt is None:
                raise Unsupported(ins.op_str)
            op = "==" if ins.id == A.ARM_INS_CBZ else "!="
            lines = ["if (c->r[%d] %s 0) goto %s;" % (r, op, self.label(tgt))]
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
                rm = isa.reg_num(ops[2]) if len(ops) > 2 else rn
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
