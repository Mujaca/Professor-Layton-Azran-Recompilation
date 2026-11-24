#include "arm_cpu.h"
#include "arm_trace.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <fenv.h>

static uint32_t g_memwatchrd = 0;
static uint32_t g_memwatchrd_end = 0;
static int g_memwatchrd_prints = 0;
static uint32_t g_memwatch = 0;
static uint32_t g_memwatch_end = 0;
static int g_memwatch_prints = 0;

ArmCpu *g_pool_cpu = NULL;   /* fuer runtime/workarounds.c */

void arm_cpu_init(ArmCpu *c, void *mem, uint32_t size) {
    memset(c, 0, sizeof(*c));
    c->mem = (uint8_t *)mem;
    c->mem_size = size;
    /* 3DS/Azahar-Thread-Kontext: Default-NaN, Flush-to-Zero, Rundung Richtung
     * Null, Inexact-Cumulative. Das steuert VFP-Ergebnisse und Vergleiche. */
    c->fpscr = 0x03C00010u;
    vfp_apply_rounding(c->fpscr);
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
    /* 3DS-Busalias: 0x8xxxxxxx/0xBxxxxxxx zeigen auf dieselben FCRAM-Seiten. */
    if (addr >= 0x20000000u) addr &= 0x1FFFFFFFu;
    if (g_memwatchrd != 0 && addr >= g_memwatchrd && addr < g_memwatchrd_end &&
        g_memwatchrd_prints < 200) {
        g_memwatchrd_prints++;
        fprintf(stderr, "[mrd] %08X size=%d blk=%08X lr=%08X th=%X\n",
                addr, size, arm_last_block, c->r[14],
                (unsigned)((c->tpidruro >> 12) & 0xFu));
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

/* ---------------- exclusive monitor ---------------- */

/* Globaler Monitor: pro (Adresse, Groesse) ein Slot mit Generationszaehler.
 * Ein LDREX merkt sich (Slot, Generation) im ArmCpu; jeder Store auf den
 * ueberwachten Bereich, ein CLREX oder ein Threadwechsel erhoeht die
 * Generation bzw. entwertet den Slot. STREX gelingt nur, wenn die gemerkte
 * Generation noch aktuell ist. Damit kann ein STREX nach einem Threadwechsel
 * nicht mehr den Wert eines anderen Threads ueberschreiben. */
#define ARM_EXCL_SLOTS 8
typedef struct {
    int used;
    uint32_t addr;
    uint32_t size;
    uint32_t gen;
} ArmExclSlot;
static ArmExclSlot g_excl[ARM_EXCL_SLOTS];
static unsigned g_excl_rr = 0;
static volatile int g_excl_active = 0;
static void arm_excl_invalidate(uint32_t addr, uint32_t size);

void arm_mem_write(ArmCpu *c, uint32_t addr, int size, uint32_t val) {
    if (addr >= 0x20000000u) addr &= 0x1FFFFFFFu;
    /* Jede Schreiboperation auf einen ueberwachten Bereich macht die
     * Reservierung ungueltig (ARMv6K: lokaler Monitor wird von einem
     * Store auf die ueberwachte Adresse geloescht). */
    if (g_excl_active) arm_excl_invalidate(addr, (uint32_t)size);
    if (g_memwatch != 0 &&
        ((g_memwatch_end != 0) ? (addr >= g_memwatch && addr < g_memwatch_end)
                               : ((addr & ~3u) == g_memwatch)) &&
        g_memwatch_prints < 400) {
        g_memwatch_prints++;
        fprintf(stderr, "[mw] %08X <- %08X size=%d pc=%08X lr=%08X th=%X\n",
                addr, val, size, arm_last_block, c->r[14],
                (unsigned)((c->tpidruro >> 12) & 0xFu));
    }
    if ((uint64_t)addr + (uint64_t)size > c->mem_size) {
        mem_fault(c, addr, size);
        return;
    }
    for (int i = 0; i < size; i++) {
        c->mem[addr + i] = (uint8_t)(val >> (8 * i));
    }
}

/* ---- Exklusiv-Monitor: siehe Kommentar oben ---- */

static uint32_t excl_norm(uint32_t addr) {
    return (addr >= 0x20000000u) ? (addr & 0x1FFFFFFFu) : addr;
}

static void arm_excl_invalidate(uint32_t addr, uint32_t size) {
    uint32_t end;
    if (size == 0) return;
    end = addr + size;
    for (int i = 0; i < ARM_EXCL_SLOTS; i++) {
        if (!g_excl[i].used) continue;
        /* Byte-Bereiche ueberlappen sich? */
        if (addr < g_excl[i].addr + g_excl[i].size && g_excl[i].addr < end) {
            g_excl[i].used = 0;
            if (g_excl_active > 0) g_excl_active--;
        }
    }
}

static void excl_reserve(ArmCpu *c, uint32_t addr, uint32_t size) {
    int slot = -1;
    c->excl_valid = 0;
    for (int i = 0; i < ARM_EXCL_SLOTS; i++) {
        if (g_excl[i].used && g_excl[i].addr == addr &&
            g_excl[i].size == size) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        int free_slot = -1;
        for (int i = 0; i < ARM_EXCL_SLOTS; i++) {
            if (!g_excl[i].used) { free_slot = i; break; }
        }
        slot = (free_slot >= 0) ? free_slot
                                : (int)(g_excl_rr++ % ARM_EXCL_SLOTS);
        if (!g_excl[slot].used) g_excl_active++;
        g_excl[slot].used = 1;
        g_excl[slot].addr = addr;
        g_excl[slot].size = size;
    }
    /* Jede neue Reservierung entwertet eine evtl. vorhandene alte auf
     * derselben Adresse (globaler Monitor, nur ein Exklusivzugriff). */
    g_excl[slot].gen++;
    c->excl_valid = 1;
    c->excl_slot = slot;
    c->excl_addr = addr;
    c->excl_size = size;
    c->excl_gen = g_excl[slot].gen;
}

uint32_t arm_excl_load(ArmCpu *c, uint32_t addr, int size) {
    uint32_t value;
    addr = excl_norm(addr);
    value = arm_mem_read(c, addr, size);
    excl_reserve(c, addr, (uint32_t)size);
    return value;
}

uint64_t arm_excl_load64(ArmCpu *c, uint32_t addr) {
    /* LDREXD: zwei Worte; die Reservierung gilt fuer den 8-Byte-Bereich. */
    uint64_t lo, hi;
    addr = excl_norm(addr);
    lo = arm_mem_read(c, addr, 4);
    hi = arm_mem_read(c, addr + 4u, 4);
    excl_reserve(c, addr, 8u);
    return lo | (hi << 32);
}

uint32_t arm_excl_store(ArmCpu *c, uint32_t addr, int size, uint32_t value) {
    int ok = 0;
    addr = excl_norm(addr);
    if (c->excl_valid && c->excl_addr == addr &&
        c->excl_size == (uint32_t)size &&
        c->excl_slot >= 0 && c->excl_slot < ARM_EXCL_SLOTS) {
        ArmExclSlot *s = &g_excl[c->excl_slot];
        if (s->used && s->gen == c->excl_gen && s->addr == addr &&
            s->size == (uint32_t)size) {
            s->used = 0;
            if (g_excl_active > 0) g_excl_active--;
            arm_mem_write(c, addr, size, value);
            ok = 1;
        }
    }
    /* STREX loescht den lokalen Monitor immer (ARM ARM, STREX). */
    c->excl_valid = 0;
    return ok ? 0u : 1u;
}

uint32_t arm_excl_store64(ArmCpu *c, uint32_t addr, uint32_t lo, uint32_t hi) {
    int ok = 0;
    addr = excl_norm(addr);
    if (c->excl_valid && c->excl_addr == addr && c->excl_size == 8u &&
        c->excl_slot >= 0 && c->excl_slot < ARM_EXCL_SLOTS) {
        ArmExclSlot *s = &g_excl[c->excl_slot];
        if (s->used && s->gen == c->excl_gen && s->addr == addr &&
            s->size == 8u) {
            s->used = 0;
            if (g_excl_active > 0) g_excl_active--;
            arm_mem_write(c, addr, 4, lo);
            arm_mem_write(c, addr + 4u, 4, hi);
            ok = 1;
        }
    }
    c->excl_valid = 0;
    return ok ? 0u : 1u;
}

void arm_excl_clear(ArmCpu *c) {
    if (c->excl_valid && c->excl_slot >= 0 && c->excl_slot < ARM_EXCL_SLOTS) {
        ArmExclSlot *s = &g_excl[c->excl_slot];
        if (s->used && s->addr == c->excl_addr && s->size == c->excl_size) {
            s->used = 0;
            if (g_excl_active > 0) g_excl_active--;
        }
    }
    c->excl_valid = 0;
}

void arm_excl_clear_all(void) {
    for (int i = 0; i < ARM_EXCL_SLOTS; i++) g_excl[i].used = 0;
    g_excl_active = 0;
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
            if (amount == 0 || amount >= 32) {      /* LSR #32 */
                if (carry) *carry = (int)(value >> 31);
                return 0;
            }
            if (carry) *carry = (int)((value >> (amount - 1)) & 1);
            return value >> amount;
        case SH_ASR: {
            int32_t sv = (int32_t)value;
            if (amount == 0 || amount >= 32) {      /* ASR #32 */
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

/* RRX braucht das eingehende C-Flag; der Shifter-Carry ist ein
 * Ausgabeparameter, daher eigener Helfer (nicht arm_shift_imm mit *carry=-1). */
uint32_t arm_rrx(ArmCpu *c, uint32_t value, int *carry) {
    uint32_t res = (value >> 1) | (c->c ? 0x80000000u : 0u);
    if (carry) *carry = (int)(value & 1u);
    return res;
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
            /* ARM: ADC rechnet +C, SBC/RSC dagegen -NOT(C) (Borrow = !C).
             * Das C-Flag ist danach "kein Borrow" (a >= b + cin). */
            uint32_t cin = 0u;
            if (op == DP_ADC) cin = c->c;
            else if (op == DP_SBC || op == DP_RSC) cin = c->c ? 0u : 1u;
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
            if (i == ARM_PC) {
                /* Laden von PC ist ein Interworking-Sprung: Bit 0 waehlt den
                 * Befehlssatz (ARM ARM A8.8.107). */
                c->t = c->r[15] & 1u;
                c->r[15] &= ~1u;
                pc_loaded = 1;
            }
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

static ArmSvcHandler g_svc_handler = NULL;

void arm_set_svc_handler(ArmSvcHandler handler) {
    g_svc_handler = handler;
}

void arm_svc(ArmCpu *c, uint32_t imm) {
    /* Exception-Eintritt loescht den lokalen Exklusiv-Monitor (ARMv6K). */
    arm_excl_clear(c);
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

/* FPSCR-Bits, die das Rechenverhalten steuern (Azahar/ARM ARM). Der 3DS-
 * Thread-Kontext startet mit DEFAULT_NAN | FLUSH_TO_ZERO | ROUND_TOZERO | IXC. */
#define VFP_FPSCR_NZCV       0xF0000000u
#define VFP_FPSCR_DN         (1u << 25)
#define VFP_FPSCR_FZ         (1u << 24)
#define VFP_FPSCR_RMODE_MASK (3u << 22)
#define VFP_DEFAULT_FPSCR    0x03C00010u

/* Die Host-Rundung wird pro Thread einmal gesetzt; die FPSCR-Rundung ist
 * Gastzustand und kann sich per VMSR aendern. */
static _Thread_local int g_vfp_fenv_rmode = -1;

void vfp_apply_rounding(uint32_t fpscr) {
    int mode = (int)((fpscr >> 22) & 3u);
    if (g_vfp_fenv_rmode == mode)
        return;
    switch (mode) {
        case 0: fesetround(FE_TONEAREST); break;
        case 1: fesetround(FE_UPWARD); break;
        case 2: fesetround(FE_DOWNWARD); break;
        default: fesetround(FE_TOWARDZERO); break;
    }
    g_vfp_fenv_rmode = mode;
}

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

/* FTZ: denormalisierte Ergebnisse werden zu Null mit erhaltenem Vorzeichen. */
static float vfp_finish_s(ArmCpu *c, float r) {
    uint32_t b;
    memcpy(&b, &r, 4);
    if ((c->fpscr & VFP_FPSCR_FZ) && (b & 0x7F800000u) == 0u &&
        (b & 0x007FFFFFu) != 0u)
        b &= 0x80000000u;
    if ((c->fpscr & VFP_FPSCR_DN) && (b & 0x7F800000u) == 0x7F800000u &&
        (b & 0x007FFFFFu) != 0u)
        b = 0x7FC00000u;
    memcpy(&r, &b, 4);
    return r;
}

static double vfp_finish_d(ArmCpu *c, double r) {
    uint64_t b;
    memcpy(&b, &r, 8);
    if ((c->fpscr & VFP_FPSCR_FZ) && (b & 0x7FF0000000000000ull) == 0ull &&
        (b & 0x000FFFFFFFFFFFFFull) != 0ull)
        b &= 0x8000000000000000ull;
    if ((c->fpscr & VFP_FPSCR_DN) && (b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
        (b & 0x000FFFFFFFFFFFFFull) != 0ull)
        b = 0x7FF8000000000000ull;
    memcpy(&r, &b, 8);
    return r;
}

/* Denormalisierte Eingaben werden bei FTZ als Null (mit Vorzeichen) gelesen. */
static float vfp_load_s(ArmCpu *c, int s) {
    float f = vfp_get_s(c, s);
    if (c->fpscr & VFP_FPSCR_FZ) {
        uint32_t b;
        memcpy(&b, &f, 4);
        if ((b & 0x7F800000u) == 0u && (b & 0x007FFFFFu) != 0u) {
            b &= 0x80000000u;
            memcpy(&f, &b, 4);
        }
    }
    return f;
}

static double vfp_load_d(ArmCpu *c, int d) {
    double v = vfp_get_d(c, d);
    if (c->fpscr & VFP_FPSCR_FZ) {
        uint64_t b;
        memcpy(&b, &v, 8);
        if ((b & 0x7FF0000000000000ull) == 0ull &&
            (b & 0x000FFFFFFFFFFFFFull) != 0ull) {
            b &= 0x8000000000000000ull;
            memcpy(&v, &b, 8);
        }
    }
    return v;
}

/* Getrennte Rundung fuer VMLA/VMLS/VNMUL: VFPv2 (ARM11) rundet das Produkt
 * vor der Addition (Unicorn/Azahar verhalten sich ebenso). volatile
 * verhindert, dass der Compiler eine FMA-Kontraktion erzeugt. */
static float vfp_mul_s(float x, float y) { volatile float r = x * y; return r; }
static float vfp_add_s(float x, float y) { volatile float r = x + y; return r; }
static double vfp_mul_d(double x, double y) { volatile double r = x * y; return r; }
static double vfp_add_d(double x, double y) { volatile double r = x + y; return r; }

/* Bei ungueltiger Operation (0/0, inf-inf, sqrt(negativ) ...) liefert die
 * VFP-Hardware/Unicorn den positiven Default-qNaN, auch ohne FPSCR.DN. C
 * erzeugt dagegen je nach Operation ein negatives NaN. */
static float vfp_invalid_nan_s(void) {
    float r;
    uint32_t b = 0x7FC00000u;
    memcpy(&r, &b, 4);
    return r;
}

static double vfp_invalid_nan_d(void) {
    double r;
    uint64_t b = 0x7FF8000000000000ull;
    memcpy(&r, &b, 8);
    return r;
}

void vfp_op(ArmCpu *c, int op, int dbl, int dest, int a, int b) {
    if (!dbl) {
        float x = vfp_load_s(c, a), y = vfp_load_s(c, b), r = 0.0f;
        int in_nan = isnan(x) || isnan(y);
        switch (op) {
            case 0: r = x + y; break;
            case 1: r = x - y; break;
            case 2: r = vfp_mul_s(x, y); break;
            case 3: r = x / y; break;
            /* VNEG/VABS sind Bitoperationen: kein Flush, keine NaN-Kanonisierung. */
            case 4: {
                uint32_t bx, br;
                bx = c->sreg[a];
                br = bx ^ 0x80000000u;
                memcpy(&r, &br, 4);
                vfp_set_s(c, dest, r);
                return;
            }
            case 5: {
                uint32_t bx, br;
                bx = c->sreg[a];
                br = bx & 0x7FFFFFFFu;
                memcpy(&r, &br, 4);
                vfp_set_s(c, dest, r);
                return;
            }
            case 6: r = sqrtf(x); in_nan = isnan(x); break;
            default: r = x; break;
        }
        if (!in_nan && isnan(r))
            r = vfp_invalid_nan_s();
        vfp_set_s(c, dest, vfp_finish_s(c, r));
    } else {
        double x = vfp_load_d(c, a), y = vfp_load_d(c, b), r = 0.0;
        int in_nan = isnan(x) || isnan(y);
        switch (op) {
            case 0: r = x + y; break;
            case 1: r = x - y; break;
            case 2: r = vfp_mul_d(x, y); break;
            case 3: r = x / y; break;
            case 4: {
                uint64_t bx, br;
                bx = ((uint64_t)c->sreg[a * 2 + 1] << 32) | c->sreg[a * 2];
                br = bx ^ 0x8000000000000000ull;
                memcpy(&r, &br, 8);
                vfp_set_d(c, dest, r);
                return;
            }
            case 5: {
                uint64_t bx, br;
                bx = ((uint64_t)c->sreg[a * 2 + 1] << 32) | c->sreg[a * 2];
                br = bx & 0x7FFFFFFFFFFFFFFFull;
                memcpy(&r, &br, 8);
                vfp_set_d(c, dest, r);
                return;
            }
            case 6: r = sqrt(x); in_nan = isnan(x); break;
            default: r = x; break;
        }
        if (!in_nan && isnan(r))
            r = vfp_invalid_nan_d();
        vfp_set_d(c, dest, vfp_finish_d(c, r));
    }
}

/* VMLA/VMLS/VNMLA/VNMLS/VNMUL. mode: 0=add, 1=sub, 2=negmul,
 * 3=neg(addend)+(-prod) (VNMLA), 4=neg(addend)+prod (VNMLS). */
void vfp_muladd(ArmCpu *c, int dbl, int dest, int n, int m, int mode) {
    if (!dbl) {
        float xn = vfp_load_s(c, n), xm = vfp_load_s(c, m);
        float prod = vfp_finish_s(c, vfp_mul_s(xn, xm));
        float acc = vfp_load_s(c, dest), r;
        int in_nan = isnan(xn) || isnan(xm) || isnan(acc);
        if (mode != 2 && isnan(acc)) {
            /* UNICORN/ARM: Ist der Addend NaN, wird er (gequietet) propagiert;
             * VNMLA/VNMLS negieren ihn. */
            uint32_t b;
            memcpy(&b, &acc, 4);
            b |= 0x00400000u;
            if (mode == 3 || mode == 4)
                b ^= 0x80000000u;
            memcpy(&r, &b, 4);
            vfp_set_s(c, dest, vfp_finish_s(c, r));
            return;
        }
        /* Ungueltiges Produkt (inf*0, 0*inf) -> positiver Default-qNaN; erst
         * danach negiert VMLS/VNMLA/VNMUL das Vorzeichen. */
        if (isnan(prod) && !isnan(xn) && !isnan(xm))
            prod = vfp_invalid_nan_s();
        /* Das Produkt wird fuer VMLS/VNMLA/VNMUL im Vorzeichen gedreht. */
        if (mode == 1 || mode == 2 || mode == 3) {
            uint32_t b;
            memcpy(&b, &prod, 4);
            b ^= 0x80000000u;
            memcpy(&prod, &b, 4);
            if (isnan(prod)) {
                memcpy(&r, &prod, 4);
                if (mode == 2 && (c->fpscr & VFP_FPSCR_DN))
                    vfp_set_s(c, dest, r);   /* VNMUL: DN erst nach der Negation */
                else
                    vfp_set_s(c, dest, vfp_finish_s(c, r));
                return;
            }
        }
        switch (mode) {
            case 0: r = vfp_add_s(acc, prod); break;
            case 1: r = vfp_add_s(acc, prod); break;
            case 2: r = prod; break;
            case 3: r = vfp_add_s(-acc, prod); break;
            default: r = vfp_add_s(-acc, prod); break;
        }
        if (!in_nan && isnan(r) && !isnan(prod))
            r = vfp_invalid_nan_s();
        vfp_set_s(c, dest, vfp_finish_s(c, r));
    } else {
        double xn = vfp_load_d(c, n), xm = vfp_load_d(c, m);
        double prod = vfp_finish_d(c, vfp_mul_d(xn, xm));
        double acc = vfp_load_d(c, dest), r;
        int in_nan = isnan(xn) || isnan(xm) || isnan(acc);
        if (mode != 2 && isnan(acc)) {
            uint64_t b;
            memcpy(&b, &acc, 8);
            b |= 0x0008000000000000ull;
            if (mode == 3 || mode == 4)
                b ^= 0x8000000000000000ull;
            memcpy(&r, &b, 8);
            vfp_set_d(c, dest, vfp_finish_d(c, r));
            return;
        }
        if (isnan(prod) && !isnan(xn) && !isnan(xm))
            prod = vfp_invalid_nan_d();
        if (mode == 1 || mode == 2 || mode == 3) {
            uint64_t b;
            memcpy(&b, &prod, 8);
            b ^= 0x8000000000000000ull;
            memcpy(&prod, &b, 8);
            if (isnan(prod)) {
                memcpy(&r, &prod, 8);
                if (mode == 2 && (c->fpscr & VFP_FPSCR_DN))
                    vfp_set_d(c, dest, r);   /* VNMUL: DN erst nach der Negation */
                else
                    vfp_set_d(c, dest, vfp_finish_d(c, r));
                return;
            }
        }
        switch (mode) {
            case 0: r = vfp_add_d(acc, prod); break;
            case 1: r = vfp_add_d(acc, prod); break;
            case 2: r = prod; break;
            case 3: r = vfp_add_d(-acc, prod); break;
            default: r = vfp_add_d(-acc, prod); break;
        }
        if (!in_nan && isnan(r) && !isnan(prod))
            r = vfp_invalid_nan_d();
        vfp_set_d(c, dest, vfp_finish_d(c, r));
    }
}

void vfp_cmp(ArmCpu *c, int dbl, int a, int b, int to_zero) {
    double x, y;
    uint32_t n = 0, z = 0, cc = 0, v = 0;
    if (!dbl) {
        x = (double)vfp_load_s(c, a);
        y = to_zero ? 0.0 : (double)vfp_load_s(c, b);
    } else {
        x = vfp_load_d(c, a);
        y = to_zero ? 0.0 : vfp_load_d(c, b);
    }
    /* ARM-Vergleich: NaN -> unordered (C=1,V=1); gleich -> Z=1,C=1;
     * kleiner -> N=1; groesser -> C=1. */
    if (isnan(x) || isnan(y)) { cc = 1; v = 1; }
    else if (x == y) { z = 1; cc = 1; }
    else if (x < y) { n = 1; }
    else { cc = 1; }
    c->fpscr = (c->fpscr & ~VFP_FPSCR_NZCV)
             | (n << 31) | (z << 30) | (cc << 29) | (v << 28);
}

void vfp_set_fpscr(ArmCpu *c, uint32_t value) {
    c->fpscr = value;
    vfp_apply_rounding(c->fpscr);
}

/* Konvertierung Gleitkomma -> Ganzzahl: immer Rundung Richtung Null, NaN -> 0,
 * Ueberlauf saettigt (IOC). */
void vfp_cvt_f2i(ArmCpu *c, int src_dbl, int is_signed, int dst_s, int src) {
    double x = src_dbl ? vfp_load_d(c, src) : (double)vfp_load_s(c, src);
    uint32_t r;
    if (isnan(x)) {
        r = 0;
    } else if (is_signed) {
        if (x >= 2147483648.0) r = 0x7FFFFFFFu;
        else if (x < -2147483648.0) r = 0x80000000u;
        else r = (uint32_t)(int32_t)x;
    } else {
        if (x >= 4294967296.0) r = 0xFFFFFFFFu;
        else if (x <= 0.0) r = 0;
        else r = (uint32_t)x;
    }
    c->sreg[dst_s] = r;
}

/* Festkomma-Varianten: Skalierung um 2^fbits vor/nach der Konvertierung. */
void vfp_cvt_f2i_fixed(ArmCpu *c, int src_dbl, int is_signed, int dst_s, int src,
                       int fbits) {
    double x = src_dbl ? vfp_load_d(c, src) : (double)vfp_load_s(c, src);
    x *= ldexp(1.0, fbits);
    uint32_t r;
    if (isnan(x)) {
        r = 0;
    } else if (is_signed) {
        if (x >= 2147483648.0) r = 0x7FFFFFFFu;
        else if (x < -2147483648.0) r = 0x80000000u;
        else r = (uint32_t)(int32_t)x;
    } else {
        if (x >= 4294967296.0) r = 0xFFFFFFFFu;
        else if (x <= 0.0) r = 0;
        else r = (uint32_t)x;
    }
    c->sreg[dst_s] = r;
}

/* Ganzzahl -> Gleitkomma; die Casts folgen der aktuellen Host-Rundung, die
 * ueber vfp_apply_rounding an die FPSCR-Rundung gekoppelt ist. */
void vfp_cvt_i2f(ArmCpu *c, int dst_dbl, int is_signed, int dst, int src_s) {
    uint32_t raw = c->sreg[src_s];
    double v = is_signed ? (double)(int32_t)raw : (double)raw;
    if (dst_dbl)
        vfp_set_d(c, dst, vfp_finish_d(c, v));
    else
        vfp_set_s(c, dst, vfp_finish_s(c, (float)v));
}

void vfp_cvt_i2f_fixed(ArmCpu *c, int dst_dbl, int is_signed, int dst, int src_s,
                       int fbits) {
    uint32_t raw = c->sreg[src_s];
    double v = is_signed ? (double)(int32_t)raw : (double)raw;
    v = ldexp(v, -fbits);
    if (dst_dbl)
        vfp_set_d(c, dst, vfp_finish_d(c, v));
    else
        vfp_set_s(c, dst, vfp_finish_s(c, (float)v));
}

/* f32 <-> f64; die Verengung rundet nach FPSCR. */
void vfp_cvt_fp(ArmCpu *c, int to_dbl, int dst, int from_dbl, int src) {
    if (to_dbl) {
        double v = from_dbl ? vfp_load_d(c, src) : (double)vfp_load_s(c, src);
        vfp_set_d(c, dst, vfp_finish_d(c, v));
    } else {
        float v = from_dbl ? (float)vfp_load_d(c, src) : vfp_load_s(c, src);
        vfp_set_s(c, dst, vfp_finish_s(c, v));
    }
}

void vfp_fpscr_to_flags(ArmCpu *c) {
    c->n = (c->fpscr >> 31) & 1u;
    c->z = (c->fpscr >> 30) & 1u;
    c->c = (c->fpscr >> 29) & 1u;
    c->v = (c->fpscr >> 28) & 1u;
}

void vfp_flags_to_fpscr(ArmCpu *c) {
    c->fpscr = (c->fpscr & ~VFP_FPSCR_NZCV) | (c->n << 31) | (c->z << 30)
             | (c->c << 29) | (c->v << 28);
}

/* ---------------- ARMv6 SIMD / DSP ---------------- */

static int32_t sat_range(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* SSAT/USAT: 32-Bit-Wert auf bits Stellen saettigen und CPSR.Q setzen. */
uint32_t arm_sat32(ArmCpu *c, int is_signed, uint32_t value, int bits) {
    int64_t v = (int64_t)(int32_t)value;
    int64_t lo = is_signed ? -(1LL << (bits - 1)) : 0;
    int64_t hi = is_signed ? ((1LL << (bits - 1)) - 1)
                           : ((1LL << bits) - 1);
    if (v < lo) { v = lo; c->qflag = 1; }
    if (v > hi) { v = hi; c->qflag = 1; }
    return (uint32_t)v;
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
                             ge |= (uint32_t)(t >= 0) << i; r = t; break; }
            case PAR_SSUB: { int t = (int8_t)ab - (int8_t)bb;
                             ge |= (uint32_t)(t >= 0) << i; r = t; break; }
            case PAR_UQADD: r = sat_range(ab + bb, 0, 255); break;
            case PAR_SQADD: r = sat_range((int8_t)ab + (int8_t)bb, -128, 127); break;
            case PAR_UQSUB: r = sat_range(ab - bb, 0, 255); break;
            case PAR_SQSUB: r = sat_range((int8_t)ab - (int8_t)bb, -128, 127); break;
            case PAR_UHADD: r = (ab + bb) >> 1; break;
            case PAR_SHADD: r = ((int8_t)ab + (int8_t)bb) >> 1; break;
            /* UHSUB8: arithmetische Halbierung der Differenz, negative
             * Differenz wird nicht geklemmt (Ergebnis 0xFF bei 1-2). */
            case PAR_UHSUB: r = (int32_t)(ab - bb) >> 1; break;
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
            case PAR_UADD: { int t = ab + bb; ge |= (uint32_t)(t > 0xFFFF) << (2 * i); r = t; break; }
            case PAR_USUB: { ge |= (uint32_t)(ab >= bb) << (2 * i); r = ab - bb; break; }
            case PAR_SADD: { int t = (int16_t)ab + (int16_t)bb;
                             ge |= (uint32_t)(t >= 0) << (2 * i); r = t; break; }
            case PAR_SSUB: { int t = (int16_t)ab - (int16_t)bb;
                             ge |= (uint32_t)(t >= 0) << (2 * i); r = t; break; }
            case PAR_UQADD: r = sat_range(ab + bb, 0, 0xFFFF); break;
            case PAR_SQADD: r = sat_range((int16_t)ab + (int16_t)bb, -32768, 32767); break;
            case PAR_UQSUB: r = sat_range(ab - bb, 0, 0xFFFF); break;
            case PAR_SQSUB: r = sat_range((int16_t)ab - (int16_t)bb, -32768, 32767); break;
            case PAR_UHADD: r = (ab + bb) >> 1; break;
            case PAR_SHADD: r = ((int16_t)ab + (int16_t)bb) >> 1; break;
            case PAR_UHSUB: r = (int32_t)(ab - bb) >> 1; break;
            case PAR_SHSUB: r = ((int16_t)ab - (int16_t)bb) >> 1; break;
            default: r = 0; break;
        }
        res |= (uint32_t)(r & 0xFFFF) << (16 * i);
    }
    /* 16-Bit-Lanes belegen GE-Paare: GE[2i] und GE[2i+1] sind gleich. */
    ge = (ge | (ge << 1)) & 0xFu;
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
    if (keep_low) {
        /* PKHBT: rd = rn[15:0] | (rm << shift)[31:16]; shift 0 = LSL #0. */
        uint32_t hi = (shift ? (b << shift) : b);
        return (a & 0xFFFFu) | (hi & 0xFFFF0000u);
    }
    /* PKHTB: rd = rn[31:16] | (rm ASR shift)[15:0]; Encoding 0 = ASR #32. */
    uint32_t lo;
    if (shift == 0) {
        lo = (uint32_t)((int32_t)b >> 31);
    } else if (shift >= 32) {
        lo = (uint32_t)((int32_t)b >> 31);
    } else {
        lo = (uint32_t)((int32_t)b >> shift);
    }
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
        case QADD_DADD:
            /* QDADD: Rd = sat(Rm + sat(2*Rn)) – verdoppelt wird der
             * *zweite* Quelloperand (Azahar dyncom QDADD_INST,
             * ARM ARM A8.8.129). Die Verdopplung saturiert selbst. */
            r = 2 * y;
            if (r > 2147483647LL) { r = 2147483647LL; c->qflag = 1; }
            else if (r < -2147483648LL) { r = -2147483648LL; c->qflag = 1; }
            r += x;
            break;
        default:
            /* QDSUB: Rd = sat(Rm - sat(2*Rn)) */
            r = 2 * y;
            if (r > 2147483647LL) { r = 2147483647LL; c->qflag = 1; }
            else if (r < -2147483648LL) { r = -2147483648LL; c->qflag = 1; }
            r = x - r;
            break;
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
        /* SMULxy: 16x16 passt immer in 32 Bit, keine Saettigung. */
        return (uint32_t)(int32_t)prod;
    }
    int64_t sum = prod + (int64_t)(int32_t)ra;
    /* SMLAxy setzt bei Ueberlauf nur Q; das Ergebnis ist die untere
     * 32-Bit-Summe (Azahar handle_Smlaxy, ARM ARM A8.8.135). */
    if (sum > 2147483647LL || sum < -2147483648LL) {
        c->qflag = 1;
    }
    return (uint32_t)sum;
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
            fprintf(stderr, "[sp] blk=0x%08X lr=0x%08X: SP %08X -> %08X (korrigiert)\n",
                    arm_last_block, c->r[14], sp0, c->r[13]);
        c->r[13] = sp0;
    }
}
