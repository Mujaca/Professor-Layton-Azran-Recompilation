/* Laufzeit-Helfer fuer spielspezifische Workarounds.
 *
 * Technische Schuld (docs/WORKAROUNDS.md). Aufgerufen nur aus Code, den
 * tools/workarounds/block_hooks.json in den recompilierten Code einfuegt.
 * Ziel: Datei loeschen, sobald Kernel/Heap korrekt sind.
 */
#include "arm_cpu.h"

#include <string.h>

extern ArmCpu *g_pool_cpu;

/* Bump-Allocator im Gastbereich 0x1E000000..0x1E400000 (zwischen Linear-Heap
 * und VRAM). Ersatz, wenn der SDK-Allocator 0x287DD4 0 liefert. */
#define WA_POOL_BASE 0x1E000000u
#define WA_POOL_SIZE 0x00400000u
static uint32_t g_pool_used = 0;

uint32_t arm_pool_alloc(unsigned size) {
    size = (size + 15u) & ~15u;
    if (size == 0) size = 16;
    if (g_pool_cpu == NULL) return 0;
    if (g_pool_used + size > WA_POOL_SIZE) return 0;
    uint32_t addr = WA_POOL_BASE + g_pool_used;
    g_pool_used += size;
    memset(g_pool_cpu->mem + addr, 0, size);
    return addr;
}

