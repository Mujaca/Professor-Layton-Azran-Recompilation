/* Minimal-Runtime fuer recompilierten ARM11-Code (MVP).
 * Zustand + Speicher + Hilfsfunktionen; keine Emulation der 3DS-Services. */
#ifndef ARM_CPU_H
#define ARM_CPU_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define ARM_SP 13
#define ARM_LR 14
#define ARM_PC 15

typedef struct ArmCpu {
    uint32_t r[16];
    /* APSR-Flags */
    uint32_t n, z, c, v;
    uint32_t t;              /* Thumb-Bit */
    uint32_t cpsr_mode;      /* CPSR-Modusbits (Gast laeuft im User-Mode 0x10) */
    /* VFP: rohe 32-Bit-Ansicht, 32 s-Register / 16 d-Register */
    uint32_t sreg[64];
    uint32_t fpscr;
    uint8_t *mem;
    uint32_t mem_size;
    uint32_t tpidruro;      /* mrc p15,c13,c0,3 (TLS-Zeiger) */
    uint32_t ge;            /* GE[3:0] der SIMD-Operationen */
    uint32_t qflag;         /* CPSR.Q (Saturation) */
    int aborted;
    uint64_t insn_count;
    /* Lokaler Exklusiv-Monitor (LDREX/STREX, ARMv6K). Der Monitor gilt pro
     * Gast-Thread; die globalen Slots in arm_cpu.c tragen die Generation. */
    int excl_valid;
    int excl_slot;
    uint32_t excl_addr;
    uint32_t excl_size;
    uint32_t excl_gen;
} ArmCpu;

/* Datenverarbeitungs-Operationen (muessen zu tools/recomp/emit.py passen) */
enum {
    DP_AND = 1, DP_EOR, DP_SUB, DP_RSB, DP_ADD, DP_ADC, DP_SBC, DP_RSC,
    DP_TST, DP_TEQ, DP_CMP, DP_CMN, DP_ORR, DP_MOV, DP_BIC, DP_MVN
};

enum { SH_LSL = 0, SH_LSR, SH_ASR, SH_ROR, SH_RRX };

void arm_cpu_init(ArmCpu *c, void *mem, uint32_t size);

uint32_t arm_mem_read(ArmCpu *c, uint32_t addr, int size);
void arm_mem_write(ArmCpu *c, uint32_t addr, int size, uint32_t val);

/* ---- Hot-Path-Inlines -------------------------------------------------
 * Der generierte Code ruft diese Helfer millionenfach. Die schnellen Pfade
 * stehen deshalb im Header (einfacher Zeigerzugriff auf c->mem mit
 * Bereichspruefung); alles, was Diagnose (ARM_MEMWATCH*), den Exklusiv-
 * Monitor oder einen Fehlzugriff betrifft, geht unveraendert durch die
 * langsamen Fallbacks in arm_cpu.c.
 *
 * arm_mem_traced: 1, sobald ARM_MEMWATCH/RD/RANGE gesetzt ist.
 * arm_excl_live:  Anzahl offener LDREX-Reservierungen (Schreibzugriffe
 *                 muessen sie entwerten).                                          */
extern int arm_mem_traced;
extern volatile int arm_excl_live;

uint32_t arm_ld32_slow(ArmCpu *c, uint32_t addr);
uint32_t arm_ld16_slow(ArmCpu *c, uint32_t addr);
uint32_t arm_ld16s_slow(ArmCpu *c, uint32_t addr);
uint32_t arm_ld8_slow(ArmCpu *c, uint32_t addr);
uint32_t arm_ld8s_slow(ArmCpu *c, uint32_t addr);
void arm_st32_slow(ArmCpu *c, uint32_t addr, uint32_t val);
void arm_st16_slow(ArmCpu *c, uint32_t addr, uint32_t val);
void arm_st8_slow(ArmCpu *c, uint32_t addr, uint32_t val);

static inline int arm_mem_flat(ArmCpu *c, uint32_t addr, uint32_t size,
                               uint32_t *out)
{
    /* 3DS-Busalias 0x8xxxxxxx/0xBxxxxxxx -> dieselben FCRAM-Seiten. */
    if (addr >= 0x20000000u) addr &= 0x1FFFFFFFu;
    if ((uint64_t)addr + (uint64_t)size > (uint64_t)c->mem_size)
        return 0;
    *out = addr;
    return 1;
}

static inline uint32_t arm_ld32(ArmCpu *c, uint32_t addr)
{
    uint32_t v, a;
    if (arm_mem_traced || !arm_mem_flat(c, addr, 4u, &a))
        return arm_ld32_slow(c, addr);
    memcpy(&v, c->mem + a, 4);
    return v;
}

static inline uint32_t arm_ld16(ArmCpu *c, uint32_t addr)
{
    uint16_t v;
    uint32_t a;
    if (arm_mem_traced || !arm_mem_flat(c, addr, 2u, &a))
        return arm_ld16_slow(c, addr);
    memcpy(&v, c->mem + a, 2);
    return (uint32_t)v;
}

static inline uint32_t arm_ld16s(ArmCpu *c, uint32_t addr)
{
    uint16_t v;
    uint32_t a;
    if (arm_mem_traced || !arm_mem_flat(c, addr, 2u, &a))
        return arm_ld16s_slow(c, addr);
    memcpy(&v, c->mem + a, 2);
    return (uint32_t)(int32_t)(int16_t)v;
}

static inline uint32_t arm_ld8(ArmCpu *c, uint32_t addr)
{
    uint32_t a;
    if (arm_mem_traced || !arm_mem_flat(c, addr, 1u, &a))
        return arm_ld8_slow(c, addr);
    return (uint32_t)c->mem[a];
}

static inline uint32_t arm_ld8s(ArmCpu *c, uint32_t addr)
{
    uint32_t a;
    if (arm_mem_traced || !arm_mem_flat(c, addr, 1u, &a))
        return arm_ld8s_slow(c, addr);
    return (uint32_t)(int32_t)(int8_t)c->mem[a];
}

static inline void arm_st32(ArmCpu *c, uint32_t addr, uint32_t val)
{
    uint32_t a;
    if (arm_mem_traced || arm_excl_live || !arm_mem_flat(c, addr, 4u, &a))
        arm_st32_slow(c, addr, val);
    else
        memcpy(c->mem + a, &val, 4);
}

static inline void arm_st16(ArmCpu *c, uint32_t addr, uint32_t val)
{
    uint16_t v = (uint16_t)val;
    uint32_t a;
    if (arm_mem_traced || arm_excl_live || !arm_mem_flat(c, addr, 2u, &a))
        arm_st16_slow(c, addr, val);
    else
        memcpy(c->mem + a, &v, 2);
}

static inline void arm_st8(ArmCpu *c, uint32_t addr, uint32_t val)
{
    uint32_t a;
    if (arm_mem_traced || arm_excl_live || !arm_mem_flat(c, addr, 1u, &a))
        arm_st8_slow(c, addr, val);
    else
        c->mem[a] = (uint8_t)val;
}

/* Der Recompiler emittiert arm_ld(c, a, 4, 0) mit konstanter Groesse; das
 * Makro waehlt daraus die passende Inline-Variante (kein Laufzeit-Switch). */
#define arm_ld(c_, a_, size_, sgn_) \
    ((size_) == 4 ? arm_ld32((c_), (a_)) : \
     (size_) == 2 ? ((sgn_) ? arm_ld16s((c_), (a_)) : arm_ld16((c_), (a_))) : \
     ((sgn_) ? arm_ld8s((c_), (a_)) : arm_ld8((c_), (a_))))
#define arm_st(c_, a_, size_, v_) \
    ((size_) == 4 ? arm_st32((c_), (a_), (v_)) : \
     (size_) == 2 ? arm_st16((c_), (a_), (v_)) : arm_st8((c_), (a_), (v_)))

static inline int arm_cond(const ArmCpu *c, int cc)
{
    switch (cc) {
        case 0x0: return c->z;                     /* EQ */
        case 0x1: return !c->z;                    /* NE */
        case 0x2: return c->c;                     /* CS */
        case 0x3: return !c->c;                    /* CC */
        case 0x4: return c->n;                     /* MI */
        case 0x5: return !c->n;                    /* PL */
        case 0x6: return c->v;                     /* VS */
        case 0x7: return !c->v;                    /* VC */
        case 0x8: return c->c && !c->z;            /* HI */
        case 0x9: return !c->c || c->z;            /* LS */
        case 0xA: return c->n == c->v;             /* GE */
        case 0xB: return c->n != c->v;             /* LT */
        case 0xC: return !c->z && (c->n == c->v);  /* GT */
        case 0xD: return c->z || (c->n != c->v);   /* LE */
        default:  return 1;                        /* AL */
    }
}

uint32_t arm_shift_imm_slow(int type, uint32_t amount, uint32_t value, int *carry);

static inline uint32_t arm_shift_imm(int type, uint32_t amount, uint32_t value,
                                     int *carry)
{
    /* LSL ist mit Abstand am haeufigsten (Adressrechnung/Index). */
    if (type == SH_LSL) {
        if (amount == 0) return value;
        if (carry) *carry = (int)((value >> (32 - amount)) & 1u);
        return value << amount;
    }
    return arm_shift_imm_slow(type, amount, value, carry);
}

uint32_t arm_shift_reg(int type, uint32_t amount, uint32_t value, int *carry);
uint32_t arm_rrx(ArmCpu *c, uint32_t value, int *carry);

void arm_dp(ArmCpu *c, int op, int rd, uint32_t rn, uint32_t op2,
            int set_flags, int shifter_carry);

static inline uint32_t arm_mul(ArmCpu *c, uint32_t a, uint32_t b, int set_flags)
{
    uint32_t res = a * b;
    if (set_flags) {
        c->n = (res >> 31) & 1u;
        c->z = (res == 0) ? 1u : 0u;
    }
    return res;
}

void arm_mla(ArmCpu *c, int rd, uint32_t rn, uint32_t rm, uint32_t ra, int set_flags);
void arm_mls(ArmCpu *c, int rd, uint32_t rn, uint32_t rm, uint32_t ra);
void arm_mull(ArmCpu *c, int rdlo, int rdhi, uint32_t rn, uint32_t rm,
              int is_signed, int accumulate, int set_flags);


/* mode: 0=IA, 1=IB, 2=DA, 3=DB. Rueckgabe 1, wenn PC geladen wurde. */
int arm_ldm_stm(ArmCpu *c, int base, int load, int writeback, int mode, uint32_t mask);

void arm_svc(ArmCpu *c, uint32_t imm);
uint32_t arm_pool_alloc(unsigned size);
void arm_sp_check(ArmCpu *c, uint32_t sp0, uint32_t tgt);
void arm_restore_sp_slow(ArmCpu *c, uint32_t sp0);

/* ABI-Garantie: ein Guest-Aufruf darf SP nicht veraendern. Der Normalfall
 * (SP unveraendert) bleibt so ein Vergleich ohne Funktionsaufruf. */
static inline void arm_restore_sp(ArmCpu *c, uint32_t sp0)
{
    if (c->r[13] != sp0)
        arm_restore_sp_slow(c, sp0);
}

/* ---- Exklusiv-Monitor (LDREX/STREX/CLREX) ----
 * arm_excl_load() liest size Bytes und setzt die Reservierung.
 * arm_excl_store() schreibt nur, wenn die Reservierung noch gilt, und liefert
 * 0 (Erfolg) bzw. 1 (Fehlschlag, kein Speicherschreiben). Jede Schreiboperation
 * auf den ueberwachten Bereich sowie jeder Threadwechsel (arm_excl_clear_all)
 * macht die Reservierung ungueltig. */
uint32_t arm_excl_load(ArmCpu *c, uint32_t addr, int size);
uint32_t arm_excl_store(ArmCpu *c, uint32_t addr, int size, uint32_t value);
uint64_t arm_excl_load64(ArmCpu *c, uint32_t addr);
uint32_t arm_excl_store64(ArmCpu *c, uint32_t addr, uint32_t lo, uint32_t hi);
void arm_excl_clear(ArmCpu *c);
void arm_excl_clear_all(void);

/* Host-Handler fuer SVCs (Rueckgabewert landet in r0). */
typedef uint32_t (*ArmSvcHandler)(ArmCpu *c, uint32_t imm);
void arm_set_svc_handler(ArmSvcHandler handler);
void arm_unimplemented(ArmCpu *c, uint32_t pc, uint32_t insn, const char *mnem);

/* VFP */
float vfp_get_s(ArmCpu *c, int s);
void vfp_set_s(ArmCpu *c, int s, float v);
double vfp_get_d(ArmCpu *c, int d);
void vfp_set_d(ArmCpu *c, int d, double v);
void vfp_op(ArmCpu *c, int op, int dbl, int dest, int a, int b);
void vfp_muladd(ArmCpu *c, int dbl, int dest, int n, int m, int mode);
void vfp_cmp(ArmCpu *c, int dbl, int a, int b, int to_zero);
void vfp_set_fpscr(ArmCpu *c, uint32_t value);
void vfp_apply_rounding(uint32_t fpscr);
void vfp_cvt_f2i(ArmCpu *c, int src_dbl, int is_signed, int dst_s, int src);
void vfp_cvt_f2i_fixed(ArmCpu *c, int src_dbl, int is_signed, int dst_s, int src,
                       int fbits);
void vfp_cvt_i2f(ArmCpu *c, int dst_dbl, int is_signed, int dst, int src_s);
void vfp_cvt_i2f_fixed(ArmCpu *c, int dst_dbl, int is_signed, int dst, int src_s,
                       int fbits);
void vfp_cvt_fp(ArmCpu *c, int to_dbl, int dst, int from_dbl, int src);
void vfp_fpscr_to_flags(ArmCpu *c);
void vfp_flags_to_fpscr(ArmCpu *c);

/* ---- ARMv6 SIMD / DSP ---- */
enum {
    PAR_UADD = 0, PAR_SADD, PAR_USUB, PAR_SSUB,
    PAR_UQADD, PAR_SQADD, PAR_UQSUB, PAR_SQSUB,
    PAR_UHADD, PAR_SHADD, PAR_UHSUB, PAR_SHSUB
};
enum { QADD_ADD = 0, QADD_SUB, QADD_DADD, QADD_DSUB };

uint32_t arm_par8(ArmCpu *c, int op, uint32_t a, uint32_t b);
uint32_t arm_par16(ArmCpu *c, int op, uint32_t a, uint32_t b);
uint32_t arm_sel(ArmCpu *c, uint32_t a, uint32_t b);
uint32_t arm_ext16(int is_signed, uint32_t v, int rot8);
uint32_t arm_pack16(int keep_low, uint32_t a, uint32_t b, int shift, int asr);
uint32_t arm_sat16(ArmCpu *c, int is_signed, uint32_t v, int bits);
uint32_t arm_sat32(ArmCpu *c, int is_signed, uint32_t v, int bits);
uint32_t arm_usad(ArmCpu *c, uint32_t a, uint32_t b, int accumulate, uint32_t acc);
uint32_t arm_qadd32(ArmCpu *c, int op, uint32_t a, uint32_t b);
uint32_t arm_smla16(ArmCpu *c, uint32_t rn, uint32_t rm, uint32_t ra,
                    int ntop, int mtop, int accumulate);

/* ARMv6-DSP: SMMUL/SMMLA/SMMLS, SMUAD/SMUSD, SMLAD/SMLSD, SMLALD/SMLSLD,
 * UMAAL (Azahar dyncom, ARM ARM A8.8). */uint32_t arm_smmul(int mode, uint32_t a, uint32_t b, uint32_t acc);
uint32_t arm_dual16(ArmCpu *c, int sub, uint32_t a, uint32_t b, int exchange);
uint32_t arm_smlad(ArmCpu *c, int sub, uint32_t a, uint32_t b, uint32_t acc,
                   int exchange);
void arm_smlald(ArmCpu *c, int rdlo, int rdhi, int sub, uint32_t a, uint32_t b,
                int exchange);
void arm_umaal(ArmCpu *c, int rdlo, int rdhi, uint32_t a, uint32_t b);

#endif
