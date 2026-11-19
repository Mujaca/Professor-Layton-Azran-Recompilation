#include "arm_cpu.h"
#include "arm_trace.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static uint32_t g_memwatchrd = 0;
static uint32_t g_memwatchrd_end = 0;
static int g_memwatchrd_prints = 0;
static uint32_t g_memwatch = 0;
static uint32_t g_memwatch_end = 0;
static int g_memwatch_prints = 0;

static ArmCpu *g_pool_cpu = NULL;

void arm_cpu_init(ArmCpu *c, void *mem, uint32_t size) {
    memset(c, 0, sizeof(*c));
    c->mem = (uint8_t *)mem;
    c->mem_size = size;
    g_pool_cpu = c;
    {
        const char *mw = getenv("ARM_MEMWATCH");
        if (mw) g_memwatch = (uint32_t)strtoul(mw, NULL, 16);
        {
            const char *mrd = getenv("ARM_MEMWATCHRD");
            if (mrd) {
                unsigned long base = strtoul(mrd, NULL, 16);
                unsigned long size = 0x40;
                char *comma = strchr(mrd, ',');
                if (comma) size = strtoul(comma + 1, NULL, 16);
                g_memwatchrd = (uint32_t)base;
                g_memwatchrd_end = (uint32_t)(base + size);
            }
            const char *mr = getenv("ARM_MEMWATCHRANGE");
            if (mr) {
                unsigned long base = strtoul(mr, NULL, 16);
                unsigned long size = 0x600000;
                char *comma = strchr(mr, ',');
                if (comma) size = strtoul(comma + 1, NULL, 16);
                g_memwatch = (uint32_t)base;
                g_memwatch_end = (uint32_t)(base + size);
            }
        }
    }
}

/* ---------------- memory ---------------- */

static int g_fault_prints = 0;

static void mem_fault(ArmCpu *c, uint32_t addr, int size) {
    if (!c->aborted && g_fault_prints < 60) {
        g_fault_prints++;
        fprintf(stderr, "arm: access outside memory: 0x%08X (size %d) block=0x%08X\n",
                addr, size, arm_last_block);
        fprintf(stderr, "     r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X sp=%08X lr=%08X\n",
                c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7],
                c->r[13], c->r[14]);
    }
    /* Fehlzugriffe nicht fatal: Best-Effort-Modus (0 zurueckliefern) */
    if (getenv("ARM_STRICT")) {
        c->aborted = 1;
    }
}

uint32_t arm_mem_read(ArmCpu *c, uint32_t addr, int size) {
    uint32_t v = 0;
    static int s_irq_on = -1;
    if (s_irq_on < 0) s_irq_on = getenv("HLE_IRQ_FORCE") != NULL;
    /* Diagnose: das Relay-Flag 0x10002001 liest die Engine, der Gast
       ueberschreibt es aber selbst mit 0 -> konstant 1 liefern. */
    if (s_irq_on && ((addr & 0x1FFFFFFFu) == 0x10002001u && size == 1))
        return 1;
    /* 3DS-Busalias: 0x8xxxxxxx/0xBxxxxxxx zeigen auf dieselben FCRAM-Seiten. */
    if (addr >= 0x20000000u) addr &= 0x1FFFFFFFu;
    if (g_memwatchrd != 0 && addr >= g_memwatchrd && addr < g_memwatchrd_end &&
        g_memwatchrd_prints < 200) {
        g_memwatchrd_prints++;
        fprintf(stderr, "[mrd] %08X size=%d blk=%08X lr=%08X\n",
                addr, size, arm_last_block, c->r[14]);
    }
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
    /* Env-Lookups einmalig cachen: getenv() pro Speicherzugriff war der
       groesste Bremsklotz (der Gast kopiert Daten in den Linear-Heap). */
    static int s_init = 0, s_mwval_on = 0, s_keep = 1, s_reg = 0, s_fsobj = 1;
    static uint32_t s_want = 0;
    if (!s_init) {
        const char *v;
        s_init = 1;
        v = getenv("HLE_MEMWATCHVAL");
        if (v != NULL && *v != 0) { s_mwval_on = 1; s_want = (uint32_t)strtoul(v, NULL, 16); }
        v = getenv("HLE_KEEP_SLOT");
        s_keep = !(v != NULL && v[0] == '0');
        s_reg = getenv("HLE_REG_ENABLE") != NULL;
        v = getenv("HLE_FS_OBJ");
        s_fsobj = !(v != NULL && v[0] == '0');
    }
    if (addr >= 0x20000000u) addr &= 0x1FFFFFFFu;
    if (s_mwval_on && size == 4 && val == s_want) {
        printf("[mwval] %08X <- %08X pc=%08X\n", addr, val, arm_last_block);
        fflush(stdout);
    }
    if (size == 4) {
        if (s_keep && val == 0 && (addr == 0x3D67C4u || addr == 0x3D66C4u))
            val = 1;
        if (s_reg && addr == 0x3ECA5Cu && val == 0)
            val = 0x00401000u;
        if (s_fsobj && addr == 0x3D8D8Cu && val == 0)
            val = 0x00408000u;
    }
    if (g_memwatch != 0 &&
        ((g_memwatch_end != 0) ? (addr >= g_memwatch && addr < g_memwatch_end)
                               : ((addr & ~3u) == g_memwatch)) &&
        g_memwatch_prints < 400) {
        g_memwatch_prints++;
        fprintf(stderr, "[mw] %08X <- %08X size=%d pc=%08X lr=%08X\n",
                addr, val, size, arm_last_block, c->r[14]);
    }
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
        /* IA: +4n, IB: start-4+total, DA: start-4, DB: start */
        if (mode == 3) {
            c->r[base] = start;
        } else if (mode == 2) {
            c->r[base] = start - 4u;
        } else if (mode == 1) {
            c->r[base] = start - 4u + (uint32_t)total;
        } else {
            c->r[base] = start + (uint32_t)total;
        }
    }
    return pc_loaded;
}

/* ---------------- svc / fallback ---------------- */

/* Bump-Allocator fuer SDK-Objekte im Gast-Adressraum (Ersatz fuer leere SDK-Pools).
 * Region 0x1E000000..0x1E400000 liegt zwischen Linear-Heap und VRAM und wird
 * von Spiel/HLE sonst nicht benutzt. */
#define ARM_POOL_BASE 0x1E000000u
#define ARM_POOL_SIZE 0x00400000u
static uint32_t g_pool_used = 0;

uint32_t arm_pool_alloc(unsigned size) {
    size = (size + 15u) & ~15u;
    if (size == 0) size = 16;
    if (g_pool_cpu == NULL) return 0;
    if (g_pool_used + size > ARM_POOL_SIZE) return 0;
    uint32_t addr = ARM_POOL_BASE + g_pool_used;
    g_pool_used += size;
    memset(g_pool_cpu->mem + addr, 0, size);
    return addr;
}

/* Null-initialisierte BSS-Semaphore beim ersten Warten als frei behandeln. */
void arm_mutex_bootstrap(ArmCpu *c, uint32_t addr) {
    uint32_t a = addr & 0x1FFFFFFFu;
    if (a + 4 > c->mem_size) return;
    uint32_t v;
    memcpy(&v, c->mem + a, 4);
    if (v == 0) {
        v = 1;
        memcpy(c->mem + a, &v, 4);
    }
}

static ArmSvcHandler g_svc_handler = NULL;

void arm_set_svc_handler(ArmSvcHandler handler) {
    g_svc_handler = handler;
}

void arm_svc(ArmCpu *c, uint32_t imm) {
    arm_trace_svc(imm);
    if (g_svc_handler != NULL) {
        c->r[0] = g_svc_handler(c, imm);
    }
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
    c->fpscr = (c->fpscr & 0x0FFFFFFFu) | (c->n << 31) | (c->z << 30)
             | (c->c << 29) | (c->v << 28);
}

/* ---------------- ARMv6 SIMD / DSP ---------------- */

static int32_t sat_range(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

uint32_t arm_par8(ArmCpu *c, int op, uint32_t a, uint32_t b) {
    uint32_t res = 0, ge = 0;
    for (int i = 0; i < 4; i++) {
        int ab = (int)((a >> (8 * i)) & 0xFFu);
        int bb = (int)((b >> (8 * i)) & 0xFFu);
        int r = 0;
        switch (op) {
            case PAR_UADD: { int t = ab + bb; ge |= (uint32_t)(t > 0xFF) << i; r = t; break; }
            case PAR_USUB: { ge |= (uint32_t)(ab >= bb) << i; r = ab - bb; break; }
            case PAR_SADD: { int t = (int8_t)ab + (int8_t)bb;
                             ge |= (uint32_t)(t >= -128 && t <= 127) << i; r = t; break; }
            case PAR_SSUB: { int t = (int8_t)ab - (int8_t)bb;
                             ge |= (uint32_t)(t >= -128 && t <= 127) << i; r = t; break; }
            case PAR_UQADD: r = sat_range(ab + bb, 0, 255); break;
            case PAR_SQADD: r = sat_range((int8_t)ab + (int8_t)bb, -128, 127); break;
            case PAR_UQSUB: r = sat_range(ab - bb, 0, 255); break;
            case PAR_SQSUB: r = sat_range((int8_t)ab - (int8_t)bb, -128, 127); break;
            case PAR_UHADD: r = (ab + bb) >> 1; break;
            case PAR_SHADD: r = ((int8_t)ab + (int8_t)bb) >> 1; break;
            case PAR_UHSUB: r = (ab - bb) > 0 ? ((ab - bb) >> 1) : 0; break;
            case PAR_SHSUB: r = ((int8_t)ab - (int8_t)bb) >> 1; break;
            default: r = 0; break;
        }
        res |= (uint32_t)(r & 0xFF) << (8 * i);
    }
    if (op <= PAR_SSUB) c->ge = ge;
    return res;
}

uint32_t arm_par16(ArmCpu *c, int op, uint32_t a, uint32_t b) {
    uint32_t res = 0, ge = 0;
    for (int i = 0; i < 2; i++) {
        int ab = (int)((a >> (16 * i)) & 0xFFFFu);
        int bb = (int)((b >> (16 * i)) & 0xFFFFu);
        int r = 0;
        switch (op) {
            case PAR_UADD: { int t = ab + bb; ge |= (uint32_t)(t > 0xFFFF) << i; r = t; break; }
            case PAR_USUB: { ge |= (uint32_t)(ab >= bb) << i; r = ab - bb; break; }
            case PAR_SADD: { int t = (int16_t)ab + (int16_t)bb;
                             ge |= (uint32_t)(t >= -32768 && t <= 32767) << i; r = t; break; }
            case PAR_SSUB: { int t = (int16_t)ab - (int16_t)bb;
                             ge |= (uint32_t)(t >= -32768 && t <= 32767) << i; r = t; break; }
            case PAR_UQADD: r = sat_range(ab + bb, 0, 0xFFFF); break;
            case PAR_SQADD: r = sat_range((int16_t)ab + (int16_t)bb, -32768, 32767); break;
            case PAR_UQSUB: r = sat_range(ab - bb, 0, 0xFFFF); break;
            case PAR_SQSUB: r = sat_range((int16_t)ab - (int16_t)bb, -32768, 32767); break;
            case PAR_UHADD: r = (ab + bb) >> 1; break;
            case PAR_SHADD: r = ((int16_t)ab + (int16_t)bb) >> 1; break;
            case PAR_UHSUB: r = (ab - bb) > 0 ? ((ab - bb) >> 1) : 0; break;
            case PAR_SHSUB: r = ((int16_t)ab - (int16_t)bb) >> 1; break;
            default: r = 0; break;
        }
        res |= (uint32_t)(r & 0xFFFF) << (16 * i);
    }
    if (op <= PAR_SSUB) c->ge = ge;
    return res;
}

uint32_t arm_sel(ArmCpu *c, uint32_t a, uint32_t b) {
    uint32_t res = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t byte = ((c->ge >> i) & 1u) ? ((a >> (8 * i)) & 0xFFu)
                                            : ((b >> (8 * i)) & 0xFFu);
        res |= byte << (8 * i);
    }
    return res;
}

static uint32_t ror32(uint32_t v, int n) {
    n &= 31;
    if (n == 0) return v;
    return (v >> n) | (v << (32 - n));
}

uint32_t arm_ext16(int is_signed, uint32_t v, int rot8) {
    uint32_t x = ror32(v, rot8 * 8);
    uint32_t lo = x & 0xFFu;
    uint32_t hi = (x >> 16) & 0xFFu;
    if (is_signed) {
        return ((uint32_t)(int32_t)(int8_t)lo & 0xFFFFu)
             | ((uint32_t)(int32_t)(int8_t)hi << 16);
    }
    return lo | (hi << 16);
}

uint32_t arm_pack16(int keep_low, uint32_t a, uint32_t b, int shift, int asr) {
    (void)asr;
    if (shift == 0) {
        shift = 32;
    }
    if (keep_low) {
        /* PKHBT: rd = rn[15:0] | (rm << shift)[31:16] */
        uint32_t hi = (shift >= 32) ? 0u : (b << shift);
        return (a & 0xFFFFu) | (hi & 0xFFFF0000u);
    }
    /* PKHTB: rd = rn[31:16] | (rm ASR shift)[15:0] */
    uint32_t lo = (shift >= 32) ? 0u : (uint32_t)((int32_t)b >> shift);
    return (a & 0xFFFF0000u) | (lo & 0xFFFFu);
}

uint32_t arm_sat16(ArmCpu *c, int is_signed, uint32_t v, int bits) {
    uint32_t res = 0;
    for (int i = 0; i < 2; i++) {
        int32_t h = (int16_t)((v >> (16 * i)) & 0xFFFFu);
        int32_t lo = is_signed ? -(1 << (bits - 1)) : 0;
        int32_t hi = is_signed ? ((1 << (bits - 1)) - 1) : ((1 << bits) - 1);
        int32_t s = sat_range(h, lo, hi);
        if (s != h) {
            c->qflag = 1;
        }
        res |= ((uint32_t)s & 0xFFFFu) << (16 * i);
    }
    return res;
}

uint32_t arm_usad(ArmCpu *c, uint32_t a, uint32_t b, int accumulate, uint32_t acc) {
    (void)c;
    uint32_t sum = accumulate ? acc : 0;
    for (int i = 0; i < 4; i++) {
        int x = (int)((a >> (8 * i)) & 0xFFu);
        int y = (int)((b >> (8 * i)) & 0xFFu);
        sum += (uint32_t)(x > y ? x - y : y - x);
    }
    return sum;
}

uint32_t arm_qadd32(ArmCpu *c, int op, uint32_t a, uint32_t b) {
    int64_t x = (int64_t)(int32_t)a;
    int64_t y = (int64_t)(int32_t)b;
    int64_t r;
    switch (op) {
        case QADD_ADD: r = x + y; break;
        case QADD_SUB: r = x - y; break;
        case QADD_DADD: r = x + 2 * y; break;
        default: r = x - 2 * y; break;
    }
    if (r > 2147483647LL) { r = 2147483647LL; c->qflag = 1; }
    if (r < -2147483648LL) { r = -2147483648LL; c->qflag = 1; }
    return (uint32_t)(int32_t)r;
}

uint32_t arm_smla16(ArmCpu *c, uint32_t rn, uint32_t rm, uint32_t ra,
                    int ntop, int mtop, int accumulate) {
    int32_t x = (int16_t)(ntop ? (rn >> 16) : rn);
    int32_t y = (int16_t)(mtop ? (rm >> 16) : rm);
    int64_t prod = (int64_t)x * (int64_t)y;
    if (!accumulate) {
        if (prod > 2147483647LL || prod < -2147483648LL) {
            prod = (prod > 0) ? 2147483647LL : -2147483648LL;
            c->qflag = 1;
        }
        return (uint32_t)(int32_t)prod;
    }
    int64_t sum = prod + (int64_t)(int32_t)ra;
    if (sum > 2147483647LL || sum < -2147483648LL) {
        sum = (sum > 0) ? 2147483647LL : -2147483648LL;
        c->qflag = 1;
    }
    return (uint32_t)(int32_t)sum;
}


void arm_sp_check(ArmCpu *c, uint32_t sp0, uint32_t tgt) {
    if (c->r[13] != sp0) {
        static int n = 0;
        if (n++ < 25)
            fprintf(stderr, "[sp] call 0x%08X changed sp %08X -> %08X (lr=%08X)\n",
                    tgt, sp0, c->r[13], c->r[14]);
        /* ABI-Garantie erzwingen: ein Guest-Aufruf darf SP nicht veraendern. */
        c->r[13] = sp0;
    }
}

/* ABI-Garantie: ein Guest-Aufruf darf SP nicht veraendern. Falls doch
 * (Recompiler-Bug), SP wiederherstellen und einmalig melden. */
void arm_restore_sp(ArmCpu *c, uint32_t sp0) {
    if (c->r[13] != sp0) {
        static int n = 0;
        if (n++ < 40)
            fprintf(stderr, "[sp] 0x%08X: SP %08X -> %08X (korrigiert)\n",
                    arm_last_block, sp0, c->r[13]);
        c->r[13] = sp0;
    }
}
