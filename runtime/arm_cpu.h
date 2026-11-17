/* Minimal-Runtime fuer recompilierten ARM11-Code (MVP).
 * Zustand + Speicher + Hilfsfunktionen; keine Emulation der 3DS-Services. */
#ifndef ARM_CPU_H
#define ARM_CPU_H

#include <stdint.h>
#include <stddef.h>

#define ARM_SP 13
#define ARM_LR 14
#define ARM_PC 15

typedef struct ArmCpu {
    uint32_t r[16];
    /* APSR-Flags */
    uint32_t n, z, c, v;
    uint32_t t;              /* Thumb-Bit */
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

int arm_cond(const ArmCpu *c, int cc);

uint32_t arm_shift_imm(int type, uint32_t amount, uint32_t value, int *carry);
uint32_t arm_shift_reg(int type, uint32_t amount, uint32_t value, int *carry);

void arm_dp(ArmCpu *c, int op, int rd, uint32_t rn, uint32_t op2,
            int set_flags, int shifter_carry);

uint32_t arm_mul(ArmCpu *c, uint32_t a, uint32_t b, int set_flags);
void arm_mla(ArmCpu *c, int rd, uint32_t rn, uint32_t rm, uint32_t ra, int set_flags);
void arm_mull(ArmCpu *c, int rdlo, int rdhi, uint32_t rn, uint32_t rm,
              int is_signed, int accumulate, int set_flags);

uint32_t arm_ld(ArmCpu *c, uint32_t addr, int size, int is_signed);
void arm_st(ArmCpu *c, uint32_t addr, int size, uint32_t val);

/* mode: 0=IA, 1=IB, 2=DA, 3=DB. Rueckgabe 1, wenn PC geladen wurde. */
int arm_ldm_stm(ArmCpu *c, int base, int load, int writeback, int mode, uint32_t mask);

void arm_svc(ArmCpu *c, uint32_t imm);

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
void vfp_cmp(ArmCpu *c, int dbl, int a, int b, int to_zero);
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
uint32_t arm_usad(ArmCpu *c, uint32_t a, uint32_t b, int accumulate, uint32_t acc);
uint32_t arm_qadd32(ArmCpu *c, int op, uint32_t a, uint32_t b);
uint32_t arm_smla16(ArmCpu *c, uint32_t rn, uint32_t rm, uint32_t ra,
                    int ntop, int mtop, int accumulate);

#endif
