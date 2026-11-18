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
    /* 0x101364 bl 0x1034F4 -> bei 0x101370 wird sonst die gesamte
       Asset-Registrierung (0x1013F0) uebersprungen. */
    if (c != NULL && pc == 0x00101368u && getenv("HLE_FORCE_ASSET"))
        c->r[0] = 1;
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
