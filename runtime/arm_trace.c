/* Trace-Runtime: schreibt Records im Format "CTRTRC01" (siehe tools/recomp/trace.py). */
#include "arm_trace.h"
#include "arm_cpu.h"

#include <stdio.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

uint32_t arm_last_block = 0;
void (*arm_pump_hook)(void) = NULL;
uint32_t arm_watch_pc = 0;
static int g_watch_hits = 0;

/* Ringpuffer der zuletzt ausgefuehrten Bloecke (Diagnose bei Abbruch). */
#define PC_RING 65536
static uint32_t g_pc_ring[PC_RING];
static uint32_t g_lr_ring[PC_RING];
static unsigned g_pc_pos = 0;
uint32_t arm_thread_last_pc[16];
static uint32_t g_t_ring[16][64];
static uint32_t g_t_ring_lr[16][64];
static uint32_t g_t_pos[16];

void arm_trace_thread_dump(int ti, unsigned n) {
    if (ti < 0 || ti > 15) return;
    if (n > 64) n = 64;
    fprintf(stderr, "[tdump] thread %X letzte %u Bloecke:\n", ti, n);
    for (unsigned i = 0; i < n; i++) {
        uint32_t idx = (g_t_pos[ti] + 64 - n + i) & 63u;
        fprintf(stderr, "[tdump]   %02u pc=%08X lr=%08X\n", i, g_t_ring[ti][idx], g_t_ring_lr[ti][idx]);
    }
    fflush(stderr);
}

uint32_t arm_thread_last_lr[16];

void arm_trace_pc_ring_dump(unsigned count) {
    unsigned n = count > PC_RING ? PC_RING : count;
    fprintf(stderr, "[ring] letzte %u Bloecke (neueste zuerst):\n", n);
    for (unsigned i = 0; i < n; i++) {
        unsigned idx = (g_pc_pos + PC_RING - 1u - i) & (PC_RING - 1);
        fprintf(stderr, "[ring] %04u pc=%08X lr=%08X\n", i, g_pc_ring[idx], g_lr_ring[idx]);
    }
    fflush(stderr);
}

static FILE *g_file = NULL;
static unsigned long long g_records = 0;
static unsigned int g_flush_counter = 0;
static unsigned long long g_max_records = 20000000ULL;
static int g_unique_only = 1;

/* Offene Adressierung: jede Blockadresse nur einmal schreiben. */
#define SEEN_SLOTS (1u << 21)
static unsigned int *g_seen = NULL;

static int mark_seen(unsigned int pc) {
    unsigned int h = (pc * 2654435761u) & (SEEN_SLOTS - 1);
    for (unsigned int i = 0; i < SEEN_SLOTS; i++) {
        unsigned int slot = (h + i) & (SEEN_SLOTS - 1);
        if (g_seen[slot] == 0) {
            g_seen[slot] = pc + 1;
            return 1;
        }
        if (g_seen[slot] == pc + 1) {
            return 0;
        }
    }
    return 0;
}

static void put_u8(unsigned v) {
    unsigned char b = (unsigned char)(v & 0xFF);
    if (g_file) fwrite(&b, 1, 1, g_file);
}

static void put_u16(unsigned v) {
    put_u8(v);
    put_u8(v >> 8);
}

static void put_u32(unsigned v) {
    put_u8(v);
    put_u8(v >> 8);
    put_u8(v >> 16);
    put_u8(v >> 24);
}

void arm_trace_open(const char *path) {
    const char *all = getenv("ARM_TRACE_ALL");
    const char *max = getenv("ARM_TRACE_MAX");
    if (g_file) return;
    g_unique_only = (all == NULL || *all == '\0' || *all == '0');
    if (max != NULL) {
        g_max_records = strtoull(max, NULL, 0);
    }
    g_records = 0;
    g_seen = (unsigned int *)calloc(SEEN_SLOTS, sizeof(unsigned int));
    g_file = fopen(path ? path : "trace.ctrt", "wb");
    if (g_file) {
        fwrite("CTRTRC01", 1, 8, g_file);
        fflush(g_file);
    }
}

void arm_trace_close(void) {
    if (g_file) {
        fflush(g_file);
        fclose(g_file);
        g_file = NULL;
    }
    if (g_seen) {
        free(g_seen);
        g_seen = NULL;
    }
}

int arm_trace_is_open(void) {
    return g_file != NULL;
}


void arm_trace_block(ArmCpu *c, uint32_t pc, uint32_t thumb) {
    arm_last_block = pc;
    /* Kalibrierte SDK-Busy-Wait-Verzoegerung sofort beenden: ohne echte
       Tick-Zaehlung spinnt sie zig-tausend Runden und blockiert den
       kooperativen Scheduler (Thread 2 blieb dort haengen). */
    if (c != NULL && pc == 0x0011A818u) c->r[0] = 0;
    if (c != NULL && pc == 0x0024FD74u && getenv("HLE_FA_DEBUG")) {
        static int nfamax = 0;
        if (nfamax < 12) {
            nfamax++;
            printf("[fahash] ns=%08X hash=%08X\n", c->r[1], c->r[0]);
            fflush(stdout);
        }
    }

    /* Asset-Registry synthetisch bestuecken: Deskriptor {basis,stride,count}
       plus Eintraege. Layout laut 0x2B95F8: [+0x14] != 0 (aktiv),
       [+0x1C] = Zeiger auf den Dateinamen. */
    if (c != NULL && getenv("HLE_REG_ENABLE") &&
        (pc == 0x00104E44u || pc == 0x0036C54Cu)) {
        static const uint32_t names[4] = { 0x001015F8u, 0x00101608u,
                                           0x00101618u, 0x00101628u };
        uint32_t d0 = 0x00400000u, d1 = 0x000000C0u, d2 = 4u, g = 0x00401000u;
        uint32_t one = 1u;
        for (int i = 0; i < 4; i++) {
            uint32_t e = 0x00400000u + (uint32_t)i * 0xC0u;
            uint32_t ns = 0x00403000u;
            memcpy(c->mem + e + 0x14u, &ns, 4);
            memcpy(c->mem + e + 0x1Cu, &names[i], 4);
        }
        /* Fake-Namespace (2 Ebenen) fuer 0x24FD74:
           ns 0x403000 {+4=0x100 Eintraege, +12=0x200 Kindtabelle, +24=1}
           Eintrag 0x403100 {Hash(arc)=0x10AAF679, +8=0, +10=1}
           Kind    0x403200 {Hash(lt6_a.fa)=0xD56041E4} */
        {
            uint32_t z = 0, eoff = 0x100u, ctab = 0x200u, cnt = 1u;
            uint32_t hash_arc = 0x10AAF679u, hash_fa = 0xD56041E4u;
            uint16_t cidx = 0u, ccnt = 1u;
            memcpy(c->mem + 0x00403000u, &z, 4);
            memcpy(c->mem + 0x00403004u, &eoff, 4);
            memcpy(c->mem + 0x0040300Cu, &ctab, 4);
            memcpy(c->mem + 0x00403018u, &cnt, 2);
            memcpy(c->mem + 0x00403100u, &hash_arc, 4);
            memcpy(c->mem + 0x00403108u, &cidx, 2);
            memcpy(c->mem + 0x0040310Au, &ccnt, 2);
            memcpy(c->mem + 0x00403200u, &hash_fa, 4);
        }
        memcpy(c->mem + 0x00401000u, &d0, 4);
        memcpy(c->mem + 0x00401004u, &d1, 4);
        memcpy(c->mem + 0x00401008u, &d2, 4);
        memcpy(c->mem + 0x3ECA5Cu, &g, 4);
        if (getenv("HLE_REG_DEBUG")) {
            printf("[regfill] pc=%08X desc=%08X cnt=%u name0=%08X\n",
                   pc, d0, d2, names[0]);
            fflush(stdout);
        }
    }
    /* Diagnose: beim FA-Scan den Request-Struct zeigen, den 0x2B95F8 parst. */
    if (c != NULL && pc == 0x002B9664u && getenv("HLE_REG_DEBUG")) {
        static int dbg = 0;
        if (dbg < 3) {
            dbg++;
            printf("[fastruct] entry=%08X off20=%08X req=%08X:",
                   c->r[4], arm_mem_read(c, c->r[4] + 0x20u, 4), c->r[5]);
            for (int i = 0; i < 8; i++)
                printf(" %08X", arm_mem_read(c, c->r[5] + (uint32_t)i * 4u, 4));
            printf("\n");
            fflush(stdout);
        }
    }
    /* Diagnose: Asset-Kette (Registrierung -> Dispatcher) mitzaehlen. */
    if (c != NULL && getenv("HLE_ASSET_TRACE")) {
        if (pc == 0x001013F0u || pc == 0x001014D4u || pc == 0x00291A88u ||
            pc == 0x002919DCu || pc == 0x00291AA0u) {
            printf("[asset-trace] pc=%08X r0=%08X r1=%08X r2=%08X r3=%08X\n",
                   pc, c->r[0], c->r[1], c->r[2], c->r[3]);
            fflush(stdout);
        }
    }

    /* Diagnose: Blockpfad in [lo, lo+0x4000) protokollieren. */
    if (c != NULL && getenv("HLE_BLKLOGF")) {
        static uint32_t last = 0xFFFFFFFFu;
        uint32_t lo = (uint32_t)strtoul(getenv("HLE_BLKLOGF"), NULL, 16);
        if (pc >= lo && pc < lo + 0x4000u) {
            if (pc != last) {
                last = pc;
                printf("[blk] %08X r0=%08X r4=%08X r8=%08X\n",
                       pc, c->r[0], c->r[4], c->r[8]);
                fflush(stdout);
            }
        }
    }

    /* 0x101364 bl 0x1034F4 -> bei 0x101370 wird sonst die gesamte
       Asset-Registrierung (0x1013F0) uebersprungen. */
    if (c != NULL && getenv("HLE_FORCE_ASSET") &&
        (pc == 0x00101368u || pc == 0x00101370u || pc == 0x00101374u)) {
        static int fc2 = 0;
        if (fc2 < 5) { fc2++; printf("[forceasset] pc=%08X r0=%08X\n", pc, c->r[0]); }
        c->r[0] = 1;
    }
    /* Diagnose: an den Datei-Open-Fatal-Sites die Register zeigen. */
    if (c != NULL && getenv("HLE_DUMP_FATAL") &&
        (pc == 0x00107A4Cu || pc == 0x00107B34u)) {
        printf("[fatal-site] pc=%08X r0=%08X r1=%08X r2=%08X r3=%08X "
               "r4=%08X r5=%08X r6=%08X r7=%08X\n",
               pc, c->r[0], c->r[1], c->r[2], c->r[3],
               c->r[4], c->r[5], c->r[6], c->r[7]);
        fflush(stdout);
    }

    /* Diagnose: beim "rom:"-Open-Wrapper die Struktur hinter r3 zeigen. */
    if (c != NULL && pc == 0x00107AE4u && getenv("HLE_DUMP_R3")) {
        uint32_t a = c->r[3] & 0x1FFFFFFFu;
        printf("[r3] r1=%08X r2=%08X r3=%08X:", c->r[1], c->r[2], c->r[3]);
        for (int i = 0; i < 8 && a + i * 4 + 4 <= c->mem_size; i++) {
            uint32_t w = 0;
            memcpy(&w, c->mem + a + i * 4, 4);
            printf(" %08X", w);
        }
        printf("\n");
        fflush(stdout);
    }
    if (arm_pump_hook != NULL) arm_pump_hook();
    if (c != NULL) {
        {
            uint32_t ti = (c->tpidruro >> 12) & 0xFu;
            arm_thread_last_pc[ti] = pc;
            arm_thread_last_lr[ti] = c->r[14];
            if (ti < 16) { g_t_ring[ti][g_t_pos[ti] & 63u] = pc; g_t_ring_lr[ti][g_t_pos[ti] & 63u] = c->r[14]; g_t_pos[ti]++; }
        }
        g_pc_ring[g_pc_pos] = pc;
        g_lr_ring[g_pc_pos] = c->r[14];
        g_pc_pos = (g_pc_pos + 1) & (PC_RING - 1);
    }
    if (arm_watch_pc != 0 && pc == arm_watch_pc && c != NULL && g_watch_hits < 600) {
        g_watch_hits++;
        fprintf(stderr, "[watch] 0x%08X r0=%08X r1=%08X r2=%08X r3=%08X "
                        "r4=%08X r5=%08X r6=%08X r7=%08X sp=%08X lr=%08X\n",
                pc, c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6],
                c->r[7], c->r[13], c->r[14]);
        {
            uint32_t s8 = (c->r[13] + 8) & ~3u;
            if (s8 + 8 <= c->mem_size) {
                fprintf(stderr, "[watch]   [sp+8]=%08X [sp+0x20]=%08X\n",
                        (unsigned)arm_mem_read(c, s8, 4),
                        (unsigned)arm_mem_read(c, (c->r[13] + 0x20) & ~3u, 4));
            }
        }
        {
            uint32_t a = c->r[4] & ~3u;
            if (a + 16 <= c->mem_size) {
                fprintf(stderr, "[watch]   mem@r4: %08X %08X %08X %08X\n",
                        (unsigned)arm_mem_read(c, a, 4), (unsigned)arm_mem_read(c, a + 4, 4),
                        (unsigned)arm_mem_read(c, a + 8, 4), (unsigned)arm_mem_read(c, a + 12, 4));
            }
        }
        if (getenv("ARM_WATCH_POOL")) {
            uint32_t a = 0x004080C8u;
            fprintf(stderr, "[watch]   pool:");
            for (int i = 0; i < 16; i++)
                fprintf(stderr, " %08X", (unsigned)arm_mem_read(c, a + (uint32_t)i * 4u, 4));
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }
    if (!g_file) return;
    if (g_records >= g_max_records) return;
    if (g_unique_only && g_seen != NULL && !mark_seen(pc)) return;
    g_records++;
    if ((++g_flush_counter & 0x3Fu) == 0) {
        fflush(g_file);
    }
    put_u8('B');
    put_u32(pc);
    put_u8(thumb ? 1 : 0);
}

void arm_trace_svc(uint32_t imm) {
    if (!g_file) return;
    put_u8('S');
    put_u32(imm);
}

void arm_trace_ipc(const char *service, uint32_t cmd_id) {
    if (!g_file) return;
    const char *name = service ? service : "";
    size_t len = strlen(name);
    if (len > 64) len = 64;
    put_u8('I');
    put_u8((unsigned)len);
    if (fwrite(name, 1, len, g_file) != len) { /* ignore */ }
    put_u16((unsigned)(cmd_id & 0xFFFF));
}

void arm_trace_frame(void) {
    static unsigned counter = 0;
    if (!g_file) return;
    put_u8('F');
    put_u32(counter++);
}

void arm_trace_event(uint32_t kind, uint32_t a, uint32_t b) {
    if (!g_file) return;
    put_u8('E');
    put_u32(kind);
    put_u32(a);
    put_u32(b);
}
