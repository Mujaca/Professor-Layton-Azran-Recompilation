"""Differentieller VFP-Test: Unicorn (Referenz) vs. recompilierter C-Code.

Deckt die Float-Semantik des ARM11/VFP ab, die der Recompiler erzeugt:
Arithmetik (f32/f64), VMLA/VMLS/VNMUL/VNMLA/VNMLS, VCVT (inkl. Rundung,
Saettigung, Festkomma), VCMP/VCMPE + VMRS APSR_nzcv, VMOV zwischen Kern- und
VFP-Registern sowie VLDR/VSTR/VLDM/VSTM/VPUSH/VPOP. Neben dem Rechenergebnis
werden FPSCR.NZCV, Kernregister, SP und der Speicher-Digest verglichen.

  python tools/vfp_difftest.py --count 300
  python tools/vfp_difftest.py --filter vcvt --keep
  python tools/vfp_difftest.py --seed 7 --count 1000

Exit-Code 0 = alle Faelle gleich, 1 = mindestens ein Unterschied. Reines
Testwerkzeug (Unicorn ist Referenz, kein Produktbestandteil).
"""
import argparse
import os
import random
import struct
import subprocess
import sys

import capstone
from keystone import Ks, KS_ARCH_ARM, KS_MODE_ARM, KS_MODE_LITTLE_ENDIAN
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_MODE_LITTLE_ENDIAN
import unicorn.arm_const as U

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from recomp import isa, cli  # noqa: E402

ARM_BASE = isa.ARM_BASE
DATA_BASE = 0x20000
DATA_SIZE = 0x1000
MEM_SIZE = 0x200000
STACK = DATA_BASE + 0x800
SCRATCH = DATA_BASE + 0x100
STRIDE = 0x40
OUT = os.path.join(ROOT, "out", "vfp_difftest")

F32_POOL = [
    0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x40000000, 0xC0000000,
    0x3F000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7FC00001, 0xFFC00001,
    0x7F800001, 0x00000001, 0x80000001, 0x007FFFFF, 0x00800000, 0x4B000000,
    0x4F000000, 0x3F801000, 0x3A000000, 0x7F7FFFFF,
]
F64_POOL = [
    0x0000000000000000, 0x8000000000000000, 0x3FF0000000000000,
    0xBFF0000000000000, 0x4000000000000000, 0x7FF0000000000000,
    0xFFF0000000000000, 0x7FF8000000000000, 0x7FF8000000000001,
    0x0000000000000001, 0x8000000000000001, 0x000FFFFFFFFFFFFF,
    0x0010000000000000, 0x4330000000000000, 0x41E0000000000000,
]
F32_POOL_NO_NAN = [v for v in F32_POOL
                   if not ((v & 0x7F800000) == 0x7F800000 and (v & 0x007FFFFF))]
F64_POOL_NO_NAN = [v for v in F64_POOL
                   if not ((v & 0x7FF0000000000000) == 0x7FF0000000000000
                           and (v & 0x000FFFFFFFFFFFFF))]
FPSCR_POOL = [    0x03C00010,  # 3DS-Default: Default-NaN + FTZ + Rundung Richtung Null + IXC
    0x00000000,  # IEEE-Default: nearest, kein FTZ/DN
    0x01000010,  # FTZ
    0x02000010,  # Default-NaN
    0x00C00010,  # Rundung Richtung Null
    0x00400010,  # Rundung +inf
    0x00800010,  # Rundung -inf
    0x03000010,  # FTZ + Default-NaN
]


def _value(rng, pool):
    if rng.randrange(3) == 0:
        return rng.getrandbits(32)
    return rng.choice(pool)


def _f64_value(rng):
    if rng.randrange(3) == 0:
        return rng.getrandbits(64)
    return rng.choice(F64_POOL)


def _value32_nonan(rng):
    while True:
        v = _value(rng, F32_POOL_NO_NAN)
        if not ((v & 0x7F800000) == 0x7F800000 and (v & 0x007FFFFF)):
            return v


def _value64_nonan(rng):
    while True:
        v = _f64_value(rng)
        if not ((v & 0x7FF0000000000000) == 0x7FF0000000000000
                and (v & 0x000FFFFFFFFFFFFF)):
            return v


def _case(asm, rng, fpscr=None, sregs=None, regs=None, check_flags=False):
    return {
        "asm": asm.strip(),
        "sregs": sregs or {},
        "regs": regs or {},
        "fpscr": rng.choice(FPSCR_POOL) if fpscr is None else fpscr,
        "check_flags": check_flags,
    }


def build_cases(rng, count):
    cases = []
    # Arithmetik f32/f64
    for op in ("vadd", "vsub", "vmul", "vdiv"):
        for _ in range(count // 24 + 2):
            d, n, m = rng.sample(range(16), 3)
            cases.append(_case("%s.f32 s%d, s%d, s%d" % (op, d, n, m), rng,
                               sregs={n: _value(rng, F32_POOL),
                                      m: _value(rng, F32_POOL),
                                      d: _value(rng, F32_POOL)}))
        # f64: alle Register muessen gerade/ungerade Wortpaare treffen
        for _ in range(count // 32 + 2):
            d, n, m = rng.sample(range(8), 3)
            cases.append(_case("%s.f64 d%d, d%d, d%d" % (op, d, n, m), rng,
                               sregs={d * 2: _value(rng, F32_POOL),
                                      d * 2 + 1: _value(rng, F32_POOL),
                                      n * 2: _value(rng, F32_POOL),
                                      n * 2 + 1: _value(rng, F32_POOL),
                                      m * 2: _value(rng, F32_POOL),
                                      m * 2 + 1: _value(rng, F32_POOL)}))
    for op in ("vabs", "vneg", "vsqrt"):
        for _ in range(count // 32 + 2):
            d, m = rng.sample(range(16), 2)
            cases.append(_case("%s.f32 s%d, s%d" % (op, d, m), rng,
                               sregs={m: _value(rng, F32_POOL)}))
            d64, m64 = rng.sample(range(8), 2)
            cases.append(_case("%s.f64 d%d, d%d" % (op, d64, m64), rng,
                               sregs={m64 * 2: _value(rng, F32_POOL),
                                      m64 * 2 + 1: _value(rng, F32_POOL)}))
    # Multiply-accumulate (NaN-Weitergabe der VMLA-Familie ist
    # implementierungsabhaengig; hier zaehlen Rundung/FTZ/DN der Rechnung).
    for op in ("vmla", "vmls", "vnmul", "vnmla", "vnmls"):
        for _ in range(count // 32 + 2):
            d, n, m = rng.sample(range(12), 3)
            cases.append(_case("%s.f32 s%d, s%d, s%d" % (op, d, n, m), rng,
                               sregs={d: _value32_nonan(rng),
                                      n: _value32_nonan(rng),
                                      m: _value32_nonan(rng)}))
            d64, n64, m64 = rng.sample(range(6), 3)
            vd, vn, vm = _value64_nonan(rng), _value64_nonan(rng), _value64_nonan(rng)
            cases.append(_case("%s.f64 d%d, d%d, d%d" % (op, d64, n64, m64), rng,
                               sregs={d64 * 2: vd & 0xFFFFFFFF,
                                      d64 * 2 + 1: vd >> 32,
                                      n64 * 2: vn & 0xFFFFFFFF,
                                      n64 * 2 + 1: vn >> 32,
                                      m64 * 2: vm & 0xFFFFFFFF,
                                      m64 * 2 + 1: vm >> 32}))
    # Vergleiche + VMRS
    for op in ("vcmp", "vcmpe"):
        for _ in range(count // 16 + 2):
            a, b = rng.sample(range(16), 2)
            asm = "%s.f32 s%d, s%d\nvmrs APSR_nzcv, fpscr" % (op, a, b)
            cases.append(_case(asm, rng, check_flags=True,
                               sregs={a: _value(rng, F32_POOL),
                                      b: _value(rng, F32_POOL)}))
            a64, b64 = rng.sample(range(8), 2)
            asm = "%s.f64 d%d, d%d\nvmrs APSR_nzcv, fpscr" % (op, a64, b64)
            cases.append(_case(asm, rng, check_flags=True,
                               sregs={a64 * 2: _value(rng, F32_POOL),
                                      a64 * 2 + 1: _value(rng, F32_POOL),
                                      b64 * 2: _value(rng, F32_POOL),
                                      b64 * 2 + 1: _value(rng, F32_POOL)}))
    # Konvertierungen (inkl. Festkomma)
    for to, frm in (("s32", "f32"), ("u32", "f32"), ("f32", "s32"), ("f32", "u32"),
                    ("s32", "f64"), ("u32", "f64"), ("f64", "s32"), ("f64", "u32"),
                    ("f32", "f64"), ("f64", "f32")):
        for _ in range(count // 40 + 2):
            if "64" in to or "64" in frm:
                src_dbl = "64" in frm
                dst_dbl = "64" in to
                s = rng.randrange(8 if src_dbl else 16)
                d = rng.randrange(8 if dst_dbl else 16)
                sd = "d%d" % s if src_dbl else "s%d" % s
                dd = "d%d" % d if dst_dbl else "s%d" % d
                if src_dbl:
                    sregs = {s * 2: _value(rng, F32_POOL),
                             s * 2 + 1: _value(rng, F32_POOL)}
                else:
                    sregs = {s: _value(rng, F32_POOL)}
            else:
                s, d = rng.sample(range(16), 2)
                sd, dd = "s%d" % s, "s%d" % d
                sregs = {s: _value(rng, F32_POOL)}
            asm = "vcvt.%s.%s %s, %s" % (to, frm, dd, sd)
            cases.append(_case(asm, rng, sregs=sregs))
            # Festkomma nur fuer Ganzzahl<->Gleitkomma
            if (to in ("s32", "u32")) != (frm in ("s32", "u32")):
                bits = rng.choice([1, 4, 8, 12, 16, 24, 31])
                asm += ", #%d" % bits
                cases.append(_case(asm, rng, sregs=sregs))
    # VMOV Kern <-> VFP
    for _ in range(count // 40 + 2):
        a, b, c = rng.sample(range(13), 3)
        cases.append(_case("vmov d%d, r%d, r%d" % (a % 8, b, c), rng,
                           regs={b: rng.getrandbits(32), c: rng.getrandbits(32)}))
        cases.append(_case("vmov r%d, r%d, d%d" % (b, c, a % 8), rng,
                           sregs={a * 2: rng.getrandbits(32),
                                  a * 2 + 1: rng.getrandbits(32)}))
        cases.append(_case("vmov s%d, r%d" % (b, a), rng,
                           regs={a: rng.getrandbits(32)}))
        cases.append(_case("vmov r%d, s%d" % (b, a), rng,
                           sregs={a: rng.getrandbits(32)}))
    # Speicherzugriffe: VLDR/VSTR (f32/f64), VSTM/VLDM, VPUSH/VPOP
    for _ in range(count // 40 + 2):
        off = rng.randrange(0, 0x200, 4)
        s = rng.randrange(16)
        cases.append(_case("vstr s%d, [r4, #%d]\nvldr s%d, [r4, #%d]" % (s, off, s, off),
                           rng, sregs={s: _value(rng, F32_POOL)}))
        off = rng.randrange(0, 0x200, 8)
        d = rng.randrange(8)
        cases.append(_case("vstr d%d, [r4, #%d]\nvldr d%d, [r4, #%d]" % (d, off, d, off),
                           rng, sregs={d * 2: rng.getrandbits(32),
                                       d * 2 + 1: rng.getrandbits(32)}))
        lo = rng.randrange(0, 12)
        n = rng.randrange(1, 4)
        cases.append(_case("vstmia r4!, {s%d-s%d}\nvldmia r4!, {s%d-s%d}"
                           % (lo, lo + n - 1, lo, lo + n - 1), rng,
                           sregs={i: rng.getrandbits(32) for i in range(lo, lo + n)}))
        lo = rng.randrange(0, 12)
        n = rng.randrange(1, 4)
        cases.append(_case("vpush {s%d-s%d}\nvpop {s%d-s%d}"
                           % (lo, lo + n - 1, lo, lo + n - 1), rng,
                           sregs={i: rng.getrandbits(32) for i in range(lo, lo + n)}))
    for _ in range(count // 40 + 2):
        d = rng.randrange(8)
        cases.append(_case("vpush {d%d}\nvpop {d%d}" % (d, d), rng,
                           sregs={d * 2: rng.getrandbits(32),
                                  d * 2 + 1: rng.getrandbits(32)}))
    # VMSR/VMRS: FPSCR-Rundungswechsel wirkt auf die naechste Konvertierung
    for _ in range(count // 80 + 2):
        val = rng.choice([0x00000010, 0x00C00010, 0x00400010, 0x00800010])
        s = rng.randrange(16)
        cases.append(_case(
            "vmov r5, #0x%08X\nvmsr fpscr, r5\n"
            "vcvt.f32.s32 s%d, s%d\nvmrs r6, fpscr" % (val, s, s), rng,
            sregs={s: rng.getrandbits(32)}))
    return cases


def assemble_cases(cases):
    ks = Ks(KS_ARCH_ARM, KS_MODE_ARM | KS_MODE_LITTLE_ENDIAN)
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    out = []
    for case in cases:
        addr = ARM_BASE + len(out) * STRIDE
        text = case["asm"].strip() + "\nbx lr"
        try:
            enc, _ = ks.asm(text, addr)
        except Exception:
            continue
        blob = bytes(enc)
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
    x = (seed * 2654435761 + 1) & 0xFFFFFFFF
    buf = bytearray(DATA_SIZE)
    for i in range(DATA_SIZE):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        buf[i] = (x >> 16) & 0xFF
    return bytes(buf)


UREGS = [getattr(U, "UC_ARM_REG_R%d" % i) for i in range(14)]


def reference_run(cases, rng):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM | UC_MODE_LITTLE_ENDIAN)
    code_size = 0x1000
    while code_size < len(cases) * STRIDE:
        code_size *= 2
    uc.mem_map(ARM_BASE, code_size)
    uc.mem_map(DATA_BASE, DATA_SIZE)
    uc.reg_write(U.UC_ARM_REG_FPEXC, 0x40000000)
    refs, errors = [], []
    for idx, case in enumerate(cases):
        uc.mem_write(ARM_BASE + idx * STRIDE, case["blob"])
        uc.mem_write(DATA_BASE, _rng_data(case["data_seed"]))
        init_s = [rng.getrandbits(32) for _ in range(32)]
        for i, v in case["sregs"].items():
            init_s[i] = v & 0xFFFFFFFF
        case["init_s"] = init_s
        for i in range(32):
            uc.reg_write(getattr(U, "UC_ARM_REG_S%d" % i), init_s[i])
        init_r = [rng.getrandbits(32) for _ in range(14)]
        for i, v in case["regs"].items():
            init_r[i] = v & 0xFFFFFFFF
        init_r[4] = SCRATCH
        init_r[13] = STACK
        case["init_r"] = init_r
        for i in range(14):
            uc.reg_write(UREGS[i], init_r[i])
        uc.reg_write(U.UC_ARM_REG_CPSR, 0x13)
        uc.reg_write(U.UC_ARM_REG_FPSCR, case["fpscr"])
        try:
            uc.emu_start(case["addr"], 0, count=case["ninsn"])
        except Exception as e:
            errors.append((idx, case["asm"].strip(), str(e)))
            refs.append(None)
            continue
        if uc.reg_read(U.UC_ARM_REG_PC) != 0:
            errors.append((idx, case["asm"].strip(), "PC nicht 0"))
            refs.append(None)
            continue
        refs.append({
            "s": [uc.reg_read(getattr(U, "UC_ARM_REG_S%d" % i)) for i in range(32)],
            "r": [uc.reg_read(UREGS[i]) for i in range(14)],
            "cpsr": uc.reg_read(U.UC_ARM_REG_CPSR),
            "fpscr": uc.reg_read(U.UC_ARM_REG_FPSCR),
            "data": bytes(uc.mem_read(DATA_BASE, DATA_SIZE)),
        })
    return refs, errors


def mem_digest(buf):
    h = 2166136261
    for b in buf:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def gen_harness(cases, refs, path, img):
    L = ["/* VFP-Differenztest gegen Unicorn */",
         "#include <stdio.h>", "#include <string.h>",
         "#define DATA_BASE 0x%Xu" % DATA_BASE,
         "#define DATA_SIZE %du" % DATA_SIZE,
         "static uint8_t g_mem[%du];" % MEM_SIZE,
         "static int g_fail;",
         "static const unsigned char g_code[%d] = {" % len(img)]
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
    L.append('    if (g_fail < 80) printf("FAIL %d %s: 0x%08X (referenz 0x%08X)\\n",')
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
        for i, v in enumerate(case["init_s"]):
            L.append("  c.sreg[%d] = 0x%08Xu;" % (i, v))
        for i, v in enumerate(case["init_r"]):
            L.append("  c.r[%d] = 0x%08Xu;" % (i, v))
        L.append("  vfp_set_fpscr(&c, 0x%08Xu);" % case["fpscr"])
        L.append("  fn_%08X(&c);" % case["addr"])
        L.append('  if (c.aborted) { printf("FAIL %d aborted\\n"); g_fail++; return; }' % idx)
        L.append("  { static const uint32_t w[32] = {%s};"
                 % ", ".join("0x%08Xu" % v for v in refs[idx]["s"]))
        L.append("    for (int i = 0; i < 32; i++) if (c.sreg[i] != w[i]) {")
        L.append('      if (g_fail < 80) printf("FAIL ' + str(idx) +
                 ' s%d: 0x%08X (referenz 0x%08X)\\n", i, c.sreg[i], w[i]);')
        L.append("      g_fail++; } }")
        L.append("  { static const uint32_t w[14] = {%s};"
                 % ", ".join("0x%08Xu" % v for v in refs[idx]["r"]))
        L.append("    for (int i = 0; i < 14; i++) if (c.r[i] != w[i]) {")
        L.append('      if (g_fail < 80) printf("FAIL ' + str(idx) +
                 ' r%d: 0x%08X (referenz 0x%08X)\\n", i, c.r[i], w[i]);')
        L.append("      g_fail++; } }")
        L.append('  chk(%d, "fpscr_nzcv", c.fpscr & 0xF0000000u, 0x%08Xu);'
                 % (idx, refs[idx]["fpscr"] & 0xF0000000))
        L.append('  chk(%d, "mem", mem_digest(), 0x%08Xu);'
                 % (idx, mem_digest(refs[idx]["data"])))
        if case["check_flags"]:
            cpsr = refs[idx]["cpsr"]
            for name, bit in (("n", 31), ("z", 30), ("c", 29), ("v", 28)):
                L.append('  chk(%d, "flag_%s", c.%s, %du);'
                         % (idx, name, name, (cpsr >> bit) & 1))
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


def main(argv=None):
    ap = argparse.ArgumentParser(description="Differentieller VFP-Test")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=300)
    ap.add_argument("--filter", default=None)
    ap.add_argument("--keep", action="store_true", help="generierte C-Dateien behalten")
    args = ap.parse_args(argv)
    rng = random.Random(args.seed)
    cases = build_cases(rng, args.count)
    if args.filter:
        f = args.filter.lower()
        cases = [c for c in cases if f in c["asm"].lower()]
    cases = assemble_cases(cases)
    for i, c in enumerate(cases):
        c["data_seed"] = (args.seed * 131 + i * 7) & 0xFFFFFFFF
    if not cases:
        print("keine Faelle assembliert")
        return 0
    refs, errors = reference_run(cases, random.Random(args.seed * 977 + 13))
    for idx, asm, msg in errors[:8]:
        print("Fall %d uebersprungen (%s): %s" % (idx, msg, asm.replace("\n", "; ")))
    good = [(c, r) for c, r in zip(cases, refs) if r is not None]
    if not good:
        print("kein Fall ausfuehrbar")
        return 0
    os.makedirs(OUT, exist_ok=True)
    img = bytearray(len(cases) * STRIDE)
    for i, c in enumerate(cases):
        img[i * STRIDE:i * STRIDE + len(c["blob"])] = c["blob"]
    res = cli.scanmod.scan(bytes(img), [(c["addr"], "case", False) for c in cases])
    gen_c = os.path.join(OUT, "gen_vfp.c")
    cli.generate(res, gen_c)
    good_cases = [c for c, _ in good]
    good_refs = [r for _, r in good]
    harness = os.path.join(OUT, "harness_vfp.c")
    gen_harness(good_cases, good_refs, harness, bytes(img))
    p = compile_and_run(gen_c, harness,
                        os.path.join(OUT, "test_vfp.c"),
                        os.path.join(OUT, "test_vfp.exe"))
    print((p.stdout.strip() or p.stderr.strip()).replace("\n", "\n       "))
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
        print("betroffene Faelle:")
        for cid in bad[:40]:
            print("  #%d %s" % (cid, good_cases[cid]["asm"].replace("\n", "; ")))
    return 0 if p.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
