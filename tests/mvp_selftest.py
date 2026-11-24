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
        # Pseudo-Aufruf aus handgeschriebenem Assembler (Mobiclip-Decoder):
        # LR wird per `add lr, pc, #k` auf die Fortsetzung gesetzt, der
        # Aufrufer baut den Stack-Frame des Ziels selbst und springt mit `b`
        # hinter dessen Prolog. Das Ziel kehrt mit `pop {r4, pc}` zur
        # Fortsetzung zurueck. Frueher war das `b` ein Sprung, die Rueckkehr
        # verliess die C-Funktion und `add r0, #10` fehlte (r0 = 3 statt 13).
        "name": "pseudo_aufruf_b_mit_lr",
        "asm": """
            push {lr}
            mov r0, #1
            add lr, pc, #4
            push {r4, lr}
            b 0x100104
            add r0, r0, #10
            pop {pc}
        """,
        "extra": {
            0x100100: """
                push {r4, lr}
                add r0, r0, #2
                pop {r4, pc}
            """,
        },
        "regs": {},
        "expect": {"r0": 13, "r13": 0x8000},
    },
    {
        # Rotierter Immediate-Operand setzt bei logischen S-Befehlen das
        # Carry auf Bit 31 des Werts (tst r3, #0x80000000 im Mobiclip-
        # Bitreader). Frueher blieb C unveraendert.
        "name": "tst_rotiertes_immediate_carry",
        "asm": """
            mov r3, #0x40000000
            mov r0, #0
            cmp r0, #1
            tst r3, #0x80000000
            movcs r1, #1
            movcc r1, #2
            mov r2, #0
            cmp r2, #1
            ands r4, r3, #0xFF
            movcs r5, #1
            movcc r5, #2
            bx lr
        """,
        "regs": {},
        "expect": {"r1": 1, "r5": 2},
    },
    {
        # Berechneter Aufruf ueber `add pc, ip, r2` mit LR auf eine Fortsetzung,
        # die NICHT direkt hinter dem Sprung liegt (Mobiclip 0x16C31C ->
        # 0x16C340). Die Fortsetzung muss ein Blockanfang sein; frueher endete
        # die C-Funktion nach der Rueckkehr (r0 = 3 statt 13).
        "name": "berechneter_aufruf_lr_fortsetzung",
        "asm": """
            push {lr}
            mov r0, #1
            cmp r0, r0
            blne 0x100100
            mov r2, #0
            mov ip, #0x100000
            orr ip, ip, #0x100
            add lr, pc, #4
            add pc, ip, r2
            add r0, r0, #100
            add r0, r0, #10
            pop {pc}
        """,
        "extra": {
            0x100100: """
                add r0, r0, #2
                bx lr
            """,
        },
        "regs": {},
        "expect": {"r0": 13, "r13": 0x8000},
    },
    {
        # Bedingter Pseudo-Aufruf (Mobiclip 0x16BE84: `add lr, pc, #0xc;
        # bne 0x16C308`): das Ziel kehrt zur LR-Fortsetzung hinter dem Sprung
        # zurueck, die dazwischenliegende Instruktion wird uebersprungen.
        "name": "bedingter_pseudo_aufruf",
        "asm": """
            push {lr}
            mov r0, #1
            cmp r0, #0
            add lr, pc, #4
            bne 0x100100
            add r0, r0, #100
            add r0, r0, #10
            pop {pc}
        """,
        "extra": {
            0x100100: """
                add r0, r0, #2
                bx lr
            """,
        },
        "regs": {},
        "expect": {"r0": 13, "r13": 0x8000},
    },
    {
        # LR-Fortsetzung in zwei Schritten (Mobiclip 0x16C42C:
        # `add lr, pc, #0x80; add lr, lr, #0x38`) vor einem berechneten Aufruf.
        "name": "berechneter_aufruf_lr_zweischritt",
        "asm": """
            push {lr}
            mov r0, #1
            cmp r0, r0
            blne 0x100100
            mov r2, #0
            mov ip, #0x100000
            orr ip, ip, #0x100
            add lr, pc, #0
            add lr, lr, #8
            add pc, ip, r2
            add r0, r0, #100
            add r0, r0, #10
            pop {pc}
        """,
        "extra": {
            0x100100: """
                add r0, r0, #2
                bx lr
            """,
        },
        "regs": {},
        "expect": {"r0": 13, "r13": 0x8000},
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
    {
        # strlen (fn_0028F5E8) als nachgeschriebene Originalsequenz: wortweise
        # Schleife ueber uqsub8 (0x01010101 - Wort) und lsls-Kaskade, die den
        # Nullbyte-Index bestimmt. Muss fuer einen 8-Zeichen-Namen 8 liefern.
        # Der Literal-Pool liegt wie im Original 0x40 Byte hinter dem
        # `ldreq r2,[pc,#0x38]` (hier 0x10005C) und wird im Gastpuffer abgelegt.
        "name": "strlen_wortschleife",
        "asm": """
            add r3, r0, #1
            b check
        byte_loop:
            ldrb r1, [r0], #1
            cmp r1, #0
            subeq r0, r0, r3
            bxeq lr
        check:
            tst r0, #3
            ldreq r2, [pc, #0x38]
            bne byte_loop
        word_loop:
            ldr r1, [r0], #4
            uqsub8 r1, r2, r1
            cmp r1, #0
            beq word_loop
            lsls r2, r1, #24
            sub r0, r0, r3
            subne r0, r0, #3
            bxne lr
            lsls r2, r1, #16
            subne r0, r0, #2
            bxne lr
            lsls r1, r1, #8
            subne r0, r0, #1
            bx lr
            .word 0x01010101
        """,
        "regs": {"r0": 0x200},
        "mem_bytes": [(0x200, b"gsp::Gpu\x00"), (0x10005C, b"\x01\x01\x01\x01")],
        "expect": {"r0": 8},
    },
    {
        # SBC rechnet -NOT(C): nach cmp geht das C-Flag als "kein Borrow"
        # ein. Vorher wurde cin = C verwendet (Ergebnis um 1 zu klein).
        "name": "sbc_borrow",
        "asm": """
            cmp r2, #0
            sbc r0, r1, r4
            bx lr
        """,
        "regs": {"r1": 100, "r4": 1, "r2": 0},
        "expect": {"r0": 99, "c": 1},
    },
    {
        # LSR/ASR #32: die Kodierung meint den Sonderfall (amount 0).
        "name": "shift_32",
        "asm": """
            mov r0, #-1
            mov r1, r0, lsr #32
            mov r2, #0x80000000
            mov r3, r2, asr #32
            bx lr
        """,
        "regs": {},
        "expect": {"r1": 0, "r3": 0xFFFFFFFF},
    },
    {
        # RRX braucht das eingehende C-Flag (Shifter-Carry ist nur Ausgabe).
        "name": "rrx_carry_in",
        "asm": """
            cmp r2, #0
            mov r3, #2
            mov r0, r3, rrx
            bx lr
        """,
        "regs": {"r2": 0},
        "expect": {"r0": 0x80000001},
    },
    {
        # UXTAH extendiert 16 Bit, UXTAB dagegen 8 Bit (und optional ROR).
        "name": "uxtab_uxtah",
        "asm": """
            uxtab r0, r1, r2
            uxtah r3, r1, r2
            uxtab r4, r5, r2, ror #16
            bx lr
        """,
        "regs": {"r1": 0x100, "r2": 0x01FF01FF, "r5": 0},
        "expect": {"r0": 0x1FF, "r3": 0x2FF, "r4": 0xFF},
    },
    {
        # UBFX/SBFX/BFI/BFC (Bitfeld) waren nicht implementiert.
        "name": "bitfield",
        "asm": """
            ubfx r0, r1, #4, #8
            sbfx r2, r1, #4, #8
            mvn r3, #0
            bfi r3, r1, #4, #8
            mvn r4, #0
            bfc r4, #4, #8
            bx lr
        """,
        "regs": {"r1": 0x12345678},
        "expect": {"r0": 0x67, "r2": 0x67, "r3": 0xFFFFF78F, "r4": 0xFFFFF00F},
    },
    {
        # PKHBT ohne Shift: rn[15:0] | rm[31:16].
        "name": "pkhbt_shift0",
        "asm": """
            pkhbt r0, r1, r2
            bx lr
        """,
        "regs": {"r1": 0x11223344, "r2": 0xAABBCCDD},
        "expect": {"r0": 0xAABB3344},
    },
    {
        # 16-Bit-Thumb im IT-Block setzt keine Flags (ARM ARM A7.7.3).
        "name": "thumb_it_flags",
        "thumb": True,
        "asm": """
            push {lr}
            movs r0, #0
            itt eq
            moveq r1, #1
            addeq r2, r3, #1
            pop {pc}
        """,
        "regs": {"r3": 41},
        "expect": {"r1": 1, "r2": 42, "z": 1},
    },
    {
        # QDADD: Rd = sat(Rm + sat(2*Rn)); verdoppelt wird der zweite
        # Quelloperand, die Verdopplung saturiert selbst (Q).
        "name": "qdadd_saturation",
        "asm": """
            qdadd r0, r1, r2
            bx lr
        """,
        "regs": {"r1": 3, "r2": 0x40000000},
        "expect": {"r0": 0x7FFFFFFF, "q": 1},
    },
    {
        # `mov<cond> pc, lr` ist eine bedingte Rueckkehr: bei falscher
        # Bedingung muss der Folgeblock laufen.
        "name": "movcond_pc_lr",
        "asm": """
            push {r4, lr}
            mov r4, r0
            cmp r1, #0
            moveq pc, lr
            add r0, r4, r1
            pop {r4, pc}
        """,
        "regs": {"r0": 7, "r1": 5},
        "expect": {"r0": 12},
    },
    {
        # VCMP: gleich -> Z=1,C=1 (vorher fehlte C). Vergleich ueber VMRS.
        "name": "vfp_vcmp_equal",
        "asm": """
            vmov s0, r0
            vmov s1, r1
            vcmp.f32 s0, s1
            vmrs APSR_nzcv, fpscr
            bx lr
        """,
        "regs": {"r0": 0x3FC00000, "r1": 0x3FC00000},
        "expect": {"n": 0, "z": 1, "c": 1, "v": 0},
    },
    {
        # VCMP: groesser -> C=1 (vorher fehlte C).
        "name": "vfp_vcmp_greater",
        "asm": """
            vmov s0, r0
            vmov s1, r1
            vcmp.f32 s0, s1
            vmrs APSR_nzcv, fpscr
            bx lr
        """,
        "regs": {"r0": 0x40000000, "r1": 0x3FC00000},
        "expect": {"n": 0, "z": 0, "c": 1, "v": 0},
    },
    {
        # VCMP: kleiner -> N=1, C=0.
        "name": "vfp_vcmp_less",
        "asm": """
            vmov s0, r0
            vmov s1, r1
            vcmp.f32 s0, s1
            vmrs APSR_nzcv, fpscr
            bx lr
        """,
        "regs": {"r0": 0x3FC00000, "r1": 0x40000000},
        "expect": {"n": 1, "z": 0, "c": 0, "v": 0},
    },
    {
        # VCMP: NaN -> unordered (C=1,V=1).
        "name": "vfp_vcmp_unordered",
        "asm": """
            vmov s0, r0
            vmov s1, r1
            vcmp.f32 s0, s1
            vmrs APSR_nzcv, fpscr
            bx lr
        """,
        "regs": {"r0": 0x7FC00000, "r1": 0x3FC00000},
        "expect": {"n": 0, "z": 0, "c": 1, "v": 1},
    },
    {
        # Default-FPSCR des 3DS rundet ganzzahlig->f32 Richtung Null:
        # 16777219 -> 16777218.0 (0x4B800001), nicht 16777220 (nearest).
        "name": "vfp_vcvt_i2f_round_zero",
        "asm": """
            vmov s0, r0
            vcvt.f32.s32 s0, s0
            vmov r0, s0
            bx lr
        """,
        "regs": {"r0": 16777219},
        "expect": {"r0": 0x4B800001},
    },
    {
        # f32->s32 rundet immer Richtung Null (auch bei FPSCR=nearest).
        "name": "vfp_vcvt_f2i_trunc",
        "asm": """
            vmov s0, r0
            vcvt.s32.f32 s0, s0
            vmov r0, s0
            bx lr
        """,
        "regs": {"r0": 0xC02CCCCD},  # -2.7f
        "expect": {"r0": 0xFFFFFFFE},
    },
    {
        # Ueberlauf/NaN bei f32->s32 saettigt: 3e9 -> INT_MAX, NaN -> 0.
        "name": "vfp_vcvt_f2i_sat",
        "asm": """
            vmov s0, r0
            vcvt.s32.f32 s0, s0
            vmov r0, s0
            vmov s1, r1
            vcvt.s32.f32 s1, s1
            vmov r1, s1
            bx lr
        """,
        "regs": {"r0": 0x4F32D05E, "r1": 0x7FC00000},  # 3e9f, NaN
        "expect": {"r0": 0x7FFFFFFF, "r1": 0},
    },
    {
        # Flush-to-zero (Default-FPSCR): denormale Ergebnisse werden zu
        # Null mit erhaltenem Vorzeichen.
        "name": "vfp_ftz_signed_zero",
        "asm": """
            vmov s0, r0
            vmov s1, r0
            vadd.f32 s2, s0, s1
            vmov r0, s2
            vmov s3, r1
            vmul.f32 s4, s3, s3
            vmov r1, s4
            bx lr
        """,
        "regs": {"r0": 0x80000001, "r1": 0x00000001},
        "expect": {"r0": 0x80000000, "r1": 0x00000000},
    },
    {
        # Default-NaN-Modus (FPSCR.DN): NaN-Ergebnis wird kanonisch 0x7FC00000.
        "name": "vfp_default_nan",
        "asm": """
            vmov s0, r0
            vadd.f32 s2, s0, s0
            vmov r0, s2
            bx lr
        """,
        "regs": {"r0": 0x7FC00001},
        "expect": {"r0": 0x7FC00000},
    },
    {
        # VMOV zwischen Kern- und Doppelregister (zwei Worte).
        "name": "vfp_vmov_d_pair",
        "asm": """
            vmov d0, r0, r1
            vmov r2, r3, d0
            bx lr
        """,
        "regs": {"r0": 0x11223344, "r1": 0x55667788},
        "expect": {"r2": 0x11223344, "r3": 0x55667788},
    },
    {
        # VMLA rundet das Produkt vor der Addition (VFPv2/Unicorn):
        # -1 + (1+2^-12)^2 -> 2^-11 = 0x3A000000.
        "name": "vfp_vmla_separate_round",
        "asm": """
            vmov s0, r0
            vmov s1, r1
            vmov s3, r2
            vmla.f32 s3, s0, s1
            vmov r0, s3
            bx lr
        """,
        "regs": {"r0": 0x3F800800, "r1": 0x3F800800, "r2": 0xBF800000},
        "expect": {"r0": 0x3A000000},
    },
    {
        # Festkomma-Varianten: s32 -> f32 mit #16 und zurueck.
        "name": "vfp_vcvt_fixed",
        "asm": """
            vmov s0, r0
            vcvt.f32.s32 s0, s0, #16
            vmov r0, s0
            vcvt.s32.f32 s0, s0, #16
            vmov r1, s0
            bx lr
        """,
        "regs": {"r0": 0x18000},
        "expect": {"r0": 0x3FC00000, "r1": 0x18000},
    },
    {
        # VPush/VPOP und VSTR/VLDR mit Doppelregister.
        "name": "vfp_stack_mem",
        "asm": """
            vmov d0, r0, r1
            vpush {d0}
            vpop {d1}
            vmov r2, r3, d1
            vstr d1, [r4]
            vldr d2, [r4]
            vmov r0, r1, d2
            bx lr
        """,
        "regs": {"r0": 0xDEADBEEF, "r1": 0x12345678, "r4": 0x2000},
        "expect": {"r2": 0xDEADBEEF, "r3": 0x12345678,
                   "r0": 0xDEADBEEF, "r1": 0x12345678},
    },
    {
        # f64: vadd, vsqrt, vneg/vabs und f64->s32.
        "name": "vfp_double_ops",
        "asm": """
            vmov d0, r0, r1
            vadd.f64 d1, d0, d0
            vmov r2, r3, d1
            vsqrt.f64 d2, d0
            vmov r0, r1, d2
            vcvt.s32.f64 s0, d1
            vmov r4, s0
            bx lr
        """,
        # d0 = 9.0
        "regs": {"r0": 0x00000000, "r1": 0x40220000},
        "expect": {"r2": 0x00000000, "r3": 0x40320000,
                   "r0": 0x00000000, "r1": 0x40080000, "r4": 18},
    },
    {
        # LDREX/STREX ohne Threadwechsel: Reservierung gilt, STREX schreibt.
        "name": "ldrex_strex_erfolg",
        "asm": """
            ldrex r0, [r1]
            strex r2, r3, [r1]
            ldr r4, [r1]
            bx lr
        """,
        "regs": {"r1": 0x1000, "r3": 0x55667788},
        "mem": [(0x1000, 0x44), (0x1001, 0x33), (0x1002, 0x22), (0x1003, 0x11)],
        "expect": {"r0": 0x11223344, "r2": 0, "r4": 0x55667788},
    },
    {
        # CLREX loescht den lokalen Monitor; STREX muss scheitern und darf
        # den Speicher nicht schreiben.
        "name": "ldrex_clrex_strex_fehler",
        "asm": """
            ldrex r0, [r1]
            clrex
            strex r2, r3, [r1]
            ldr r4, [r1]
            bx lr
        """,
        "regs": {"r1": 0x1000, "r3": 0x55667788},
        "mem": [(0x1000, 0x44), (0x1001, 0x33), (0x1002, 0x22), (0x1003, 0x11)],
        "expect": {"r0": 0x11223344, "r2": 1, "r4": 0x11223344},
    },
    {
        # Ein normaler Store auf die ueberwachte Adresse entwertet die
        # Reservierung (ARMv6K); STREX scheitert.
        "name": "ldrex_store_strex_fehler",
        "asm": """
            ldrex r0, [r1]
            str r5, [r1]
            strex r2, r3, [r1]
            ldr r4, [r1]
            bx lr
        """,
        "regs": {"r1": 0x1000, "r3": 0x55667788, "r5": 0x0A0B0C0D},
        "mem": [(0x1000, 0x44), (0x1001, 0x33), (0x1002, 0x22), (0x1003, 0x11)],
        "expect": {"r0": 0x11223344, "r2": 1, "r4": 0x0A0B0C0D},
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
        if name in ("n", "z", "c", "v", "q"):
            field = "qflag" if name == "q" else name
            checks.append('  if (c.%s != %du) { printf("FAIL %s: %%u\\n", c.%s); return 1; }'
                          % (field, val, name, field))
        else:
            idx_r = int(name[1:])
            checks.append('  if (c.r[%d] != 0x%08Xu) { printf("FAIL %s: 0x%%08X (erwartet 0x%08X)\\n", c.r[%d]); return 1; }'
                          % (idx_r, val, name, val, idx_r))

    main = ['#include "arm_cpu.h"', '#include <stdio.h>', '#include <string.h>',
            'void fn_00100000(ArmCpu *c);', 'int main(void) {',
            '  static uint8_t mem[0x140000];  /* muss den Textbereich ab 0x100000 fassen */', '  ArmCpu c;',
            '  arm_cpu_init(&c, mem, sizeof(mem));',
            '  c.r[13] = 0x8000;  /* Stack */']
    for k, v in regs.items():
        main.append('  c.r[%d] = 0x%08Xu;' % (int(k[1:]), v))
    for addr, val in mem:
        main.append('  mem[0x%X] = 0x%02Xu;' % (addr, val))
    for addr, blob in (case.get("mem_bytes") or []):
        for i, b in enumerate(blob):
            main.append('  mem[0x%X] = 0x%02Xu;' % (addr + i, b))
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


def exclusive_monitor_test():
    """Prueft den Exklusiv-Monitor direkt: Threadwechsel/Store/clrex
    invalidieren die Reservierung, passende LDREX/STREX-Folgen gelingen."""
    src = r'''
#include "arm_cpu.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s\n", msg); fails++; } } while (0)

int main(void) {
    static uint8_t mem[0x4000];
    ArmCpu a, b;
    uint64_t d;
    memset(mem, 0, sizeof mem);
    mem[0x40] = 0x44; mem[0x41] = 0x33; mem[0x42] = 0x22; mem[0x43] = 0x11;
    mem[0x48] = 0x08; mem[0x49] = 0x07; mem[0x4A] = 0x06; mem[0x4B] = 0x05;
    mem[0x4C] = 0x04; mem[0x4D] = 0x03; mem[0x4E] = 0x02; mem[0x4F] = 0x01;
    arm_cpu_init(&a, mem, sizeof mem);
    arm_cpu_init(&b, mem, sizeof mem);

    /* 1. LDREX/STREX ohne Wechsel gelingt. */
    CHECK(arm_excl_load(&a, 0x40, 4) == 0x11223344u, "ldrex liest Wort");
    CHECK(arm_excl_store(&a, 0x40, 4, 0xAABBCCDDu) == 0, "strex gelingt");
    CHECK(arm_mem_read(&a, 0x40, 4) == 0xAABBCCDDu, "strex schreibt");

    /* 2. Store eines anderen Threads entwertet die Reservierung. */
    arm_excl_load(&a, 0x40, 4);
    arm_mem_write(&b, 0x40, 4, 0x01020304u);
    CHECK(arm_excl_store(&a, 0x40, 4, 0xDEADBEEFu) == 1, "strex nach Fremd-Store scheitert");
    CHECK(arm_mem_read(&a, 0x40, 4) == 0x01020304u, "strex schreibt nach Fehler nicht");

    /* 3. Threadwechsel (arm_excl_clear_all) entwertet alle Monitore. */
    arm_excl_load(&a, 0x40, 4);
    arm_excl_clear_all();
    CHECK(arm_excl_store(&a, 0x40, 4, 0xDEADBEEFu) == 1, "strex nach Threadwechsel scheitert");

    /* 4. CLREX entwertet den lokalen Monitor. */
    arm_excl_load(&a, 0x40, 4);
    arm_excl_clear(&a);
    CHECK(arm_excl_store(&a, 0x40, 4, 0xDEADBEEFu) == 1, "strex nach clrex scheitert");

    /* 5. Fremde LDREX auf derselben Adresse entwertet die erste Reservierung. */
    arm_excl_load(&a, 0x40, 4);
    arm_excl_load(&b, 0x40, 4);
    CHECK(arm_excl_store(&a, 0x40, 4, 0xDEADBEEFu) == 1, "alte Reservierung entwertet");

    /* 6. Groesse muss passen (STREXB nach LDREX-Wort scheitert). */
    arm_excl_load(&a, 0x40, 4);
    CHECK(arm_excl_store(&a, 0x40, 1, 0x5Au) == 1, "falsche Groesse scheitert");

    /* 7. LDREXB/STREXB. */
    arm_excl_load(&a, 0x40, 1);
    CHECK(arm_excl_store(&a, 0x40, 1, 0x5Au) == 0, "strexb gelingt");
    CHECK(arm_mem_read(&a, 0x40, 1) == 0x5Au, "strexb schreibt Byte");

    /* 8. LDREXD/STREXD (Adresse muss 8 Byte ausgerichtet sein). */
    d = arm_excl_load64(&a, 0x48);
    CHECK(d == 0x0102030405060708ull, "ldrexd liest Doppelwort");
    CHECK(arm_excl_store64(&a, 0x48, 0x11223344u, 0x55667788u) == 0, "strexd gelingt");
    CHECK(arm_mem_read(&a, 0x48, 4) == 0x11223344u, "strexd schreibt lo");
    CHECK(arm_mem_read(&a, 0x4C, 4) == 0x55667788u, "strexd schreibt hi");

    if (fails) { printf("%d Fehler\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
'''
    os.makedirs(OUT, exist_ok=True)
    src_path = os.path.join(OUT, "exclusive_monitor.c")
    with open(src_path, "w", encoding="utf-8") as f:
        f.write(src)
    exe = os.path.join(OUT, "exclusive_monitor.exe")
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), "zig.exe")
    runtime = os.path.join(ROOT, "runtime")
    cmd = [zig, "cc", "-std=c11", "-O1", "-I", os.path.join(runtime, "cpu"),
           src_path, os.path.join(runtime, "cpu", "arm_cpu.c"),
           os.path.join(runtime, "cpu", "arm_trace.c"),
           os.path.join(runtime, "workarounds.c"), "-o", exe]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print("[exklusiv-monitor] COMPILE-FEHLER\n%s\n%s" % (p.stdout, p.stderr))
        return False
    r = subprocess.run([exe], capture_output=True, text=True)
    if r.returncode == 0:
        print("[exklusiv-monitor] OK")
        return True
    print("[exklusiv-monitor] FEHLER: %s" % (r.stdout.strip() or r.stderr.strip()))
    return False


def discovery_tests():
    """Recompiler-Discovery: Interworking-Modus und Zeiger-Seeds.

    Prueft drei Regeln, die frueher ganze Thumb-Regionen im ARM-Dekoder
    landen liessen:
      * Thumb `blx #imm` wechselt nach ARM (und ARM `blx #imm` nach Thumb),
      * ein Zeigerwort mit Bit 0 wird als Thumb-Funktion geseedet, auch in
        den ersten 4 KiB (Init-/Konstruktor-Tabellen),
      * ein Zeiger mitten in eine bereits dekodierte Funktion wird verworfen.
    """
    from recomp import scan as scanmod
    img = bytearray(0x4000)
    ks_a = Ks(KS_ARCH_ARM, KS_MODE_ARM | KS_MODE_LITTLE_ENDIAN)
    ks_t = Ks(KS_ARCH_ARM, KS_MODE_THUMB | KS_MODE_LITTLE_ENDIAN)

    def put(ks, addr, text):
        enc, _ = ks.asm(text, addr)
        off = addr - isa.ARM_BASE
        img[off:off + len(enc)] = bytes(enc)

    def word(addr, val):
        off = addr - isa.ARM_BASE
        img[off:off + 4] = val.to_bytes(4, "little")

    put(ks_a, 0x100000, "blx 0x100100\nnop\nnop\npush {r4, lr}\nbx lr")
    put(ks_t, 0x100100, "push {r4, lr}\nblx 0x100200\npop {r4, pc}")
    put(ks_a, 0x100200, "push {r4, lr}\nbx lr")
    put(ks_t, 0x100300, "push {r4, lr}\nbx lr")
    word(0x100400, 0x100301)   # Thumb-Zeiger in die ersten 4 KiB
    word(0x100404, 0x100008)   # mitten in fn_00100000 (kein Prolog)
    word(0x100408, 0x10000C)   # linear ueberlaufener Prolog (echter Anfang)

    res = scanmod.scan(bytes(img), [(isa.ARM_BASE, "entry", False)],
                       seed_pointers=True)
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)

    f_thumb = res.functions.get(0x100100)
    check(f_thumb is not None and f_thumb.thumb,
          "Thumb-Ziel von ARM blx nicht als Thumb geseedet")
    f_arm = res.functions.get(0x100200)
    check(f_arm is not None and not f_arm.thumb,
          "ARM-Ziel von Thumb blx nicht als ARM geseedet")
    f_ptr = res.functions.get(0x100300)
    check(f_ptr is not None and f_ptr.thumb
          and f_ptr.source.startswith("pointer@"),
          "Thumb-Zeiger in den ersten 4 KiB nicht als Seed benutzt")
    check(0x100008 not in res.functions,
          "Zeiger mitten in eine Funktion wurde als Funktion geseedet")
    f_over = res.functions.get(0x10000C)
    check(f_over is not None,
          "ueberlaufener Prolog-Seed (push mit lr) wurde verworfen")
    if fails:
        for m in fails:
            print("[discovery] FAIL %s" % m)
        return False
    print("[discovery] OK")
    return True


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

    if not exclusive_monitor_test():
        failed += 1
    if not discovery_tests():
        failed += 1
    # Scheduler-/Kernel-Entscheidungen (reine Logik, ohne OS-Threads).
    print("[sched] pruefe Prioritaeten/Ticks/Wait-Index ...")
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "sched_selftest.py")],
                       capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    if r.returncode != 0:
        sys.stdout.write(r.stderr)
        failed += 1
    total = len(CASES) + 3
    print("\n%d/%d Tests inkl. Exklusiv-Monitor, Discovery und Scheduler bestanden" % (total - failed, total))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
