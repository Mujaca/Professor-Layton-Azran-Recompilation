#include "arm_cpu.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

void arm_cpu_init(ArmCpu *c, void *mem, uint32_t size) {
    memset(c, 0, sizeof(*c));
    c->mem = (uint8_t *)mem;
    c->mem_size = size;
}

/* ---------------- memory ---------------- */

static void mem_fault(ArmCpu *c, uint32_t addr, int size) {
    if (!c->aborted) {
        fprintf(stderr, "arm: access outside memory: 0x%08X (size %d)\n", addr, size);
    }
    c->aborted = 1;
}

uint32_t arm_mem_read(ArmCpu *c, uint32_t addr, int size) {
    uint32_t v = 0;
    if ((uint64_t)addr + (uint64_t)size > c->mem_size) {
        mem_fault(c, addr, size);
        return 0;
    }
    for (int i = size - 1; i >= 0; i--) {
        v = (v << 8) | c->mem[addr + i];
    }
    return v;
}

void arm_mem_write(ArmCpu *c, uint32_t addr, int size, uint32_t val) {
    if ((uint64_t)addr + (uint64_t)size > c->mem_size) {
        mem_fault(c, addr, size);
        return;
    }
    for (int i = 0; i < size; i++) {
        c->mem[addr + i] = (uint8_t)(val >> (8 * i));
    }
}

/* ---------------- conditions ---------------- */

int arm_cond(const ArmCpu *c, int cc) {
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

/* ---------------- shifts ---------------- */

uint32_t arm_shift_imm(int type, uint32_t amount, uint32_t value, int *carry) {
    switch (type) {
        case SH_LSL:
            if (amount == 0) return value;
             if (carry) *carry = (int)((value >> (32 - amount)) & 1);
            return value << amount;
        case SH_LSR:
            if (amount == 0) {                      /* LSR #32 */
                if (carry) *carry = (int)(value >> 31);
                return 0;
            }
            if (carry) *carry = (int)((value >> (amount - 1)) & 1);
            return value >> amount;
        case SH_ASR: {
            int32_t sv = (int32_t)value;
            if (amount == 0) {                      /* ASR #32 */
                if (carry) *carry = (int)((sv >> 31) & 1);
                return (uint32_t)(sv >> 31);
            }
            if (carry) *carry = (int)((sv >> (amount - 1)) & 1);
            return (uint32_t)(sv >> amount);
        }
        case SH_ROR: {
            if (amount == 0) {                      /* RRX */
                uint32_t res = (value >> 1) | ((carry && *carry) ? 0x80000000u : 0u);
                if (carry) *carry = (int)(value & 1);
                return res;
            }
            uint32_t n = amount & 31u;
            uint32_t res = (n == 0) ? value : ((value >> n) | (value << (32 - n)));
            if (carry) *carry = (int)(res >> 31);
            return res;
        }
        default:
            return value;
    }
}

uint32_t arm_shift_reg(int type, uint32_t amount, uint32_t value, int *carry) {
    uint32_t n = amount & 0xFFu;
    if (n == 0) return value;
    switch (type) {
        case SH_LSL:
            if (n < 32) {
                if (carry) *carry = (int)((value >> (32 - n)) & 1);
                return value << n;
            }
            if (carry) *carry = (n == 32) ? (int)(value & 1) : 0;
            return 0;
        case SH_LSR:
            if (n < 32) {
                if (carry) *carry = (int)((value >> (n - 1)) & 1);
                return value >> n;
            }
            if (carry) *carry = (n == 32) ? (int)(value >> 31) : 0;
            return 0;
        case SH_ASR: {
            int32_t sv = (int32_t)value;
            if (n < 32) {
                if (carry) *carry = (int)((sv >> (n - 1)) & 1);
                return (uint32_t)(sv >> n);
            }
            if (carry) *carry = (int)((sv >> 31) & 1);
            return (uint32_t)(sv >> 31);
        }
        case SH_ROR: {
            uint32_t nn = n & 31u;
            if (nn == 0) {
                if (carry) *carry = (int)(value >> 31);
                return value;
            }
            uint32_t res = (value >> nn) | (value << (32 - nn));
            if (carry) *carry = (int)(res >> 31);
            return res;
        }
        default:
            return value;
    }
}

/* ---------------- data processing ---------------- */

static void set_nz(ArmCpu *c, uint32_t res) {
    c->n = (res >> 31) & 1u;
    c->z = (res == 0) ? 1u : 0u;
}

void arm_dp(ArmCpu *c, int op, int rd, uint32_t rn, uint32_t op2,
            int set_flags, int shifter_carry) {
    uint32_t res = 0;
    int arithmetic = 0;

    switch (op) {
        case DP_AND: res = rn & op2; break;
        case DP_EOR: res = rn ^ op2; break;
        case DP_ORR: res = rn | op2; break;
        case DP_BIC: res = rn & ~op2; break;
        case DP_MOV: res = op2; break;
        case DP_MVN: res = ~op2; break;
        case DP_TST: res = rn & op2; break;
        case DP_TEQ: res = rn ^ op2; break;

        case DP_ADD: case DP_ADC: case DP_CMN:
        case DP_SUB: case DP_SBC: case DP_CMP:
        case DP_RSB: case DP_RSC: {
            uint32_t cin = (op == DP_ADC || op == DP_SBC || op == DP_RSC) ? c->c : 0u;
            int subtract = (op == DP_SUB || op == DP_SBC || op == DP_CMP ||
                            op == DP_RSB || op == DP_RSC);
            uint32_t a, b;
            arithmetic = 1;
            if (op == DP_RSB || op == DP_RSC) { a = op2; b = rn; }
            else { a = rn; b = op2; }

            if (!subtract) {
                uint64_t wide = (uint64_t)a + (uint64_t)b + (uint64_t)cin;
                res = (uint32_t)wide;
                if (set_flags) {
                    c->c = (uint32_t)((wide >> 32) & 1u);
                    c->v = (uint32_t)(((~(a ^ b) & (a ^ res)) >> 31) & 1u);
                }
            } else {
                uint64_t wide = (uint64_t)a - (uint64_t)b - (uint64_t)cin;
                res = (uint32_t)wide;
                if (set_flags) {
                    c->c = ((uint64_t)a >= ((uint64_t)b + (uint64_t)cin)) ? 1u : 0u;
                    c->v = (uint32_t)((((a ^ b) & (a ^ res)) >> 31) & 1u);
                }
            }
            break;
        }
        default:
            res = op2;
            break;
    }

    if (set_flags) {
        set_nz(c, res);
        if (!arithmetic && shifter_carry >= 0) {
            c->c = (uint32_t)shifter_carry;
        }
    }
    if (rd >= 0) {
        c->r[rd] = res;
    }
}

/* ---------------- multiply ---------------- */

uint32_t arm_mul(ArmCpu *c, uint32_t a, uint32_t b, int set_flags) {
    uint32_t res = a * b;
    if (set_flags) set_nz(c, res);
    return res;
}

void arm_mla(ArmCpu *c, int rd, uint32_t rn, uint32_t rm, uint32_t ra, int set_flags) {
    uint32_t res = rn * rm + ra;
    if (set_flags) set_nz(c, res);
    c->r[rd] = res;
}

void arm_mull(ArmCpu *c, int rdlo, int rdhi, uint32_t rn, uint32_t rm,
              int is_signed, int accumulate, int set_flags) {
    uint64_t res;
    if (is_signed) {
        int64_t r = (int64_t)(int32_t)rn * (int64_t)(int32_t)rm;
        if (accumulate) {
            uint64_t acc = ((uint64_t)c->r[rdhi] << 32) | (uint64_t)c->r[rdlo];
            r += (int64_t)acc;
        }
        res = (uint64_t)r;
    } else {
        uint64_t r = (uint64_t)rn * (uint64_t)rm;
        if (accumulate) {
            r += ((uint64_t)c->r[rdhi] << 32) | (uint64_t)c->r[rdlo];
        }
        res = r;
    }
    if (set_flags) {
        c->n = (uint32_t)((res >> 63) & 1u);
        c->z = (res == 0) ? 1u : 0u;
    }
    c->r[rdlo] = (uint32_t)res;
    c->r[rdhi] = (uint32_t)(res >> 32);
}

/* ---------------- load / store ---------------- */

uint32_t arm_ld(ArmCpu *c, uint32_t addr, int size, int is_signed) {
    uint32_t v = arm_mem_read(c, addr, size);
    if (is_signed) {
        if (size == 1) v = (uint32_t)(int32_t)(int8_t)v;
        else if (size == 2) v = (uint32_t)(int32_t)(int16_t)v;
    }
    return v;
}

void arm_st(ArmCpu *c, uint32_t addr, int size, uint32_t val) {
    arm_mem_write(c, addr, size, val);
}

int arm_ldm_stm(ArmCpu *c, int base, int load, int writeback, int mode, uint32_t mask) {
    uint32_t addr = c->r[base];
    int count = 0;
    for (int i = 0; i < 16; i++) if (mask & (1u << i)) count++;
    int total = 4 * count;

    if (mode == 1) addr += 4;
    else if (mode == 2) addr -= (uint32_t)(total - 4);
    else if (mode == 3) addr -= (uint32_t)total;

    uint32_t start = addr;
    int pc_loaded = 0;
    for (int i = 0; i < 16; i++) {
        if (!(mask & (1u << i))) continue;
        if (load) {
            c->r[i] = arm_mem_read(c, addr, 4);
            if (i == ARM_PC) pc_loaded = 1;
        } else {
            arm_mem_write(c, addr, 4, c->r[i]);
        }
        addr += 4;
    }
    if (writeback) {
        c->r[base] = start + (uint32_t)total;
    }
    return pc_loaded;
}

/* ---------------- svc / fallback ---------------- */

void arm_svc(ArmCpu *c, uint32_t imm) {
    (void)c; (void)imm;
}

void arm_unimplemented(ArmCpu *c, uint32_t pc, uint32_t insn, const char *mnem) {
    fprintf(stderr, "arm: not implemented: %s (pc 0x%08X, insn 0x%08X)\n", mnem, pc, insn);
    c->aborted = 1;
}

/* ---------------- VFP ---------------- */

float vfp_get_s(ArmCpu *c, int s) {
    float f;
    memcpy(&f, &c->sreg[s], 4);
    return f;
}

void vfp_set_s(ArmCpu *c, int s, float v) {
    memcpy(&c->sreg[s], &v, 4);
}

double vfp_get_d(ArmCpu *c, int d) {
    double v;
    uint32_t raw[2];
    raw[0] = c->sreg[d * 2];
    raw[1] = c->sreg[d * 2 + 1];
    memcpy(&v, raw, 8);
    return v;
}

void vfp_set_d(ArmCpu *c, int d, double v) {
    uint32_t raw[2];
    memcpy(raw, &v, 8);
    c->sreg[d * 2] = raw[0];
    c->sreg[d * 2 + 1] = raw[1];
}

void vfp_op(ArmCpu *c, int op, int dbl, int dest, int a, int b) {
    if (!dbl) {
        float x = vfp_get_s(c, a), y = vfp_get_s(c, b), r = 0.0f;
        switch (op) {
            case 0: r = x + y; break;
            case 1: r = x - y; break;
            case 2: r = x * y; break;
            case 3: r = x / y; break;
            case 4: r = -x; break;
            case 5: r = fabsf(x); break;
            case 6: r = sqrtf(x); break;
            default: r = x; break;
        }
        vfp_set_s(c, dest, r);
    } else {
        double x = vfp_get_d(c, a), y = vfp_get_d(c, b), r = 0.0;
        switch (op) {
            case 0: r = x + y; break;
            case 1: r = x - y; break;
            case 2: r = x * y; break;
            case 3: r = x / y; break;
            case 4: r = -x; break;
            case 5: r = fabs(x); break;
            case 6: r = sqrt(x); break;
            default: r = x; break;
        }
        vfp_set_d(c, dest, r);
    }
}

void vfp_cmp(ArmCpu *c, int dbl, int a, int b, int to_zero) {
    double x, y;
    uint32_t n = 0, z = 0, cc = 0, v = 0;
    if (!dbl) {
        x = (double)vfp_get_s(c, a);
        y = to_zero ? 0.0 : (double)vfp_get_s(c, b);
    } else {
        x = vfp_get_d(c, a);
        y = to_zero ? 0.0 : vfp_get_d(c, b);
    }
    if (isnan(x) || isnan(y)) { n = 0; z = 0; cc = 1; v = 1; }
    else if (x == y) { z = 1; }
    else if (x < y) { n = 1; }
    c->fpscr = (c->fpscr & 0x0FFFFFFFu) | (n << 31) | (z << 30) | (cc << 29) | (v << 28);
}

void vfp_fpscr_to_flags(ArmCpu *c) {
    c->n = (c->fpscr >> 31) & 1u;
    c->z = (c->fpscr >> 30) & 1u;
    c->c = (c->fpscr >> 29) & 1u;
    c->v = (c->fpscr >> 28) & 1u;
}

void vfp_flags_to_fpscr(ArmCpu *c) {
    c->fpscr = (c->fpscr & 0x0FFFFFFFu) | (c->n << 31) | (c->z << 30) | (c->c << 29) | (c->v << 28);
}
