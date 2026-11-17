/* Boot-Host: fuehrt den recompilierten Code mit flachem 3DS-Speicher aus.
 * Phase A: alles loggen, Services noch generisch beantworten. */
#include "arm_cpu.h"
#include "arm_trace.h"
#include "hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "hle.h"

#define MEM_SIZE 0x30000000u   /* 768 MiB: FCRAM + VRAM (bis 0x20500000) + MMIO */

void fn_00100000(ArmCpu *c);

static unsigned char *g_mem;
static volatile ArmCpu *g_cpu_view = NULL;

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(2000);
        if (g_cpu_view) {
            printf("[wd] pc=0x%08X sp=0x%08X lr=0x%08X\n",
                   g_cpu_view->r[15], g_cpu_view->r[13], g_cpu_view->r[14]);
            if (g_cpu_view->aborted) {
                hle_report();
                exit(3);
            }
        }
    }
    return 0;
}

static void load_code(ArmCpu *c) {
    FILE *f = fopen("out/exefs/code_decomp.bin", "rb");
    if (!f) {
        printf("code_decomp.bin fehlt\n");
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0x300000) n = 0x300000;
    if (fread(g_mem + 0x100000, 1, (size_t)n, f) != (size_t)n) {
        printf("code kurz gelesen\n");
    }
    fclose(f);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *romfs = (argc > 1) ? argv[1] : "out/romfs";
    g_mem = (unsigned char *)calloc(1, MEM_SIZE);
    if (!g_mem) { printf("kein Speicher\n"); return 2; }

    ArmCpu c;
    arm_cpu_init(&c, g_mem, MEM_SIZE);
    load_code(&c);

    c.r[13] = 0x0FFF0000u;                 /* App-Heap-Ende - Stack */
    c.r[14] = 0u;
    c.tpidruro = 0x1FF82000u;              /* TLS_AREA_VADDR */
    c.r[15] = 0x00100000u;

    if (getenv("ARM_TRACE_FILE")) {
        arm_trace_open(getenv("ARM_TRACE_FILE"));
    }

    g_cpu_view = &c;
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    hle_init(&c, romfs);
    arm_set_svc_handler(hle_svc);

    printf("boot: entry 0x00100000, romfs=%s\n", romfs);

    fn_00100000(&c);

    printf("halt: aborted=%d pc=0x%08X r0=0x%08X\n", c.aborted, c.r[15], c.r[0]);
    hle_report();
    arm_trace_close();
    return 0;
}
