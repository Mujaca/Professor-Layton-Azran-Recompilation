"""Selbsttest: assembliert ARM-Snippets, recompiliert sie und prueft das Ergebnis.

  python tests/mvp_selftest.py
"""
import os
import subprocess
import sys

from keystone import Ks, KS_ARCH_ARM, KS_MODE_ARM, KS_MODE_THUMB, KS_MODE_LITTLE_ENDIAN

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from recomp import isa, emit, cli  # noqa: E402

OUT = os.path.join(ROOT, "out", "selftest")


def assemble(blocks, thumb=False, raw=None):
    """blocks: [(addr, asm_text)] -> bytearray ab 0x100000."""
    img = bytearray(0x4000)
    mode = KS_MODE_THUMB if thumb else KS_MODE_ARM
    ks = Ks(KS_ARCH_ARM, mode | KS_MODE_LITTLE_ENDIAN)
    for addr, text in blocks:
        encoding, count = ks.asm(text, addr)
        off = addr - isa.ARM_BASE
        img[off:off + len(encoding)] = bytes(encoding)
    for addr, blob in (raw or []):
        off = addr - isa.ARM_BASE
        img[off:off + len(blob)] = blob
    # Instruktionscache leeren (sonst ARM11-Selbstmodifikationsproblematik)
    return img, ks


CASES = [
    {
        "name": "schleife_summe",
        "asm": """
            mov r0, #0
            mov r1, #10
        loop:
            add r0, r0, r1
            subs r1, r1, #1
            bne loop
            bx lr
        """,
        "regs": {},
        "expect": {"r0": 55, "r1": 0, "z": 1},
    },
    {
        "name": "speicher",
        "asm": """
            ldr r0, [r1]
            add r0, r0, #1
            str r0, [r1, #4]
            ldr r2, [r1, #4]
            ldrb r3, [r1, #4]
            bx lr
        """,
        "regs": {"r1": 0x1000},
        "mem": [(0x1000, 41)],
        "expect": {"r0": 42, "r2": 42, "r3": 42},
    },
    {
        "name": "aufruf",
        "asm": """
            mov r0, #7
            bl 0x100100
            add r0, r0, #1
            bx lr
        """,
        "extra": {
            0x100100: """
                add r0, r0, r0
                bx lr
            """,
        },
        "regs": {},
        "expect": {"r0": 15},
    },
    {
        "name": "push_pop_cond",
        "asm": """
            push {r4, lr}
            mov r4, #5
            cmp r4, #5
            moveq r0, #100
            movne r0, #200
            pop {r4, pc}
        """,
        "regs": {},
        "expect": {"r0": 100, "r4": 0},
    },
    {
        "name": "shifts",
        "asm": """
            mov r0, r1, lsl #2
            add r2, r1, r1, lsr #1
            and r3, r0, #0xff
            lsl r3, r3, #1
            bx lr
        """,
        "regs": {"r1": 5},
        "expect": {"r0": 20, "r2": 7, "r3": 40},
    },
    {
        "name": "vfp_float",
        "asm": """
            mov r0, #0x3f800000
            vmov s0, r0
            vmov s1, r0
            vadd.f32 s2, s0, s1
            vmov r0, s2
            bx lr
        """,
        "regs": {},
        "expect": {"r0": 0x40000000},
    },

    {
        "name": "simd_uadd8_sel",
        "asm": """
            uadd8 r0, r1, r2
            sel r3, r1, r2
            bx lr
        """,
        "regs": {"r1": 0x04030201, "r2": 0xFFFFFFFF},
        "expect": {"r0": 0x03020100, "r3": 0x04030201},
    },
    {
        "name": "simd_saturierung",
        "asm": """
            uqadd8 r0, r1, r2
            uqsub8 r3, r1, r2
            bx lr
        """,
        "regs": {"r1": 0xF0F0F0F0, "r2": 0x20202020},
        "expect": {"r0": 0xFFFFFFFF, "r3": 0xD0D0D0D0},
    },
    {
        "name": "dsp_smlabb",
        "asm": """
            smlabb r0, r1, r2, r3
            smulbb r4, r1, r2
            bx lr
        """,
        "regs": {"r1": 3, "r2": 4, "r3": 10},
        "expect": {"r0": 22, "r4": 12},
    },
    {
        "name": "pack_und_extend",
        "asm": """
            pkhbt r0, r1, r2, lsl #8
            uxtb16 r3, r4
            bx lr
        """,
        "regs": {"r1": 0xAAAAAAAA, "r2": 0xBBBBBBBB, "r4": 0x11223344},
        "expect": {"r0": 0xBBBBAAAA, "r3": 0x00220044},
    },
    {
        "name": "rotated_immediate",
        "asm": """
            mov r0, #0
            bx lr
        """,
        "raw": [(0x100000, bytes.fromhex("b06f8fe2"))],
        "regs": {},
        "expect": {"r0": 0, "r6": 0x1002C8},
    },
    {
        "name": "thumb_schleife",
        "thumb": True,
        "asm": """
            movs r0, #0
            movs r1, #5
        loop:
            adds r0, r0, r1
            subs r1, #1
            bne loop
            bx lr
        """,
        "regs": {},
        "expect": {"r0": 15, "r1": 0},
    },
    {
        "name": "ldm_stm",
        "asm": """
            sub sp, sp, #16
            mov r0, #11
            mov r1, #22
            stmia sp, {r0, r1}
            mov r0, #0
            mov r1, #0
            ldmia sp, {r2, r3}
            add sp, sp, #16
            bx lr
        """,
        "regs": {},
        "expect": {"r2": 11, "r3": 22},
    },
    {
        # ARM-Rueckkehr ueber ein Register, das aus dem Stack geladen wurde:
        # `pop {r3}; bx r3` ist ein Rueckkehr-Idiom (Thumb-Interworking), kein
        # indirekter Aufruf. Frueher wurde das Ziel per arm_call_guest als neue
        # Gastfunktion gestartet -> Prolog uebersprungen, SP driftete (+0x50).
        "name": "bx_rN_rueckkehr_thumb",
        "asm": """
            mov r0, #2
            blx 0x100200
            add r0, r0, #1
            bx lr
        """,
        "thumb_raw": {
            0x100200: """
                push {lr}
                movs r0, #7
                pop {r3}
                bx r3
            """,
        },
        "regs": {},
        "expect": {"r0": 8, "r13": 0x8000},
    },
    {
        # Bedingte Terminatoren (bx<cond> lr, pop<cond> {pc}) haben einen
        # Fallthrough. Wurde der Folgeblock nicht angelegt, endete die Funktion
        # nach `bxne lr` sofort; im Boot uebersprang das die FS-Session-Init
        # (fn_00291C64 -> 0x291C74).
        "name": "bx_cond_fallthrough",
        "asm": """
            mov r0, #1
            cmp r0, #2
            bxeq lr
            mov r0, #42
            cmp r0, #41
            popeq {pc}
            bx lr
        """,
        "regs": {},
        "expect": {"r0": 42},
    },
]


def build_case(case, idx):
    raw = list(case.get("raw") or [])
    if case.get("thumb_raw"):
        ks_t = Ks(KS_ARCH_ARM, KS_MODE_THUMB | KS_MODE_LITTLE_ENDIAN)
        for addr, text in case["thumb_raw"].items():
            encoding, count = ks_t.asm(text, addr)
            raw.append((addr, bytes(encoding)))
    img, ks = assemble([(isa.ARM_BASE, case["asm"])] +
                       sorted((case.get("extra") or {}).items()),
                       thumb=case.get("thumb", False), raw=raw)
    data = bytes(img)
    res = cli.scanmod.scan(data, [(isa.ARM_BASE, "entry", case.get("thumb", False))])
    c_path = os.path.join(OUT, "gen_%d.c" % idx)
    n_arm, n_thumb = cli.generate(res, c_path)

    regs = {"r0": 0, "r1": 0, "r2": 0, "r3": 0}
    regs.update(case.get("regs", {}))
    mem = case.get("mem", [])
    checks = []
    for name, val in case["expect"].items():
        if name in ("n", "z", "c", "v"):
            checks.append('  if (c.%s != %du) { printf("FAIL %s: %%u\\n", c.%s); return 1; }'
                          % (name, val, name, name))
        else:
            idx_r = int(name[1:])
            checks.append('  if (c.r[%d] != 0x%08Xu) { printf("FAIL %s: 0x%%08X (erwartet 0x%08X)\\n", c.r[%d]); return 1; }'
                          % (idx_r, val, name, val, idx_r))

    main = ['#include "arm_cpu.h"', '#include <stdio.h>', '#include <string.h>',
            'void fn_00100000(ArmCpu *c);', 'int main(void) {',
            '  static uint8_t mem[0x10000];', '  ArmCpu c;',
            '  arm_cpu_init(&c, mem, sizeof(mem));',
            '  c.r[13] = 0x8000;  /* Stack */']
    for k, v in regs.items():
        main.append('  c.r[%d] = 0x%08Xu;' % (int(k[1:]), v))
    for addr, val in mem:
        main.append('  mem[0x%X] = 0x%02Xu;' % (addr, val))
    main.append('  fn_00100000(&c);')
    main.append('  if (c.aborted) { printf("FAIL aborted\\n"); return 1; }')
    main += checks
    main.append('  printf("OK\\n");')
    main.append('  return 0;')
    main.append('}')
    main_path = os.path.join(OUT, "main_%d.c" % idx)
    with open(main_path, "w", encoding="utf-8") as f:
        f.write("\n".join(main))
    return c_path, main_path


def main():
    os.makedirs(OUT, exist_ok=True)
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), "zig.exe")
    runtime = os.path.join(ROOT, "runtime")

    failed = 0
    for idx, case in enumerate(CASES):
        c_path, main_path = build_case(case, idx)
        exe = os.path.join(OUT, "case_%d.exe" % idx)
        cmd = [zig, "cc", "-std=c11", "-O1", "-I", os.path.join(runtime, "cpu"), main_path, c_path,
               os.path.join(runtime, "cpu", "arm_cpu.c"), os.path.join(runtime, "cpu", "arm_trace.c"),
               os.path.join(runtime, "workarounds.c"), "-o", exe]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode != 0:
            print("[%s] COMPILE-FEHLER\n%s\n%s" % (case["name"], p.stdout, p.stderr))
            failed += 1
            continue
        r = subprocess.run([exe], capture_output=True, text=True)
        status = r.stdout.strip() or r.stderr.strip()
        if r.returncode == 0:
            print("[%s] OK" % case["name"])
        else:
            print("[%s] FEHLER: %s" % (case["name"], status))
            failed += 1

    print("\n%d/%d Tests bestanden" % (len(CASES) - failed, len(CASES)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
