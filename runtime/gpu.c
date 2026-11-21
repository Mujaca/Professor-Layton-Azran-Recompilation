/* Minimaler PICA200-Kommandolisten-Ausfuehrer + Software-Rasterizer.
 *
 * Der Port hatte bisher keine Grafikpipeline: gsp_command hat
 * SetCommandList (0x0006) und TriggerCmdReqQueue (0x000C) nur bestaetigt.
 * Hier wird eine kleine, ehrliche Teilmenge ausgefuehrt:
 *
 *   Wortformat (wie PICA200):
 *     word = (cmd << 24) | param
 *     param == 0xFFFFF  ->  naechstes Wort ist die ANZAHL der Parameter
 *
 * Unterstuetzte Kommandos (bewusst klein gehalten):
 *   0x04  ClearColor      (param = 0xRRGGBB)
 *   0x05  FillRect        (param = Anzahl; 4 Parameter: x,y,w,h)
 *   0x06  DrawTriangle    (param = Anzahl; 9 Parameter: x1,y1,x2,y2,x3,y3,rgb,_)
 *
 * Ziel ist ein Beleg, dass Kommandolisten wirklich Pixel in den VRAM
 * schreiben (0x1F380000). Ein vollstaendiger PICA-Renderer (Texturen,
 * Shader, Tiefentest) ist damit noch nicht erreicht.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "arm_cpu.h"

#define GPU_FB_DEFAULT 0x1F000000u  /* Port-Fenster liest hier */
#define GPU_W 400
#define GPU_H 240

static uint32_t g_fb = GPU_FB_DEFAULT;
static uint32_t g_clear = 0x000000u;

void gpu_set_fb(uint32_t addr) { if (addr != 0) g_fb = addr; }
uint32_t gpu_get_fb(void) { return g_fb; }

static void put_px(ArmCpu *c, int x, int y, uint32_t rgb)
{
    if (x < 0 || y < 0 || x >= GPU_W || y >= GPU_H) return;
    uint32_t off = (g_fb & 0x1FFFFFFFu) + (uint32_t)(y * GPU_W + x) * 3u;
    if (off + 3u > c->mem_size) return;
    arm_mem_write(c, off + 0u, 1, (rgb >> 0) & 0xFFu);
    arm_mem_write(c, off + 1u, 1, (rgb >> 8) & 0xFFu);
    arm_mem_write(c, off + 2u, 1, (rgb >> 16) & 0xFFu);
}

static void fill_rect(ArmCpu *c, int x, int y, int w, int h, uint32_t rgb)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            put_px(c, x + i, y + j, rgb);
}

static void draw_tri(ArmCpu *c, int x1, int y1, int x2, int y2, int x3, int y3,
                     uint32_t rgb)
{
    int minx = x1 < x2 ? (x1 < x3 ? x1 : x3) : (x2 < x3 ? x2 : x3);
    int maxx = x1 > x2 ? (x1 > x3 ? x1 : x3) : (x2 > x3 ? x2 : x3);
    int miny = y1 < y2 ? (y1 < y3 ? y1 : y3) : (y2 < y3 ? y2 : y3);
    int maxy = y1 > y2 ? (y1 > y3 ? y1 : y3) : (y2 > y3 ? y2 : y3);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= GPU_W) maxx = GPU_W - 1;
    if (maxy >= GPU_H) maxy = GPU_H - 1;

    int d = (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3);
    if (d == 0) return;

    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            int a = (y2 - y3) * (x - x3) + (x3 - x2) * (y - y3);
            int b = (y3 - y1) * (x - x3) + (x1 - x3) * (y - y3);
            int cc = d - a - b;
            if ((a >= 0 && b >= 0 && cc >= 0) || (a <= 0 && b <= 0 && cc <= 0))
                put_px(c, x, y, rgb);
        }
    }
}

int gpu_run_command_list(ArmCpu *c, uint32_t addr, uint32_t size)
{
    uint32_t a = addr;
    uint32_t end = addr + size;
    int ops = 0;

    /* Diagnose: Kommandoliste roh ausgeben (Format unbekannt -> lernen). */
    {
        static int log_n = -2;
        if (log_n == -2) {
            const char *e = getenv("GPU_LIST_LOG");
            log_n = (e != NULL && *e != 0) ? atoi(e) : 0;
        }
        if (log_n > 0) {
            static int cnt = 0;
            if (cnt < log_n) {
                printf("[gpulist] addr=%08X size=%X:", addr, size);
                for (uint32_t i = 0; i < 48u && i * 4u < size; i++)
                    printf(" %08X", (unsigned)*(const uint32_t *)(c->mem + ((addr + i * 4u) & 0x1FFFFFFFu)));
                printf("\n");
                fflush(stdout);
                cnt++;
            }
        }
    }

    while (a + 4u <= end) {
        uint32_t w = 0;
        uint32_t off = a & 0x1FFFFFFFu;
        if (off + 4u > c->mem_size) break;
        memcpy(&w, c->mem + off, 4);
        a += 4;

        uint32_t cmd = (w >> 24) & 0xFFu;
        uint32_t param = w & 0xFFFFFFu;

        if (param == 0xFFFFFu) {
            uint32_t cnt = 0;
            if ((a & 0x1FFFFFFFu) + 4u > c->mem_size) break;
            memcpy(&cnt, c->mem + (a & 0x1FFFFFFFu), 4);
            a += 4;
            if (cmd == 0x05u && cnt >= 4u) {
                uint32_t p[4] = { 0, 0, 0, 0 };
                for (uint32_t i = 0; i < 4u && i < cnt; i++) {
                    memcpy(&p[i], c->mem + ((a + i * 4u) & 0x1FFFFFFFu), 4);
                }
                fill_rect(c, (int)p[0], (int)p[1], (int)p[2], (int)p[3], g_clear);
                ops++;
            } else if (cmd == 0x06u && cnt >= 9u) {
                uint32_t p[9];
                for (uint32_t i = 0; i < 9u; i++)
                    memcpy(&p[i], c->mem + ((a + i * 4u) & 0x1FFFFFFFu), 4);
                draw_tri(c, (int)p[0], (int)p[1], (int)p[2], (int)p[3],
                         (int)p[4], (int)p[5], p[6]);
                ops++;
            }
            a += cnt * 4u;
        } else if (cmd == 0x04u) {
            g_clear = param;
        } else if (cmd == 0x05u) {
            /* FillRect mit 4 Parametern als 0xFFFFF-Form erwartet */
        } else if (cmd == 0x06u) {
            /* DrawTriangle mit 9 Parametern als 0xFFFFF-Form erwartet */
        }
    }
    return ops;
}
