"""Differentieller Recompiler-Test: Unicorn (Referenz) vs. generierter C-Code.

Erzeugt systematische und zufaellige ARM- bzw. Thumb-2-Instruktionsfolgen,
assembliert sie mit keystone, fuehrt sie einmal in Unicorn (reiner
Referenz-Interpreter) und einmal ueber ``tools/recomp`` -> C (zig cc) aus und
vergleicht Register r0-r14, NZCVQ/GE und den Speicher-Digest.

  python tools/recomp_difftest.py
  python tools/recomp_difftest.py --seed 7 --count 200
  python tools/recomp_difftest.py --mode arm --quick
  python tools/recomp_difftest.py --filter pkhbt

Exit-Code 0 = alle Faelle gleich, 1 = mindestens ein Unterschied.  Das
Werkzeug ist reines Testwerkzeug.  Jeder gemeldete Unterschied ist ein
Recompiler-/Runtime-Semantikfehler und gehoert als fester Fall in
``tests/mvp_selftest.py``.
"""
import argparse
import os
import random
import re
import subprocess
import sys

from keystone import Ks, KS_ARCH_ARM, KS_MODE_ARM, KS_MODE_THUMB, KS_MODE_LITTLE_ENDIAN
import capstone
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_MODE_THUMB, UC_MODE_LITTLE_ENDIAN
import unicorn.arm_const as U

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from recomp import isa, cli  # noqa: E402

ARM_BASE = isa.ARM_BASE
DATA_BASE = 0x20000
DATA_SIZE = 0x1000
MEM_SIZE = 0x200000
STACK = DATA_BASE + 0x800
STRIDE = 0x40

CC = ["", "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls",
      "ge", "lt", "gt", "le"]

OUT = os.path.join(ROOT, "out", "difftest")


# --------------------------------------------------------------------------
# Fall-Generatoren
# --------------------------------------------------------------------------
def _add(cases, asm, regs=None, small=False, note=""):
    cases.append({"asm": asm, "regs": regs or {}, "small": small, "note": note})


def _rot_imm(rng):
    imm8 = rng.randrange(1, 256)
    rot = rng.randrange(16)
    v = imm8 & 0xFF
    r = 2 * rot
    if r:
        v = ((v >> r) | (v << (32 - r))) & 0xFFFFFFFF
    return v


def arm_dp_cases(rng):
    cases = []
    R = lambda: rng.randrange(13)
    for op in ("and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc", "orr", "bic"):
        _add(cases, "%s r%d, r%d, r%d" % (op, R(), R(), R()), note=op)
        _add(cases, "%ss r%d, r%d, r%d" % (op, R(), R(), R()), note=op + "s")
        sh = rng.choice(["lsl", "lsr", "asr", "ror"])
        amt = rng.randrange(1, 32)
        _add(cases, "%s r%d, r%d, r%d, %s #%d" % (op, R(), R(), R(), sh, amt),
             note=op + " shift")
        _add(cases, "%s r%d, r%d, r%d, %s r%d" % (op, R(), R(), R(), sh, R()),
             note=op + " regshift")
        _add(cases, "%s r%d, r%d, #0x%08X" % (op, R(), R(), _rot_imm(rng)),
             note=op + " imm")
        _add(cases, "%ss r%d, r%d, r%d, ror #%d" % (op, R(), R(), R(), amt),
             note=op + "s ror")
    for op in ("mov", "mvn"):
        _add(cases, "%s r%d, r%d" % (op, R(), R()), note=op)
        _add(cases, "%ss r%d, r%d, lsl #%d" % (op, R(), R(), rng.randrange(1, 32)),
             note=op + "s lsl")
        _add(cases, "%s r%d, #0x%08X" % (op, R(), _rot_imm(rng)), note=op + " imm")
        _add(cases, "%s r%d, r%d, rrx" % (op, R(), R()), note=op + " rrx")
        _add(cases, "%s r%d, r%d, ror r%d" % (op, R(), R(), R()), note=op + " ror reg")
    for op in ("tst", "teq", "cmp", "cmn"):
        _add(cases, "%s r%d, r%d" % (op, R(), R()), note=op)
        _add(cases, "%s r%d, r%d, lsr #%d" % (op, R(), R(), rng.randrange(1, 32)),
             note=op + " lsr")
        _add(cases, "%s r%d, r%d, asr r%d" % (op, R(), R(), R()), note=op + " asr reg")
        _add(cases, "%s r%d, #0x%08X" % (op, R(), _rot_imm(rng)), note=op + " imm")
    for sh in ("lsl #0", "lsr #1", "lsr #32", "asr #1", "asr #32",
               "ror #1", "ror #31", "rrx"):
        _add(cases, "mov r%d, r%d, %s" % (R(), R(), sh), note="shift " + sh)
        _add(cases, "adds r%d, r%d, r%d, %s" % (R(), R(), R(), sh), note="shiftc " + sh)
        _add(cases, "adcs r%d, r%d, r%d, %s" % (R(), R(), R(), sh), note="adc shift")
    for cc in CC[1:]:
        _add(cases, "add%s r%d, r%d, r%d" % (cc, R(), R(), R()), note="cond add " + cc)
        _add(cases, "mov%s r%d, #0x55" % (cc, R()), note="cond mov " + cc)
        _add(cases, "cmp%s r%d, r%d" % (cc, R(), R()), note="cond cmp " + cc)
        _add(cases, "add%ss r%d, r%d, r%d, lsl #3" % (cc, R(), R(), R()),
             note="cond adds " + cc)
    return cases


def arm_mul_cases(rng):
    cases = []
    R = lambda: rng.randrange(13)
    for _ in range(3):
        _add(cases, "mul r%d, r%d, r%d" % (R(), R(), R()), note="mul")
        _add(cases, "muls r%d, r%d, r%d" % (R(), R(), R()), note="muls")
        _add(cases, "mla r%d, r%d, r%d, r%d" % (R(), R(), R(), R()), note="mla")
        _add(cases, "mlas r%d, r%d, r%d, r%d" % (R(), R(), R(), R()), note="mlas")
        lo = rng.randrange(0, 12, 2)
        hi = lo + 1
        _add(cases, "umull r%d, r%d, r%d, r%d" % (lo, hi, R(), R()), note="umull")
        _add(cases, "umlals r%d, r%d, r%d, r%d" % (lo, hi, R(), R()), note="umlals")
        _add(cases, "smull r%d, r%d, r%d, r%d" % (lo, hi, R(), R()), note="smull")
        _add(cases, "smlals r%d, r%d, r%d, r%d" % (lo, hi, R(), R()), note="smlals")
    return cases


def arm_misc_cases(rng):
    cases = []
    R = lambda: rng.randrange(13)
    for m in ("clz", "rev", "rev16", "revsh", "sxtb", "sxth", "uxtb", "uxth",
              "sxtb16", "uxtb16"):
        _add(cases, "%s r%d, r%d" % (m, R(), R()), note=m)
        if m != "clz":
            _add(cases, "%s r%d, r%d, ror #8" % (m, R(), R()), note=m + " ror")
    for m in ("uxtab", "uxtah", "sxtab", "sxtah"):
        _add(cases, "%s r%d, r%d, r%d" % (m, R(), R(), R()), note=m)
        _add(cases, "%s r%d, r%d, r%d, ror #16" % (m, R(), R(), R()), note=m + " ror")
    for _ in range(2):
        _add(cases, "movw r%d, #0x%04X" % (R(), rng.randrange(0x10000)), note="movw")
        _add(cases, "movt r%d, #0x%04X" % (R(), rng.randrange(0x10000)), note="movt")
    return cases


def arm_media_cases(rng):
    cases = []
    R = lambda: rng.randrange(13)
    for m in ("uadd8", "sadd8", "usub8", "ssub8", "uqadd8", "sqadd8",
              "uqsub8", "sqsub8", "uhadd8", "shadd8", "uhsub8", "shsub8"):
        _add(cases, "%s r%d, r%d, r%d" % (m, R(), R(), R()), note=m)
    for m in ("uadd16", "sadd16", "usub16", "ssub16", "uqadd16", "sqadd16",
              "uqsub16", "sqsub16", "uhadd16", "shadd16", "uhsub16", "shsub16"):
        _add(cases, "%s r%d, r%d, r%d" % (m, R(), R(), R()), note=m)
    _add(cases, "sel r%d, r%d, r%d" % (R(), R(), R()), note="sel")
    _add(cases, "usad8 r%d, r%d, r%d" % (R(), R(), R()), note="usad8")
    _add(cases, "usada8 r%d, r%d, r%d, r%d" % (R(), R(), R(), R()), note="usada8")
    for m in ("qadd", "qsub", "qdadd", "qdsub"):
        _add(cases, "%s r%d, r%d, r%d" % (m, R(), R(), R()), note=m)
    for m in ("smulbb", "smulbt", "smultb", "smultt"):
        _add(cases, "%s r%d, r%d, r%d" % (m, R(), R(), R()), note=m)
    for m in ("smlabb", "smlabt", "smlatb", "smlatt"):
        _add(cases, "%s r%d, r%d, r%d, r%d" % (m, R(), R(), R(), R()), note=m)
    for m in ("pkhbt", "pkhtb"):
        _add(cases, "%s r%d, r%d, r%d" % (m, R(), R(), R()), note=m + " #0")
        _add(cases, "%s r%d, r%d, r%d, lsl #%d" % (m, R(), R(), R(), rng.randrange(1, 32)),
             note=m)
        _add(cases, "%s r%d, r%d, r%d, asr #%d" % (m, R(), R(), R(), rng.randrange(1, 25)),
             note=m + " asr")
    for m in ("usat", "ssat"):
        _add(cases, "%s r%d, #%d, r%d" % (m, R(), rng.randrange(1, 32), R()), note=m)
        _add(cases, "%s r%d, #%d, r%d, %s #%d"
             % (m, R(), rng.randrange(1, 32), R(), rng.choice(["lsl", "asr"]),
                rng.randrange(1, 32)), note=m + " shift")
    for m in ("usat16", "ssat16"):
        _add(cases, "%s r%d, #%d, r%d" % (m, R(), rng.randrange(1, 17), R()), note=m)
    for m in ("ubfx", "sbfx"):
        _add(cases, "%s r%d, r%d, #%d, #%d"
             % (m, R(), R(), rng.randrange(0, 24), rng.randrange(1, 25)), note=m)
    _add(cases, "bfi r%d, r%d, #%d, #%d"
         % (R(), R(), rng.randrange(0, 24), rng.randrange(1, 25)), note="bfi")
    _add(cases, "bfc r%d, #%d, #%d"
         % (R(), rng.randrange(0, 24), rng.randrange(1, 25)), note="bfc")
    return cases


def _mem_base(rng):
    return DATA_BASE + 0x400 + 4 * rng.randrange(0, 32)


def arm_mem_cases(rng):
    cases = []
    base = 1
    for op in ("ldr", "str", "ldrb", "strb", "ldrh", "strh", "ldrsb", "ldrsh"):
        b = _mem_base(rng)
        rd = rng.choice([r for r in range(13) if r != base])
        ro = rng.choice([r for r in range(13) if r not in (base, rd)])
        for form in ("[rB]", "[rB, #4]", "[rB, #-4]", "[rB, rO]",
                     "[rB, rO, lsl #2]", "[rB, #4]!", "[rB], #4", "[rB], rO"):
            txt = "%s r%d, %s" % (op, rd, form)
            txt = txt.replace("rB", "r%d" % base).replace("rO", "r%d" % ro)
            _add(cases, txt, regs={base: b}, small=True, note=op + " " + form)
            rd = rng.choice([r for r in range(13) if r != base])
    for op in ("ldrd", "strd"):
        for _ in range(4):
            b = _mem_base(rng)
            rd = rng.randrange(0, 12, 2)
            form = rng.choice(["[rB, #8]", "[rB, #8]!", "[rB], #8"])
            txt = "%s r%d, r%d, %s" % (op, rd, rd + 1, form.replace("rB", "r%d" % base))
            _add(cases, txt, regs={base: b}, small=True, note=op)
    mask = []
    while len(mask) < 3:
        r = rng.randrange(13)
        if r not in mask:
            mask.append(r)
    lst = "{%s}" % ", ".join("r%d" % r for r in sorted(mask))
    for m in ("ldmia", "ldmib", "ldmda", "ldmdb", "stmia", "stmib", "stmda", "stmdb"):
        b = _mem_base(rng)
        _add(cases, "%s r%d, %s" % (m, base, lst), regs={base: b}, small=True, note=m)
        b = _mem_base(rng)
        _add(cases, "%s r%d!, %s" % (m, base, lst), regs={base: b}, small=True,
             note=m + " wb")
    _add(cases, "push {r0, r4, lr}", note="push")
    _add(cases, "pop {r0, r4}", note="pop")
    _add(cases, "push {r1-r5}", note="push range")
    _add(cases, "pop {r1-r5}", note="pop range")
    R = lambda: rng.randrange(13)
    _add(cases, "ldr r%d, [pc, #4]\nbx lr\n.word 0x%08X" % (R(), rng.getrandbits(32)),
         note="ldr literal")
    _add(cases, "ldrb r%d, [pc, #4]\nbx lr\n.word 0x%08X" % (R(), rng.getrandbits(32)),
         note="ldrb literal")
    # Exklusiv-Zugriffe ohne Threadwechsel: muessen in Unicorn und im
    # recompilierten Code gleich wirken (STREX schreibt, Rueckgabe 0).
    b = _mem_base(rng)
    _add(cases, "ldrex r0, [r1]\nstrex r2, r3, [r1]\nldr r4, [r1]",
         regs={1: b, 3: 0x12345678}, small=True, note="ldrex strex erfolg")
    _add(cases, "ldrexb r0, [r1]\nstrexb r2, r3, [r1]\nldrb r4, [r1]",
         regs={1: b, 3: 0x9A}, small=True, note="ldrexb strexb erfolg")
    _add(cases, "ldrexh r0, [r1]\nstrexh r2, r3, [r1]\nldrh r4, [r1]",
         regs={1: b, 3: 0x9ABC}, small=True, note="ldrexh strexh erfolg")
    # CLREX entwertet die Reservierung: STREX scheitert (Rueckgabe 1) und
    # schreibt nicht.
    _add(cases, "ldrex r0, [r1]\nclrex\nstrex r2, r3, [r1]\nldr r4, [r1]",
         regs={1: b, 3: 0x12345678}, small=True, note="ldrex clrex strex fehler")
    # Ein normaler Store auf die ueberwachte Adresse entwertet sie ebenfalls.
    _add(cases, "ldrex r0, [r1]\nstr r5, [r1]\nstrex r2, r3, [r1]\nldr r4, [r1]",
         regs={1: b, 3: 0x12345678, 5: 0x0A0B0C0D}, small=True,
         note="ldrex store strex fehler")
    # LDREXD/STREXD mit geradem Registerpaar und 8-Byte-Ausrichtung.
    b8 = DATA_BASE + 0x400 + 8 * rng.randrange(0, 16)
    _add(cases, "ldrexd r0, r1, [r2]\nstrexd r3, r4, r5, [r2]\nldr r6, [r2]\nldr r7, [r2, #4]",
         regs={2: b8, 4: 0x11223344, 5: 0x55667788}, small=True,
         note="ldrexd strexd erfolg")
    return cases


def thumb_dp_cases(rng):
    cases = []
    R = lambda: rng.randrange(13)
    for op in ("and", "eor", "sub", "rsb", "add", "adc", "sbc", "orr", "bic"):
        _add(cases, "%s.w r%d, r%d, r%d" % (op, R(), R(), R()), note=op + ".w")
        _add(cases, "%ss.w r%d, r%d, r%d" % (op, R(), R(), R()), note=op + "s.w")
        _add(cases, "%s.w r%d, r%d, r%d, %s #%d"
             % (op, R(), R(), R(), rng.choice(["lsl", "lsr", "asr"]),
                rng.randrange(1, 32)), note=op + ".w shift")
        _add(cases, "%s.w r%d, r%d, r%d, %s r%d"
             % (op, R(), R(), R(), rng.choice(["lsl", "lsr", "asr", "ror"]), R()),
             note=op + ".w regshift")
        _add(cases, "%s.w r%d, r%d, #0x%08X" % (op, R(), R(), _rot_imm(rng)),
             note=op + ".w imm")
    for op in ("mov", "mvn"):
        _add(cases, "%s.w r%d, r%d" % (op, R(), R()), note=op + ".w")
        _add(cases, "%ss r%d, r%d" % (op, R(), R()), note=op + "s")
        _add(cases, "%ss.w r%d, r%d, lsl #%d" % (op, R(), R(), rng.randrange(1, 32)),
             note=op + "s.w")
        _add(cases, "%s r%d, #0x%02X" % (op, R(), rng.randrange(256)), note=op + " imm")
    for op in ("tst", "teq", "cmp", "cmn"):
        _add(cases, "%s.w r%d, r%d" % (op, R(), R()), note=op + ".w")
        _add(cases, "%s.w r%d, r%d, lsl #%d" % (op, R(), R(), rng.randrange(1, 32)),
             note=op + ".w lsl")
        if op != "teq":
            _add(cases, "%s r%d, #0x%02X" % (op, R(), rng.randrange(256)), note=op + " imm")
    for _ in range(3):
        rd, rn = R(), R()
        _add(cases, "adds r%d, r%d, #%d" % (rd, rn, rng.randrange(1, 8)), note="adds imm3")
        _add(cases, "subs r%d, r%d, #%d" % (rd, rn, rng.randrange(1, 8)), note="subs imm3")
        _add(cases, "lsls r%d, r%d, #%d" % (rd, rn, rng.randrange(1, 32)), note="lsls")
        _add(cases, "lsrs r%d, r%d, #%d" % (rd, rn, rng.randrange(1, 32)), note="lsrs")
        _add(cases, "asrs r%d, r%d, #%d" % (rd, rn, rng.randrange(1, 32)), note="asrs")
        _add(cases, "ands r%d, r%d" % (rd, rn), note="ands")
        _add(cases, "orrs r%d, r%d" % (rd, rn), note="orrs")
        _add(cases, "eors r%d, r%d" % (rd, rn), note="eors")
        _add(cases, "muls r%d, r%d, r%d" % (rd, rn, R()), note="muls")
    for cc in CC[1:]:
        _add(cases, "it %s\nadd%s r%d, r%d, r%d" % (cc, cc, R(), R(), R()),
             note="it " + cc)
        _add(cases, "itt %s\nmov%s r%d, r%d\nadd%s r%d, r%d, #1"
             % (cc, cc, R(), R(), cc, R(), R()), note="itt " + cc)
    return cases


def thumb_mem_cases(rng):
    cases = []
    base = 1
    for op in ("ldr", "str", "ldrb", "strb", "ldrh", "strh", "ldrsb", "ldrsh"):
        b = _mem_base(rng)
        rd = rng.choice([r for r in range(13) if r != base])
        ro = rng.choice([r for r in range(13) if r not in (base, rd)])
        for form in ("[rB]", "[rB, #4]", "[rB, #-4]", "[rB, rO]",
                     "[rB, rO, lsl #2]", "[rB, #4]!", "[rB], #4"):
            txt = "%s.w r%d, %s" % (op, rd, form)
            txt = txt.replace("rB", "r%d" % base).replace("rO", "r%d" % ro)
            _add(cases, txt, regs={base: b}, small=True, note=op + ".w " + form)
            rd = rng.choice([r for r in range(13) if r != base])
    for op in ("ldrd", "strd"):
        for _ in range(4):
            b = _mem_base(rng)
            rd = rng.randrange(0, 12, 2)
            form = rng.choice(["[rB, #8]", "[rB, #8]!", "[rB], #8"])
            txt = "%s r%d, r%d, %s" % (op, rd, rd + 1, form.replace("rB", "r%d" % base))
            _add(cases, txt, regs={base: b}, small=True, note=op)
    mask = []
    while len(mask) < 3:
        r = rng.randrange(13)
        if r not in mask:
            mask.append(r)
    lst = "{%s}" % ", ".join("r%d" % r for r in sorted(mask))
    for m in ("ldmia", "ldmdb", "stmia", "stmdb"):
        b = _mem_base(rng)
        _add(cases, "%s.w r%d, %s" % (m, base, lst), regs={base: b}, small=True,
             note=m + ".w")
        b = _mem_base(rng)
        _add(cases, "%s.w r%d!, %s" % (m, base, lst), regs={base: b}, small=True,
             note=m + ".w wb")
    _add(cases, "push {r0, r4, lr}", note="push")
    _add(cases, "pop {r0, r4}", note="pop")
    _add(cases, "push.w {r1-r5, lr}", note="push.w")
    return cases


def thumb_media_cases(rng):
    cases = []
    R = lambda: rng.randrange(13)
    for m in ("uadd8", "sadd8", "usub8", "ssub8", "uqadd8", "sqadd8",
              "uqsub8", "sqsub8", "uhadd8", "shadd8", "uhsub8", "shsub8"):
        _add(cases, "%s r%d, r%d, r%d" % (m, R(), R(), R()), note=m)
    for m in ("uadd16", "sadd16", "usub16", "ssub16", "uqadd16", "sqadd16",
              "uqsub16", "sqsub16"):
        _add(cases, "%s r%d, r%d, r%d" % (m, R(), R(), R()), note=m)
    _add(cases, "sel r%d, r%d, r%d" % (R(), R(), R()), note="sel")
    _add(cases, "usad8 r%d, r%d, r%d" % (R(), R(), R()), note="usad8")
    _add(cases, "usada8 r%d, r%d, r%d, r%d" % (R(), R(), R(), R()), note="usada8")
    for m in ("qadd", "qsub", "qdadd", "qdsub"):
        _add(cases, "%s r%d, r%d, r%d" % (m, R(), R(), R()), note=m)
    for m in ("smulbb", "smulbt", "smultb", "smultt"):
        _add(cases, "%s r%d, r%d, r%d" % (m, R(), R(), R()), note=m)
    for m in ("smlabb", "smlabt", "smlatb", "smlatt"):
        _add(cases, "%s r%d, r%d, r%d, r%d" % (m, R(), R(), R(), R()), note=m)
    for m in ("pkhbt", "pkhtb"):
        _add(cases, "%s r%d, r%d, r%d" % (m, R(), R(), R()), note=m + " #0")
        _add(cases, "%s r%d, r%d, r%d, lsl #%d" % (m, R(), R(), R(), rng.randrange(1, 32)),
             note=m)
        _add(cases, "%s r%d, r%d, r%d, asr #%d" % (m, R(), R(), R(), rng.randrange(1, 25)),
             note=m + " asr")
    for m in ("usat", "ssat"):
        _add(cases, "%s r%d, #%d, r%d" % (m, R(), rng.randrange(1, 32), R()), note=m)
        _add(cases, "%s r%d, #%d, r%d, %s #%d"
             % (m, R(), rng.randrange(1, 32), R(), rng.choice(["lsl", "asr"]),
                rng.randrange(1, 32)), note=m + " shift")
    for m in ("usat16", "ssat16"):
        _add(cases, "%s r%d, #%d, r%d" % (m, R(), rng.randrange(1, 17), R()), note=m)
    for m in ("ubfx", "sbfx"):
        _add(cases, "%s r%d, r%d, #%d, #%d"
             % (m, R(), R(), rng.randrange(0, 24), rng.randrange(1, 25)), note=m)
    _add(cases, "bfi r%d, r%d, #%d, #%d"
         % (R(), R(), rng.randrange(0, 24), rng.randrange(1, 25)), note="bfi")
    _add(cases, "bfc r%d, #%d, #%d"
         % (R(), rng.randrange(0, 24), rng.randrange(1, 25)), note="bfc")
    for m in ("clz", "rev", "rev16", "revsh", "sxtb", "sxth", "uxtb", "uxth"):
        _add(cases, "%s r%d, r%d" % (m, R(), R()), note=m)
        _add(cases, "%s.w r%d, r%d, ror #%d" % (m, R(), R(), rng.choice([8, 16, 24])),
             note=m + ".w ror")
    return cases


def _extra_random(cases, rng, count, thumb):
    pool = [c for c in cases if "\n" not in c["asm"]]
    pool = [c for c in pool if not c["regs"] and not c["small"]]
    if not pool:
        return
    for _ in range(count):
        n = rng.randrange(1, 4)
        lines = [rng.choice(pool)["asm"] for _ in range(n)]
        _add(cases, "\n".join(lines), note="random")


def build_cases(rng, count, mode):
    if mode == "arm":
        cases = (arm_dp_cases(rng) + arm_mul_cases(rng) + arm_misc_cases(rng)
                 + arm_media_cases(rng) + arm_mem_cases(rng))
    else:
        cases = (thumb_dp_cases(rng) + thumb_mem_cases(rng)
                 + thumb_media_cases(rng))
    _extra_random(cases, rng, count, mode == "thumb")
    return cases


# --------------------------------------------------------------------------
# Assemblieren + Referenzlauf (Unicorn)
# --------------------------------------------------------------------------
def assemble_cases(cases, thumb):
    ks = Ks(KS_ARCH_ARM, (KS_MODE_THUMB if thumb else KS_MODE_ARM) | KS_MODE_LITTLE_ENDIAN)
    md = capstone.Cs(capstone.CS_ARCH_ARM,
                     capstone.CS_MODE_THUMB if thumb else capstone.CS_MODE_ARM)
    out = []
    for case in cases:
        addr = ARM_BASE + len(out) * STRIDE
        text = case["asm"].strip() + "\nbx lr"
        try:
            enc, _ = ks.asm(text, addr)
            blob = bytes(enc)
        except Exception:
            continue
        if not blob or len(blob) > STRIDE:
            continue
        n = 0
        stop = False
        for ins in md.disasm(blob, addr):
            n += 1
            if ins.mnemonic.startswith("bx") and "lr" in ins.op_str:
                stop = True
                break
        if not stop:
            continue
        c = dict(case)
        c["addr"] = addr
        c["blob"] = blob
        c["ninsn"] = n
        out.append(c)
    return out


def _rng_data(seed):
    """Muss zu fill_data() im C-Harness passen."""
    x = (seed * 2654435761 + 1) & 0xFFFFFFFF
    buf = bytearray(DATA_SIZE)
    for i in range(DATA_SIZE):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        buf[i] = (x >> 16) & 0xFF
    return bytes(buf)


UREGS = [U.UC_ARM_REG_R0, U.UC_ARM_REG_R1, U.UC_ARM_REG_R2, U.UC_ARM_REG_R3,
         U.UC_ARM_REG_R4, U.UC_ARM_REG_R5, U.UC_ARM_REG_R6, U.UC_ARM_REG_R7,
         U.UC_ARM_REG_R8, U.UC_ARM_REG_R9, U.UC_ARM_REG_R10, U.UC_ARM_REG_R11,
         U.UC_ARM_REG_R12, U.UC_ARM_REG_SP, U.UC_ARM_REG_LR]


def reference_run(cases, thumb, rng):
    uc = Uc(UC_ARCH_ARM,
            (UC_MODE_THUMB if thumb else UC_MODE_ARM) | UC_MODE_LITTLE_ENDIAN)
    code_size = 0x1000
    while code_size < len(cases) * STRIDE:
        code_size *= 2
    uc.mem_map(ARM_BASE, code_size)
    uc.mem_map(DATA_BASE, DATA_SIZE)
    refs = []
    errors = []
    for idx, case in enumerate(cases):
        uc.mem_write(ARM_BASE + idx * STRIDE, case["blob"])
        seed = case["data_seed"]
        uc.mem_write(DATA_BASE, _rng_data(seed))
        regs = []
        for r in range(13):
            if r in case["regs"]:
                regs.append(case["regs"][r] & 0xFFFFFFFF)
            elif case["small"]:
                regs.append(rng.randrange(0, 0x1000))
            else:
                regs.append(rng.getrandbits(32))
        regs.append(STACK)
        regs.append(0)
        case["init_regs"] = regs
        for r in range(15):
            uc.reg_write(UREGS[r], regs[r])
        flags = {"n": rng.randrange(2), "z": rng.randrange(2),
                 "c": rng.randrange(2), "v": rng.randrange(2)}
        case["flags"] = flags
        ge = rng.randrange(16)
        case["ge"] = ge
        cpsr = 0x13 | (flags["n"] << 31) | (flags["z"] << 30) | \
               (flags["c"] << 29) | (flags["v"] << 28) | (ge << 16)
        if thumb:
            cpsr |= 0x20  # T-Bit: Unicorn waehlt Thumb ueber CPSR.T/Startadresse
        uc.reg_write(U.UC_ARM_REG_CPSR, cpsr)
        try:
            # Unicorn erkennt Thumb am Bit 0 der Startadresse (Uc-Modus allein
            # reicht in 2.1.4 nicht aus).
            begin = case["addr"] | (1 if thumb else 0)
            uc.emu_start(begin, 0, count=case["ninsn"])
        except Exception as e:
            errors.append((idx, case["asm"].strip(), str(e)))
            refs.append(None)
            continue
        pc = uc.reg_read(U.UC_ARM_REG_PC)
        if pc != 0:
            errors.append((idx, case["asm"].strip(), "PC=0x%08X" % pc))
            refs.append(None)
            continue
        refs.append({
            "regs": [uc.reg_read(u) for u in UREGS],
            "cpsr": uc.reg_read(U.UC_ARM_REG_CPSR),
            "data": bytes(uc.mem_read(DATA_BASE, DATA_SIZE)),
        })
    return refs, errors


# --------------------------------------------------------------------------
# C-Harness
# --------------------------------------------------------------------------
def mem_digest(buf):
    h = 2166136261
    for b in buf:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def gen_harness(cases, refs, path, img):
    L = []
    L.append("/* Harness: differentieller Test gegen Unicorn */")
    L.append("#include <stdio.h>")
    L.append("#include <string.h>")
    L.append("#define DATA_BASE 0x%Xu" % DATA_BASE)
    L.append("#define DATA_SIZE %du" % DATA_SIZE)
    L.append("static uint8_t g_mem[%du];" % MEM_SIZE)
    L.append("static int g_fail;")
    L.append("static const unsigned char g_code[%d] = {" % len(img))
    for i in range(0, len(img), 24):
        L.append("  " + ",".join(str(b) for b in img[i:i + 24]) + ",")
    L.append("};")
    L.append("static void fill_data(uint32_t seed) {")
    L.append("  uint32_t x = (seed * 2654435761u) + 1u;")
    L.append("  for (uint32_t i = 0; i < DATA_SIZE; i++) {")
    L.append("    x = x * 1103515245u + 12345u;")
    L.append("    g_mem[DATA_BASE + i] = (uint8_t)(x >> 16);")
    L.append("  }")
    L.append("}")
    L.append("static uint32_t mem_digest(void) {")
    L.append("  uint32_t h = 2166136261u;")
    L.append("  for (uint32_t i = 0; i < DATA_SIZE; i++)")
    L.append("    h = (h ^ g_mem[DATA_BASE + i]) * 16777619u;")
    L.append("  return h;")
    L.append("}")
    L.append("static void chk(int id, const char *what, uint32_t got, uint32_t want) {")
    L.append("  if (got != want) {")
    L.append('    if (g_fail < 60) printf("FAIL %d %s: 0x%08X (referenz 0x%08X)\\n",')
    L.append("                       id, what, got, want);")
    L.append("    g_fail++;")
    L.append("  }")
    L.append("}")
    for idx, case in enumerate(cases):
        L.append("/* case %d: %s */" % (idx, case["asm"].replace("\n", "; ")))
        L.append("static void run_%d(void) {" % idx)
        L.append("  ArmCpu c;")
        L.append("  memset(g_mem, 0, sizeof g_mem);")
        L.append("  memcpy(g_mem + 0x%Xu, g_code, sizeof g_code);" % ARM_BASE)
        L.append("  fill_data(0x%Xu);" % case["data_seed"])
        L.append("  arm_cpu_init(&c, g_mem, sizeof g_mem);")
        for r, v in enumerate(case["init_regs"]):
            L.append("  c.r[%d] = 0x%08Xu;" % (r, v))
        f = case["flags"]
        L.append("  c.n=%du; c.z=%du; c.c=%du; c.v=%du; c.ge=0x%Xu;"
                 % (f["n"], f["z"], f["c"], f["v"], case["ge"]))
        L.append("  fn_%08X(&c);" % case["addr"])
        L.append('  if (c.aborted) { printf("FAIL %d aborted\\n"); g_fail++; return; }' % idx)
        want = "{%s}" % ", ".join("0x%08Xu" % v for v in refs[idx]["regs"][:15])
        L.append("  { static const uint32_t w[15] = %s;" % want)
        L.append("    for (int i = 0; i < 15; i++) if (c.r[i] != w[i]) {")
        L.append('      if (g_fail < 60) printf("FAIL ' + str(idx) +
                 ' r%d: 0x%08X (referenz 0x%08X)\\n", i, c.r[i], w[i]);')
        L.append("      g_fail++; } }")
        cpsr = refs[idx]["cpsr"]
        for name, bit, field in (("n", 31, "n"), ("z", 30, "z"), ("c", 29, "c"),
                                 ("v", 28, "v"), ("q", 27, "qflag")):
            L.append('  chk(%d, "%s", c.%s, %du);' % (idx, name, field, (cpsr >> bit) & 1))
        L.append('  chk(%d, "ge", c.ge, 0x%Xu);' % (idx, (cpsr >> 16) & 0xF))
        L.append('  chk(%d, "mem", mem_digest(), 0x%08Xu);'
                 % (idx, mem_digest(refs[idx]["data"])))
        L.append("}")
    L.append("int main(void) {")
    for idx in range(len(cases)):
        L.append("  run_%d();" % idx)
    L.append('  printf("%d Faelle, %%d Fehler\\n", g_fail);' % len(cases))
    L.append("  return g_fail ? 1 : 0;")
    L.append("}")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(L))


def compile_and_run(gen_c, harness_c, out_c, exe):
    with open(out_c, "w", encoding="utf-8") as f:
        f.write(open(gen_c, encoding="utf-8").read())
        f.write("\n")
        f.write(open(harness_c, encoding="utf-8").read())
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), "zig.exe")
    runtime = os.path.join(ROOT, "runtime")
    cmd = [zig, "cc", "-std=c11", "-O1", "-I", os.path.join(runtime, "cpu"),
           out_c, os.path.join(runtime, "cpu", "arm_cpu.c"),
           os.path.join(runtime, "cpu", "arm_trace.c"),
           os.path.join(runtime, "workarounds.c"), "-o", exe]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout)
        print(p.stderr)
        raise SystemExit("Kompilieren fehlgeschlagen")
    return subprocess.run([exe], capture_output=True, text=True)


def run_mode(mode, seed, count, filt):
    thumb = mode == "thumb"
    rng = random.Random((seed << 1) ^ (1 if thumb else 0))
    cases = build_cases(rng, count, mode)
    if filt:
        f = filt.lower()
        cases = [c for c in cases if f in c["asm"].lower() or f in c["note"].lower()]
    cases = assemble_cases(cases, thumb)
    for i, c in enumerate(cases):
        c["data_seed"] = (seed * 131 + i * 7 + (0x5A if thumb else 0)) & 0xFFFFFFFF
    if not cases:
        print("[%s] keine Faelle assembliert" % mode)
        return 0, 0
    refs, errors = reference_run(cases, thumb, random.Random(seed * 977 + 13))
    for idx, asm, msg in errors[:8]:
        print("[%s] Fall %d uebersprungen (%s): %s" % (mode, idx, msg, asm.replace("\n", "; ")))
    good = [(c, r) for c, r in zip(cases, refs) if r is not None]
    if not good:
        print("[%s] kein Fall ausfuehrbar" % mode)
        return 0, 0
    os.makedirs(OUT, exist_ok=True)
    img = bytearray(len(cases) * STRIDE)
    for i, c in enumerate(cases):
        img[i * STRIDE:i * STRIDE + len(c["blob"])] = c["blob"]
    res = cli.scanmod.scan(bytes(img), [(c["addr"], "case", thumb) for c in cases])
    gen_c = os.path.join(OUT, "gen_%s.c" % mode)
    cli.generate(res, gen_c)
    good_cases = [c for c, _ in good]
    good_refs = [r for _, r in good]
    harness = os.path.join(OUT, "harness_%s.c" % mode)
    gen_harness(good_cases, good_refs, harness, bytes(img))
    p = compile_and_run(gen_c, harness,
                        os.path.join(OUT, "test_%s.c" % mode),
                        os.path.join(OUT, "test_%s.exe" % mode))
    print("[%s] %s" % (mode, (p.stdout.strip() or p.stderr.strip()).replace("\n", "\n       ")))
    # Fehlende Faelle mit Assemblertext auflisten (Diagnose)
    bad = []
    for line in p.stdout.splitlines():
        if line.startswith("FAIL "):
            try:
                cid = int(line.split()[1])
            except (IndexError, ValueError):
                continue
            if cid not in bad:
                bad.append(cid)
    if bad:
        print("[%s] betroffene Faelle:" % mode)
        for cid in bad[:40]:
            c = good_cases[cid]
            print("  #%d %-28s %s" % (cid, c["note"],
                                      c["asm"].replace("\n", "; ")))
    return len(good_cases), (0 if p.returncode == 0 else 1)


# --------------------------------------------------------------------------
# Kontrollfluss-Suite (eigene Funktionen, Interworking, Tabellen)
# --------------------------------------------------------------------------
CF_BASE = ARM_BASE
CF_STACK = DATA_BASE + 0x800
CF_CODE = 0x20000

CF_CASES = []


def _cf(name, funcs, entry=0, thumb=False, regs=None, mem=None, stack=None,
        raw=None):
    CF_CASES.append({"name": name, "funcs": funcs, "entry": entry,
                     "thumb": thumb, "regs": regs or {}, "mem": mem or [],
                     "stack": stack or [], "raw": raw or []})


def _cf_cases():
    if CF_CASES:
        return
    # --- ARM: bedingte Rueckkehr + Fallthrough -----------------------------
    _cf("arm_bxeq_fallthrough", [("".join([
        "push {r4, lr}\n",
        "mov r4, r0\n",
        "cmp r1, #0\n",
        "bxeq lr\n",
        "add r0, r4, r1\n",
        "pop {r4, pc}\n"]))],
        regs={"r0": 7, "r1": 5})
    _cf("arm_bxeq_taken", [("".join([
        "push {r4, lr}\n",
        "mov r4, r0\n",
        "cmp r1, #0\n",
        "bxeq lr\n",
        "add r0, r4, r1\n",
        "pop {r4, pc}\n"]))],
        regs={"r0": 7, "r1": 0})
    _cf("arm_popeq_fallthrough", [("".join([
        "push {r4, lr}\n",
        "mov r4, r0\n",
        "cmp r1, #3\n",
        "popeq {pc}\n",
        "add r0, r4, r1\n",
        "pop {r4, pc}\n"]))],
        regs={"r0": 100, "r1": 4})
    _cf("arm_ldreq_pc", [("".join([
        "push {r4, lr}\n",
        "mov r4, r0\n",
        "cmp r1, #9\n",
        "ldreq pc, [sp], #4\n",
        "add r0, r4, r1\n",
        "pop {r4, pc}\n"]))],
        regs={"r0": 100, "r1": 9})
    _cf("arm_movne_pc", [("".join([
        "push {r4, lr}\n",
        "mov r4, r0\n",
        "cmp r1, #1\n",
        "movne pc, lr\n",
        "add r0, r4, r1\n",
        "pop {r4, pc}\n"]))],
        regs={"r0": 100, "r1": 1})
    _cf("arm_movpc_reg", [("".join([
        "push {r4, lr}\n",
        "mov r4, r0\n",
        "adr r3, target\n",
        "cmp r1, #0\n",
        "movne pc, r3\n",
        "add r0, r4, r1\n",
        "pop {r4, pc}\n",
        "target:\n",
        "add r0, r4, #1000\n",
        "pop {r4, pc}\n"]))],
        regs={"r0": 100, "r1": 7})

    # --- ARM: Tail-Call `b` in eine andere Funktion -----------------------
    _cf("arm_tail_b", [
        ("push {r4, lr}\nmov r4, r0\nmov r0, r1\nb 0x100200\n"),
        (0x200, "add r0, r0, #3\nadd r0, r0, r4\npop {r4, pc}\n"),
    ], regs={"r0": 10, "r1": 5})

    # --- ARM: Sprungtabelle ueber `add pc, pc, rN, lsl #2` ----------------
    _cf("arm_addpc_table", [("".join([
        "push {r4, lr}\n",
        "mov r4, r0\n",
        "cmp r1, #2\n",
        "addls pc, pc, r1, lsl #2\n",
        "b 0x100038\n",
        "b 0x100020\n",   # index 0
        "b 0x100028\n",   # index 1
        "b 0x100030\n",   # index 2
        "add r0, r4, #10\n",
        "b 0x100038\n",
        "add r0, r4, #20\n",
        "b 0x100038\n",
        "add r0, r4, #30\n",
        "b 0x100038\n",
        "add r0, r4, #99\n",
        "pop {r4, pc}\n"]))],
        regs={"r0": 1, "r1": 2})

    # --- ARM: Sprungtabelle ueber `ldrls pc, [pc, rN, lsl #2]` -----------
    # ldrls bei 0x0C: PC = 0x14 -> Tabelle 0x14..0x1F, Fallthrough 0x10.
    _cf("arm_ldrls_pc_table", [("".join([
        "push {r4, lr}\n",
        "mov r4, r0\n",
        "cmp r1, #2\n",
        "ldrls pc, [pc, r1, lsl #2]\n",
        "b 0x100040\n"])),
        (0x28, "add r0, r4, #10\nb 0x100040\n"),
        (0x30, "add r0, r4, #20\nb 0x100040\n"),
        (0x38, "add r0, r4, #30\nb 0x100040\n"),
        (0x40, "add r0, r4, #99\npop {r4, pc}\n"),
    ], raw=[(0x14, (0x100028).to_bytes(4, "little")),
            (0x18, (0x100030).to_bytes(4, "little")),
            (0x1c, (0x100038).to_bytes(4, "little"))],
        regs={"r0": 1, "r1": 1})

    # --- ARM: Rueckkehr ueber ldm sp!, {.., pc} (Tail-epilog) -------------
    _cf("arm_ldm_pc", [("".join([
        "push {r4, r5, lr}\n",
        "mov r4, r0\n",
        "add r0, r4, r1\n",
        "ldm sp!, {r4, r5, pc}\n"]))],
        regs={"r0": 3, "r1": 4})

    # --- Thumb: IT-Bloecke, alle Masken -----------------------------------
    it_forms = [
        ("it", "t"), ("itt", "tt"), ("ite", "te"),
        ("ittt", "ttt"), ("itte", "tte"), ("itet", "tet"), ("itee", "tee"),
        ("itttt", "tttt"), ("ittte", "ttte"), ("ittet", "ttet"),
        ("ittee", "ttee"), ("itett", "tett"), ("itete", "tete"),
        ("iteet", "teet"), ("iteee", "teee"),
    ]
    for mn, pat in it_forms:
        lines = [mn + " eq"]
        for i, p in enumerate(pat):
            cc = "eq" if p == "t" else "ne"
            lines.append("mov%s r%d, #%d" % (cc, 5 + i, i + 1))
        lines.append("add r0, r4, r5")   # ausserhalb des IT-Blocks
        lines.append("pop {r4, pc}")
        _cf("thumb_" + mn, [("\n".join(lines))], thumb=True,
            regs={"r0": 40})

    # --- Thumb: cbz/cbnz-Schleifen ---------------------------------------
    _cf("thumb_cbz_loop", [("".join([
        "push {r4, lr}\n",
        "movs r0, #0\n",
        "movs r1, #5\n",
        "adds r0, r0, r1\n",
        "subs r1, #1\n",
        "cbz r1, 0x100012\n",
        "b 0x100006\n",
        "cbnz r1, 0x100012\n",
        "movs r3, #1\n",
        "pop {r4, pc}\n"]))], thumb=True, regs={})

    # --- Thumb: blx immediate -> ARM-Funktion, Rueckkehr interworking -----
    _cf("thumb_blx_imm_arm", [
        ("push {r4, lr}\nmov r4, r0\nmovs r0, #7\nblx 0x100200\nadd r0, r0, r4\npop {r4, pc}\n", True),
        (0x200, "push {r4, lr}\nadd r0, r0, #5\nldm sp!, {r4, pc}\n", False),
    ], thumb=True, regs={"r0": 100})

    # --- ARM: blx reg -> Thumb-Funktion, Rueckkehr ueber bx lr ------------
    _cf("arm_blx_reg_thumb", [
        ("push {r4, lr}\nmov r4, r0\nmovs r0, #3\n"
         "movw r3, #0x0201\nmovt r3, #0x0010\nblx r3\n"
         "add r0, r0, r4\npop {r4, pc}\n"),
        (0x200, "adds r0, #4\nbx lr\n", True),
    ], regs={"r0": 10})

    # --- Thumb: tbb/tbh-Sprungtabellen -----------------------------------
    # tbb bei 0x04 -> Basis Align(0x08,4)=0x08. Tabelle: 2 Byte.
    _cf("thumb_tbb", [
        ("push {r4, lr}\nmovs r4, #0\ntbb [pc, r1]\n", True),
        (0x10, "adds r0, #1\nb 0x100020\n", True),
        (0x18, "adds r0, #21\nb 0x100020\n", True),
        (0x20, "pop {r4, pc}\n", True),
    ], thumb=True, regs={"r0": 5, "r1": 1},
        raw=[(0x08, bytes([(0x10 - 0x08) // 2, (0x18 - 0x08) // 2]))])

    # tbh bei 0x04 -> Basis 0x08, Halbwort-Tabelle.
    _cf("thumb_tbh", [
        ("push {r4, lr}\nmovs r4, #0\ntbh [pc, r1, lsl #1]\n", True),
        (0x10, "adds r0, #1\nb 0x100020\n", True),
        (0x18, "adds r0, #21\nb 0x100020\n", True),
        (0x20, "pop {r4, pc}\n", True),
    ], thumb=True, regs={"r0": 5, "r1": 1},
        raw=[(0x08, (4).to_bytes(2, "little")),
             (0x0a, (8).to_bytes(2, "little"))])

    # --- Thumb: Fallthrough in die naechste Funktion (mehrere Einstiege) --
    _cf("thumb_fallthrough_multi", [
        ("movs r0, #62\n", True),
        (0x2, "adds r0, #1\npop {pc}\n", True),
    ], thumb=True, regs={})


def cf_assemble(case):
    img = bytearray(CF_CODE)
    entries = []
    for spec in case["funcs"]:
        if isinstance(spec, str):
            off, text, th = 0, spec, case["thumb"]
        elif isinstance(spec[0], str):
            text, th = spec[0], spec[1]
            off = 0
        elif len(spec) == 2:
            off, text = spec
            th = case["thumb"]
        else:
            off, text, th = spec
        ks = Ks(KS_ARCH_ARM, (KS_MODE_THUMB if th else KS_MODE_ARM)
                | KS_MODE_LITTLE_ENDIAN)
        enc, _ = ks.asm(text, CF_BASE + off)
        if enc is None:
            raise SystemExit("cf: Assembler-Fehler %s" % case["name"])
        img[off:off + len(enc)] = bytes(enc)
        entries.append((CF_BASE + off, th))
    for off, blob in case["raw"]:
        img[off:off + len(blob)] = blob
    return bytes(img), entries


def cf_reference(case, img, rng):
    uc = Uc(UC_ARCH_ARM,
            (UC_MODE_THUMB if case["thumb"] else UC_MODE_ARM)
            | UC_MODE_LITTLE_ENDIAN)
    uc.mem_map(CF_BASE, CF_CODE)
    uc.mem_map(DATA_BASE, DATA_SIZE)
    uc.mem_write(CF_BASE, img)
    for addr, blob in case["mem"]:
        uc.mem_write(addr, blob)
    for addr, val in case["stack"]:
        uc.mem_write(addr, int(val).to_bytes(4, "little"))
    regs = [0] * 15
    regs[13] = CF_STACK
    for k, v in case["regs"].items():
        regs[int(k[1:])] = v
    f = {"n": rng.randrange(2), "z": rng.randrange(2),
         "c": rng.randrange(2), "v": rng.randrange(2)}
    ge = rng.randrange(16)
    for i, v in enumerate(regs):
        uc.reg_write(UREGS[i], v)
    cpsr = 0x13 | (f["n"] << 31) | (f["z"] << 30) | (f["c"] << 29) | \
           (f["v"] << 28) | (ge << 16)
    if case["thumb"]:
        cpsr |= 0x20
    uc.reg_write(U.UC_ARM_REG_CPSR, cpsr)
    start = CF_BASE + case["entry"] + (1 if case["thumb"] else 0)
    try:
        uc.emu_start(start, 0, count=200000)
    except Exception as e:
        return None, "Unicorn: %s" % e
    pc = uc.reg_read(U.UC_ARM_REG_PC)
    if pc != 0:
        return None, "nicht terminiert (PC=0x%08X)" % pc
    ref = {
        "regs": [uc.reg_read(u) for u in UREGS],
        "cpsr": uc.reg_read(U.UC_ARM_REG_CPSR),
        "data": bytes(uc.mem_read(DATA_BASE, DATA_SIZE)),
        "init": {"regs": regs, "flags": f, "ge": ge},
    }
    return ref, None


def cf_harness(case, img, ref, path):
    L = ["#include \"arm_cpu.h\"", "#include <stdio.h>", "#include <string.h>",
         "#include <stdint.h>",
         "#define DATA_BASE 0x%Xu" % DATA_BASE,
         "#define DATA_SIZE %du" % DATA_SIZE,
         "#define CF_STACK 0x%Xu" % CF_STACK,
         "static uint8_t g_mem[%du];" % MEM_SIZE,
         "static int g_fail;",
         "static const unsigned char g_code[%d] = {" % len(img)]
    for i in range(0, len(img), 24):
        L.append("  " + ",".join(str(b) for b in img[i:i + 24]) + ",")
    L.append("};")
    L.append("static uint32_t mem_digest(void) {")
    L.append("  uint32_t h = 2166136261u;")
    L.append("  for (uint32_t i = 0; i < DATA_SIZE; i++)")
    L.append("    h = (h ^ g_mem[DATA_BASE + i]) * 16777619u;")
    L.append("  return h;")
    L.append("}")
    L.append("static void put32(uint32_t addr, uint32_t v) {")
    L.append("  g_mem[addr] = (uint8_t)v; g_mem[addr+1] = (uint8_t)(v>>8);")
    L.append("  g_mem[addr+2] = (uint8_t)(v>>16); g_mem[addr+3] = (uint8_t)(v>>24);")
    L.append("}")
    L.append("int main(void) {")
    L.append("  ArmCpu c;")
    L.append("  memset(g_mem, 0, sizeof g_mem);")
    L.append("  memcpy(g_mem + 0x%Xu, g_code, sizeof g_code);" % CF_BASE)
    for addr, blob in case["mem"]:
        for i, b in enumerate(blob):
            L.append("  g_mem[0x%Xu] = %du;" % (addr + i, b))
    for addr, val in case["stack"]:
        L.append("  put32(0x%Xu, 0x%08Xu);" % (addr, val))
    L.append("  arm_cpu_init(&c, g_mem, sizeof g_mem);")
    for i, v in enumerate(ref["init"]["regs"]):
        L.append("  c.r[%d] = 0x%08Xu;" % (i, v))
    f = ref["init"]["flags"]
    L.append("  c.n=%du; c.z=%du; c.c=%du; c.v=%du; c.ge=0x%Xu;"
             % (f["n"], f["z"], f["c"], f["v"], ref["init"]["ge"]))
    L.append("  fn_%08X(&c);" % (CF_BASE + case["entry"]))
    L.append('  if (c.aborted) { printf("FAIL aborted\\n"); return 1; }')
    want = "{%s}" % ", ".join("0x%08Xu" % v for v in ref["regs"][:15])
    L.append("  { static const uint32_t w[15] = %s;" % want)
    L.append("    for (int i = 0; i < 15; i++) if (c.r[i] != w[i]) {")
    L.append('      printf("FAIL r%d: 0x%08X (referenz 0x%08X)\\n", i, c.r[i], w[i]);')
    L.append("      g_fail++; } }")
    cpsr = ref["cpsr"]
    for name, field in (("n", "n"), ("z", "z"), ("c", "c"), ("v", "v"),
                        ("q", "qflag")):
        bit = {"n": 31, "z": 30, "c": 29, "v": 28, "q": 27}[name]
        L.append('  if (c.%s != %du) { printf("FAIL %s: %%u (referenz %du)\\n", c.%s); g_fail++; }'
                 % (field, (cpsr >> bit) & 1, name, (cpsr >> bit) & 1, field))
    L.append('  if (c.ge != 0x%Xu) { printf("FAIL ge: 0x%%X\\n", c.ge); g_fail++; }'
             % ((cpsr >> 16) & 0xF))
    L.append('  if (mem_digest() != 0x%08Xu) { printf("FAIL mem\\n"); g_fail++; }'
             % mem_digest(ref["data"]))
    L.append("  return g_fail ? 1 : 0;")
    L.append("}")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(L))


def run_control_flow(seed, filt):
    _cf_cases()
    rng = random.Random(seed * 7919 + 3)
    fails = 0
    total = 0
    os.makedirs(OUT, exist_ok=True)
    for idx, case in enumerate(CF_CASES):
        if filt and filt.lower() not in case["name"].lower():
            continue
        total += 1
        img, entries = cf_assemble(case)
        ref, err = cf_reference(case, img, rng)
        if ref is None:
            print("[cf] %-28s uebersprungen: %s" % (case["name"], err))
            continue
        res = cli.scanmod.scan(img, [(a, "cf", th) for a, th in entries])
        gen_c = os.path.join(OUT, "cf_gen_%d.c" % idx)
        cli.generate(res, gen_c, data=img)
        harness = os.path.join(OUT, "cf_main_%d.c" % idx)
        cf_harness(case, img, ref, harness)
        p = compile_and_run(gen_c, harness,
                            os.path.join(OUT, "cf_test_%d.c" % idx),
                            os.path.join(OUT, "cf_test_%d.exe" % idx))
        out = (p.stdout.strip() or p.stderr.strip()).replace("\n", "\n       ")
        if p.returncode == 0:
            print("[cf] %-28s OK" % case["name"])
        else:
            print("[cf] %-28s FEHLER: %s" % (case["name"], out))
            fails += 1
    print("\n[cf] %d Faelle, %d Fehler" % (total, fails))
    return total, fails


def main(argv=None):
    ap = argparse.ArgumentParser(description="Differentieller Recompiler-Test")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=80, help="zusaetzliche Zufallsfolgen")
    ap.add_argument("--mode", choices=("arm", "thumb", "both", "cf"), default="both")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--filter", default=None)
    args = ap.parse_args(argv)
    if args.quick:
        args.count = 5
    if args.mode == "cf":
        run_control_flow(args.seed, args.filter)
        return 0
    total = fails = 0
    modes = ("arm", "thumb") if args.mode == "both" else (args.mode,)
    for mode in modes:
        n, fail = run_mode(mode, args.seed, args.count, args.filter)
        total += n
        fails += fail
    print("\n%d Faelle geprueft, %d Modus-Fehler" % (total, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
