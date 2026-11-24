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


def main(argv=None):
    ap = argparse.ArgumentParser(description="Differentieller Recompiler-Test")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=80, help="zusaetzliche Zufallsfolgen")
    ap.add_argument("--mode", choices=("arm", "thumb", "both"), default="both")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--filter", default=None)
    args = ap.parse_args(argv)
    if args.quick:
        args.count = 5
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
