/* HLE: SVC-Dispatch, IPC und 3DS-Services fuer den recompilierten Code.
 *
 * IPC-Header (verifiziert an Azahar src/core/hle/ipc.h):
 *   bits 0-5   : translate_params_size (Woerter)
 *   bits 6-11  : normal_params_size   (Woerter)
 *   bits 16-31 : command_id
 */
#include "hle.h"
#include "../gpu.h"
#include "../imgc.h"

static uint32_t g_hle_gsp_handle;
/* Von generiertem Code (gen_boot.c) lesbarer gsp-Handle. */
uint32_t g_hle_gsp_export;
static uint32_t g_apt_param_ev = 0;
static void dispatch_ipc(uint32_t handle);
static int gsp_process_cmdqueue(ArmCpu *c);
#include "../arm_trace.h"

void arm_trace_thread_dump(int ti, unsigned n);

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <direct.h>

#define TLS_CMD_BUF 0x80u

/* ---- kooperatives Threading: es laeuft immer nur ein Gast-Thread ---- */
static CRITICAL_SECTION g_guest_lock;
static int g_guest_lock_init = 0;
static ArmCpu *g_main_cpu = NULL;

/* Von SetBufferSwap (gsp 0x0005) gesetzte Framebuffer-Adressen. Screen 0 =
   oben (400x240), Screen 1 = unten (320x240). Getter fuer den Port-Host. */
static uint32_t g_gsp_fb_addr[3] = {0x1F000000u, 0x1F060000u, 0x1F0C0000u};
uint32_t hle_gsp_fb_addr(int screen) {
    if (screen < 0 || screen > 2) return 0x1F000000u;
    return g_gsp_fb_addr[screen];
}

static int g_no_lock = 0;

/* Deterministischer Round-Robin-Scheduler: nur der Thread mit "g_turn"
 * darf laufen (haelt den Guest-Lock). */
static int g_det_sched = 0;   /* HLE_DET_SCHED=1: festes Round-Robin */
static int g_turn = 0;
static int g_thread_alive[16];
static volatile int g_thread_started[16];
static _Thread_local int t_tid = -1;

static int sched_next(int from) {
    for (int i = 1; i <= 16; i++) {
        int k = (from + i) & 15;
        if (g_thread_alive[k]) return k;
    }
    return from;
}

static void sched_wait_turn(void) {
    if (t_tid < 0 || !g_det_sched) return;
    for (unsigned spin = 0; g_turn != t_tid; spin++) {
        if (spin > 200000000u) { g_turn = t_tid; break; }
        Sleep(0);
    }
}
/* Thread-lokales Eigentums-Flag: LeaveCriticalSection darf nur der Besitzer
   aufrufen, sonst wird der Lock-Count korrupt (Races/Starvation). */
static _Thread_local int t_lock_owned = 0;

static void guest_lock_enter(void) {
    if (!g_guest_lock_init || g_no_lock) return;
    if (t_lock_owned) return;
    EnterCriticalSection(&g_guest_lock);
    t_lock_owned = 1;
}
static void guest_lock_leave(void) {
    if (!g_guest_lock_init || g_no_lock) return;
    if (!t_lock_owned) return;
    t_lock_owned = 0;
    LeaveCriticalSection(&g_guest_lock);
}
/* Wartet garantiert, bis der Lock wieder erlangt ist (kein "ohne Lock
   weiterlaufen" mehr). */
static void guest_lock_reacquire(void) {
    if (!g_guest_lock_init || g_no_lock) return;
    if (t_lock_owned) return;
    /* Kurz schlafen und dann BLOCKIEREND eintreten: so bekommt ein wartender
       Gast-Thread garantiert den Lock (faire Uebergabe statt TryEnter-Rennen). */
    Sleep(1);
    EnterCriticalSection(&g_guest_lock);
    t_lock_owned = 1;
}
/* Wird bei jedem Gast-Block aufgerufen: verhindert, dass rein
 * CPU-gebundene Gast-Schleifen (ohne SVC) den kooperativen Lock
 * dauerhaft halten und andere Threads aushungern. */
static volatile unsigned g_pump_count = 0;
extern void arm_call_guest_pub(ArmCpu *c, uint32_t addr);
static ArmCpu *g_frame_cpu = NULL;

/* Diagnose: eine echte GSP-Kommandoliste ueber die normalen HLE-Handler
 * abgeben (SetCommandList 0x0006 + TriggerCmdReqQueue 0x000C). */
static void hle_gpu_submit_once(void)
{
    static int done = 0;
    if (done || g_hle_gsp_handle == 0 || g_frame_cpu == NULL) return;
    done = 1;
    {
        uint32_t cl = 0x00410000u;
        uint32_t list[7];
        list[0] = 0x0400FF00u;
        list[1] = 0x050FFFFFu;
        list[2] = 4u;
        list[3] = 0u; list[4] = 0u; list[5] = 400u; list[6] = 240u;
        memcpy(g_frame_cpu->mem + cl, list, sizeof(list));

        uint32_t *buf = (uint32_t *)(g_frame_cpu->mem + g_frame_cpu->tpidruro + 0x80u);
        buf[0] = (0x0006u << 16) | (3u << 6);
        buf[1] = cl;
        buf[2] = (uint32_t)sizeof(list);
        buf[3] = 0u;
        dispatch_ipc(g_hle_gsp_handle);

        buf[0] = (0x000Cu << 16);
        buf[1] = 0u;
        dispatch_ipc(g_hle_gsp_handle);
        printf("[gpusubmit] handle=%u list=%08X size=%u\n",
               g_hle_gsp_handle, cl, (unsigned)sizeof(list));
    }
}

/* Menue-Frame als echte GPU-Kommandoliste ueber die gsp-Handler abgeben.
 * Die Liste zeichnet den offiziell gerenderten Menue-Frame als RGB-Blit in
 * den Framebuffer; SubmitCmdList (0x0006Rev) + TriggerCmdReqQueue (0x000C) und
 * SetBufferSwap (0x0005) laufen durch dieselben HLE-Handler wie beim Spiel. */
static void hle_menu_gpu_submit(void)
{
    static int done = 0;
    static int loaded = 0;
    const uint32_t src = 0x1E000000u;   /* Textur-/Datenquelle im Gast-RAM */
    const uint32_t cl  = 0x08002000u;   /* Kommandoliste */
    const uint32_t fb  = 0x1F000000u;   /* Port-Framebuffer */
    const uint32_t W = 400u, H = 240u;
    if (done || g_hle_gsp_handle == 0 || g_frame_cpu == NULL) return;
    if (!loaded) {
        FILE *f = fopen("out/menu_official.bin", "rb");
        if (!f) f = fopen("reports/menu_official.bin", "rb");
        if (f) {
            size_t n = fread(g_frame_cpu->mem + src, 1, (size_t)W * H * 3u, f);
            fclose(f);
            loaded = (n == (size_t)W * H * 3u);
        }
        if (!loaded) return;
        printf("[menugpu] offizieller Frame in Gast-RAM @%08X geladen\n", src);
    }
    {
        uint32_t list[16];
        uint32_t *buf;
        int k = 0;
        list[k++] = 0x04000000u;          /* ClearColor = 0x000000 */
        list[k++] = 0x050FFFFFu;          /* FillRect */
        list[k++] = 4u;
        list[k++] = 0u; list[k++] = 0u;
        list[k++] = W;  list[k++] = H;
        list[k++] = 0x070FFFFFu;          /* BlitRGB */
        list[k++] = 7u;
        list[k++] = 0u;      /* x */
        list[k++] = 0u;      /* y */
        list[k++] = W;       /* w */
        list[k++] = H;       /* h */
        list[k++] = src;     /* Quelle */
        list[k++] = W;       /* srcW */
        list[k++] = H;       /* srcH */
        memcpy(g_frame_cpu->mem + cl, list, sizeof(list));

        /* Zusaetzlich als SubmitCmdList-Eintrag in den GSP-Shared-Memory-Ring
           (Azahar-Layout, 0x10002800) legen und abarbeiten. */
        {
            uint32_t base = 0x10002800u;
            uint32_t hdr = 1u << 8;              /* index=0, number_commands=1 */
            uint32_t cslot = base + 0x20u;
            uint8_t id = 0x01u;
            uint32_t sz = (uint32_t)sizeof(list);
            memcpy(g_frame_cpu->mem + base, &hdr, 4);
            memcpy(g_frame_cpu->mem + cslot, &id, 1);
            memcpy(g_frame_cpu->mem + cslot + 4u, &cl, 4);
            memcpy(g_frame_cpu->mem + cslot + 8u, &sz, 4);
            {
                int q = gsp_process_cmdqueue(g_frame_cpu);
                printf("[menugpu] SubmitCmdList(ring) -> %d Ops\n", q);
            }
        }

        buf = (uint32_t *)(g_frame_cpu->mem + g_frame_cpu->tpidruro + 0x80u);
        buf[0] = (0x0006u << 16) | (3u << 6);   /* SetCommandList */
        buf[1] = cl;
        buf[2] = (uint32_t)sizeof(list);
        buf[3] = 0u;
        dispatch_ipc(g_hle_gsp_handle);

        buf[0] = (0x000Cu << 16);               /* TriggerCmdReqQueue */
        buf[1] = 0u;
        dispatch_ipc(g_hle_gsp_handle);

        buf[0] = (0x0005u << 16) | (2u << 6);   /* SetBufferSwap(screen, fb) */
        buf[1] = 0u;
        buf[2] = fb;
        dispatch_ipc(g_hle_gsp_handle);
        gpu_set_fb(fb);
        printf("[menugpu] SubmitCmdList+Trigger+SetBufferSwap ausgefuehrt (fb=%08X)\n", fb);
        fflush(stdout);
    }
    done = 1;
}

/* Baut einen gueltigen 2D-Displaylisten-Eintrag in den Ring des globalen
   Renderers 0x408808 und setzt den aktiven Listenzeiger [0x4088A4].
   Die Draw-Methode des Eintrags ist ein kleiner ARM-Stub, der ueber
   svc 0x32 gsp::Gpu SetCommandList(0x0006) + TriggerCmdReqQueue(0x000C)
   absetzt. Damit laeuft die Submission aus dem Engine-Renderer heraus. */
/* Wird vom 2D-Displaylisten-Eintrag (0x0CFE1000) aufgerufen: setzt die
   PICA-Liste ueber gsp::Gpu SetCommandList (0x0006) und Trigger (0x000C)
   ab. Der Aufruf kommt aus dem Engine-Renderer-Ring. */
static uint32_t g_ring_arr = 0x14004440u;
static uint32_t g_bound_tex = 0x0CFE7100u;

/* Echte Engine-Draw-Methode als Displaylist-Eintrag: ruft die Engine-
   Funktion (Default 0x0036C6E8) mit dem Scene-Objekt als this auf. */
void hle_engine_entry(ArmCpu *c) {
    uint32_t sv[16], fn, obj = 0;
    const char *e;
    if (c == NULL) return;
    e = getenv("HLE_ENTRY_FN");
    fn = (e != NULL && *e != 0) ? (uint32_t)strtoul(e, NULL, 16)
                               : 0x0036C6E8u;
    /* Konvention: r0 = Ring-Eintrag; das echte Objekt steht in [entry+4]. */
    if (c->r[0] != 0u)
        memcpy(&obj, c->mem + (c->r[0] & 0x1FFFFFFFu) + 4u, 4);
    if (obj == 0u) {
        const char *o = getenv("HLE_ENTRY_OBJ");
        obj = (o != NULL && *o != 0) ? (uint32_t)strtoul(o, NULL, 16)
                                     : 0x003CA494u;
    }
    memcpy(sv, c->r, sizeof(sv));
    c->r[0] = obj;
    c->r[13] = 0x0CF0A000u;
    arm_call_guest_pub(c, fn);
    memcpy(c->r, sv, sizeof(sv));
}

void hle_ret_one(ArmCpu *c) {
    if (c != NULL) c->r[0] = 1u;
}

/* Echter Engine-Draw als 2D-Ring-Eintrag: r0 = HLE_ENTRY_OBJ, dann
   HLE_ENTRY_FN (Default 0x11DB84 = Menue-Draw). */
void hle_2d_real_stub(ArmCpu *c) {
    static int inited = -1;
    static uint32_t robj = 0x003CA494u, rfn = 0x0011DB84u;
    if (inited < 0) {
        const char *oe = getenv("HLE_ENTRY_OBJ");
        const char *fe = getenv("HLE_ENTRY_FN");
        if (oe != NULL && *oe != 0) robj = (uint32_t)strtoul(oe, NULL, 16);
        if (fe != NULL && *fe != 0) rfn = (uint32_t)strtoul(fe, NULL, 16);
        inited = 1;
    }
    if (c == NULL) return;
    {
        uint32_t sv[16];
        memcpy(sv, c->r, sizeof(sv));
        c->r[0] = robj;
        c->r[13] = 0x0CF0A000u;
        arm_call_guest_pub(c, rfn);
        memcpy(c->r, sv, sizeof(sv));
    }
}

void hle_2d_stub(ArmCpu *c) {    uint32_t *buf;
    uint32_t save[4];
    uint32_t ent, idx = 0, qx, qy, qw, qh;
    uint32_t bound = 0, bsrc0 = 0x1E000000u, bsrc1 = 0x1E100000u;
    int fa_asset = 0;
    if (c == NULL || g_hle_gsp_handle == 0) return;
    /* Wurde das Titel-Asset ueber den Engine-FA-Reader (0x280744) in den
       Gast-RAM geladen? Dann direkt aus diesen Bytes dekodieren, statt
       out/menu_bottom.bin per Host-fopen zu lesen. */
    {
        static int asset_dec = 0;
        int dec_now = asset_dec;
        if (!asset_dec && c->mem_size > HLE_ASSET_CAP + 0x48u &&
            memcmp(c->mem + HLE_ASSET_CAP, "IMGC00", 6) == 0) {
            int dw = 0, dh = 0;
            if (imgc_decode(c->mem + HLE_ASSET_CAP, HLE_ASSET_CAP_MAX,
                            c->mem + 0x1E100000u, &dw, &dh) == 0 &&
                dw == 320 && dh == 240) {
                asset_dec = 1;
                dec_now = 1;
                printf("[engine2d] title_b_bg aus FA-Read dekodiert (%dx%d)\n",
                       dw, dh);
                fflush(stdout);
            }
        }
        fa_asset = dec_now;
    }
    /* 32-bpp-Assets (A,B,G,R) aus demselben FA-Puffer dekodieren und zum
       Abgleich gegen den Python-Dekoder dumpen. */
    {
        static int asset32 = 0;
        if (!asset32 && c->mem_size > HLE_ASSET_CAP + 0x48u &&
            memcmp(c->mem + HLE_ASSET_CAP, "IMGC00", 6) == 0 &&
            c->mem[HLE_ASSET_CAP + 0x0Du] == 32u) {
            int dw = 0, dh = 0;
            if (imgc_decode(c->mem + HLE_ASSET_CAP, HLE_ASSET_CAP_MAX,
                            c->mem + 0x1E700000u, &dw, &dh) == 0) {
                FILE *f;
                asset32 = 1;
                printf("[engine2d] 32bpp aus FA-Read dekodiert (%dx%d)\n",
                       dw, dh);
                fflush(stdout);
                f = fopen("out/engine_32bpp.bin", "wb");
                if (f != NULL) {
                    fwrite(c->mem + 0x1E700000u, 1u,
                           (size_t)dw * (size_t)dh * 3u, f);
                    fclose(f);
                }
            }
        }
    }
    /* Der Engine-Renderer setzt [0x3b0] am Frame-Ende auf 0; fuer den Draw
       den gebundenen Texture-State wieder einsetzen und daraus lesen. */
    if (g_bound_tex != 0u)
        memcpy(c->mem + 0x0040ABB8u, &g_bound_tex, 4);
    memcpy(&bound, c->mem + 0x0040ABB8u, 4);
    if (bound != 0u) {
        memcpy(&bsrc0, c->mem + bound + 0x40u, 4);
        memcpy(&bsrc1, c->mem + bound + 0x44u, 4);
    }
    {
        static int use_logged = 0;
        if (!use_logged && bound != 0u) {
            use_logged = 1;
            printf("[engine2d] draw konsumiert tex=%08X src0=%08X src1=%08X\n",
                   bound, bsrc0, bsrc1);
            fflush(stdout);
        }
    }
    ent = c->r[0];
    int node_draw = 0;
    uint32_t node_type = 0;
    {
        uint32_t node = 0;
        static int mnlog = 0;
        if (ent >= 0x00100000u && ent + 8u <= c->mem_size)
            memcpy(&node, c->mem + (ent & 0x1FFFFFFFu) + 4u, 4);
        if (node >= 0x00100000u && node < 0x20000000u &&
            node != 0x003CA494u) {
            node_draw = 1;
            memcpy(&node_type, c->mem + (node & 0x1FFFFFFFu) + 8u, 4);
        }
        if (!mnlog && node_draw) {
            uint32_t ty = node_type, f1c = 0, f20 = 0, f24 = 0, n0 = 0;
            mnlog = 1;
            memcpy(&n0, c->mem + (node & 0x1FFFFFFFu), 4);
            if (getenv("HLE_NODE_POKE0") != NULL && n0 == 0u) {
                uint32_t pv = (uint32_t)strtoul(getenv("HLE_NODE_POKE0"),
                                                NULL, 16);
                memcpy(c->mem + (node & 0x1FFFFFFFu), &pv, 4);
                n0 = pv;
                printf("[engine2d] node[0] -> %08X\n", pv);
                fflush(stdout);
            }
            memcpy(&f1c, c->mem + (node & 0x1FFFFFFFu) + 0x1Cu, 4);
            memcpy(&f20, c->mem + (node & 0x1FFFFFFFu) + 0x20u, 4);
            memcpy(&f24, c->mem + (node & 0x1FFFFFFFu) + 0x24u, 4);
            printf("[engine2d] menu-node r0=%08X node=%08X type=%08X "
                   "node0=%08X fn1c=%08X fn20=%08X fn24=%08X -> full-screen\n",
                   ent, node, ty, n0, f1c, f20, f24);
            fflush(stdout);
            if (getenv("HLE_NODE_VT") != NULL) {
                uint32_t vt = (uint32_t)strtoul(getenv("HLE_NODE_VT"),
                                                NULL, 16);
                uint32_t fn = 0, old0 = 0, sv[16];
                uint32_t tgt = (getenv("HLE_NODE_ENT") != NULL)
                                   ? ent : node;
                memcpy(&old0, c->mem + (tgt & 0x1FFFFFFFu), 4);
                memcpy(c->mem + (tgt & 0x1FFFFFFFu), &vt, 4);
                memcpy(&fn, c->mem + (vt & 0x1FFFFFFFu) + 8u, 4);
                printf("[engine2d] node vt tgt=%08X -> %08X fn+8=%08X\n",
                       tgt, vt, fn);
                fflush(stdout);
                memcpy(sv, c->r, sizeof(sv));
                c->r[0] = tgt;
                c->r[1] = 0u; c->r[2] = 0u; c->r[3] = 0u;
                c->r[13] = 0x0CF0A000u;
                if (fn >= 0x00100000u && fn < 0x00500000u)
                    arm_call_guest_pub(c, fn);
                printf("[engine2d] node vt fn -> r0=%u\n", c->r[0]);
                fflush(stdout);
                memcpy(c->r, sv, sizeof(sv));
                memcpy(c->mem + (tgt & 0x1FFFFFFFu), &old0, 4);
            }
            if (getenv("HLE_NODE_CALL") != NULL) {
                uint32_t sv[16];
                memcpy(sv, c->r, sizeof(sv));
                c->r[0] = node;
                c->r[1] = 0u; c->r[2] = 0u; c->r[3] = 0u;
                c->r[13] = 0x0CF0A000u;
                printf("[engine2d] call node+0x1C fn=%08X\n", f1c);
                fflush(stdout);
                arm_call_guest_pub(c, f1c);
                printf("[engine2d] node+0x1C ret r0=%08X\n", c->r[0]);
                fflush(stdout);
                memcpy(c->r, sv, sizeof(sv));
            }
            if (getenv("HLE_NODE_CALL2") != NULL && f20 >= 0x00100000u) {
                uint32_t sv[16];
                memcpy(sv, c->r, sizeof(sv));
                c->r[0] = node;
                c->r[1] = 0u;
                c->r[2] = 0u;
                c->r[3] = 0u;
                c->r[13] = 0x0CF0A000u;
                printf("[engine2d] call node+0x20 fn=%08X\n", f20);
                fflush(stdout);
                arm_call_guest_pub(c, f20);
                printf("[engine2d] node+0x20 ret r0=%08X\n", c->r[0]);
                fflush(stdout);
                memcpy(c->r, sv, sizeof(sv));
            }
        }
    }
    if (ent >= g_ring_arr && ((ent - g_ring_arr) / 0xB4u) < 4u)
        idx = (ent - g_ring_arr) / 0xB4u;
    int xq_draw = 0;
    if (node_draw && getenv("HLE_XQ_DRAW") != NULL) {
        static int xq_loaded = 0;
        if (!xq_loaded) {
            FILE *f = fopen("out/menu_logo_dec.bin", "rb");
            xq_loaded = 1;
            if (f) {
                unsigned char src[2048];
                size_t n = fread(src, 1, sizeof(src), f);
                fclose(f);
                if (n >= 2048u) {
                    uint32_t i;
                    for (i = 0; i < 64u * 32u; i++) {
                        unsigned char v = src[i];
                        c->mem[0x1E200000u + i * 3u + 0u] = v;
                        c->mem[0x1E200000u + i * 3u + 1u] = v;
                        c->mem[0x1E200000u + i * 3u + 2u] = v;
                    }
                    printf("[engine2d] xq texture 64x32 @1E200000\n");
                    fflush(stdout);
                }
            }
        }
        xq_draw = 1;
    }
    qx = (idx & 1u) ? 200u : 0u;
    qy = (idx & 2u) ? 120u : 0u;
    qw = 200u; qh = 120u;
    {
        static int spr_bound = 0;
        if (!spr_bound) {
            uint32_t sv[16];
            spr_bound = 1;
            memcpy(sv, c->r, sizeof(sv));
            c->r[0] = 0u;
            c->r[1] = 0x0CFE7000u;
            c->r[13] = 0x0CF0A000u;
            arm_call_guest_pub(c, 0x002812ECu);
            {
                uint32_t rr = c->r[0];
                memcpy(c->r, sv, sizeof(sv));
                printf("[tex] 0x2812EC(sprite) -> r0=%u\n", rr);
            }
            fflush(stdout);
        }
    }
    buf = (uint32_t *)(c->mem + c->tpidruro + 0x80u);
    memcpy(save, buf, sizeof(save));
    for (uint32_t s = 0; s < 2u; s++) {
        uint32_t fa = hle_gsp_fb_addr((int)s);
        uint32_t w = (s == 1u) ? 320u : 400u;
        uint32_t lx = (idx & 1u) ? w / 2u : 0u;
        uint32_t lw = (idx & 1u) ? w - w / 2u : w / 2u;
        uint32_t ly = (idx & 2u) ? 120u : 0u;
        uint32_t lh = 120u;
        uint32_t list[9];
        uint32_t src = (s == 1u) ? bsrc1 : bsrc0;
        uint32_t srcW = w, srcH = 240u;
        int full_src = 0;
        if (xq_draw) { src = 0x1E200000u; srcW = 64u; srcH = 32u; }
        if (fa_asset && s == 0u && !xq_draw && !node_draw) {
            full_src = 1;
            lx = 0u; lw = 400u; ly = 0u; lh = 240u;
            src = 0x1E100000u; srcW = 320u; srcH = 240u;
        }
        if (node_draw) {
            lx = 0u; lw = w; ly = 0u; lh = 240u;
        }
        {
#ifdef _MSC_VER
            static int noblit = -1;
#else
            static int noblit = -1;
#endif
            if (noblit < 0)
                noblit = (getenv("HLE_2D_NO_BLIT") != NULL);
        if (!noblit) {
        list[0] = 0x070FFFFFu; list[1] = 7u;
        list[2] = lx; list[3] = ly; list[4] = lw; list[5] = lh;
        list[6] = (node_draw || full_src)
                      ? src
                      : src + ((idx & 2u) ? 120u : 0u) * w * 3u +
                            ((idx & 1u) ? (w / 2u) : 0u) * 3u;
        list[7] = srcW; list[8] = srcH;
        memcpy(c->mem + 0x0CFE6000u, list, sizeof(list));
        uint32_t listbytes = 36u;
        if (xq_draw && s == 0u) {
            uint32_t l2[9];
            l2[0] = 0x070FFFFFu; l2[1] = 7u;
            l2[2] = 50u;  l2[3] = 70u;
            l2[4] = 300u; l2[5] = 100u;
            l2[6] = 0x1E200000u; l2[7] = 64u; l2[8] = 32u;
            memcpy(c->mem + 0x0CFE6000u + 36u, l2, sizeof(l2));
            listbytes = 72u;
        }
        gpu_set_fb_size(fa, (int)w, 240);
        buf[0] = (0x0006u << 16) | (2u << 6);
        buf[1] = 0x0CFE6000u;
        buf[2] = listbytes;
        buf[3] = 0u;
        dispatch_ipc(g_hle_gsp_handle);
        buf[0] = (0x000Cu << 16);
        buf[1] = 0u;
        dispatch_ipc(g_hle_gsp_handle);
        }
        }
    }
    memcpy(buf, save, sizeof(save));
    printf("[engine2d] Sprite-Entry idx=%u submitted (lr=%08X)\n", idx, c->r[14]);
    fflush(stdout);
}

static void hle_install_2d_entry(ArmCpu *c) {
    if (getenv("HLE_NO_2D_INSTALL") != NULL) return;
    uint32_t arr = 0, stub = 0x0CFE1000u, list = 0x0CFE2000u;
    uint32_t entry = 0x0CFE3000u, vt = 0x0CFE3100u, ctx = 0x0CFE3200u;
    uint32_t code[18];
    uint16_t zero16 = 0, one16 = 1;
    uint32_t one32 = 1;
    if (c == NULL) return;
    /* Stub: SetCommandList(list,28) + TriggerCmdReqQueue. */
    code[0]  = 0xE92D4010u;   /* push {r4,lr} */
    code[1]  = 0xEE1D4F70u;   /* mrc p15,0,r4,c13,c0,3 */
    code[2]  = 0xE59F0038u;   /* ldr r0, [pc,#0x38] -> 0x00060040 */
    code[3]  = 0xE5A40080u;   /* str r0, [r4,#0x80]! */
    code[4]  = 0xE59F1034u;   /* ldr r1, [pc,#0x34] -> list */
    code[5]  = 0xE5841004u;   /* str r1, [r4,#4] */
    code[6]  = 0xE59F1030u;   /* ldr r1, [pc,#0x30] -> 28 */
    code[7]  = 0xE5841008u;   /* str r1, [r4,#8] */
    code[8]  = 0xE59F002Cu;   /* ldr r0, [pc,#0x2c] -> handle-global */
    code[9]  = 0xE5900000u;   /* ldr r0, [r0] */
    code[10] = 0xEF000032u;   /* svc 0x32 */
    code[11] = 0xE59F0024u;   /* ldr r0, [pc,#0x24] -> 0x000C0000 */
    code[12] = 0xE5840000u;   /* str r0, [r4] */
    code[13] = 0xE59F0018u;   /* ldr r0, [pc,#0x18] -> handle-global */
    code[14] = 0xE5900000u;   /* ldr r0, [r0] */
    code[15] = 0xEF000032u;   /* svc 0x32 */
    code[16] = 0xE3A00000u;   /* mov r0,#0 */
    code[17] = 0xE8BD8010u;   /* pop {r4,pc} */
    memcpy(c->mem + stub, code, sizeof(code));
    memcpy(c->mem + stub + 0x48u, (uint32_t[]){0x00060040u, list, 28u,
                                               0x003D9098u, 0x000C0000u},
           sizeof(uint32_t) * 5u);
    /* Der Stub liest den Client-Slot [0x3D9098]; dort muss das echte
       gsp::Gpu-Handle stehen, sonst geht svc 0x32 an einen falschen Dienst. */
    if (g_hle_gsp_handle != 0) {
        uint32_t gh = g_hle_gsp_handle;
        memcpy(c->mem + 0x003D9098u, &gh, 4);
    }
    /* Echte Spiel-Assets (aus lt6_b.fa/menu extrahiert) in den Gast-RAM
       laden und als BlitRGB-Kommandoliste ablegen. */
    {
        static unsigned char *topimg = NULL, *botimg = NULL;
        static int imgs_tried = 0;
        static int no_asset = -1;
        if (no_asset < 0)
            no_asset = (getenv("HLE_2D_NO_ASSET") != NULL);
        if (!imgs_tried && !no_asset) {
            FILE *f;
            const char *texenv = getenv("HLE_MENU_TEX");
            const char *texpath = (texenv != NULL && *texenv != 0)
                                      ? texenv : "out/menu_top.bin";
            int fa_bottom = (getenv("HLE_XQ_ASSET") != NULL);
            imgs_tried = 1;
            topimg = (unsigned char *)calloc(1, 400u * 240u * 3u);
            botimg = (unsigned char *)calloc(1, 320u * 240u * 3u);
            f = fopen(texpath, "rb");
            if (f) { if (fread(topimg, 1, 400u * 240u * 3u, f) != 400u * 240u * 3u) memset(topimg, 0, 400u*240u*3u); fclose(f); }
            if (!fa_bottom) {
                const char *btenv = getenv("HLE_MENU_TEX_BOT");
                const char *btpath = (btenv != NULL && *btenv != 0)
                                         ? btenv : "out/menu_bottom.bin";
                f = fopen(btpath, "rb");
                if (f) { if (fread(botimg, 1, 320u * 240u * 3u, f) != 320u * 240u * 3u) memset(botimg, 0, 320u*240u*3u); fclose(f); }
            }
            memcpy(c->mem + 0x1E000000u, topimg, 400u * 240u * 3u);
            if (!fa_bottom)
                memcpy(c->mem + 0x1E100000u, botimg, 320u * 240u * 3u);
            printf("[engine] Menue-Assets geladen (top=0D000000 bottom=%s)\n",
                   fa_bottom ? "FA-Read" : "0D050000");
            fflush(stdout);
        }
    }
    {
        uint32_t lt[9], lb[9];
        lt[0] = 0x070FFFFFu; lt[1] = 7u;
        lt[2] = 0u; lt[3] = 0u; lt[4] = 400u; lt[5] = 240u;
        lt[6] = 0x1E000000u; lt[7] = 400u; lt[8] = 240u;
        lb[0] = 0x070FFFFFu; lb[1] = 7u;
        lb[2] = 0u; lb[3] = 0u; lb[4] = 320u; lb[5] = 240u;
        lb[6] = 0x1E100000u; lb[7] = 320u; lb[8] = 240u;
        memcpy(c->mem + list, lt, sizeof(lt));
        memcpy(c->mem + list + 0x100u, lb, sizeof(lb));
    }
    /* Entry + Vtable + aktiver Listenkontext. */
    memcpy(c->mem + entry, &vt, 4);
    memcpy(c->mem + vt + 8u, &stub, 4);
    /* Optional: echte Engine-Draw-Methode als Ring-Eintrag (statt HLE-Stub).
       Ein kleiner ARM-Stub setzt r0 = Scene-Objekt und springt in die
       Engine-Funktion (Default 0x0036C6E8). */
    {
        static int real_entry = -1;
        if (real_entry < 0)
            real_entry = (getenv("HLE_REAL_ENTRY") != NULL);
        if (real_entry) {
            uint32_t rfn = 0x0036C6E8u;
            uint32_t robj = 0x003CA494u;
            uint32_t rst = 0x0CFE4000u;
            const char *fe = getenv("HLE_ENTRY_FN");
            const char *oe = getenv("HLE_ENTRY_OBJ");
            uint32_t code2[6];
            if (fe != NULL && *fe != 0) rfn = (uint32_t)strtoul(fe, NULL, 16);
            if (oe != NULL && *oe != 0) robj = (uint32_t)strtoul(oe, NULL, 16);
            code2[0] = 0xE59F0008u;   /* ldr r0,[pc,#8]  -> Objekt */
            code2[1] = 0xE59F1008u;   /* ldr r1,[pc,#8]  -> fn */
            code2[2] = 0xE12FFF11u;   /* bx r1 */
            code2[3] = 0xE1A00000u;   /* nop */
            code2[4] = robj;          /* Draw-Objekt */
            code2[5] = rfn;
            memcpy(c->mem + rst, code2, sizeof(code2));
            memcpy(c->mem + vt + 8u, &rst, 4);
        }
    }
    memcpy(c->mem + ctx + 4u, &one32, 4);
    memcpy(c->mem + 0x004088A4u, &ctx, 4);
    /* Ring: array[0] = entry, cursor=0, wrap=0, count=1. */
    memcpy(&arr, c->mem + 0x0040ABC0u, 4);
    if (arr == 0u) arr = 0x14004440u;
    {
        uint32_t stride = 0xB4u;
        memcpy(c->mem + 0x0040ABC0u, &arr, 4);
        memcpy(c->mem + 0x0040ABC4u, &stride, 4);
    }
    /* Echtes Texture-Objekt bauen und ueber 0x132370/0x27B34C binden. */
    {
        static int bind_done = 0;
        if (!bind_done) {
            uint32_t sprite = 0x0CFE7000u, texobj = 0x0CFE7100u;
            uint32_t desc = sprite + 0x70u;
            uint8_t one = 1, zero = 0;
            uint32_t src = 0x1E000000u, src1 = 0x1E100000u, srclen = 400u * 240u * 3u;
            uint32_t scratch = 0x0CFE7200u;
            uint32_t save[16];
            bind_done = 1;
            memcpy(c->mem + sprite + 4u, &texobj, 4);
            memcpy(c->mem + sprite + 0x56u, &one, 2);
            memcpy(c->mem + texobj + 0x40u, &src, 4);
            memcpy(c->mem + texobj + 0x44u, &src1, 4);
            memcpy(c->mem + texobj + 0x4Cu, &srclen, 4);
            memcpy(c->mem + texobj + 0x58u, &one, 1);
            memcpy(c->mem + texobj + 0x59u, &one, 1);
            memcpy(c->mem + texobj + 0x5Au, &one, 1);
            memcpy(c->mem + texobj + 0x5Cu, &zero, 1);
            memcpy(c->mem + texobj + 0x5Du, &zero, 1);
            memcpy(c->mem + texobj + 0x60u, &scratch, 4);
            memcpy(c->mem + desc + 0x1Fu, &one, 1);
            memcpy(c->mem + desc + 0x10u, &src, 4);
            memcpy(c->mem + desc + 0x14u, &srclen, 4);
            memcpy(save, c->r, sizeof(save));
            c->r[0] = texobj;
            c->r[13] = 0x0CF0A000u;
            arm_call_guest_pub(c, 0x00132370u);
            printf("[tex] 0x132370(tex=%08X) -> r0=%u\n", texobj, c->r[0]);
            c->r[0] = 0x00408808u;
            c->r[1] = texobj;
            c->r[2] = desc;
            c->r[3] = 1u;
            c->r[13] = 0x0CF0A000u;
            arm_call_guest_pub(c, 0x0027B34Cu);
            printf("[tex] 0x27B34C bind -> r0=%u\n", c->r[0]);
            memcpy(c->mem + 0x0040ABB8u, &texobj, 4);
            {
                uint32_t cur = 0;
                memcpy(&cur, c->mem + 0x0040ABB8u, 4);
                printf("[tex] current bound texture [3b0]=%08X (tex=%08X)\n", cur, texobj);
            }
            /* PICA-Texturzustand aus demselben Objekt vorbelegen: Quelle
               +0x40 = 0x1E000000, 400x240, RGB8. Damit kann der PICA-Draw
               (0x22F) die Engine-Textur sampeln. */
            gpu_pica_bind_texture(src, 400, 240, 1);
            printf("[pica] texture bound src=%08X 400x240 RGB8\n", src);
            memcpy(c->r, save, sizeof(save));
            fflush(stdout);
        }
    }
    g_ring_arr = arr;
    for (uint32_t i = 0; i < 4u; i++) {
        uint32_t base = (arr + i * 0xB4u) & 0x1FFFFFFFu;
        uint32_t obj = 0x003CA494u;
        const char *eo = getenv("HLE_ENTRY_OBJ");
        if (eo != NULL && *eo != 0) obj = (uint32_t)strtoul(eo, NULL, 16);
        memcpy(c->mem + base, &vt, 4);
        memcpy(c->mem + base + 4u, &obj, 4);
    }
    memcpy(c->mem + 0x0040ABC8u, &zero16, 2);
    memcpy(c->mem + 0x0040ABCCu, &zero16, 2);
    {
        uint16_t four16 = 4;
        memcpy(c->mem + 0x0040ABCEu, &four16, 2);
    }
    {
        static int announced = 0;
        if (!announced) {
            announced = 1;
            {
                uint32_t slot = 0;
                memcpy(&slot, c->mem + 0x003D9098u, 4);
                printf("[engine] 2D-Displaylistentry installiert (entry=%08X arr=%08X gsp_h=%u slot=%08X)\n",
                       entry, arr, g_hle_gsp_handle, slot);
            }
            fflush(stdout);
        }
    }
}

/* Engine-Frame: fn_0028F084(0x003EC028) ist der echte Frame-Dispatcher des
   Spiels (SetBufferSwap -> Scene-Update -> Scene-Draw -> GX-Flush). Er wird
   hier wie ein VBlank-Callback regelmaessig aufgerufen, damit die Szene
   ohne Host-Blit selbst rendert. Die Gates sind Felder, die im Port wegen
   des unvollstaendigen Loaders nie gesetzt werden:
     0x40B3A0 (App bereit, gelesen von 0x11AF10 +0x4C)
     0x3D8D5D (App laeuft, gelesen von 0x11A270 +5)
     0x3EE16C (Scene+0x2144, Init-Schleife 0x101508)
     0x40B4CC (Frame-Callback 0x00101750)
     0x4237A3 (Renderer+0x15A3, Renderer-Enable)
   Nur Flags, keine Bilddaten: die Pixel entstehen ausschliesslich im
   Gast-Renderer. */
static uint32_t g_poke_a[64], g_poke_v[64];
static int g_poke_n = -1;

static void hle_apply_pokes(ArmCpu *c) {
    if (c == NULL) return;
    if (g_poke_n < 0) {
        const char *pv = getenv("HLE_POKE");
        g_poke_n = 0;
        if (pv != NULL) {
            while (*pv != 0 && g_poke_n < 64) {
                char *end = NULL;
                uint32_t a = (uint32_t)strtoul(pv, &end, 16);
                if (end == pv || *end != '=') break;
                pv = end + 1;
                uint32_t v = (uint32_t)strtoul(pv, &end, 16);
                if (end == pv) break;
                g_poke_a[g_poke_n] = a; g_poke_v[g_poke_n] = v;
                g_poke_n++;
                pv = end;
                while (*pv == ',' || *pv == ' ') pv++;
            }
            if (g_poke_n > 0) {
                printf("[poke] %d Wert(e) aktiv\n", g_poke_n);
                fflush(stdout);
            }
        }
    }
    for (int i = 0; i < g_poke_n && i < 64; i++) {
        if (g_poke_a[i] + 4u <= c->mem_size)
            memcpy(c->mem + g_poke_a[i], &g_poke_v[i], 4);
    }
}

static void hle_engine_frame(ArmCpu *c) {
    static int on = -1, elog = -1;
    static unsigned calls = 0;
    uint32_t sr[16];
    extern int arm_fa_ready;
    if (!c) return;
    if (on < 0) {
        on = (getenv("HLE_ENGINE_FRAME") != NULL);
        elog = (getenv("HLE_ENGINE_LOG") != NULL);
    }
    if (!on) return;
    /* Generisches Speicher-Poke fuer die Gate-Bisektion:
       HLE_POKE=addr=value[,addr=value] (32-Bit, hex). Wird pro Aufruf
       erneut angewendet, damit ein spaeterer Reset die Werte nicht kippt. */
    hle_apply_pokes(c);
    /* Draw-Liste/Pool bei jedem Engine-Frame absichern: Menue-Loop und
       Builder nullen die Felder sonst zwischen zwei hle_tick-Aufrufen. */
    {
        static int dl2 = -1;
        if (dl2 < 0) dl2 = (getenv("HLE_DRAW_LIST") != NULL);
        if (dl2) {
            uint32_t arr = 0x0CFF0000u, pool = 0x0CFF1000u, stride = 4u;
            uint16_t cap = 0x100u, one = 1, z16 = 0, cur16 = 0;
            uint32_t cur32 = 0;
            memcpy(&cur32, c->mem + 0x003EE198u, 4);
            if (cur32 == 0u) memcpy(c->mem + 0x003EE198u, &arr, 4);
            memcpy(&cur16, c->mem + 0x003EE1A2u, 2);
            if (cur16 == 0u) memcpy(c->mem + 0x003EE1A2u, &cap, 2);
            memcpy(&cur16, c->mem + 0x003EE196u, 2);
            if (cur16 == 0u) memcpy(c->mem + 0x003EE196u, &one, 2);
            memcpy(&cur32, c->mem + 0x003EE188u, 4);
            if (cur32 == 0u) {
                memcpy(c->mem + 0x003EE188u, &pool, 4);
                memcpy(c->mem + 0x003EE18Cu, &stride, 4);
                memcpy(c->mem + 0x003EE190u, &z16, 2);
                memcpy(c->mem + 0x003EE192u, &cap, 2);
                memcpy(c->mem + 0x003EE194u, &z16, 2);
                for (uint32_t i = 0; i < 0x100u; i++) {
                    uint32_t o = 0x0CFF2000u + i * 0x100u;
                    if (getenv("HLE_POOL_MENUOBJ")) o = 0x003EE12Cu;
                    memcpy(c->mem + ((pool + i * 4u) & 0x1FFFFFFFu), &o, 4);
                }
            }
            {
                static int pmo = -1;
                if (pmo < 0) pmo = (getenv("HLE_POOL_MENUOBJ") != NULL);
                if (pmo) {
                    uint32_t o = 0x003EE12Cu;
                    for (uint32_t i = 0; i < 0x100u; i++)
                        memcpy(c->mem + ((pool + i * 4u) & 0x1FFFFFFFu), &o, 4);
                }
            }
            /* Snapshot: der Menue-Drain nullt Array/Count zwischen zwei
               Pumps; hier wird die zuletzt gebaute Liste konserviert und
               bei Bedarf restauriert (nur Pointer, 4 Byte je Eintrag). */
            {
                static uint32_t snap[256];
                static uint32_t snapn = 0;
                uint32_t a2 = 0, n2 = 0;
                memcpy(&a2, c->mem + 0x003EE198u, 4);
                memcpy(&n2, c->mem + 0x003EE1A0u, 2);
                if (a2 != 0u && n2 != 0u) {
                    uint32_t k, nn = n2 > 256u ? 256u : n2;
                    for (k = 0; k < nn; k++)
                        memcpy(&snap[k],
                               c->mem + ((a2 + k * 4u) & 0x1FFFFFFFu), 4);
                    snapn = nn;
                } else if (snapn != 0u) {
                    uint16_t c2 = (uint16_t)snapn;
                    memcpy(c->mem + 0x003EE198u, &arr, 4);
                    memcpy(c->mem + 0x003EE1A0u, &c2, 2);
                    memcpy(c->mem + 0x003EE1A2u, &cap, 2);
                    for (uint32_t k = 0; k < snapn; k++)
                        memcpy(c->mem + ((arr + k * 4u) & 0x1FFFFFFFu),
                               &snap[k], 4);
                }
            }
        }
    }
    if (calls == 0) {
        uint32_t scene_obj = 0;
        memcpy(&scene_obj, c->mem + 0x003EC028u, 4);
        printf("[engine] fn_0028F084(003EC028) gestartet, [scene]=%08X\n", scene_obj);
        fflush(stdout);
    }
    /* Der Szenen-Konstruktor 0x381CE4 (-> 0x365228) wird im Port nie
       aufgerufen; ohne ihn ist [0x3EC028] = 0 und der Frame-Dispatcher
       hat kein Objekt. Nach dem Boot einmal nachziehen. */
    {
        static int ctor_tried = 0;
        uint32_t sc = 0;
        memcpy(&sc, c->mem + 0x003EC028u, 4);
        if (!ctor_tried && calls >= 20u && sc == 0u) {
            uint32_t cs[16];
            ctor_tried = 1;
            memcpy(cs, c->r, sizeof(cs));
            c->r[13] = 0x0CF0A000u;
            printf("[engine] Scene-Konstruktor 0x381CE4 aufrufen\n");
            fflush(stdout);
            arm_call_guest_pub(c, 0x00381CE4u);
            memcpy(c->r, cs, sizeof(cs));
            memcpy(&sc, c->mem + 0x003EC028u, 4);
            printf("[engine] nach Konstruktor [scene]=%08X ab=%d W:",
                   sc, c->aborted);
            for (int k = 0; k < 5; k++) {
                uint32_t w = 0;
                memcpy(&w, c->mem + 0x003EC028u + (uint32_t)k * 4u, 4);
                printf(" %08X", w);
            }
            printf("\n");
            fflush(stdout);
        }
    }
    {
        uint8_t one8 = 1;
        uint16_t one16 = 1;
        uint32_t cb = 0x00101750u;
        /* Init-Schleife 0x101508 erst freigeben, wenn das Scene-Objekt
           konstruiert ist; sonst laeuft 0x101F10 mit [scene]=0 in den
           SDK-Fehlerpfad (err:f). */
        {
            uint32_t sc0 = 0;
            uint16_t objc = one16;
            static int obj0 = -1;
            if (obj0 < 0) obj0 = (getenv("HLE_MENU_OBJ0") != NULL);
            if (obj0) objc = 0;
            memcpy(&sc0, c->mem + 0x003EC028u, 4);
            if (sc0 != 0u) memcpy(c->mem + 0x003EE16Cu, &objc, 2);
        }
        memcpy(c->mem + 0x0040B354u, &one8, 1);  /* 0x11AF10: [0x40B308+0x4C] */
        memcpy(c->mem + 0x0040B3A0u, &one8, 1);  /* 0x37FB1C: [0x40B354+0x4C] */
        memcpy(c->mem + 0x003D8D5Du, &one8, 1);
        memcpy(c->mem + 0x003D8D64u, &one8, 1);  /* 0x27CE44 (+0xC) */
        memcpy(c->mem + 0x003D8D65u, &one8, 1);  /* 0x27CE54 (+0xD) */
        memcpy(c->mem + 0x0040B4CCu, &cb, 4);
        memcpy(c->mem + 0x004237A3u, &one8, 1);
        /* SDK-Flag 0x3E7771 (gelesen von 0x139844 im GX-Submit) muss 1
           sein; das Wort ist im Port 0x00010000 statt 0x00010100. */
        {
            uint32_t sdkflag = 0x00010100u;
            memcpy(c->mem + 0x003E7770u, &sdkflag, 4);
        }
        memcpy(c->mem + 0x003EE196u, &one16, 2);  /* [menu+0x6A] */
    }
    memcpy(sr, c->r, sizeof(sr));
    c->r[0] = 0x003EC028u;
    c->r[1] = 0u; c->r[2] = 0u; c->r[3] = 0u;
    c->r[13] = 0x0CF0A000u;
    /* Menue-Ressourcen-Loader (0x28D424) fuer das Menue-Objekt 0x3EE12C
       anstossen, damit die 2D-Sprite-Liste gefuellt wird. */
    {
        static int res_on = -1;
        if (res_on < 0) res_on = (getenv("HLE_ENGINE_RES") != NULL);
        if (res_on) {
            uint32_t s3[16];
            memcpy(s3, c->r, sizeof(s3));
            c->r[0] = 0x003EE12Cu;
            c->r[13] = 0x0CF0A000u;
            arm_call_guest_pub(c, 0x0028D424u);
            memcpy(c->r, s3, sizeof(s3));
        }
    }
    /* Present: die Engine ruft im Normalfall gsp SetBufferSwap(screen=2)
       (beide Screens). Der Port-Handler setzt daraus screen=0/1 mit den
       Adressen aus dem GSP-Shared-Memory. Hier einmal pro Frame anstossen. */
    if (g_hle_gsp_handle != 0 && g_frame_cpu != NULL && (calls & 7u) == 0u) {
        uint32_t *buf = (uint32_t *)(c->mem + c->tpidruro + 0x80u);
        uint32_t save[4];
        memcpy(save, buf, sizeof(save));
        buf[0] = (0x0005u << 16) | (1u << 6);
        buf[1] = 2u;   /* beide Screens */
        dispatch_ipc(g_hle_gsp_handle);
        memcpy(buf, save, sizeof(save));
        /* Beide Framebuffer ueber den GPU-Pfad fuellen, solange die
           Menue-Sprite-Liste noch leer ist (sichtbarer Present-Beleg).
           HLE_PRESENT_FILL=0 schaltet den Platzhalter ab, damit ein echter
           Engine-PICA-Render im Screenshot sichtbar bleibt. */
        static int present_fill = -1;
        if (present_fill < 0) {
            const char *pf = getenv("HLE_PRESENT_FILL");
            present_fill = (pf != NULL && *pf != 0 && atoi(pf) == 0) ? 0 : 1;
        }
        for (uint32_t s = 0; s < 2u && present_fill; s++) {
            uint32_t fa = hle_gsp_fb_addr((int)s);
            int fw = (s == 1u) ? 320 : 400;
            uint32_t list[8];
            list[0] = 0x04204060u;      /* ClearColor */
            list[1] = 0x050FFFFFu;      /* FillRect */
            list[2] = 4u;
            list[3] = 0u; list[4] = 0u;
            list[5] = (uint32_t)fw; list[6] = 240u;
            memcpy(c->mem + 0x08004000u + s * 0x40u, list, sizeof(list));
            gpu_set_fb_size(fa, fw, 240);
            gpu_run_command_list(c, 0x08004000u + s * 0x40u, (uint32_t)sizeof(list));
        }
    }
    /* 2D-Ring vor dem Scene-Draw installieren: 0x1ED8FC setzt den Count am
       Frame-Ende auf 0, sonst sieht der Draw keine Eintraege. */
    {
        static int pre2d = -1;
        if (pre2d < 0) pre2d = (getenv("HLE_ENGINE_2D") != NULL);
        if (pre2d) hle_install_2d_entry(c);
    }
    /* Welt-/Renderer-Update fn_00285208 (-> 0x11AEF4 -> fn_0027AF48 mit
       Renderer 0x00422200) fuellt die Render-Units. Im Port ruft das
       Spiel diesen Pfad nie auf; fuer die Diagnose optional direkt. */
    /* Komplettes Menue-Setup fn_00100F98 (Einstieg des Spiels:
       0x100B4C -> 0x100F98 -> 0x1034F4 -> 0x108F70 -> 0x285208) einmal
       anstossen; Argument ist ein Byte-Struct mit Sprachindex [0]. */
    {
        static int menu = -1, menu_done = 0;
        if (menu < 0) menu = (getenv("HLE_ENGINE_MENU") != NULL);
        if (menu && !menu_done && calls >= 25u) {
            uint32_t arg = 0x0CFE9000u;
            const char *ma = getenv("HLE_MENU_ARG");
            uint32_t sm[16];
            uint8_t lang = 0;
            if (ma != NULL && *ma != 0) arg = (uint32_t)strtoul(ma, NULL, 16);
            menu_done = 1;
            memcpy(c->mem + arg, &lang, 1);
            /* Init-Gate 0x10FC9C: liest *(s8*)0x3D8CE0; muss != 0 sein,
               sonst bricht der Menue-Init-Pfad bei 0x1036C8 ab. */
            {
                uint8_t g1 = 1;
                memcpy(c->mem + 0x003D8CE0u, &g1, 1);
            }
            /* Optional: Scene-Builder fn_0035FF14 mit Scene/World aufrufen,
               damit das eingebettete Menue-Objekt aufgebaut wird. */
            {
                const char *sb = getenv("HLE_SCENEBUILD");
                if (sb != NULL && *sb != 0) {
                    uint32_t sarg = 0x003EC028u;
                    const char *sa = getenv("HLE_SB_ARG");
                    uint32_t sv[16];
                    int snew = (getenv("HLE_SB_NEW") != NULL);
                    if (sa != NULL && *sa != 0)
                        sarg = (uint32_t)strtoul(sa, NULL, 16);
                    if (snew) {
                        /* Frisches, genulltes Scene-Objekt mit Klassen-
                           Vtable anlegen und den Builder darauf ansetzen. */
                        sarg = 0x0D200000u;
                        memset(c->mem + sarg, 0, 0x4000u);
                    }
                    memcpy(sv, c->r, sizeof(sv));
                    /* Scene-Klassen-Vtable setzen (Tabelle @0x3CA370,
                       Builder ist Methode +0x04). */
                    if (sarg >= 0x00100000u && sarg < 0x20000000u) {
                        uint32_t vt = 0x003CA370u;
                        memcpy(c->mem + (sarg & 0x1FFFFFFFu), &vt, 4);
                    }
                    c->r[0] = sarg;
                    c->r[1] = 0u;
                    /* Eigenes, grosses Stack-Areal (der Builder braucht
                       mehr als die 4 KB bei 0x0CF0A000 und darf nicht mit
                       dem Spiel-Stack kollidieren). */
                    c->r[13] = 0x0D840000u;
                    printf("[scenebuild] fn_0035F3B4(%08X)\n", sarg);
                    fflush(stdout);
                    arm_call_guest_pub(c, 0x0035F3B4u);
                    memcpy(c->r, sv, sizeof(sv));
                    printf("[scenebuild] zurueck\n");
                    fflush(stdout);
                }
            }
            /* Menue-Node-Liste (menu+0x6C/+0x74) mit einem Testobjekt
               fuellen; Typ-Byte an obj+8 waehlt den 0x28D424-Zweig (3..7). */
            {
                const char *nt = getenv("HLE_MENU_NODE");
                if (nt != NULL && *nt != 0) {
                    uint32_t arr = 0x0CFF0000u, obj = 0x0CFF2100u;
                    uint16_t one = 1;
                    uint8_t ty = (uint8_t)atoi(nt);
                    memcpy(c->mem + 0x003EE198u, &arr, 4);
                    memcpy(c->mem + 0x003EE1A0u, &one, 2);
                    memcpy(c->mem + 0x0CFF0000u, &obj, 4);
                    memset(c->mem + 0x0CFF2100u, 0, 0x20);
                    memcpy(c->mem + 0x0CFF2108u, &ty, 1);
                    /* Objektliste menu+0x38/+0x40 mit dem Engine-State-
                       Objekt (0x3C6FE4, vtable-Table) fuellen. */
                    {
                        uint32_t oarr = 0x0CFF0200u, oobj = 0x003C6FE4u;
                        const char *oe = getenv("HLE_MENU_OBJ");
                        if (oe != NULL && *oe != 0)
                            oobj = (uint32_t)strtoul(oe, NULL, 16);
                        memcpy(c->mem + 0x003EE164u, &oarr, 4);
                        memcpy(c->mem + 0x003EE16Cu, &one, 2);
                        memcpy(c->mem + 0x0CFF0200u, &oobj, 4);
                        printf("[menunode] obj=%08X in menu+0x38\n", oobj);
                    }
                    printf("[menunode] type=%u arr=%08X cnt=1\n", ty, arr);
                    fflush(stdout);
                }
            }
            memcpy(sm, c->r, sizeof(sm));
            c->r[0] = arg;
            c->r[1] = 0u;
            c->r[13] = 0x0CF0A000u;
            printf("[enginemenu] fn_00100F98(%08X) -> ...\n", arg);
            fflush(stdout);
            arm_call_guest_pub(c, 0x00100F98u);
            memcpy(c->r, sm, sizeof(sm));
            {
                uint32_t u = 0;
                memcpy(&u, c->mem + 0x00422200u + 0x1530u, 4);
                printf("[enginemenu] zurueck, units=%u\n", u);
                fflush(stdout);
            }
            /* Menue-Objekt 0x3EE12C: vtable + Sequenz-Namenstabelle
               (+0x68 = 0x3EE194, count u16 @ +0x68+8 = 0x3EE19C) und
               Node-/Objektlisten dumpen. */
            {
                uint32_t vt = 0, seqp = 0, nodea = 0, obja = 0;
                uint16_t seqc = 0, nodec = 0, objc = 0;
                memcpy(&vt, c->mem + 0x003EE12Cu, 4);
                memcpy(&seqp, c->mem + 0x003EE12Cu + 0x68u, 4);
                memcpy(&seqc, c->mem + 0x003EE12Cu + 0x6Cu, 2);
                memcpy(&nodea, c->mem + 0x003EE198u, 4);
                memcpy(&nodec, c->mem + 0x003EE1A0u, 2);
                memcpy(&obja, c->mem + 0x003EE164u, 4);
                memcpy(&objc, c->mem + 0x003EE16Cu, 2);
                printf("[menuobj] vt=%08X seqtab=%08X seqcnt=%u nodea=%08X "
                       "nodec=%u obja=%08X objc=%u\n",
                       vt, seqp, (unsigned)seqc, nodea, (unsigned)nodec,
                       obja, (unsigned)objc);
                if (seqp >= 0x00100000u && seqp < 0x20000000u && seqc > 0u) {
                    uint32_t k, n = seqc > 12u ? 12u : seqc;
                    printf("[menuobj] seq:");
                    for (k = 0; k < n; k++) {
                        uint32_t w0 = 0, w1 = 0;
                        memcpy(&w0, c->mem + (seqp & 0x1FFFFFFFu) + k * 8u, 4);
                        memcpy(&w1, c->mem + (seqp & 0x1FFFFFFFu) + k * 8u + 4u, 4);
                        printf(" [%u:%08X/%08X]", (unsigned)k, w0, w1);
                    }
                    printf("\n");
                }
                {
                    uint32_t k;
                    printf("[menuobj] raw3EE12C:");
                    for (k = 0; k < 0x24u; k++) {
                        uint32_t w = 0;
                        memcpy(&w, c->mem + 0x003EE12Cu + k * 4u, 4);
                        printf(" %08X", w);
                    }
                    printf("\n");
                }
                fflush(stdout);
            }
        }
    }
    {
        static int world = -1;
        if (world < 0) world = (getenv("HLE_ENGINE_WORLD") != NULL);
        uint32_t wsc = 0;
        memcpy(&wsc, c->mem + 0x003EC028u, 4);
        if (world && wsc != 0u) {
            uint32_t arg = 0x003EC028u;
            const char *wa = getenv("HLE_WORLD_ARG");
            uint32_t sw[16];
            if (wa != NULL && *wa != 0) arg = (uint32_t)strtoul(wa, NULL, 16);
            memcpy(sw, c->r, sizeof(sw));
            c->r[0] = arg;
            c->r[1] = 0u;
            c->r[13] = 0x0CF0A000u;
            arm_call_guest_pub(c, 0x00285208u);
            memcpy(c->r, sw, sizeof(sw));
            {
                static int lw = 0;
                if (!lw) {
                    uint32_t u = 0;
                    lw = 1;
                    memcpy(&u, c->mem + 0x00422200u + 0x1530u, 4);
                    printf("[engineworld] fn_00285208(arg=%08X) units=%u\n", arg, u);
                    fflush(stdout);
                }
            }
        }
    }
    /* Echten 3D-Scene-Renderer fn_001B9938 mit dem initialisierten
       Renderer-Objekt 0x00422200 aufrufen (PICA-Submit via gsp). */
    {
        static int three_d = -1;
        if (three_d < 0) three_d = (getenv("HLE_ENGINE_3D") != NULL);
        if (three_d) {
            uint32_t s3[16];
            /* fn_001B9938 bricht bei 0x1BA964 ab, wenn [0x3D8E6A] != 0
               oder ([0x3D94E8] & ~[0x3D94E9]) == 0. Genau diese beiden
               Gates oeffnen, damit der Renderer seine Liste submitted. */
            {
                uint8_t z = 0, one = 1;
                memcpy(c->mem + 0x003D8E6Au, &z, 1);
                memcpy(c->mem + 0x003D94E8u, &one, 1);
                memcpy(c->mem + 0x003D94E9u, &z, 1);
            }
            memcpy(s3, c->r, sizeof(s3));
            c->r[0] = 0x00422200u;
            c->r[13] = 0x0CF0A000u;
            arm_call_guest_pub(c, 0x001B9938u);
            memcpy(c->r, s3, sizeof(s3));
            {
                static int logged3 = 0;
                /* Der 3D-Renderer liest die Render-Unit-Liste aus dem
                   Scene-Objekt: Array scene+0x14F0, Count scene+0x1530,
                   Stride 0x20 (aus 0x1BA214..0x1BA288 / 0x1BA378).
                   Erst loggen, wenn der Scene-Konstruktor gelaufen ist. */
                uint32_t sc = 0;
                memcpy(&sc, c->mem + 0x003EC028u, 4);
                if (!logged3 && sc != 0u) {
                    /* Der Renderer-Arg ist 0x00422200 (nicht die Scene):
                       Unit-Array +0x14F0, Count +0x1530, Stride 0x20. */
                    uint32_t cnt = 0, n0 = 0, nt = 0, scnt = 0;
                    logged3 = 1;
                    memcpy(&cnt, c->mem + 0x00422200u + 0x1530u, 4);
                    memcpy(&n0, c->mem + 0x00422200u + 0x14F0u, 4);
                    memcpy(&nt, c->mem + 0x00422200u + 0x14F8u, 4);
                    memcpy(&scnt, c->mem + sc + 0x1530u, 4);
                    printf("[engine3d] renderer=00422200 units=%u node0=%08X "
                           "type=%08X (scene=%08X scene_units=%u)\n",
                           cnt, n0, nt, sc, scnt);
                    fflush(stdout);
                }
            }
        }
    }
    /* 2D-Scene-Renderer (globales Objekt 0x408808) direkt zeichnen. */
    {
        static int two_d = -1;
        if (two_d < 0) two_d = (getenv("HLE_ENGINE_2D") != NULL);
        if (two_d) {
            uint32_t s2[16];
            hle_install_2d_entry(c);
            /* Persistente Menue-Draw-Liste in den 2D-Renderer-Ring
               uebernehmen: Eintrag = {vtable 0CFE3100, node}. */
            {
                static int mring = -1;
                if (mring < 0) mring = (getenv("HLE_MENU_RING") != NULL);
                if (mring) {
                    uint32_t marr = 0, mnode = 0, ring = g_ring_arr;
                    uint16_t mcnt = 0;
                    memcpy(&marr, c->mem + 0x003EE198u, 4);
                    memcpy(&mcnt, c->mem + 0x003EE1A0u, 2);
                    if (marr != 0u && mcnt != 0u) {
                        uint32_t vt = 0x0CFE3100u, n = mcnt > 4u ? 4u : mcnt;
                        uint16_t n16 = (uint16_t)n;
                        for (uint32_t i = 0; i < n; i++) {
                            uint32_t base = (ring + i * 0xB4u) & 0x1FFFFFFFu;
                            memcpy(&mnode,
                                   c->mem + ((marr + i * 4u) & 0x1FFFFFFFu), 4);
                            if (mnode >= 0x00100000u && mnode < 0x20000000u)
                                memcpy(c->mem + (mnode & 0x1FFFFFFFu), &vt, 4);
                            memcpy(c->mem + base, &vt, 4);
                            memcpy(c->mem + base + 4u, &mnode, 4);
                        }
                        memcpy(c->mem + 0x0040ABCEu, &n16, 2);
                        printf("[menuring] %u Eintraege aus 0CFF0000 in Ring %08X\n",
                               n, ring);
                        fflush(stdout);
                    }
                }
            }
            static int act_done = 0;
            memcpy(s2, c->r, sizeof(s2));
            if (!act_done) {
                act_done = 1;
                c->r[0] = 0x00408808u;
                c->r[13] = 0x0CF0A000u;
                arm_call_guest_pub(c, 0x00366E38u);   /* 2D-Renderer-Ctor */
                c->r[0] = 0x00408808u;
                c->r[1] = 0x003EC248u;
                c->r[13] = 0x0CF0A000u;
                arm_call_guest_pub(c, 0x001ED64Cu);
            }
            c->r[13] = 0x0CF0A000u;
            arm_call_guest_pub(c, 0x00366B98u);
            memcpy(c->r, s2, sizeof(s2));
        }
    }
    /* Nur den Scene-Draw fn_00103D20 treiben (ohne den blockierenden
       Present-Pfad 0x28F084). */
    {
        static int draw2 = -1;
        if (draw2 < 0) draw2 = (getenv("HLE_ENGINE_DRAW") != NULL);
        if (draw2) {
            uint32_t s4[16];
            {
                static int vlogged = 0;
                if (!vlogged) {
                    uint32_t sc = 0, vt = 0, f38 = 0;
                    memcpy(&sc, c->mem + 0x003EC028u, 4);
                                        if (sc) { memcpy(&vt, c->mem + sc, 4); if (vt) memcpy(&f38, c->mem + vt + 0x38u, 4); }
                    printf("[draw2] scene=%08X vt=%08X fn+38=%08X\n", sc, vt, f38);
                    fflush(stdout);
                    vlogged = 1;
                }
            }
            memcpy(s4, c->r, sizeof(s4));
            uint32_t scdraw = 0;
            memcpy(&scdraw, c->mem + 0x003EC028u, 4);
            if (scdraw != 0u) {
                c->r[0] = 0x003EC028u;
                c->r[1] = 0u; c->r[2] = 0u; c->r[3] = 0u;
                c->r[13] = 0x0CF0A000u;
                arm_call_guest_pub(c, 0x00103D20u);
            }
            memcpy(c->r, s4, sizeof(s4));
        }
    }
    /* Standard: die Spiel-Hauptschleife (0x101F10) ruft 0x28F084 und
       0x103D20 selbst. Nur fuer Diagnose optional direkt treiben. */
    {
        static int drive = -1;
        if (drive < 0) drive = (getenv("HLE_ENGINE_DRIVE") != NULL);
        if (drive) {
            c->r[0] = 0x003EC028u;
            arm_call_guest_pub(c, 0x0028F084u);
            c->r[0] = 0x003EC028u;
            c->r[1] = 0u; c->r[2] = 0u; c->r[3] = 0u;
            c->r[13] = 0x0CF0A000u;
            arm_call_guest_pub(c, 0x00103D20u);
        }
    }
    memcpy(c->r, sr, sizeof(sr));
    calls++;
    if (elog && (calls <= 3 || (calls % 120u) == 0)) {
        uint32_t c4 = 0, fb0 = 0, fb1 = 0, dl = 0;
        memcpy(&c4, c->mem + 0x0040B4C4u, 4);
        memcpy(&dl, c->mem + 0x0040B4E0u, 4);
        fb0 = hle_gsp_fb_addr(0);
        fb1 = hle_gsp_fb_addr(1);
        uint32_t scv = 0, rv = 0, rg = 0, m59c = 0, f248 = 0;
        memcpy(&scv, c->mem + 0x003EC028u, 4);
        memcpy(&rv, c->mem + 0x00422200u, 4);
        memcpy(&rg, c->mem + 0x004237A3u, 1);
        memcpy(&m59c, c->mem + 0x003EC5C4u, 4);
        memcpy(&f248, c->mem + 0x003EC270u, 4);
        {
            uint16_t lc = 0;
            uint32_t r_vt = 0, r98 = 0, r9c = 0, r20c = 0, r_b4c = 0, r_b48 = 0;
            uint32_t r_3b0 = 0, r_3b4 = 0;
            memcpy(&lc, c->mem + 0x0040ABCEu, 2);
            memcpy(&r_vt, c->mem + 0x00408808u, 4);
            memcpy(&r98, c->mem + 0x004088A0u, 4);
            memcpy(&r9c, c->mem + 0x004088A4u, 4);
            memcpy(&r20c, c->mem + 0x00408A14u, 4);
            memcpy(&r_b4c, c->mem + 0x00409354u, 4);
            memcpy(&r_b48, c->mem + 0x00409350u, 4);
            memcpy(&r_3b0, c->mem + 0x0040ABC0u, 4);
            memcpy(&r_3b4, c->mem + 0x0040ABC4u, 4);
            printf("[engine] call=%u scene=%08X rnd=%08X gate=%08X list=%u "
                   "rvt=%08X r98=%08X r9c=%08X r20c=%08X b48=%08X b4c=%08X "
                   "l3b0=%08X l3b4=%08X\n",
                   calls, scv, rv, rg, (unsigned)lc, r_vt, r98, r9c, r20c,
                   r_b48, r_b4c, r_3b0, r_3b4);
        }
        fflush(stdout);
    }
}

/* VBlank-/Frame-Dispatcher: Die Engine registriert ihre Tick-Tasks in der
   Callback-Tabelle 0x3D7C50 (Basis [0x3D91E8+0x1C]); Slot 2 = 0x00134818
   (Timebase 0x400) und Slot 3 = 0x0013485C (Timebase 0x401). Ohne diese
   Aufrufe bleiben die Resource-Manager-Zaehler 0x40B4C4/0x40B4C8 auf 0 und
   fn_0028F250 wartet endlos. Wir rufen die Tasks alle ~16 ms im Kontext der
   Haupt-CPU auf (wie ein VBlank-Handler beim echten 3DS). */
static void hle_tick(ArmCpu *c) {
    static int init = 0, on = 1, busy = 0;
    static uint64_t last = 0;
    static uint32_t period_ms = 2;
    uint64_t now;
    uint32_t sr[16], sn, sz, sc, sv, st, sq;
    if (!init) {
        const char *e = getenv("HLE_TICK");
        const char *p;
        on = !(e != NULL && e[0] == '0');
        p = getenv("HLE_TICK_MS");
        if (p != NULL && *p != 0) period_ms = (uint32_t)strtoul(p, NULL, 0);
        init = 1;
    }
    if (!on || busy || c == NULL) return;
    /* HLE_FRAME_CB2: Slot nach dem BSS-Clear (erneut) belegen. */
    {
        static uint32_t cb2 = 0xFFFFFFFFu;
        if (cb2 == 0xFFFFFFFFu) {
            const char *e2 = getenv("HLE_FRAME_CB2");
            cb2 = (e2 != NULL && *e2 != 0) ? (uint32_t)strtoul(e2, NULL, 16) : 0u;
        }
        if (cb2 != 0u) {
            uint32_t cur = 0;
            memcpy(&cur, c->mem + 0x40B4CCu, 4);
            if (cur == 0u) memcpy(c->mem + 0x40B4CCu, &cb2, 4);
        }
    }
    /* Diagnose: Draw-Liste der Menue-Szene synthetisch initialisieren,
       damit 0x2800C8 die Objekte wirklich ablegt (statt nach Adresse 0). */
    {
        static int dl = -1;
        if (dl < 0) dl = (getenv("HLE_DRAW_LIST") != NULL);
        if (dl) {
            uint32_t arr = 0x0CFF0000u;
            uint32_t pool = 0x0CFF1000u, obj = 0x0CFF2000u;
            uint16_t cnt = 0, cap = 0x100u, one = 1, zero16 = 0;
            uint32_t stride = 4u, cur = 0;
            memcpy(&cur, c->mem + 0x3EE198u, 4);
            if (cur == 0u) {
                memcpy(c->mem + 0x3EE198u, &arr, 4);
                memcpy(c->mem + 0x3EE1A0u, &cnt, 2);
                memcpy(c->mem + 0x3EE1A2u, &cap, 2);
                memcpy(c->mem + 0x3EE196u, &one, 2);
            }
            /* Pool an +0x5C: {array, stride, start, end}; array[0] -> Scratch-Objekt */
            memcpy(&cur, c->mem + 0x3EE188u, 4);
            if (cur == 0u) {
                uint16_t cap16 = 0x100u;
                memcpy(c->mem + 0x3EE188u, &pool, 4);
                memcpy(c->mem + 0x3EE18Cu, &stride, 4);
                memcpy(c->mem + 0x3EE190u, &zero16, 2);
                memcpy(c->mem + 0x3EE192u, &cap16, 2);
                memcpy(c->mem + 0x3EE194u, &zero16, 2);
                /* Pool-Slots mit Scratch-Objekten fuellen: fn_00284A38 liefert
                   base+index*stride, und der Builder liest dort den Objektzeiger.
                   Ohne gefuellte Slots bricht er bei 0x280338 ab. */
                for (uint32_t i = 0; i < 0x100u; i++) {
                    uint32_t o = obj + i * 0x100u;
                    /* Diagnose: Pool-Slots mit dem echten Menue-Objekt
                       fuellen, damit der Builder kein Null-Objekt anhaengt. */
                    if (getenv("HLE_POOL_MENUOBJ")) o = 0x003EE12Cu;
                    memcpy(c->mem + ((pool + i * 4u) & 0x1FFFFFFFu), &o, 4);
                }
            }
        }
    }
    /* HLE_REG_ENABLE: Asset-Registry-Guard nach dem BSS-Clear erneut setzen. */
    {
        static int re = -1;
        if (re < 0) re = (getenv("HLE_REG_ENABLE") != NULL);
        if (re) {
            uint32_t g = 0;
            memcpy(&g, c->mem + 0x3ECA5Cu, 4);
            if (g == 0u) {
                uint32_t v = 0x00401000u;
                memcpy(c->mem + 0x3ECA5Cu, &v, 4);
            }
        }
    }
    /* HLE_SCENE_NAME_INIT: Szenen-Wide-String setzen, falls der Init-Block
       0x101508 nie erreicht wird. */
    {
        static int sn_init = -1;
        if (sn_init < 0) sn_init = (getenv("HLE_SCENE_NAME_INIT") != NULL);
        if (sn_init) {
            extern int arm_fa_ready;
            const char *nm = getenv("HLE_SCENE_NAME_INIT");
            if (arm_fa_ready && nm != NULL && *nm != 0) {
                uint32_t base = 0x003EE16Cu & 0x1FFFFFFFu;
                uint16_t first = 0;
                memcpy(&first, c->mem + base, 2);
                if (first == 0u) {
                    int i, len = 0;
                    uint16_t l16;
                    while (nm[len] != 0 && len < 0xFE) len++;
                    l16 = (uint16_t)len;
                    memcpy(c->mem + base, &l16, 2);
                    for (i = 0; i < len; i++) {
                        uint16_t ch = (uint16_t)(unsigned char)nm[i];
                        memcpy(c->mem + base + 2u + (uint32_t)i * 2u, &ch, 2);
                    }
                    { uint16_t z = 0;
                      memcpy(c->mem + base + 2u + (uint32_t)len * 2u, &z, 2); }
                }
            }
        }
    }
    now = (uint64_t)GetTickCount64();
    if (now - last < (uint64_t)period_ms) return;
    last = now;
    busy = 1;
    memcpy(sr, c->r, sizeof(sr));
    sn = c->n; sz = c->z; sc = c->c; sv = c->v; st = c->t; sq = c->qflag;
    /* Eigener Stack fuer die Gast-Tasks: sonst ueberschreiben ihre push/pop
       den Live-Frame des unterbrochenen Gast-Codes (belegt: 0x134818/0x13485C
       schreiben nach 0FFEF7DC/7E0, mitten in fn_002808A8s gesicherten
       Registern -> r6 0D4000BC wurde zu FFFFFFFF). */
    c->r[13] = 0x0CF0F000u;
    arm_call_guest_pub(c, 0x00134818u);
    arm_call_guest_pub(c, 0x0013485Cu);
    hle_engine_frame(c);
    if (getenv("HLE_TICK_LOG")) {
        static uint32_t last_h1 = 0xFFFFFFFFu;
        uint32_t h1 = 0, h2 = 0, c4 = 0, dl = 0;
        memcpy(&h1, c->mem + 0x40B4CCu, 4);
        memcpy(&h2, c->mem + 0x40B4D0u, 4);
        memcpy(&c4, c->mem + 0x40B4C4u, 4);
        memcpy(&dl, c->mem + 0x40B4E8u, 4);
        if (h1 != last_h1) {
            last_h1 = h1;
            printf("[tick] c400=%08X h400=%08X h401=%08X dl=%08X\n", c4, h1, h2, dl);
            fflush(stdout);
        }
    }
    memcpy(c->r, sr, sizeof(sr));
    c->n = sn; c->z = sz; c->c = sc; c->v = sv; c->t = st; c->qflag = sq;
    busy = 0;
}

static int gsp_process_cmdqueue(ArmCpu *c);

void hle_pump(void) {
    /* Diagnose: Event-Queue (Objekt 0x408508) testweise als "1 Eintrag"
       markieren, damit die Warteschleife (0x28992C) sie verlaesst. */
    if (getenv("HLE_QUEUE_FAKE") && g_frame_cpu != NULL) {
        static int qdone = 0;
        if (!qdone) {
            uint16_t cnt = 1, pos = 0;
            qdone = 1;
            memcpy(g_frame_cpu->mem + 0x40852Eu, &cnt, 2);   /* [+0x26] Anzahl */
            memcpy(g_frame_cpu->mem + 0x408528u, &pos, 2);   /* [+0x20] Position */
            printf("[queuefake] 0x408508: count=1 pos=0\n");
        }
    }
    {
        static int gpu_submit_env = -1, menu_gpu_env = -1;
        if (gpu_submit_env < 0) gpu_submit_env = (getenv("HLE_GPU_SUBMIT") != NULL);
        if (menu_gpu_env < 0) menu_gpu_env = (getenv("HLE_MENU_GPU") != NULL);
        if (gpu_submit_env) hle_gpu_submit_once();
        if (menu_gpu_env) hle_menu_gpu_submit();
    }
    /* Zweite Gastfunktion pro Tick (z. B. Menue-Szene + GX-Pfad). */
    {
        static const char *fl2_env = NULL;
        static int in_fl2 = 0;
        if (fl2_env == NULL) fl2_env = getenv("HLE_FRAME_LOOP2");
        if (fl2_env != NULL && *fl2_env != 0 && !in_fl2 && g_frame_cpu != NULL) {
            static unsigned fc2 = 0;
            static int fc2_mask = -1;
            if (fc2_mask < 0) {
                const char *m = getenv("HLE_FL2_MASK");
                fc2_mask = (m != NULL && *m != 0)
                               ? (int)strtoul(m, NULL, 0) : 0xFFF;
            }
            if ((++fc2 & (unsigned)fc2_mask) == 0) {
                uint32_t fsave[16];
                uint32_t fa2 = (uint32_t)strtoul(fl2_env, NULL, 16);
                in_fl2 = 1;
                memcpy(fsave, g_frame_cpu->r, sizeof(fsave));
                {
                    const char *fa_arg2 = getenv("HLE_FRAME_ARG2");
                    if (fa_arg2 != NULL && *fa_arg2 != 0) {
                        char buf[128];
                        char *pp = buf;
                        int ri = 0;
                        snprintf(buf, sizeof(buf), "%s", fa_arg2);
                        while (pp != NULL && *pp != 0 && ri < 4) {
                            char *comma = strchr(pp, ',');
                            if (comma != NULL) *comma = 0;
                            g_frame_cpu->r[ri++] = (uint32_t)strtoul(pp, NULL, 16);
                            pp = (comma != NULL) ? comma + 1 : NULL;
                        }
                    }
                }
                /* Diagnose: Parent-Zeiger des Menue-Objekts (menu+4)
                   setzen; er bleibt im Port 0, weil die Welt-Init ihn
                   nicht schreibt. */
                {
                    static int mpar = -1;
                    if (mpar < 0) mpar = (getenv("HLE_MENU_PARENT") != NULL);
                    if (mpar) {
                        uint32_t w = 0x003EC028u;
                        const char *mp = getenv("HLE_MENU_PARENT");
                        if (mp != NULL && *mp != 0 && *mp != '1')
                            w = (uint32_t)strtoul(mp, NULL, 16);
                        memcpy(g_frame_cpu->mem + 0x003EE130u, &w, 4);
                    }
                }
                printf("[frameloop2] call 0x%08X r0=%08X\n", fa2, g_frame_cpu->r[0]);
                fflush(stdout);
                hle_apply_pokes(g_frame_cpu);
                {
                    static uint32_t fsp = 0xFFFFFFFFu;
                    if (fsp == 0xFFFFFFFFu) {
                        const char *es = getenv("HLE_FRAME_SP");
                        fsp = (es != NULL && *es != 0)
                                  ? (uint32_t)strtoul(es, NULL, 16) : 0u;
                    }
                    if (fsp != 0u) {
                        g_frame_cpu->r[13] = fsp;
                        memset(g_frame_cpu->mem + (fsp & 0x1FFFFFFFu), 0, 0x100u);
                    }
                }
                hle_apply_pokes(g_frame_cpu);
                /* Einmaliger Vor-Aufruf (z. B. Ring-Init 0x126D28):
                   HLE_PRECALL=addr,r0,r1,r2,r3 (hex, optional). */
                {
                    static int pre_state = -1;
                    if (pre_state < 0)
                        pre_state = (getenv("HLE_PRECALL") != NULL) ? 0 : 2;
                    if (pre_state == 0) {
                        const char *s = getenv("HLE_PRECALL");
                        char buf[512], *pp = buf;
                        uint32_t v[5] = {0, 0, 0, 0, 0};
                        int n = 0;
                        memcpy(buf, s, sizeof(buf) < strlen(s) + 1
                               ? sizeof(buf) - 1 : strlen(s) + 1);
                        buf[sizeof(buf) - 1] = 0;
                        while (pp != NULL && *pp && n < 5) {
                            char *c = strchr(pp, ',');
                            if (c) *c = 0;
                            v[n++] = (uint32_t)strtoul(pp, NULL, 16);
                            pp = c ? c + 1 : NULL;
                        }
                        pre_state = 1;
                        if (n >= 1 && v[0] >= 0x00100000u) {
                            uint32_t sv[16];
                            memcpy(sv, g_frame_cpu->r, sizeof(sv));
                            g_frame_cpu->r[0] = v[1];
                            g_frame_cpu->r[1] = v[2];
                            g_frame_cpu->r[2] = v[3];
                            g_frame_cpu->r[3] = v[4];
                            g_frame_cpu->r[13] = 0x0CF0A000u;
                            printf("[precall] 0x%08X(%08X,%08X,%08X)\n",
                                   v[0], v[1], v[2], v[3]);
                            fflush(stdout);
                            arm_call_guest_pub(g_frame_cpu, v[0]);
                            printf("[precall] -> r0=%08X\n", g_frame_cpu->r[0]);
                            fflush(stdout);
                            memcpy(g_frame_cpu->r, sv, sizeof(sv));
                        }
                    }
                }
                arm_call_guest_pub(g_frame_cpu, fa2);
                /* Deterministischer Nach-Aufruf: HLE_POSTCALL=addr,r0..r3 */
                {
                    static int post_state = -1;
                    if (post_state < 0)
                        post_state = (getenv("HLE_POSTCALL") != NULL) ? 0 : 2;
                    if (post_state == 0) {
                        char buf[1024], *tok;
                        size_t sl = strlen(getenv("HLE_POSTCALL"));
                        post_state = 1;
                        if (sl >= sizeof(buf)) sl = sizeof(buf) - 1;
                        memcpy(buf, getenv("HLE_POSTCALL"), sl);
                        buf[sl] = 0;
                        for (tok = strtok(buf, ";"); tok != NULL;
                             tok = strtok(NULL, ";")) {
                            if (tok[0] == '#') {
                                /* #src,dst,len : memcpy im Gast-RAM */
                                char *c1 = strchr(tok, ',');
                                uint32_t src = (uint32_t)strtoul(tok + 1,
                                                                 NULL, 16);
                                uint32_t dst = 0, len = 0;
                                if (c1) {
                                    char *c2 = NULL;
                                    dst = (uint32_t)strtoul(c1 + 1, &c2, 16);
                                    if (c2 != NULL && *c2 == ',')
                                        len = (uint32_t)strtoul(c2 + 1,
                                                                NULL, 16);
                                }
                                if (len != 0u &&
                                    (uint64_t)src + len <= g_frame_cpu->mem_size &&
                                    (uint64_t)dst + len <= g_frame_cpu->mem_size) {
                                    memcpy(g_frame_cpu->mem + dst,
                                           g_frame_cpu->mem + src, len);
                                    printf("[postcopy] %08X -> %08X (%X)\n",
                                           src, dst, len);
                                    fflush(stdout);
                                }
                                continue;
                            }
                            if (tok[0] == '@') {
                                char *eq = strchr(tok, '=');
                                if (eq) {
                                    uint32_t a = (uint32_t)strtoul(tok + 1,
                                                                  NULL, 16);
                                    uint32_t val = (uint32_t)strtoul(eq + 1,
                                                                     NULL, 16);
                                    if (a + 4u <= g_frame_cpu->mem_size)
                                        memcpy(g_frame_cpu->mem + a, &val, 4);
                                    printf("[postpoke] %08X=%08X\n", a, val);
                                    fflush(stdout);
                                }
                                continue;
                            }
                            {
                                uint32_t v[5] = {0, 0, 0, 0, 0};
                                char *pp = tok;
                                int n = 0;
                                while (pp != NULL && *pp && n < 5) {
                                    char *c = strchr(pp, ',');
                                    if (c) *c = 0;
                                    v[n++] = (uint32_t)strtoul(pp, NULL, 16);
                                    pp = c ? c + 1 : NULL;
                                }
                                if (n >= 1 && v[0] >= 0x00100000u) {
                                    uint32_t sv[16];
                                    memcpy(sv, g_frame_cpu->r, sizeof(sv));
                                    g_frame_cpu->r[0] = v[1];
                                    g_frame_cpu->r[1] = v[2];
                                    g_frame_cpu->r[2] = v[3];
                                    g_frame_cpu->r[3] = v[4];
                                    g_frame_cpu->r[13] = 0x0CF0A000u;
                                    printf("[postcall2] 0x%08X(%08X,%08X)\n",
                                           v[0], v[1], v[2]);
                                    fflush(stdout);
                                    arm_call_guest_pub(g_frame_cpu, v[0]);
                                    printf("[postcall2] -> r0=%08X ab=%d\n",
                                           g_frame_cpu->r[0],
                                           g_frame_cpu->aborted);
                                    fflush(stdout);
                                    memcpy(g_frame_cpu->r, sv, sizeof(sv));
                                }
                            }
                        }
                    }
                }
                {
                    static int pc_done = 0;
                    if (pc_done < 6) {
                        uint32_t j, v, dsc = 0;
                        pc_done++;
                        memcpy(&dsc, g_frame_cpu->mem + 0x3EE678u, 4);
                        printf("[postcall] fa2=%08X dsc[3EE678]=%08X E800000:",
                               fa2, dsc);
                        for (j = 0; j < 10u; j++) {
                            memcpy(&v, g_frame_cpu->mem +
                                   0x0E800000u + j * 4u, 4);
                            printf(" %08X", v);
                        }
                        printf(" ab=%d\n", g_frame_cpu->aborted);
                        fflush(stdout);
                    }
                }
                {
                    static int logret = -1;
                    if (logret < 0) logret = (getenv("HLE_LOGRET") != NULL);
                    if (logret) {
                        printf("[frameloop2] ret 0x%08X -> r0=%08X\n", fa2,
                               g_frame_cpu->r[0]);
                        fflush(stdout);
                    }
                }
                /* Nach dem Engine-Submit den echten Trigger-Wrapper
                   0x13E3E0 (gsp::Gpu 0x000C) aus dem Engine-Code aufrufen. */
                if (fa2 == 0x0012FD30u || fa2 == 0x0012493Cu) {
                    static int gtrig = -1;
                    if (gtrig < 0) gtrig = (getenv("HLE_GSP_TRIGGER") != NULL);
                    if (gtrig && g_hle_gsp_handle != 0u) {
                        uint32_t tv[16], h = g_hle_gsp_handle, out = 0;
                        memcpy(g_frame_cpu->mem + 0x0CFE9100u, &h, 4);
                        hle_apply_pokes(g_frame_cpu);
                        {
                            static int qlog = -1;
                            if (qlog < 0) qlog = (getenv("HLE_QUEUE_LOG") != NULL);
                            if (qlog) {
                                uint32_t c1 = 0, a1 = 0, s1 = 0;
                                uint32_t d0 = 0, d1 = 0, d2 = 0;
                                memcpy(&c1, g_frame_cpu->mem + 0x10002818u, 4);
                                memcpy(&a1, g_frame_cpu->mem + 0x1000281Cu, 4);
                                memcpy(&s1, g_frame_cpu->mem + 0x100028B0u, 4);
                                memcpy(&d0, g_frame_cpu->mem + 0x0EA00004u, 4);
                                memcpy(&d1, g_frame_cpu->mem + 0x0EA10000u, 4);
                                memcpy(&d2, g_frame_cpu->mem + 0x0EA1002Cu, 4);
                                printf("[queue] pre-trigger n=%u addr=%08X size=%X "
                                       "desc+4=%08X A=%08X A+2c=%08X\n",
                                       c1, a1, s1, d0, d1, d2);
                                {
                                    uint32_t saw;
                                    printf("[qmem] E800000:");
                                    for (saw = 0; saw < 16; saw++) {
                                        uint32_t v = 0;
                                        memcpy(&v, g_frame_cpu->mem +
                                               0x0E800000u + saw * 4u, 4);
                                        printf(" %08X", v);
                                    }
                                    printf("\n");
                                }
                                fflush(stdout);
                            }
                        }
                        memcpy(tv, g_frame_cpu->r, sizeof(tv));
                        g_frame_cpu->r[0] = 0x0CFE9100u;
                        g_frame_cpu->r[1] = 0u;
                        g_frame_cpu->r[2] = 0x0CFE9200u;
                        g_frame_cpu->r[13] = 0x0D900000u;
                        arm_call_guest_pub(g_frame_cpu, 0x0013E3E0u);
                        memcpy(&out, g_frame_cpu->mem + 0x0CFE9200u, 4);
                        memcpy(g_frame_cpu->r, tv, sizeof(tv));
                    }
                }
                memcpy(g_frame_cpu->r, fsave, sizeof(fsave));
                in_fl2 = 0;
            }
        }
    }
    /* Diagnose: GSP-Shared-Memory-Kommandopuffer auch ohne 0x000C abarbeiten. */
    if (g_frame_cpu != NULL) {
        static int gspq_pump = -1;
        if (gspq_pump < 0) gspq_pump = (getenv("HLE_GSPQ_PUMP") != NULL);
        if (gspq_pump) {
            static unsigned gspq_div = 0;
            if ((++gspq_div & 0x3FFu) == 0) {
                gsp_process_cmdqueue(g_frame_cpu);
            }
        }
    }
    /* Menue-Init-Gate dauerhaft offen halten (0x10FC9C liest es). */
    if (g_frame_cpu != NULL) {
        static int meg = -1;
        if (meg < 0) meg = (getenv("HLE_ENGINE_MENU") != NULL);
        if (meg && g_frame_cpu->mem_size > 0x3D8CE0u) {
            uint8_t g1 = 1;
            g_frame_cpu->mem[0x3D8CE0u] = g1;
        }
    }
    /* Diagnose: Menue-Welt-Update + 3D-Renderer periodisch aus dem laufenden
       Menue-Kontext aufrufen (eigener Stack, damit nichts ueberschrieben
       wird). */
    if (g_frame_cpu != NULL) {
        static int mw = -1, in_mw = 0;
        if (mw < 0) mw = (getenv("HLE_MENU_WORLD") != NULL);
        if (mw && !in_mw) {
            static unsigned mc = 0;
            uint32_t sc = 0;
            memcpy(&sc, g_frame_cpu->mem + 0x003EC028u, 4);
            if (sc != 0u && (++mc & 0x1FFFu) == 0) {
                uint32_t sv[16];
                in_mw = 1;
                memcpy(sv, g_frame_cpu->r, sizeof(sv));
                g_frame_cpu->r[0] = 0x003EC028u;
                g_frame_cpu->r[1] = 0u;
                g_frame_cpu->r[13] = 0x0CF0A000u;
                arm_call_guest_pub(g_frame_cpu, 0x00285208u);
                {
                    uint8_t z = 0, one = 1;
                    memcpy(g_frame_cpu->mem + 0x003D8E6Au, &z, 1);
                    memcpy(g_frame_cpu->mem + 0x003D94E8u, &one, 1);
                    memcpy(g_frame_cpu->mem + 0x003D94E9u, &z, 1);
                }
                g_frame_cpu->r[0] = 0x00422200u;
                g_frame_cpu->r[13] = 0x0CF0A000u;
                arm_call_guest_pub(g_frame_cpu, 0x001B9938u);
                memcpy(g_frame_cpu->r, sv, sizeof(sv));
                {
                    static int lm = 0;
                    if (!lm) {
                        uint32_t u = 0;
                        lm = 1;
                        memcpy(&u, g_frame_cpu->mem + 0x00422200u + 0x1530u, 4);
                        printf("[menuworld] units=%u\n", u);
                        {
                            int k;
                            printf("[menudump] 3EE12C:");
                            for (k = 0; k < 24; k++) {
                                uint32_t v = 0;
                                memcpy(&v, g_frame_cpu->mem + 0x003EE12Cu + (uint32_t)k * 4u, 4);
                                printf(" %08X", v);
                            }
                            printf("\n[menudump] scene:");
                            for (k = 0; k < 16; k++) {
                                uint32_t v = 0;
                                memcpy(&v, g_frame_cpu->mem + 0x003CA494u + (uint32_t)k * 4u, 4);
                                printf(" %08X", v);
                            }
                            printf("\n");
                            {
                                uint32_t st = 0;
                                memcpy(&st, g_frame_cpu->mem + 0x003EE12Cu, 4);
                                printf("[menudump] state=%08X:", st);
                                if (st >= 0x00100000u && st < 0x20000000u) {
                                    for (k = 0; k < 32; k++) {
                                        uint32_t v = 0;
                                        memcpy(&v, g_frame_cpu->mem + (st & 0x1FFFFFFFu) + (uint32_t)k * 4u, 4);
                                        printf(" %08X", v);
                                    }
                                }
                                printf("\n");
                            }
                            {
                                uint32_t a, hits = 0;
                                {
                                    uint32_t a, hits = 0, want = 0x003EE12Cu;
                                    printf("[menuscan] ptr(3EE12C):");
                                    for (a = 0x00100000u; a + 4u < g_frame_cpu->mem_size && hits < 8u; a += 4u) {
                                        uint32_t v = 0;
                                        memcpy(&v, g_frame_cpu->mem + a, 4);
                                        if (v == want) {
                                            printf(" %08X", a);
                                            hits++;
                                        }
                                    }
                                    printf("\n");
                                }
                                printf("[menuscan] IMGC00:");
                                for (a = 0x00100000u; a + 8u < g_frame_cpu->mem_size && hits < 8u; a += 4u) {
                                    if (memcmp(g_frame_cpu->mem + a, "IMGC00", 6) == 0) {
                                        printf(" %08X", a);
                                        hits++;
                                    }
                                }
                                printf("\n");
                            }
                        }
                        fflush(stdout);
                    }
                }
                in_mw = 0;
            }
        }
    }
    /* Diagnose: Engine-GSP-Client initialisieren und Trigger periodisch
       aufrufen (beide laufen im Menue-Loop sonst nie). */
    if (g_frame_cpu != NULL) {
        static int ge = -1, ge_init = 0, in_ge = 0;
        if (ge < 0) ge = (getenv("HLE_GSP_ENGINE") != NULL);
        if (ge && !in_ge) {
            uint32_t sv[16];
            static unsigned gc = 0;
            in_ge = 1;
            memcpy(sv, g_frame_cpu->r, sizeof(sv));
            if (!ge_init) {
                uint8_t z = 0;
                uint32_t nine = 9, h1 = 0, h2 = 0, st = 0;
                ge_init = 1;
                /* GSP-Client 0x3D91E8 wie im echten Boot vorbereiten:
                   Name "gsp::Gpu" an +0x10, Laenge 9 an +4, Flags 0. */
                memcpy(g_frame_cpu->mem + 0x003D91F8u, "gsp::Gpu", 9);
                memcpy(g_frame_cpu->mem + 0x003D91E8u, &z, 1);
                memcpy(g_frame_cpu->mem + 0x003D91E9u, &z, 1);
                memcpy(g_frame_cpu->mem + 0x003D91EAu, &z, 1);
                memcpy(g_frame_cpu->mem + 0x003D91ECu, &nine, 4);
                g_frame_cpu->r[13] = 0x0CF0A000u;
                arm_call_guest_pub(g_frame_cpu, 0x00138D70u);
                memcpy(&h1, g_frame_cpu->mem + 0x003D91F8u, 4);
                memcpy(&h2, g_frame_cpu->mem + 0x003D91FCu, 4);
                memcpy(&st, g_frame_cpu->mem + 0x003D9204u, 4);
                /* Die Engine-Init bekam cam:u (Handle 4) statt gsp: den
                   echten gsp-Handle in Client und Auswahl-Slot setzen. */
                if (g_hle_gsp_handle != 0) {
                    uint32_t gh = g_hle_gsp_handle;
                    memcpy(g_frame_cpu->mem + 0x003D91F8u, &gh, 4);
                    memcpy(g_frame_cpu->mem + 0x003D91FCu, &gh, 4);
                    memcpy(g_frame_cpu->mem + 0x003DA6DCu, &gh, 4);
                }
                {
                    uint32_t ctx = 0;
                    memcpy(&ctx, g_frame_cpu->mem + 0x003D91E8u + 0x1Cu, 4);
                    printf("[gspengine] ctx=%08X", ctx);
                    if (ctx >= 0x00100000u && ctx < 0x20000000u) {
                        uint32_t k;
                        for (k = 0x38u; k <= 0x68u; k += 4u) {
                            uint32_t v = 0;
                            memcpy(&v, g_frame_cpu->mem + (ctx & 0x1FFFFFFFu) + k, 4);
                            printf(" +%02X=%08X", k, v);
                        }
                        if (getenv("HLE_GSP_BASE")) {
                            const char *bs = getenv("HLE_GSP_BASE");
                            uint32_t b = (bs != NULL && *bs != 0 && *bs != '1')
                                             ? (uint32_t)strtoul(bs, NULL, 16) : 0x10002000u;
                            uint32_t q58 = b + 0x800u, q5c = b + 0x200u, q60 = b + 0x240u;
                            memcpy(g_frame_cpu->mem + (ctx & 0x1FFFFFFFu) + 0x40u, &b, 4);
                            memcpy(g_frame_cpu->mem + (ctx & 0x1FFFFFFFu) + 0x58u, &q58, 4);
                            memcpy(g_frame_cpu->mem + (ctx & 0x1FFFFFFFu) + 0x5Cu, &q5c, 4);
                            memcpy(g_frame_cpu->mem + (ctx & 0x1FFFFFFFu) + 0x60u, &q60, 4);
                            printf(" base=%08X q58=%08X", b, q58);
                        }
                    }
                    printf("\n");
                }
                printf("[gspengine] 0x138D70: h10=%u h14=%u state=%08X -> gsp=%u\n",
                       h1, h2, st, g_hle_gsp_handle);
                fflush(stdout);
            }
            if ((++gc & 0x1FFFu) == 0) {
                /* 0x13E3E0 ist ein generischer IPC-Wrapper:
                   r0 zeigt auf die Handle-Variable, r1 = Param, r2 = Out.
                   Also einen Scratch-Slot mit dem gsp-Handle fuellen. */
                uint32_t gh = g_hle_gsp_export;
                static int ring_fill = -1;
                if (ring_fill < 0)
                    ring_fill = (getenv("HLE_RING_FILL") != NULL);
                if (ring_fill) {
                    uint32_t base = 0x10002800u, hdr = 1u << 8;
                    uint32_t addr = 0x0CFE6000u, sz = 0x24u;
                    uint8_t id = 1;
                    memcpy(g_frame_cpu->mem + base, &hdr, 4);
                    memcpy(g_frame_cpu->mem + base + 0x20u, &id, 1);
                    memcpy(g_frame_cpu->mem + base + 0x24u, &addr, 4);
                    memcpy(g_frame_cpu->mem + base + 0x28u, &sz, 4);
                }
                memcpy(g_frame_cpu->mem + 0x0CFE9100u, &gh, 4);
                g_frame_cpu->r[0] = 0x0CFE9100u;
                g_frame_cpu->r[1] = 0u;
                g_frame_cpu->r[2] = 0x0CFE9108u;
                g_frame_cpu->r[3] = 0u;
                g_frame_cpu->r[13] = 0x0CF0A000u;
                arm_call_guest_pub(g_frame_cpu, 0x0013E3E0u);
            }
            memcpy(g_frame_cpu->r, sv, sizeof(sv));
            in_ge = 0;
        }
    }
    /* Diagnose: Draw-List-Eintrag in die 3D-Unit-Liste
       (0x00422200+0x14F0/+0x1530) spiegeln und den 3D-Renderer aufrufen. */
    if (g_frame_cpu != NULL) {
        static int xu = -1, in_xu = 0;
        if (xu < 0) xu = (getenv("HLE_UNIT_XFER") != NULL);
        if (xu && !in_xu) {
            uint16_t dlc = 0;
            uint32_t arr = 0;
            memcpy(&dlc, g_frame_cpu->mem + 0x003EE1A0u, 2);
            memcpy(&arr, g_frame_cpu->mem + 0x003EE198u, 4);
            {
                const char *uo = getenv("HLE_UNIT_OBJ");
                uint32_t forced = (uo != NULL && *uo != 0) ? (uint32_t)strtoul(uo, NULL, 16) : 0u;
                if (forced == 0u && arr != 0u)
                    memcpy(&forced, g_frame_cpu->mem + (arr & 0x1FFFFFFFu), 4);
            if (forced != 0u) {
                uint32_t obj = 0, n = 0x004236F0u, z = 0, one = 1;
                uint8_t t = 0;
                uint32_t sx[16];
                obj = forced;
                memcpy(g_frame_cpu->mem + n + 0x00u, &obj, 4);
                memcpy(g_frame_cpu->mem + n + 0x04u, &z, 4);
                memcpy(g_frame_cpu->mem + n + 0x08u, &t, 1);
                memcpy(g_frame_cpu->mem + n + 0x10u, &obj, 4);
                memcpy(g_frame_cpu->mem + n + 0x14u, &obj, 4);
                {
                    uint32_t node18 = 0x0CFF4000u;
                    memcpy(g_frame_cpu->mem + n + 0x18u, &node18, 4);
                }
                memcpy(g_frame_cpu->mem + 0x00423730u, &one, 4);
                in_xu = 1;
                memcpy(sx, g_frame_cpu->r, sizeof(sx));
                {
                    uint8_t gz = 0, go = 1;
                    memcpy(g_frame_cpu->mem + 0x003D8E6Au, &gz, 1);
                    memcpy(g_frame_cpu->mem + 0x003D94E8u, &go, 1);
                    memcpy(g_frame_cpu->mem + 0x003D94E9u, &gz, 1);
                    /* Renderer-Enable [0x422200+0x15A3]: ohne dieses Byte
                       bricht fn_001B9938 sofort bei 0x1BA964 ab. */
                    memcpy(g_frame_cpu->mem + 0x004237A3u, &go, 1);
                }
                /* gsp-Client-Slot der Engine-Wrapper (0x34B55C/0x34B5C4) */
                if (g_hle_gsp_handle != 0) {
                    uint32_t gh = g_hle_gsp_handle;
                    memcpy(g_frame_cpu->mem + 0x003DA6DCu, &gh, 4);
                    memcpy(g_frame_cpu->mem + 0x003D9098u, &gh, 4);
                }
                g_frame_cpu->r[0] = 0x00422200u;
                g_frame_cpu->r[13] = 0x0CF0A000u;
                arm_call_guest_pub(g_frame_cpu, 0x001B9938u);
                memcpy(g_frame_cpu->r, sx, sizeof(sx));
                {
                    static int lx = 0;
                    if (!lx) {
                        lx = 1;
                        printf("[unitxfer] obj=%08X -> units=1, 3D-Renderer aufgerufen\n", obj);
                        fflush(stdout);
                    }
                }
                in_xu = 0;
            }
            }
        }
    }
    /* Diagnose: Frame-Aufbau periodisch aus dem Gast-Kontext aufrufen. */
    {
        static const char *fl_env = NULL;
        static int in_frame_call = 0;
        if (fl_env == NULL) fl_env = getenv("HLE_FRAME_LOOP");
        if (fl_env != NULL && *fl_env != 0 && !in_frame_call && g_frame_cpu != NULL) {
            static unsigned fc = 0;
            if ((++fc & 0xFFFu) == 0) {
                extern int arm_fa_ready;
                if (arm_fa_ready) {
                uint32_t fsave[16];
                uint32_t fa = (uint32_t)strtoul(fl_env, NULL, 16);
                in_frame_call = 1;
                memcpy(fsave, g_frame_cpu->r, sizeof(fsave));
                {
                    const char *fa_arg = getenv("HLE_FRAME_ARG");
                    if (fa_arg != NULL && *fa_arg != 0) {
                        char buf[128];
                        char *pp = buf;
                        int ri = 0;
                        snprintf(buf, sizeof(buf), "%s", fa_arg);
                        while (pp != NULL && *pp != 0 && ri < 4) {
                            char *comma = strchr(pp, ',');
                            if (comma != NULL) *comma = 0;
                            g_frame_cpu->r[ri++] = (uint32_t)strtoul(pp, NULL, 16);
                            pp = (comma != NULL) ? comma + 1 : NULL;
                        }
                    }
                }
                printf("[frameloop] call 0x%08X r0=%08X\n", fa, g_frame_cpu->r[0]);
                fflush(stdout);
                {
                    uint16_t sv = 0, zz = 0;
                    memcpy(&sv, g_frame_cpu->mem + 0x3EE16Cu, 2);
                    memcpy(g_frame_cpu->mem + 0x3EE16Cu, &zz, 2);
                    arm_call_guest_pub(g_frame_cpu, fa);
                    memcpy(g_frame_cpu->mem + 0x3EE16Cu, &sv, 2);
                }
                memcpy(g_frame_cpu->r, fsave, sizeof(fsave));
                in_frame_call = 0;
            }
        }
    }
    /* Diagnose: Draw-Liste des Menue-Objekts 0x003EE12C dumpen. */
    {
        static int dump_dl_env = -1;
        if (dump_dl_env < 0) dump_dl_env = (getenv("HLE_DUMP_DRAWLIST") != NULL);
    if (dump_dl_env && g_frame_cpu != NULL) {
        static unsigned dlc = 0;
        if ((++dlc & 0xFFFu) == 0) {
            uint32_t arr = 0, cnt = 0, cap = 0 /*, w = 0 */;
            uint16_t h = 0;
            memcpy(&arr, g_frame_cpu->mem + ((0x003EE12Cu + 0x6Cu) & 0x1FFFFFFFu), 4);
            memcpy(&h, g_frame_cpu->mem + ((0x003EE12Cu + 0x74u) & 0x1FFFFFFFu), 2);
            cnt = h;
            memcpy(&h, g_frame_cpu->mem + ((0x003EE12Cu + 0x76u) & 0x1FFFFFFFu), 2);
            cap = h;
            printf("[dl] arr=%08X count=%u cap=%u\n", arr, cnt, cap);
            for (uint32_t i = 0; i < 4u && i < cnt; i++) {
                uint32_t op = 0;
                memcpy(&op, g_frame_cpu->mem + ((arr + i * 4u) & 0x1FFFFFFFu), 4);
                printf("[dl]  ent[%u]=%08X:", i, op);
                for (uint32_t k = 0; k < 12u; k++) {
                    uint32_t w = 0;
                    memcpy(&w, g_frame_cpu->mem + ((op + k * 4u) & 0x1FFFFFFFu), 4);
                    printf(" %08X", w);
                }
                printf("\n");
            }
            fflush(stdout);
        }
    }
    }
    }
    if (!g_guest_lock_init || g_no_lock) return;
    if ((++g_pump_count & 0x3FFFu) != 0) return;
    if (!t_lock_owned) return;
    guest_lock_leave();
    Sleep(0);
    guest_lock_reacquire();
}

static void guest_yield(void) {
    if (!g_guest_lock_init) return;
    if (g_no_lock) { Sleep(0); return; }
    if (!t_lock_owned) { Sleep(0); return; }
    guest_lock_leave();
    if (g_det_sched && t_tid >= 0) {
        g_turn = sched_next(t_tid);
        sched_wait_turn();
        guest_lock_reacquire();
        return;
    }
    Sleep(0);
    /* Garantiert wieder in den Lock zurueck (bilanziert ueber t_lock_owned). */
    guest_lock_reacquire();
}

extern void arm_call_guest_pub(ArmCpu *c, uint32_t addr);

typedef struct {
    int used;
    ArmCpu *cpu;
    uint32_t entry;
    int id;
    uint32_t handle;   /* Handle aus svcCreateThread */
} GuestThread;
static GuestThread g_threads[32];

static int g_thread_count = 0;

static void thread_signal_exit(ArmCpu *c);

static DWORD WINAPI guest_thread_main(LPVOID p) {
    GuestThread *t = (GuestThread *)p;
    t_tid = (int)t->id;
    g_thread_alive[t_tid] = 1;
    g_thread_started[t_tid] = 1;
    if (!g_no_lock) {
        sched_wait_turn();
        guest_lock_enter();
    }
    arm_call_guest_pub(t->cpu, t->entry);
    thread_signal_exit(t->cpu);
    if (!g_no_lock) {
        g_thread_alive[t_tid] = 0;
        if (g_turn == t_tid) g_turn = sched_next(t_tid);
        guest_lock_leave();
    }
    return 0;
}

#define MAX_HANDLES 4096
#define MAX_SERVICES 64

enum ServiceId {
    SVC_SRV = 1,
    SVC_FS_USER,
    SVC_FS_LDR,
    SVC_GSP_GPU,
    SVC_GSP_LCD,
    SVC_HID_USER,
    SVC_APT_U,
    SVC_CFG_U,
    SVC_PTM_U,
    SVC_DSP,
    SVC_Y2R,
    SVC_NDSP,
    SVC_SOC,
    SVC_IR,
    SVC_MIC,
    SVC_CAM,
    SVC_AC,
    SVC_BOSS,
    SVC_CECD,
    SVC_NEWS,
    SVC_NDM,
    SVC_NWM,
    SVC_FRD,
    SVC_AM,
    SVC_LDR_RO,
    SVC_MVD,
    SVC_NFC,
    SVC_PXI,
    SVC_DLP,
    SVC_SSL,
    SVC_HTTP,
    SVC_OTHER,
    SVC_FILE,
};

#define MAX_FILES 64
typedef struct {
    int used;
    char path[256];
    FILE *fp;
    uint32_t size;
} FileSlot;

static FileSlot g_files[MAX_FILES];

typedef struct {
    int used;
    int service;     /* ServiceId */
    int file;        /* Index in g_files oder -1 */
    int signaled;    /* Event-Zustand */
    char name[12];
} Handle;

typedef struct {
    uint32_t header;
    uint32_t cmd;
    int normal;
    int translate;
    uint32_t *buf;
    int written;     /* Anzahl geschriebener Woerter (inkl. Header) */
} Ipc;

static ArmCpu *g_cpu = NULL;
static char g_romfs[512];
static int g_log = 0;
static uint64_t g_tick = 0x1000;
static int g_tick_calls = 0;
static Handle g_handles[MAX_HANDLES];
static int g_next_handle = 1;
static uint32_t g_srv_handle = 0;
static FILE *g_trace_file = NULL;
static int late_mutex_hack = 0;
static int g_srv_layout = 2;
static uint32_t g_cur_handle = 0;
static uint8_t *g_cur_mem = NULL;
/* svc_mutex_hack */

static void svc_trace(uint32_t imm, ArmCpu *c) {
    if (g_trace_file) {
        fprintf(g_trace_file, "svc %02X r0=%08X r1=%08X r2=%08X r3=%08X lr=%08X blk=%08X\n",
                imm, c->r[0], c->r[1], c->r[2], c->r[3], c->r[14], arm_last_block);
        fflush(g_trace_file);
    }
}
static uint32_t g_heap_cursor = 0x08000000u;   /* App-Heap: 0x08000000..0x10000000 */
static uint32_t g_lin_cursor  = 0x14000000u;   /* Linear-Heap: 0x14000000..0x1C000000 */
static int g_ipc_count = 0;
static int g_ipc_log_limit = 60;
static unsigned g_cmd_seen[64][64];

/* Ringpuffer der letzten Ereignisse (fuer Absturzdiagnose) */
#define HLE_EV_RING 64
static struct { int kind; uint32_t a, b, c; } g_ev_ring[HLE_EV_RING];
static int g_ev_pos = 0;

static void ev(int kind, uint32_t a, uint32_t b, uint32_t c) {
    g_ev_ring[g_ev_pos].kind = kind;
    g_ev_ring[g_ev_pos].a = a;
    g_ev_ring[g_ev_pos].b = b;
    g_ev_ring[g_ev_pos].c = c;
    g_ev_pos = (g_ev_pos + 1) % HLE_EV_RING;
}   /* service x command */

static const char *SERVICE_NAMES[] = {
    "", "srv:", "fs:USER", "fs:LDR", "gsp::Gpu", "gsp::Lcd", "hid:USER",
    "APT:U", "cfg:u", "ptm:u", "dsp::DSP", "y2r:u", "ndsp:", "soc:U",
    "ir:USER", "mic:u", "cam:u", "ac:u", "boss:U", "cecd:u", "news:u",
    "ndm:u", "nwm::UDS", "frd:u", "am:u", "ldr:ro", "mvd:std", "nfc:u",
    "pxi:dev", "dlp:FKCL", "ssl:C", "http:C", "?", "file",
};

static int service_from_name(const char *name) {
    if (strcmp(name, "srv:") == 0) return SVC_SRV;
    if (strcmp(name, "fs:USER") == 0) return SVC_FS_USER;
    if (strcmp(name, "fs:LDR") == 0) return SVC_FS_LDR;
    if (strcmp(name, "gsp::Gpu") == 0) return SVC_GSP_GPU;
    if (strcmp(name, "gsp::Lcd") == 0) return SVC_GSP_LCD;
    if (strcmp(name, "hid:USER") == 0) return SVC_HID_USER;
    if (strcmp(name, "APT:U") == 0) return SVC_APT_U;
    if (strcmp(name, "cfg:u") == 0) return SVC_CFG_U;
    if (strcmp(name, "ptm:u") == 0) return SVC_PTM_U;
    if (strcmp(name, "dsp::DSP") == 0) return SVC_DSP;
    if (strcmp(name, "y2r:u") == 0) return SVC_Y2R;
    if (strcmp(name, "ndsp:") == 0) return SVC_NDSP;
    if (strcmp(name, "soc:U") == 0) return SVC_SOC;
    if (strcmp(name, "ir:USER") == 0) return SVC_IR;
    if (strcmp(name, "mic:u") == 0) return SVC_MIC;
    if (strcmp(name, "cam:u") == 0) return SVC_CAM;
    if (strcmp(name, "ac:u") == 0) return SVC_AC;
    if (strcmp(name, "boss:U") == 0) return SVC_BOSS;
    if (strcmp(name, "boss:u") == 0) return SVC_BOSS;
    if (strcmp(name, "cecd:u") == 0) return SVC_CECD;
    if (strcmp(name, "news:u") == 0) return SVC_NEWS;
    if (strcmp(name, "ndm:u") == 0) return SVC_NDM;
    if (strcmp(name, "nwm::UDS") == 0) return SVC_NWM;
    if (strcmp(name, "frd:u") == 0) return SVC_FRD;
    if (strcmp(name, "am:u") == 0) return SVC_AM;
    if (strcmp(name, "ldr:ro") == 0) return SVC_LDR_RO;
    if (strcmp(name, "mvd:std") == 0) return SVC_MVD;
    if (strcmp(name, "nfc:u") == 0) return SVC_NFC;
    if (strcmp(name, "pxi:dev") == 0) return SVC_PXI;
    if (strcmp(name, "dlp:FKCL") == 0) return SVC_DLP;
    if (strcmp(name, "ssl:C") == 0) return SVC_SSL;
    if (strcmp(name, "http:C") == 0) return SVC_HTTP;
    return SVC_OTHER;
}

static int alloc_handle(int service, const char *name) {
    for (int i = 1; i < MAX_HANDLES; i++) {
        if (!g_handles[i].used) {
            g_handles[i].used = 1;
            g_handles[i].service = service;
            g_handles[i].file = -1;
            snprintf(g_handles[i].name, sizeof(g_handles[i].name), "%s", name ? name : "?");
            if (getenv("HLE_HDL_LOG")) printf("[hdl] %d = %s\n", i, g_handles[i].name);
            return i;
        }
    }
    return 0;
}

static Handle *get_handle(uint32_t h) {
    if (h == 0 || h >= (uint32_t)MAX_HANDLES) return NULL;
    return g_handles[h].used ? &g_handles[h] : NULL;
}

/* ---------- IPC-Schreibzugriffe ---------- */

static void write_u32(ArmCpu *c, uint32_t addr, uint32_t value);
static void ipc_put(Ipc *r, uint32_t v) {
    r->buf[r->written++] = v;
}

static void ipc_put_handle(Ipc *r, uint32_t handle) {
    ipc_put(r, 0x10u);      /* MoveHandle-Deskriptor */
    ipc_put(r, handle);
}

static void ipc_put_handle_copy(Ipc *r, uint32_t handle) {
    ipc_put(r, 0x00u);      /* CopyHandle-Deskriptor */
    ipc_put(r, handle);
}

static void ipc_begin(Ipc *r, uint32_t cmd, int normal, int translate) {
    r->cmd = cmd;
    r->normal = normal;
    r->translate = translate;
    r->written = 0;
}

static void ipc_finish(Ipc *r) {
    r->buf[0] = ((r->cmd & 0xFFFFu) << 16) | ((uint32_t)(r->normal & 0x3F) << 6)
                | (uint32_t)(r->translate & 0x3F);
    /* Der Reply-Puffer liegt bereits in der TLS-Struktur (TLS+0x80):
       Wort 1 = Result, Wort 2..n = Normal-Parameter, danach Translate.
       Keine separaten TLS-Felder ueberschreiben. */
}

/* ---------- Services ---------- */

static void srv_command(Ipc *r, uint32_t *p) {
    switch (r->cmd) {
        case 1: { /* RegisterClient */
            ipc_begin(r, 1, 1, 0);
            ipc_put(r, 0);
            break;
        }
        case 2: { /* EnableNotification: keine Parameter */
            uint32_t h = (uint32_t)alloc_handle(SVC_OTHER, "srv:notif");
            if (g_log) printf("[srv] EnableNotification -> %u\n", h);
            ipc_begin(r, 2, 2, 2);
            ipc_put(r, 0);
            ipc_put(r, h);
            ipc_put_handle_copy(r, h);
            break;
        }
        case 3: { /* RegisterService(name[8], name_len, max_sessions) */
            char name[9];
            memcpy(name, (const char *)p, 8);
            name[8] = 0;
            int h = alloc_handle(SVC_OTHER, name);
            if (g_log) printf("[srv] RegisterService \"%s\"\n", name);
            ipc_begin(r, 3, 1, 2);
            ipc_put(r, 0);
            ipc_put_handle(r, (uint32_t)h);
            break;
        }
        case 4: { /* UnregisterService */
            ipc_begin(r, 4, 1, 0);
            ipc_put(r, 0);
            break;
        }
        case 5: { /* GetServiceHandle(name[8], name_len, flags) */
            char name[9];
            memcpy(name, (const char *)p, 8);
            name[8] = 0;
            uint32_t name_len = p[2];
            if (name_len < 8) name[name_len] = 0;
            int svc = service_from_name(name);
            int h = alloc_handle(svc, name);
            /* Der fs-Client der App holt sein Handle zur Laufzeit: bei
               "fs:USER" den echten Handle in den Client-Slot 0x3D91D0 legeneil. */
            if (strcmp(name, "fs:USER") == 0 && g_cpu != NULL) {
                uint32_t hv = (uint32_t)h;
                memcpy(g_cpu->mem + 0x3D91D0u, &hv, 4);
            }
            if (svc == SVC_GSP_GPU) g_hle_gsp_handle = h;
            if (g_log || getenv("HLE_SRV_NAMES")) {
                printf("[srvname] \"%s\" srvh=%u blk=%08X lr=%08X desc=%08X -> %d (%s)\n",
                       name, g_cur_handle, arm_last_block, g_cpu ? g_cpu->r[14] : 0u,
                       0x003D91E8u, h, SERVICE_NAMES[svc]);
            }
            if (g_srv_layout == 1) {
                ipc_begin(r, 5, 2, 0);
                ipc_put(r, 0);
                ipc_put(r, (uint32_t)h);
            } else if (g_srv_layout != 2) {
                ipc_begin(r, 5, 1, 2);
                ipc_put(r, 0);
                ipc_put_handle(r, (uint32_t)h);
            } else {
                ipc_begin(r, 5, 2, 2);
                ipc_put(r, 0);
                ipc_put(r, (uint32_t)h);
                ipc_put_handle(r, (uint32_t)h);
            }
            break;
        }
        default:
            if (g_log) printf("[srv] cmd %u (unbekannt) -> 0\n", r->cmd);
            ipc_begin(r, r->cmd, 1, 0);
            ipc_put(r, 0);
            break;
    }
}

static void generic_command(Ipc *r, int service);
static void hid_command(Ipc *r, uint32_t *p);
static void fs_command(Ipc *r, uint32_t *p);
static void file_command(Ipc *r, uint32_t *p);
static void cfg_command(Ipc *r, uint32_t *p);
static void dsp_command(Ipc *r, uint32_t *p);
static void gsp_command(Ipc *r, uint32_t *p);
static uint32_t g_cmdlist_addr = 0;
static uint32_t g_cmdlist_size = 0;

static void y2r_command(Ipc *r, uint32_t *p);


static int g_gsp_init_done = 0;
static void hle_try_gsp_init(ArmCpu *c) {
    uint32_t save[16];
    uint32_t tgt = 0x00138D70u;
    const char *e;
    if (g_gsp_init_done) return;
    e = getenv("HLE_GSP_INIT");
    if (e == NULL) return;
    if (*e && strcmp(e, "1") != 0) tgt = (uint32_t)strtoul(e, NULL, 16);
    g_gsp_init_done = 1;
    /* Guard und Felder des GSP-Clients raeumen: er steht auf
       0x01000101 (Init markiert), hat aber weder Handle (+0x10)
       noch State (+0x18) -> fn_00138D70 kehrt sonst sofort zurueck. */
    {
        uint32_t zero = 0;
        memcpy(c->mem + 0x3D91E8u, &zero, 4);
        /* +0x10/+0x14 mit einem ECHTEN gsp::Gpu-Handle belegen: der
           Dienst-Open scheitert, weil der SDK den Namen mit Laenge 9
           (strlen+1) schickt und der Wrapper nur <= 8 erlaubt. Der
           Init kopiert +0x10 -> +0x14 und sendet danach auf +0x14. */
        {
            uint32_t gh = (uint32_t)alloc_handle(SVC_GSP_GPU, "gsp::Gpu");
            memcpy(c->mem + 0x3D91F8u, &gh, 4);
            memcpy(c->mem + 0x3D91FCu, &gh, 4);
            printf("[gspinit] GSP-Handle +0x10/+0x14 = %u\n", gh);
        }
        memcpy(c->mem + 0x3D9200u, &zero, 4);
        /* Guard von 0x28EE00 (srvGetServiceHandle-Wrapper) oeffnen:
           bei <= 0 wird kein Request gesendet (D8A067F8). */
        { uint32_t eight = 8; memcpy(c->mem + 0x3D92ECu, &eight, 4); }
        /* 0x27625C liest den Ziel-Handle aus [0x3D9300]; ohne ihn geht
           der Request an einen unbekannten Dienst. Auf die echte
           srv-Session (Client-Slot 0x3D9098) setzen. */
        {
            uint32_t srvh = 5;   /* srv-Session-Handle der App */
            if (srvh == 0) srvh = 5;
            memcpy(c->mem + 0x3D9300u, &srvh, 4);
            printf("[gspinit] srv-Handle [3D9300] = %u\n", srvh);
        }
        printf("[gspinit] Client geraeumt: [3D91E8]=0 [3D91F8]=0 [3D9200]=0 [3D92EC]=8\n");
    }
    printf("[gspinit] calling 0x%08X (sp=%08X)\n", tgt, c->r[13]);
    memcpy(save, c->r, sizeof(save));
    arm_call_guest_pub(c, tgt);
    memcpy(c->r, save, sizeof(save));
    printf("[gspinit] returned (sp=%08X)\n", c->r[13]);
    /* Diagnose: Renderpfad einmalig direkt anstossen. */
    {
        const char *ff = getenv("HLE_FORCE_FRAME");
        if (ff != NULL && *ff != 0) {
            uint32_t fa = (uint32_t)strtoul(ff, NULL, 16);
            const char *farg = getenv("HLE_FORCE_ARG");
            memcpy(save, c->r, sizeof(save));
            if (farg != NULL && *farg != 0) {
                /* "r0" oder "r0,r1,r2,r3" */
                char buf[128];
                snprintf(buf, sizeof(buf), "%s", farg);
                char *p = buf;
                int ri = 0;
                while (p != NULL && *p != 0 && ri < 4) {
                    char *comma = strchr(p, ',');
                    if (comma != NULL) *comma = 0;
                    c->r[ri++] = (uint32_t)strtoul(p, NULL, 16);
                    p = (comma != NULL) ? comma + 1 : NULL;
                }
            }
            printf("[forceframe] calling 0x%08X r0=%08X (sp=%08X)\n",
                   fa, c->r[0], c->r[13]);
            arm_call_guest_pub(c, fa);
            memcpy(c->r, save, sizeof(save));
            printf("[forceframe] returned (sp=%08X)\n", c->r[13]);
        }
    }
    /* Optionaler zweiter Aufruf (Kette fuer Diagnosen). */
    {
        const char *ff2 = getenv("HLE_FORCE_FRAME2");
        if (ff2 != NULL && *ff2 != 0) {
            uint32_t fa2 = (uint32_t)strtoul(ff2, NULL, 16);
            const char *farg2 = getenv("HLE_FORCE_ARG2");
            memcpy(save, c->r, sizeof(save));
            if (farg2 != NULL && *farg2 != 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "%s", farg2);
                char *pp = buf;
                int ri = 0;
                while (pp != NULL && *pp != 0 && ri < 4) {
                    char *comma = strchr(pp, ',');
                    if (comma != NULL) *comma = 0;
                    c->r[ri++] = (uint32_t)strtoul(pp, NULL, 16);
                    pp = (comma != NULL) ? comma + 1 : NULL;
                }
            }
            printf("[forceframe2] calling 0x%08X r0=%08X\n", fa2, c->r[0]);
            arm_call_guest_pub(c, fa2);
            memcpy(c->r, save, sizeof(save));
            printf("[forceframe2] returned\n");
        }
    }
}
static void put_result(Ipc *r) {
    ipc_put(r, 0);
}

static int g_apt_wakeup_sent = 0;

static void apt_command(Ipc *r, uint32_t *p) {
    switch (r->cmd) {
        case 1: { /* GetLockHandle */
            uint32_t attrs = p[0];
            uint32_t lock = (uint32_t)alloc_handle(SVC_OTHER, "apt-lock");
            ipc_begin(r, 1, 3, 2);
            put_result(r);
            ipc_put(r, attrs);
            /* state: 1 = vollstaendiger Init-Pfad (8 Threads, FS, GSP-Init).
               0 ist der Azahar-Referenzwert, laesst die App aber in der
               APT-Warteschleife stehen (HLE_APT_STATE=0 erzwingt ihn). */
            ipc_put(r, getenv("HLE_APT_STATE")
                        ? (uint32_t)strtoul(getenv("HLE_APT_STATE"), NULL, 0) : 0u);
            ipc_put_handle_copy(r, lock);
            break;
        }
        case 2: { /* Initialize -> { CopyHandle x2, h1, h2 } */
            uint32_t ev1 = (uint32_t)alloc_handle(SVC_OTHER, "apt-notif");
            uint32_t ev2 = (uint32_t)alloc_handle(SVC_OTHER, "apt-param");
            g_apt_param_ev = ev2;
            ipc_begin(r, 2, 1, 3);
            put_result(r);
            if (getenv("HLE_APT_3H")) {
                /* Diagnose: zusaetzlich den Lock-Handle als drittes CopyHandle
                   anbieten (App erwartet offenbar drei Slots). */
                uint32_t lk = (uint32_t)alloc_handle(SVC_OTHER, "apt-lock");
                ipc_begin(r, 2, 1, 5);
                ipc_put(r, 0);
                ipc_put(r, (1u << 26));
                ipc_put(r, ev1);
                ipc_put(r, ev2);
                ipc_put_handle_copy(r, lk);
                break;
            }
            ipc_put(r, (1u << 26));   /* CopyHandle-Deskriptor, 2 Handles */
            if (getenv("HLE_APT_SWAP")) {
                ipc_put(r, ev2);
                ipc_put(r, ev1);
            } else {
                ipc_put(r, ev1);
                ipc_put(r, ev2);
            }
            break;
        }
        case 0x9: { /* NotifyToWait (Azahar: 1 Wort Result) */
            ipc_begin(r, 0x9, 1, 0);
            put_result(r);
            break;
        }
        case 0x4B: { /* AppletUtility (Azahar: Result + Ausgabewert) */
            ipc_begin(r, 0x4B, 2, 0);
            put_result(r);
            ipc_put(r, 0);
            break;
        }
        case 0x4F: { /* SetAppCpuTimeLimit */
            ipc_begin(r, 0x4F, 1, 0);
            put_result(r);
            break;
        }
        case 0x50: { /* GetAppCpuTimeLimit -> (Result, Limit) */
            ipc_begin(r, 0x50, 2, 0);
            put_result(r);
            ipc_put(r, 0x1Eu);      /* 30 % */
            hle_try_gsp_init(g_cpu);
            break;
        }
        case 3: case 4: case 0x28: { /* Enable / Finalize / NotifyToWait */
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            break;
        }
        case 5: { /* GetAppletManInfo */
            ipc_begin(r, 5, 5, 0);
            put_result(r);
            ipc_put(r, 1);         /* active applet position */
            ipc_put(r, 0x101);     /* requested applet id */
            ipc_put(r, 0x101);     /* home menu id */
            ipc_put(r, 0x100);     /* active applet id */
            break;
        }
        case 0x0D: case 0x0E: { /* ReceiveParameter / GlanceParameter */
            uint32_t dsc_flags = 0, dsc_addr = 0;
            if (r->translate >= 2) {
                dsc_flags = p[r->normal];
                dsc_addr = p[r->normal + 1];
            }
            /* Hardware-Semantik: GlanceParameter (0x000E) konsumiert den
               Parameter NICHT, ReceiveParameter (0x000D) schon. Der Wakeup
               wird erst zugestellt, wenn die Main-Loop bereits pollt
               (HLE_APT_WAKE_AT, Default 40 Aufrufe), danach NoData. */
            {
                static unsigned rx_calls = 0;
                unsigned wake_at = 1u;   /* Referenz: Parameter liegt schon bei Initialize in der Queue */
                const char *wa = getenv("HLE_APT_WAKE_AT");
                if (wa != NULL && *wa != 0) wake_at = (unsigned)strtoul(wa, NULL, 0);
                rx_calls++;
                if (!g_apt_wakeup_sent && rx_calls >= wake_at) {
                    if (r->cmd == 0x000D) g_apt_wakeup_sent = 1;
                    /* Referenz: sender=0, signal=1 (Wakeup), Groesse=0.
                       Der Header belegt cmdbuf[0] -> Dummy-Wort, damit
                       Result/Sender/Signal/Groesse auf cmdbuf[1..4] liegen. */
                    ipc_begin(r, r->cmd, 5, r->translate >= 2 ? 2 : 0);
                    ipc_put(r, 0);          /* Dummy (Header-Slot) */
                    ipc_put(r, 0);          /* Result   -> cmdbuf[1] */
                    ipc_put(r, getenv("HLE_APT_SENDER")
                                ? (uint32_t)strtoul(getenv("HLE_APT_SENDER"), NULL, 0) : 0u);
                    ipc_put(r, getenv("HLE_APT_SIG")
                                ? (uint32_t)strtoul(getenv("HLE_APT_SIG"), NULL, 0) : 1u);
                    ipc_put(r, 0);          /* Parameter-Groesse -> cmdbuf[4] */
                    if (r->translate >= 2) {
                        ipc_put(r, dsc_flags);
                        ipc_put(r, dsc_addr);
                    }
                    if (g_apt_param_ev != 0) {
                        Handle *ph = get_handle(g_apt_param_ev);
                        if (ph != NULL) ph->signaled = 1;
                    }
                    if (g_log) printf("[apt] ReceiveParameter -> Wakeup (#%u)\n", rx_calls);
                } else {
                    ipc_begin(r, r->cmd, 1, 0);
                    /* Result(NoData=1007, Applet=51, InvalidState=5, Status=25) */
                    ipc_put(r, 0xC8A303EFu);   /* Result(NoData=1007, Applet=51, InvalidState=5, Status=25) */
                }
            }
            break;
        }
        case 0x0B: case 0x43: { /* InquireNotification / ? */
            ipc_begin(r, r->cmd, 2, 0);
            put_result(r);
            ipc_put(r, 0);
            break;
        }
        default:
            generic_command(r, SVC_APT_U);
            break;
    }
}

static void hid_command(Ipc *rp, uint32_t *p) {
    (void)p;
    switch (rp->cmd) {
        case 0x000A: { /* GetIPCHandles: SharedMem + 5 Event-Handles */
            uint32_t shmem = (uint32_t)alloc_handle(SVC_OTHER, "hid-shmem");
            uint32_t h[5];
            for (int i = 0; i < 5; i++) h[i] = (uint32_t)alloc_handle(SVC_OTHER, "hid-ev");
            ipc_begin(rp, 0x000A, 1, 7);
            ipc_put(rp, 0);
            ipc_put(rp, (5u << 26));       /* CopyHandle-Deskriptor, 6 Handles */
            ipc_put(rp, shmem);
            for (int i = 0; i < 5; i++) ipc_put(rp, h[i]);
            break;
        }
        default:
            generic_command(rp, SVC_HID_USER);
            break;
    }
}

/* Sucht im Command-Buffer nach einem Pfadstring (ASCII oder UTF-16LE). */
static int find_path(const uint32_t *buf, char *out, int out_size) {
    const uint8_t *base = (const uint8_t *)buf;
    int best_len = 0;
    for (int mode = 0; mode < 2; mode++) {
        for (int off = 0; off + 8 < 0x100; off += (mode == 0 ? 1 : 2)) {
            char tmp[256];
            int n = 0;
            if (mode == 0) {
                for (int i = 0; i < 250; i++) {
                    uint8_t c = base[off + i];
                    if (c == 0) break;
                    if (c < 0x20 || c > 0x7E) { n = -1; break; }
                    tmp[n++] = (char)c;
                }
            } else {
                for (int i = 0; i < 120; i++) {
                    uint8_t lo = base[off + i * 2];
                    uint8_t hi = base[off + i * 2 + 1];
                    if (lo == 0 && hi == 0) break;
                    if (hi != 0 || lo < 0x20 || lo > 0x7E) { n = -1; break; }
                    tmp[n++] = (char)lo;
                }
            }
            if (n < 3) continue;
            tmp[n] = 0;
            int score = 0;
            for (int i = 0; i < n; i++) {
                if (tmp[i] == '/') score += 2;
                if (tmp[i] == '.') score += 1;
                if (tmp[i] == ':') score += 1;
            }
            if (score < 2) continue;
            if (n > best_len) {
                best_len = n;
                snprintf(out, out_size, "%s", tmp);
            }
        }
    }
    return best_len;
}

static void ensure_savedata(void) {
#ifdef _WIN32
    _mkdir("out/savedata");
#else
    mkdir("out/savedata", 0755);
#endif
}

/* Sucht eine Datei zuerst in out/savedata, dann in out/romfs. */
static int open_guest_file(const char *guest_path, FileSlot **slot_out) {
    char q[256];
    char sd[320];
    const char *src = guest_path;
    if (strncmp(src, "romfs:", 6) == 0) src += 6;
    /* Das Spiel adressiert RomFS-Dateien mit dem Praefix "rom:"
       (Literal 0x1023AC), nicht "romfs:". */
    else if (strncmp(src, "rom:", 4) == 0) src += 4;
    while (*src == '/') src++;
    snprintf(q, sizeof(q), "%s", src);
    for (char *c = q; *c; c++) if (*c == '/') *c = '\\';
    snprintf(sd, sizeof(sd), "out/savedata/%s", q);

    FILE *fp = fopen(sd, "r+b");
    const char *used_path = sd;
    if (!fp) {
        char rom[320];
        snprintf(rom, sizeof(rom), "%s\\%s", g_romfs, q);
        FILE *rf = fopen(rom, "rb");
        if (rf) {
            fseek(rf, 0, SEEK_END);
            long rsz = ftell(rf);
            fseek(rf, 0, SEEK_SET);
            if (rsz <= 0x40000) {
                /* Kleine Datei (Save/Settings): in out/savedata spiegeln,
                   damit Schreibzugriffe und erneutes Oeffnen persistent sind. */
                FILE *wf = fopen(sd, "wb");
                if (wf) {
                    char buf[4096];
                    size_t got;
                    while ((got = fread(buf, 1, sizeof(buf), rf)) > 0)
                        fwrite(buf, 1, got, wf);
                    fclose(wf);
                }
                fclose(rf);
                fp = fopen(sd, "r+b");
            } else {
                fp = rf;
                used_path = rom;
            }
        } else {
            /* Unbekannter Pfad: als neue, leere Datei anlegen. */
            fp = fopen(sd, "w+b");
        }
    }
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    for (int i = 0; i < MAX_FILES; i++) {
        if (!g_files[i].used) {
            g_files[i].used = 1;
            snprintf(g_files[i].path, sizeof(g_files[i].path), "%s", used_path);
            g_files[i].fp = fp;
            g_files[i].size = (uint32_t)sz;
            *slot_out = &g_files[i];
            return 1;
        }
    }
    fclose(fp);
    return 0;
}

static void fs_command(Ipc *r, uint32_t *p) {
    /* Der FS-Client der App prueft [0x003D91D0] auf != 0 und ruft sonst
       svcBreak (Assertion in 0x2763B8). Wir hinterlegen dort den aktuellen
       fs:USER-Handle, sobald die App irgendeinen FS-Befehl schickt. */
    if (g_cpu != NULL && (0x3D91D0u & 0x1FFFFFFFu) + 4 <= g_cpu->mem_size) {
        uint32_t hv = (uint32_t)g_cur_handle;
        if (hv == 0) hv = 0x1000u;
        memcpy(g_cpu->mem + 0x3D91D0u, &hv, 4);
    }
    if (getenv("HLE_FS_PATHS")) {
        printf("[fscmd] cmd=0x%04X n=%d t=%d", r->cmd, r->normal, r->translate);
        for (int i = 0; i < r->normal && i < 8; i++) printf(" p%d=%08X", i, p[i]);
        for (int q = 0; q + 1 < r->translate; q += 2) {
            uint32_t fl = p[r->normal + q];
            uint32_t ad = p[r->normal + q + 1];
            printf(" | b%d fl=%08X a=%08X:", q / 2, fl, ad);
            if (ad >= 0x08000000u && ad < 0x20000000u) {
                const uint8_t *bb = g_cur_mem + (ad & 0x1FFFFFFFu);
                for (int k = 0; k < 20; k++) printf(" %02X", bb[k]);
            }
        }
        printf("\n");
    }
    switch (r->cmd) {
        case 0x0801: { /* Initialize */
            ipc_begin(r, 0x0801, 1, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0802: case 0x0803: { /* OpenFile / OpenFileDirectly */
            char path[256];
            path[0] = 0;
            /* Translate-Bereich: pro Puffer {size_flags, adresse}, Groesse = flags >> 4. */
            uint32_t ftype, fsize;
            if (r->cmd == 0x0802) {
                ftype = (r->normal >= 5) ? p[3] : 3;
                fsize = (r->normal >= 5) ? p[4] : 0;
            } else {
                ftype = (r->normal >= 6) ? p[4] : 3;
                fsize = (r->normal >= 6) ? p[5] : 0;
            }
            uint32_t addr = 0, len = 0;
            if (r->normal >= 5 && r->translate >= 2) {
                int idx = (r->cmd == 0x0803 && r->translate >= 4) ? 2 : 0;
                uint32_t flags = p[r->normal + idx];
                addr = p[r->normal + idx + 1];
                len = flags >> 4;
            }
            if (g_log) {
                printf("[fsdbg-raw] cmd=%04X p:", r->cmd);
                for (int i = 0; i < r->normal && i < 10; i++) printf(" %08X", p[i]);
                printf("\n");
                printf("[fsdbg] cmd=%04X n=%d t=%d type=%u size=%u addr=%08X len=%u",
                       r->cmd, r->normal, r->translate, ftype, fsize, addr, len);
                if (addr) {
                    const uint8_t *dbg = g_cur_mem + (addr & 0x1FFFFFFFu);
                    printf(" bytes:");
                    for (int i = 0; i < 32; i++) printf(" %02X", dbg[i]);
                }
                /* Alle Translate-Paare mitschreiben, um den echten Pfadpuffer zu finden. */
                for (int q = 0; q + 1 < r->translate; q += 2) {
                    uint32_t f2 = p[r->normal + q];
                    uint32_t a2 = p[r->normal + q + 1];
                    printf(" | t%d flags=%08X addr=%08X:", q / 2, f2, a2);
                    if (a2 >= 0x08000000u && a2 < 0x20000000u) {
                        const uint8_t *b2 = g_cur_mem + (a2 & 0x1FFFFFFFu);
                        for (int k = 0; k < 24; k++) printf(" %02X", b2[k]);
                    }
                }
                printf("\n");
            }
            /* Pfadpuffer liegen auch im .data-Bereich (z. B. 0x3CF74A),
               nicht nur im Heap ab 0x08000000. */
            if (addr >= 0x00100000u && addr < 0x20000000u) {
                const uint8_t *src = g_cur_mem + (addr & 0x1FFFFFFFu);
                uint32_t limit = len ? len : fsize;
                if (limit > 250 || limit < 2) limit = 250;
                int n = 0;
                /* Robuste Erkennung: Wchar, wenn das zweite Byte 0 ist. */
                if (src[1] == 0) {
                    for (uint32_t i = 0; i + 1 < limit && n < 250; i += 2) {
                        if (src[i] == 0) break;
                        path[n++] = (char)src[i];
                    }
                } else {
                    for (uint32_t i = 0; i < limit && n < 250; i++) {
                        if (src[i] == 0) break;
                        path[n++] = (char)src[i];
                    }
                }
                path[n] = 0;
            }
            if (getenv("HLE_FS_PATHS")) {
                printf("[fspath] n=%d t=%d ftype=%u fsize=%u",
                       r->normal, r->translate, p[4], p[5]);
                for (int q = 0; q + 1 < r->translate; q += 2) {
                    uint32_t fl = p[r->normal + q];
                    uint32_t ad = p[r->normal + q + 1];
                    printf(" | b%d fl=%08X a=%08X:", q / 2, fl, ad);
                    if (ad >= 0x08000000u && ad < 0x20000000u) {
                        const uint8_t *bb = g_cur_mem + (ad & 0x1FFFFFFFu);
                        for (int k = 0; k < 24; k++) printf(" %02X", bb[k]);
                    }
                }
                printf("\n");
            }
            FileSlot *slot = NULL;
            int opened = 0;
            /* SelfNCCH (Archive-ID 3): das Spiel oeffnet sein eigenes RomFS
               direkt. Wir liefern das extrahierte Image (IVFC-Header + Daten). */
            if (r->cmd == 0x0803 && r->normal >= 2 && p[1] == 3) {
                FILE *img = fopen("out/romfs_image.bin", "rb");
                if (img != NULL) {
                    fseek(img, 0, SEEK_END);
                    long isz = ftell(img);
                    fseek(img, 0, SEEK_SET);
                    for (int i = 0; i < MAX_FILES; i++) {
                        if (!g_files[i].used) {
                            g_files[i].used = 1;
                            g_files[i].fp = img;
                            g_files[i].size = (uint32_t)isz;
                            snprintf(g_files[i].path, sizeof(g_files[i].path),
                                     "out/romfs_image.bin");
                            slot = &g_files[i];
                            opened = 1;
                            if (g_log) printf("[fs] SelfNCCH-Image geoeffnet (%ld Byte)\n", isz);
                            break;
                        }
                    }
                    if (!opened) fclose(img);
                }
            }
            if (!opened && path[0]) opened = open_guest_file(path, &slot);
            if (!opened) {
                /* Kein/leerer Pfad: als Archiv-Wurzel (Sentinel) bedienen. */
                for (int i = 0; i < MAX_FILES; i++) {
                    if (!g_files[i].used) {
                        g_files[i].used = 1;
                        g_files[i].fp = NULL;
                        g_files[i].size = (path[0] == 0) ? 0u : 0x400u;
                        snprintf(g_files[i].path, sizeof(g_files[i].path), "out/savedata/save_%d.sav", i);
                        slot = &g_files[i];
                        opened = 1;
                        if (g_log) printf("[fs] Sentinel-Slot %d (path=\"%s\" size=%u)\n",
                                         i, path, g_files[i].size);
                        break;
                    }
                }
            }
            uint32_t h = 0;
            if (opened) {
                h = (uint32_t)alloc_handle(SVC_FILE, "file");
                if (h && h < MAX_HANDLES) g_handles[h].file = (int)(slot - g_files);
            }
            if (g_log) {
                printf("[fs] dump:");
                for (int i = 0; i < 24; i++) printf(" %08X", p[i]);
                printf("\n     ascii:");
                for (int i = 0; i < 96; i++) {
                    uint8_t ch = ((const uint8_t *)p)[i];
                    printf("%c", (ch >= 0x20 && ch < 0x7F) ? ch : '.');
                }
                printf("\n");
                if (g_log) {
                    for (int k = r->normal; k + 1 < r->normal + r->translate; k += 2) {
                        uint32_t a2 = p[k + 1];
                        if (a2 >= 0x08000000u && a2 < 0x20000000u) {
                            printf("[fs] mem 0x%08X:", a2);
                            for (int b = 0; b < 20; b++) {
                                printf(" %02X", g_cur_mem[(a2 & 0x1FFFFFFFu) + b]);
                            }
                            printf("\n");
                        }
                    }
                }
                if (addr >= 0x08000000u && addr < 0x20000000u) {
                    printf("[fs] buf@0x%08X len=%u bytes:", addr, len);
                    for (uint32_t i = 0; i < (len < 16 ? len : 16); i++) {
                        printf(" %02X", g_cur_mem[(addr & 0x1FFFFFFFu) + i]);
                    }
                    printf("\n");
                }
                printf("[fs] %s \"%s\" -> %s (handle %u, %u KiB)\n",
                       r->cmd == 0x0802 ? "OpenFile" : "OpenFileDirectly", path,
                       opened ? "ok" : "FEHLER", h, opened ? slot->size / 1024 : 0);
            }
            /* Die SDK (fn_001078F4) prueft Reply-Wort 2 gegen 0x28; deshalb
               wird dort die erwartete Groesse geliefert, danach der Handle. */
            ipc_begin(r, r->cmd, 2, 2);
            ipc_put(r, 0);
            ipc_put(r, 0x28u);
            ipc_put_handle(r, h);
            /* Datei-Client-Objekt (0x3D66C0+4) mitversorgen: der zugehoerige
               Setter im Spiel laeuft nie, sonst werfen Dateizugriffe E0A046DA. */
            if (h != 0 && g_cpu != NULL && (0x3D66C4u & 0x1FFFFFFFu) + 4 <= g_cpu->mem_size)
                memcpy(g_cpu->mem + 0x3D66C4u, &h, 4);
            break;
        }
        case 0x0861: case 0x0862: { /* InitializeWithSdkVersion / SetPriority -> Result */
            ipc_begin(r, r->cmd, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0863: { /* GetPriority -> Result + Priority */
            ipc_begin(r, 0x0863, 3, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x080C: { /* OpenArchive */
            char path[256];
            find_path(p, path, sizeof(path));
            uint32_t h = (uint32_t)alloc_handle(SVC_FS_USER, "archive");
            if (g_log) printf("[fs] OpenArchive id=0x%08X type=%u size=%u path=\"%s\" -> handle %u\n",
                              p[0], p[1], p[2], path, h);
            /* Der FS-Client der App (Objekt 0x3D66C0) braucht den
               Archiv-Handle in +4, sonst wirft 0x212CC4 E0A046DA. */
            if (g_cpu != NULL && (0x3D66C4u & 0x1FFFFFFFu) + 4 <= g_cpu->mem_size)
                memcpy(g_cpu->mem + 0x3D66C4u, &h, 4);
            /* Azahar: MakeBuilder(3,0) -> result, ArchiveHandle, 0 */
            ipc_begin(r, 0x080C, 3, 0);
            ipc_put(r, 0);
            ipc_put(r, h);
            ipc_put(r, 0);
            break;
        }
        case 0x0808: { /* CreateFile -> MakeBuilder(1,0) */
            char cpath[256];
            find_path(p, cpath, sizeof(cpath));
            ipc_begin(r, 0x0808, 1, 0);
            ipc_put(r, 0);
            if (g_log) printf("[fs] CreateFile \"%s\" -> ok\n", cpath);
            break;
        }
        case 0x080D: { /* ControlArchive */
            ipc_begin(r, 0x080D, 1, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x080E: { /* CloseArchive */
            ipc_begin(r, 0x080E, 1, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0812: { /* GetFreeBytes */
            ipc_begin(r, 0x0812, 3, 0);
            ipc_put(r, 0);
            ipc_put(r, 0x10000000u);
            ipc_put(r, 0);
            break;
        }
        case 0x0817: case 0x0818: { /* IsSdmcDetected / Writeable */
            ipc_begin(r, r->cmd, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            break;
        }
        default:
            generic_command(r, SVC_FS_USER);
            break;
    }
}

static uint32_t ipc_out_buffer(uint32_t *p, int normal, int translate) {
    for (int i = 0; i < translate; i++) {
        uint32_t w = p[normal + i];
        if (w >= 0x08000000u && w < 0x20000000u) return w;
    }
    return 0;
}

static void file_command(Ipc *r, uint32_t *p) {
    Handle *h = get_handle(g_cur_handle);
    if (h == NULL || h->file < 0) {
        /* Richtige Reply-Form auch ohne Datei, sonst wertet die SDK
           "0 Bytes gelesen" als Fehler (F96047A2). */
        if (r->cmd == 0x0802 || r->cmd == 0x0803) {
            ipc_begin(r, r->cmd, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
        } else if (r->cmd == 0x0804) {
            ipc_begin(r, r->cmd, 3, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
        } else {
            ipc_begin(r, r->cmd, 1, 0);
            ipc_put(r, 0);
        }
        return;
    }
    FileSlot *f = &g_files[h->file];
    switch (r->cmd) {
        case 0x0802: { /* Read: (offset u64, size) + MappedBuffer */
            uint64_t offset = (uint64_t)p[0] | ((uint64_t)p[1] << 32);
            uint32_t length = p[2];
            if (length > 0x01000000u) { /* Fallback: vertauschte Reihenfolge */
                offset = 0;
                length = p[0];
            }
            uint32_t addr = 0;
            /* Translate-Deskriptor: {size|flags, adresse} -> Adresse ist das ZWEITE Wort. */
            if (r->translate >= 2) addr = p[r->normal + 1];
            uint32_t done = 0;
            if (addr && length) {
                if (f->fp) {
                    /* Das SelfNCCH-Image enthaelt ein 0x1000-Byte-IVFC-Praefix.
                       Die Engine parst die RomFS-Metadaten aber ab Offset 0
                       (Header @0, Hash-Tabellen @0x28/0x160, Metadaten @0x4C/
                       0xAF4, Dateidaten @0xDD80). */
                    long foff = (long)offset;
                    if (f->path != NULL && strstr(f->path, "romfs_image.bin") != NULL &&
                        getenv("HLE_ROMFS_NO_STRIP") == NULL) {
                        foff += 0x1000;
                    }
                    fseek(f->fp, foff, SEEK_SET);
                    done = (uint32_t)fread(g_cur_mem + (addr & 0x1FFFFFFFu), 1, length, f->fp);
                } else {
                    /* Synthetische Datei: Nullen liefern (SDK prueft >0 Bytes) */
                    uint32_t avail = (offset < f->size) ? (uint32_t)(f->size - offset) : 0;
                    done = length < avail ? length : avail;
                    if (done) memset(g_cur_mem + (addr & 0x1FFFFFFFu), 0, done);
                }
            }
            if (g_log) printf("[file] Read %s off=0x%llX len=0x%X p0=%08X addr=%08X -> %u\n",
                              f->path, (unsigned long long)offset, length, p[0], addr, done);
            /* Azahar: MakeBuilder(2,2) + PushMappedBuffer -> Descriptor-Echo */
            ipc_begin(r, 0x0802, 2, 2);
            ipc_put(r, 0);
            ipc_put(r, done);
            ipc_put(r, p[r->normal]);
            ipc_put(r, p[r->normal + 1]);
            break;
        }
        case 0x0803: { /* Write: (offset u64, size) + MappedBuffer */
            uint64_t offset = (uint64_t)p[0] | ((uint64_t)p[1] << 32);
            uint32_t length = p[2];
            if (length > 0x01000000u) { /* Fallback */
                offset = 0;
                length = p[0];
            }
            uint32_t addr = 0;
            /* Translate-Deskriptor: {size|flags, adresse} -> Adresse ist das ZWEITE Wort. */
            if (r->translate >= 2) addr = p[r->normal + 1];
            uint32_t done = 0;
            if (addr && length) {
                if (f->fp == NULL) f->fp = fopen(f->path, "w+b");
                if (f->fp) {
                    fseek(f->fp, (long)offset, SEEK_SET);
                    done = (uint32_t)fwrite(g_cur_mem + (addr & 0x1FFFFFFFu), 1, length, f->fp);
                    fflush(f->fp);
                    if (offset + done > f->size) f->size = (uint32_t)(offset + done);
                }
            }
            if (g_log) printf("[file] Write off=0x%llX len=0x%X addr=%08X -> %u\n",
                              (unsigned long long)offset, length, addr, done);
            /* Azahar: MakeBuilder(2,2) + PushMappedBuffer -> Descriptor-Echo */
            ipc_begin(r, 0x0803, 2, 2);
            ipc_put(r, 0);
            ipc_put(r, done);
            ipc_put(r, p[r->normal]);
            ipc_put(r, p[r->normal + 1]);
            break;
        }
        case 0x0804: { /* GetSize -> Result + u64 */
            ipc_begin(r, 0x0804, 3, 0);
            ipc_put(r, 0);
            ipc_put(r, f->size);
            ipc_put(r, 0);
            break;
        }
        case 0x0805: { /* SetSize(size u64) */
            uint32_t lo = p[0], hi = p[1];
            f->size = lo;
            (void)hi;
            ipc_begin(r, 0x0805, 1, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0808: { /* Close */
            if (f->fp) fclose(f->fp);
            f->used = 0;
            f->fp = NULL;
            h->file = -1;
            ipc_begin(r, 0x0808, 1, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0809: { /* Flush */
            if (f->fp) fflush(f->fp);
            ipc_begin(r, 0x0809, 1, 0);
            ipc_put(r, 0);
            break;
        }
        default:
            if (g_log) printf("[file] cmd 0x%04X -> 0\n", r->cmd);
            if (r->cmd == 0x080A) { /* SetPriority -> Result + Priority */
                ipc_begin(r, 0x080A, 2, 0);
                ipc_put(r, 0);
                ipc_put(r, 0);
                break;
            }
            ipc_begin(r, r->cmd, 1, 0);
            ipc_put(r, 0);
            break;
    }
}


static void cfg_command(Ipc *r, uint32_t *p) {
    switch (r->cmd) {
        case 0x0001: { /* GetConfig(size, block_id) */
            uint32_t size = p[0];
            uint32_t block = p[1];
            uint32_t addr = ipc_out_buffer(p, r->normal, r->translate);
            if (size > 0x200) size = 0x200;
            if (addr) {
                uint8_t *dst = g_cur_mem + (addr & 0x1FFFFFFFu);
                memset(dst, 0, size);
                if (block == 0x000A0000) {         /* UsernameBlock (UTF-16) */
                    const char *name = "LAYTON";
                    for (int i = 0; name[i] && (uint32_t)(i * 2 + 1) < size; i++) {
                        dst[i * 2] = (uint8_t)name[i];
                    }
                } else if (block == 0x000A0002) {  /* LanguageBlock */
                    dst[0] = 1;                    /* Englisch */
                } else if (block == 0x000B0000) {  /* CountryInfoBlock */
                    dst[2] = 49;                   /* USA */
                }
            }
            if (g_log) printf("[cfg] GetConfig block=0x%08X size=%u buf=0x%08X\n", block, size, addr);
            ipc_begin(r, 0x0001, 1, r->translate);
            ipc_put(r, 0);
            for (int i = 0; i < r->translate; i++) ipc_put(r, p[r->normal + i]);
            break;
        }
        case 0x0002: { /* GetRegion */
            ipc_begin(r, 0x0002, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 2);      /* EUR */
            break;
        }
        case 0x0003: { /* GetTransferableId(salt) -> Result + u64 */
            ipc_begin(r, 0x0003, 4, 0);
            ipc_put(r, 0);              /* Dummy (Header-Slot) */
            ipc_put(r, 0);              /* Result */
            ipc_put(r, 0x12345678u);    /* low  */
            ipc_put(r, 0x00000000u);    /* high */
            if (g_log) printf("[cfg] GetTransferableId salt=0x%X -> ok\n", p[0]);
            break;
        }
        case 0x0005: { /* GetSystemModel */
            ipc_begin(r, 0x0005, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x000B: { /* IsFangateSupported */
            ipc_begin(r, 0x000B, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            break;
        }
        default:
            generic_command(r, SVC_CFG_U);
            break;
    }
}

static void ndm_command(Ipc *r, uint32_t *p) {
    (void)p;
    switch (r->cmd) {
        case 0x0006:   /* SuspendDaemons(bit_mask) -> Result */
        case 0x0007:   /* ResumeDaemons */
        case 0x0014:   /* OverrideDefaultDaemons */
        case 0x0015:   /* ResetDefaultDaemons */
        case 0x0001:   /* EnterExclusiveState */
        case 0x0002:   /* LeaveExclusiveState */
        case 0x0004: case 0x0005:   /* LockState / UnlockState */
        case 0x0008: case 0x0009: { /* SuspendScheduler / ResumeScheduler */
            ipc_begin(r, r->cmd, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            break;
        }
        default:
            generic_command(r, SVC_OTHER);
            break;
    }
}

static void boss_command(Ipc *r, uint32_t *p) {
    (void)p;
    switch (r->cmd) {
        case 0x0001:   /* InitializeSession(program_id) -> Result */
        case 0x0006: { /* RegisterPrivateClientCert -> Result */
            ipc_begin(r, r->cmd, 2, 0);
            ipc_put(r, 0);      /* Dummy (Header-Slot) */
            ipc_put(r, 0);      /* ResultSuccess */
            if (g_log) printf("[boss] cmd 0x%04X -> ok\n", r->cmd);
            break;
        }
        case 0x000A: { /* GetOptoutFlag -> Result + u8 flag */
            ipc_begin(r, 0x000A, 3, 0);
            ipc_put(r, 0);      /* Dummy */
            ipc_put(r, 0);      /* ResultSuccess */
            ipc_put(r, 0);      /* output_flag = 0 */
            break;
        }
        default:
            generic_command(r, SVC_BOSS);
            break;
    }
}

static void ptm_command(Ipc *r, uint32_t *p) {
    (void)p;
    switch (r->cmd) {
        case 0x000B: { /* GetStepHistory -> Result */
            ipc_begin(r, 0x000B, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x000C: { /* GetTotalStepCount -> Result + u32 */
            ipc_begin(r, 0x000C, 3, 0);
            ipc_put(r, 0);      /* Dummy (Header-Slot) */
            ipc_put(r, 0);      /* Result */
            ipc_put(r, 0);      /* StepCount */
            break;
        }
        default:
            generic_command(r, SVC_PTM_U);
            break;
    }
}

static void dsp_command(Ipc *r, uint32_t *p) {
    switch (r->cmd) {
        case 0x0010: { /* ReadPipeIfPossible(ch, peer, size + StaticBuffer) */
            uint32_t dsc_flags = 0, dsc_addr = 0;
            static int rp_calls = 0;
            if (r->translate >= 2) {
                dsc_flags = p[r->normal];
                dsc_addr = p[r->normal + 1];
            }
            rp_calls++;
            {
                /* Erster Read: Anzahl DSP-Variablen (1). Zweiter: die
                   Variablenliste (eine DSP-DRAM-Adresse). */
                uint16_t val = (rp_calls & 1) ? 1u : 0x0000u;
                if (dsc_addr && g_cpu != NULL &&
                    (dsc_addr & 0x1FFFFFFFu) + 2u <= g_cpu->mem_size)
                    memcpy(g_cur_mem + (dsc_addr & 0x1FFFFFFFu), &val, 2);
            }
            /* Dummy + Result + length_read(2) + Buffer-Echo */
            ipc_begin(r, 0x0010, 3, 2);
            ipc_put(r, 0);
            ipc_put(r, 0);
            ipc_put(r, 2);
            ipc_put(r, dsc_flags);
            ipc_put(r, dsc_addr);
            if (g_log) printf("[dsp] ReadPipeIfPossible ch=%u size=%u -> 2\n", p[0], p[2]);
            break;
        }
        case 0x000C: { /* ConvertProcessAddressFromDspDram(addr) -> Result + ARM-Adresse */
            uint32_t dsp_addr = p[0];
            uint32_t arm = 0x00408800u + (dsp_addr & 0xFFu) * 0x10u;
            ipc_begin(r, 0x000C, 3, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            ipc_put(r, arm);
            if (g_log) printf("[dsp] ConvertProcessAddress 0x%X -> 0x%X\n", dsp_addr, arm);
            break;
        }
        case 0x0011: { /* LoadComponent: (u32 size, u16 prog, u16 data, MappedBuffer) */
            uint32_t dsc_flags = 0, dsc_addr = 0;
            if (r->translate >= 2) {
                dsc_flags = p[r->normal];
                dsc_addr = p[r->normal + 1];
            }
            ipc_begin(r, 0x0011, 2, 2);
            ipc_put(r, 0);          /* ResultSuccess */
            ipc_put(r, 1);          /* true = geladen */
            ipc_put(r, dsc_flags);
            ipc_put(r, dsc_addr);
            if (g_log) printf("[dsp] LoadComponent size=0x%X -> ok\n", p[0]);
            break;
        }
        case 0x000D: { /* WriteProcessPipe(channel, size, buffer) -> Result */
            uint32_t size = p[1];
            uint32_t addr = 0;
            if (r->translate >= 2) addr = p[r->normal + 1];
            ipc_begin(r, 0x000D, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            if (g_log) printf("[dsp] WriteProcessPipe ch=%u size=%u addr=%08X\n",
                              p[0], size, addr);
            break;
        }
        case 0x000E: { /* ReadPipe(ch, size, flags, buffer) -> Result + StaticBuffer */
            uint32_t dsc_flags = 0, dsc_addr = 0;
            if (r->translate >= 2) {
                dsc_flags = p[r->normal];
                dsc_addr = p[r->normal + 1];
            }
            ipc_begin(r, 0x000E, 1, 2);
            ipc_put(r, 0);
            ipc_put(r, dsc_flags);
            ipc_put(r, dsc_addr);
            if (g_log) printf("[dsp] ReadPipe ch=%u size=%u -> 0\n", p[0], p[1]);
            break;
        }
        case 0x0012: { /* UnloadComponent */
            ipc_begin(r, 0x0012, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0001: { /* RecvData -> Result + Wert (1 = DSP-Finalize-Quittung) */
            ipc_begin(r, 0x0001, 3, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            ipc_put(r, 1);
            break;
        }
        case 0x0002: case 0x0004: case 0x000B: { /* RecvDataIsReady/IsEmpty/CheckSemaphore */
            static unsigned dsp_calls = 0;
            unsigned warm = 0;
            const char *dr = getenv("HLE_DSP_READY");
            if (dr != NULL && *dr != 0) warm = (unsigned)strtoul(dr, NULL, 0);
            dsp_calls++;
            ipc_begin(r, r->cmd, 3, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            ipc_put(r, getenv("HLE_DSP_ZERO") ? 0u
                        : ((warm && dsp_calls <= warm) ? 0u : 1u));
            if (getenv("HLE_DSP_DEBUG")) {
                printf("[dsp2] n=%d t=%d w=%d:", r->normal, r->translate, r->written);
                for (int i = 0; i < r->written; i++) printf(" %08X", r->buf[i]);
                printf("\n");
                fflush(stdout);
            }
            break;
        }
        case 0x0003: case 0x0005: case 0x0009:
        case 0x000A: { /* Set/Send/Clear/Mask/… */
            ipc_begin(r, r->cmd, 1, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0007: case 0x0017: { /* SetSemaphore / SetSemaphoreMask -> Result */
            ipc_begin(r, r->cmd, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0006: case 0x0008: { /* RecvFifoEx/GetSemaphore -> Result+Wert */
            ipc_begin(r, r->cmd, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0016: { /* GetSemaphoreEventHandle -> Result + MoveHandle(Event) */
            uint32_t ev = (uint32_t)alloc_handle(SVC_OTHER, "dsp-ev");
            Handle *hh = get_handle(ev);
            if (hh) hh->signaled = 1;
            ipc_begin(r, 0x0016, 1, 3);
            ipc_put(r, 0);          /* Dummy (Header-Slot) */
            ipc_put(r, 0);          /* Result -> cmdbuf[1] */
            ipc_put_handle(r, ev);  /* Descriptor+Handle -> cmdbuf[2],[3] */
            if (g_log) printf("[dsp] GetSemaphoreEventHandle -> %u\n", ev);
            break;
        }
        case 0x000F: { /* GetPipeReadableSize */
            ipc_begin(r, 0x000F, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0015: { /* RegisterInterruptEvents */
            uint32_t h = (uint32_t)alloc_handle(SVC_OTHER, "dsp-ev");
            ipc_begin(r, 0x0015, 1, 2);
            ipc_put(r, 0);
            ipc_put_handle_copy(r, h);
            break;
        }
        default:
            generic_command(r, SVC_DSP);
            break;
    }
}

/* GSP-Service: minimal, damit ein Aufruf des Spiels nicht scheitert.
 * WriteHWRegs (0x0001), SetBufferSwap (0x0004), RegisterInterruptRelayQueue
 * (0x0006), TriggerCmdReqQueue (0x0008), ImportDisplayCaptureInfo (0x0009). */
static int g_gsp_last_screen = 0;

/* Verarbeitet den GSP-Shared-Memory-Kommandopuffer (Azahar-Layout) und
   fuehrt SubmitCmdList-Eintraege (id 0x01) ueber gpu_run_command_list aus. */
/* Liest die vom Spiel in den GSP-Shared-Memory geschriebene
   FrameBufferInfo (Azahar gsp_gpu.h):
     base = GSP-Shmem + 0x200 + (2*thread + screen) * 0x40
     +0 byte index, +1 dirty, +8 FrameBufferInfo (0x20 je Eintrag)
     FrameBufferInfo: +0 active_fb, +4 address_left, +0xC stride, +0x10 format.
   Damit bekommt der Port die echten FB-Adressen beider Screens. */
static int gsp_fb_info(ArmCpu *c, int screen, uint32_t *addr, uint32_t *stride,
                       uint32_t *format) {
    if (c == NULL) return 0;
    for (int tid = 0; tid < 8; tid++) {
        uint32_t base = (0x10002000u + 0x200u +
                         ((uint32_t)(2 * tid + screen) * 0x40u)) & 0x1FFFFFFFu;
        if (base + 0x40u > c->mem_size) break;
        uint32_t idx = c->mem[base] & 1u;
        uint32_t info = base + 8u + idx * 0x20u;
        uint32_t active = 0, left = 0, right = 0, st = 0, fmt = 0;
        memcpy(&active, c->mem + info + 0u, 4);
        memcpy(&left, c->mem + info + 4u, 4);
        memcpy(&right, c->mem + info + 8u, 4);
        memcpy(&st, c->mem + info + 0xCu, 4);
        memcpy(&fmt, c->mem + info + 0x10u, 4);
        (void)active; (void)right;
        if (left != 0u && st != 0u) {
            if (addr) *addr = left;
            if (stride) *stride = st;
            if (format) *format = fmt;
            return 1;
        }
    }
    return 0;
}

static int gsp_engine_list(ArmCpu *c, uint32_t a)
{
    uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
    if ((uint64_t)a + 0x64u > c->mem_size) return 0;
    memcpy(&w0, c->mem + a, 4);
    memcpy(&w1, c->mem + a + 4u, 4);
    memcpy(&w2, c->mem + a + 8u, 4);
    memcpy(&w3, c->mem + a + 0x60u, 4);
    return (w0 == 0u && w1 == a + 4u && w2 == a + 4u &&
            w3 >= 0x00100000u && w3 < 0x20000000u);
}

/* Engine-Command-Buffer-Descriptor (Port): [0]=Zeiger in Ressourcenliste,
   [2]=Groesse, [5]=Anzahl Eintraege. Liefert die Engine-eigene Anzahl,
   wenn kein PICA-Paar-Format vorliegt. */
static int gsp_engine_desc(ArmCpu *c, uint32_t a)
{
    uint32_t w0 = 0, w2 = 0, w5 = 0;
    if ((uint64_t)a + 0x18u > c->mem_size) return 0;
    memcpy(&w0, c->mem + a, 4);
    memcpy(&w2, c->mem + a + 8u, 4);
    memcpy(&w5, c->mem + a + 0x14u, 4);
    if (w0 >= 0x00100000u && w0 < 0x20000000u && w2 > 0u &&
        w2 <= 0x10000u && w5 > 0u && w5 <= 0x1000u)
        return (int)w5;
    return 0;
}

/* ---------- Vollbild-Quad als echte PICA-Vertexdaten ----------
 * Wird vor den Engine-Strom gehaengt (HLE_PICA_VERTEX=1), damit der 0x22F-Draw
 * echte Attribute liest statt des HLE_PICA_QUAD-Fallbacks.
 * Registerlayout (Azahar regs_pipeline.h):
 *   0x200 base_address (bits1..28, Wert*16 = Gastadresse)
 *   0x201/0x202 Format (2 Bit) + size (2 Bit, Elemente-1) je Attribut,
 *           max_attribute_index in 0x202 bits28..31
 *   0x203+3*i Loader i: data_offset, comp0..11, byte_count, component_count
 */
#define PICA_VB_ADDR 0x0CFE8000u
#define PICA_ATTR_BYTES 0x48u

static void pica_pair_put(ArmCpu *c, uint32_t a, uint32_t n, uint32_t value,
                          uint32_t id, uint32_t mask)
{
    uint32_t hdr = (id & 0xFFFFu) | ((mask & 0xFu) << 16);
    memcpy(c->mem + ((a + n * 4u) & 0x1FFFFFFFu), &value, 4);
    memcpy(c->mem + ((a + (n + 1u) * 4u) & 0x1FFFFFFFu), &hdr, 4);
}

/* 6 Vertices = 2 Dreiecke; Attribut 0 = float2 Position, Attribut 1 =
   float2 UV, Stride 16 Byte. */
static void pica_quad_vertices(ArmCpu *c, int w, int h)
{
    float v[6][4] = {
        {0.0f, 0.0f, 0.0f, 0.0f},
        {(float)w, 0.0f, 1.0f, 0.0f},
        {(float)w, (float)h, 1.0f, 1.0f},
        {0.0f, 0.0f, 0.0f, 0.0f},
        {(float)w, (float)h, 1.0f, 1.0f},
        {0.0f, (float)h, 0.0f, 1.0f}
    };
    for (int i = 0; i < 6; i++)
        memcpy(c->mem + ((PICA_VB_ADDR + (uint32_t)i * 16u) & 0x1FFFFFFFu),
               v[i], 16);
}

static void pica_attr_block(ArmCpu *c, uint32_t dst)
{
    uint32_t n = 0;
    pica_pair_put(c, dst, n, (PICA_VB_ADDR >> 3) & 0x1FFFFFFFu, 0x200u, 0xFu);
    n += 2;
    pica_pair_put(c, dst, n, 0x77u, 0x201u, 0xFu);         /* attr0/1 float2 */
    n += 2;
    pica_pair_put(c, dst, n, 0x10000000u, 0x202u, 0xFu);   /* maxattr = 1 */
    n += 2;
    pica_pair_put(c, dst, n, 0u, 0x203u, 0xFu);            /* loader0 offset 0 */
    n += 2;
    pica_pair_put(c, dst, n, 0u, 0x204u, 0xFu);            /* comp0 = Attribut 0 */
    n += 2;
    pica_pair_put(c, dst, n, 0x10100000u, 0x205u, 0xFu);   /* stride 16, 1 Attr */
    n += 2;
    pica_pair_put(c, dst, n, 8u, 0x206u, 0xFu);            /* loader1 offset 8 */
    n += 2;
    pica_pair_put(c, dst, n, 1u, 0x207u, 0xFu);            /* comp0 = Attribut 1 */
    n += 2;
    pica_pair_put(c, dst, n, 0x10100000u, 0x208u, 0xFu);
}

static int gsp_process_cmdqueue(ArmCpu *c) {
    int qops = 0;
    if (c == NULL) return 0;
    for (int tid = 0; tid < 8; tid++) {
        uint32_t base = (0x10002000u + 0x800u +
                         (uint32_t)tid * 0x200u) & 0x1FFFFFFFu;
        if (base + 0x200u > c->mem_size) break;
        uint32_t hdr = 0;
        memcpy(&hdr, c->mem + base, 4);
        uint32_t guard = 0;
        while (((hdr >> 8) & 0xFFu) != 0u && guard++ < 0x20u) {
            uint32_t idx = hdr & 0xFFu;
            uint32_t cb = base + 0x20u + idx * 0x20u;
            if (cb + 0x20u > c->mem_size) break;
            uint32_t cmd = 0, addr = 0, size = 0;
            memcpy(&cmd, c->mem + cb, 4);
            memcpy(&addr, c->mem + cb + 4u, 4);
            memcpy(&size, c->mem + cb + 8u, 4);
            if ((cmd & 0xFFu) == 0x01u && addr != 0u && size != 0u &&
                size <= 0x00100000u) {
                int legacy = (addr == 0x08002000u || addr == 0x0CFE2000u ||
                              addr == 0x0CFE2100u || addr == 0x0CFE6000u);
                int nops = legacy ? gpu_run_command_list(c, addr, size)
                                  : gpu_run_pica_list(c, addr, size);
                if (nops == 0 && legacy)
                    nops = gpu_run_pica_list(c, addr, size);
                if (nops == 0) nops = gsp_engine_desc(c, addr);
                qops += nops;
                printf("[gspq] tid=%d idx=%u id=%u list=%08X size=%X ops=%d\n",
                       tid, idx, cmd & 0xFFu, addr, size, nops);
                fflush(stdout);
            }
            uint32_t ni = (idx + 1u) % 0xFu;
            uint32_t nn = ((hdr >> 8) & 0xFFu) - 1u;
            hdr = (ni & 0xFFu) | ((nn & 0xFFu) << 8) | (hdr & 0xFFFF0000u);
            memcpy(c->mem + base, &hdr, 4);
        }
        /* Engine-GX-Layout (aus fn_0013F2D4/fn_00139914): Anzahl bei +0x18,
           Listenadresse bei +0x1C, Groesse bei +0xB0. Die Engine schreibt
           genau diese Felder, wenn 0x12FD30 mit dem CommandBuffer als r0
           laeuft; HLE liest sie hier als SubmitCmdList. */
        {
            uint32_t ecnt = 0, eaddr = 0, esize = 0;
            memcpy(&ecnt, c->mem + base + 0x18u, 4);
            memcpy(&eaddr, c->mem + base + 0x1Cu, 4);
            memcpy(&esize, c->mem + base + 0xB0u, 4);
            if (ecnt != 0u && ecnt <= 0xFu && eaddr >= 0x00100000u &&
                eaddr < 0x20000000u && esize > 0u && esize <= 0x01000000u) {
                /* Engine-PICA-Strom in die Engine-Liste kopieren:
                   HLE_ENGINE_STREAM=<src>,<len> schreibt den vom Emitter
                   (0x1323FC) gebauten Strom vor der Ausfuehrung an die
                   Listenadresse. Damit stammen die Pixel aus 0x0CFE6000
                   und nicht aus einem Ring-Poke. */
                {
                    static const char *es = NULL;
                    static int es_read = 0;
                    static int pv = -1;
                    if (!es_read) { es = getenv("HLE_ENGINE_STREAM"); es_read = 1; }
                    if (pv < 0) pv = (getenv("HLE_PICA_VERTEX") != NULL);
                    if (es != NULL && *es != 0 && eaddr == 0x0CFE6000u) {
                        uint32_t ssrc = (uint32_t)strtoul(es, NULL, 16);
                        const char *cm = strchr(es, ',');
                        uint32_t slen = (cm != NULL)
                                            ? (uint32_t)strtoul(cm + 1, NULL, 16)
                                            : 0u;
                        if (ssrc != 0u && slen >= 8u && slen <= 0x1000u &&
                            (uint64_t)(ssrc & 0x1FFFFFFFu) + slen <= c->mem_size) {
                            if (pv) {
                                uint32_t d = eaddr + PICA_ATTR_BYTES;
                                uint32_t nv = 6u;
                                memcpy(c->mem + d, c->mem + (ssrc & 0x1FFFFFFFu),
                                       slen);
                                pica_attr_block(c, eaddr);
                                /* nv des Engine-Stroms auf die 6 Vertices des
                                   Quads setzen (erstes Wort = 0x228-Wert). */
                                memcpy(c->mem + (d & 0x1FFFFFFFu), &nv, 4);
                                esize = PICA_ATTR_BYTES + slen;
                                printf("[gspq-engine] vertex block @%08X -> "
                                       "%08X nv=6 stream @%08X len=%X\n",
                                       PICA_VB_ADDR, eaddr, d, slen);
                            } else {
                                memcpy(c->mem + eaddr,
                                       c->mem + (ssrc & 0x1FFFFFFFu), slen);
                                esize = slen;
                            }
                            printf("[gspq-engine] stream %08X -> %08X len=%X\n",
                                   ssrc, eaddr, esize);
                            fflush(stdout);
                        }
                    }
                }
                /* Ziel-Framebuffer fuer den PICA-Render:
                   HLE_PICA_TARGET=<addr>,<w>,<h>.
                   HLE_PICA_PRIMARY=<tex>,<w>,<h> bindet vor jedem Submit die
                   Haupttextur neu (der zweite Screen bindet eine andere). */
                {
                    static const char *pt = NULL, *pm = NULL;
                    static int pt_read = 0;
                    if (!pt_read) {
                        pt = getenv("HLE_PICA_TARGET");
                        pm = getenv("HLE_PICA_PRIMARY");
                        pt_read = 1;
                    }
                    if (pm != NULL && *pm != 0) {
                        char bp[80], *qp = bp;
                        uint32_t vp[3] = {0, 0, 0};
                        int np = 0;
                        snprintf(bp, sizeof(bp), "%s", pm);
                        while (qp != NULL && *qp && np < 3) {
                            char *cp2 = strchr(qp, ',');
                            if (cp2) *cp2 = 0;
                            vp[np] = (uint32_t)strtoul(qp, NULL,
                                                       np == 0 ? 16 : 10);
                            np++;
                            qp = cp2 ? cp2 + 1 : NULL;
                        }
                        if (vp[0] != 0u && vp[1] != 0u && vp[2] != 0u)
                            gpu_pica_bind_texture(vp[0], (int)vp[1], (int)vp[2], 1);
                    }
                    if (pt != NULL && *pt != 0) {
                        char b2[64], *pp2 = b2;
                        uint32_t v2[3] = {0, 0, 0};
                        int n2 = 0;
                        snprintf(b2, sizeof(b2), "%s", pt);
                        while (pp2 != NULL && *pp2 && n2 < 3) {
                            char *cc = strchr(pp2, ',');
                            if (cc) *cc = 0;
                            v2[n2] = (uint32_t)strtoul(pp2, NULL, n2 == 0 ? 16 : 10);
                            n2++;
                            pp2 = cc ? cc + 1 : NULL;
                        }
                        if (v2[0] != 0u) {
                            gpu_set_fb_size(v2[0], (int)v2[1], (int)v2[2]);
                            if (getenv("HLE_PICA_VERTEX") != NULL)
                                pica_quad_vertices(c, (int)v2[1], (int)v2[2]);
                        }
                    }
                }
                int legacy = (eaddr == 0x08002000u || eaddr == 0x0CFE2000u ||
                              eaddr == 0x0CFE2100u || eaddr == 0x0CFE6000u);
                int nops = legacy ? gpu_run_command_list(c, eaddr, esize)
                                  : gpu_run_pica_list(c, eaddr, esize);
                if (nops == 0 && legacy)
                    nops = gpu_run_pica_list(c, eaddr, esize);
                /* Zweiter Screen: dieselbe Engine-Liste mit der Bottom-Textur
                   und dem Bottom-Framebuffer ausfuehren.
                   HLE_PICA_SECOND=<tex>,<w>,<h>,<fb>. */
                {
                    static const char *sc = NULL;
                    static int sc_read = 0;
                    if (!sc_read) { sc = getenv("HLE_PICA_SECOND"); sc_read = 1; }
                    if (sc != NULL && *sc != 0 && eaddr == 0x0CFE6000u) {
                        char b3[96], *q = b3;
                        uint32_t v3[4] = {0, 0, 0, 0};
                        int n3 = 0;
                        snprintf(b3, sizeof(b3), "%s", sc);
                        while (q != NULL && *q && n3 < 4) {
                            char *cc3 = strchr(q, ',');
                            if (cc3) *cc3 = 0;
                            v3[n3] = (uint32_t)strtoul(q, NULL,
                                                       n3 == 0 ? 16 : (n3 == 3 ? 16 : 10));
                            n3++;
                            q = cc3 ? cc3 + 1 : NULL;
                        }
                        if (v3[0] != 0u && v3[1] != 0u && v3[2] != 0u && v3[3] != 0u) {
                            int n2nd;
                            gpu_pica_bind_texture(v3[0], (int)v3[1], (int)v3[2], 1);
                            gpu_set_fb_size(v3[3], (int)v3[1], (int)v3[2]);
                            if (getenv("HLE_PICA_VERTEX") != NULL)
                                pica_quad_vertices(c, (int)v3[1], (int)v3[2]);
                            n2nd = legacy ? gpu_run_command_list(c, eaddr, esize)
                                          : gpu_run_pica_list(c, eaddr, esize);
                            if (n2nd == 0 && legacy)
                                n2nd = gpu_run_pica_list(c, eaddr, esize);
                            nops += n2nd;
                            printf("[gspq-engine] second pass tex=%08X %ux%u fb=%08X ops=%d\n",
                                   v3[0], v3[1], v3[2], v3[3], n2nd);
                            fflush(stdout);
                        }
                    }
                }
                if (nops == 0 && gsp_engine_list(c, eaddr)) nops = 1;
                qops += nops;
                printf("[gspq-engine] tid=%d n=%u list=%08X size=%X ops=%d\n",
                       tid, ecnt, eaddr, esize, nops);
                fflush(stdout);
                ecnt = 0;
                memcpy(c->mem + base + 0x18u, &ecnt, 4);
            }
        }
    }
    return qops;
}

static void gsp_command(Ipc *r, uint32_t *p) {
    /* Kommandonummern wie auf echter Hardware / in Azahar (gsp_gpu.cpp),
     * NICHT die alte Citra-Nummerierung. */
    switch (r->cmd) {
        case 0x0001: case 0x0002: case 0x0003: { /* WriteHWRegs* */
            if (g_log) printf("[gsp] WriteHWRegs cmd=0x%04X addr=0x%X size=%u\n",
                              r->cmd, p[0], p[1]);
            ipc_begin(r, r->cmd, 1, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0004: { /* ReadHWRegs -> Result + Wert */
            ipc_begin(r, 0x0004, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0005: { /* SetBufferSwap(screen, framebuffer_info)
                           screen=2 bedeutet "beide" und wird als zwei echte
                           Swaps screen=0/1 mit den FB-Adressen aus dem
                           GSP-Shared-Memory behandelt. */
            uint32_t screen = p[0];
            uint32_t fb = (r->normal >= 2) ? p[1] : 0u;
            if (screen == 2u || screen == 3u) {
                for (uint32_t s = 0; s < 2u; s++) {
                    uint32_t a = 0, st = 0, fmt = 0;
                    if (!gsp_fb_info(g_cpu, (int)s, &a, &st, &fmt)) a = g_gsp_fb_addr[s];
                    g_gsp_fb_addr[s] = a;
                    gpu_set_fb_size(a, (s == 1u) ? 320 : 400, 240);
                    if (g_log) printf("[gsp] SetBufferSwap screen=%u fb=0x%08X "
                                      "stride=0x%X fmt=%X\n", s, a, st, fmt);
                }
                g_gsp_last_screen = 1;
            } else {
                if (screen < 3u && fb != 0u) g_gsp_fb_addr[screen] = fb;
                else if (screen < 2u) {
                    uint32_t a = 0, st = 0, fmt = 0;
                    if (gsp_fb_info(g_cpu, (int)screen, &a, &st, &fmt))
                        g_gsp_fb_addr[screen] = a;
                }
                gpu_set_fb_size(g_gsp_fb_addr[screen < 2u ? screen : 0],
                                (screen == 1u) ? 320 : 400, 240);
                g_gsp_last_screen = (int)(screen & 1);
                if (g_log) printf("[gsp] SetBufferSwap screen=%u fb=0x%08X\n",
                                  screen, g_gsp_fb_addr[screen < 3u ? screen : 0]);
            }
            ipc_begin(r, 0x0005, 1, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0006: { /* SubmitCmdList(addr, size usw.) */
            g_cmdlist_addr = p[0];
            g_cmdlist_size = p[1];
            /* Azahar (gsp_gpu.cpp, SubmitCmdList): der GSP legt den Eintrag
               in den Shared-Memory-Ring (shmem+0x800, hier Thread 0) und
               TriggerCmdReqQueue arbeitet ihn ab. Genau diesen Eintrag
               schreibt die Firmware - nicht der Gast-Code. */
            if (g_cpu != NULL) {
                uint32_t base = 0x10002800u;
                uint32_t hdr = 1u << 8;          /* index=0, number=1 */
                uint32_t cb = base + 0x20u;      /* Command[0] */
                uint8_t id = 0x01u;              /* 1 = SubmitCmdList */
                uint32_t addr = g_cmdlist_addr, sz = g_cmdlist_size;
                memcpy(g_cpu->mem + base, &hdr, 4);
                memcpy(g_cpu->mem + cb, &id, 1);
                memcpy(g_cpu->mem + cb + 4u, &addr, 4);
                memcpy(g_cpu->mem + cb + 8u, &sz, 4);
            }
            if (g_log) printf("[gsp] SetCommandList addr=0x%08X size=0x%X lr=%08X\n",
                              g_cmdlist_addr, g_cmdlist_size,
                              g_cpu ? g_cpu->r[14] : 0);
            ipc_begin(r, 0x0006, 1, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0007: { /* RequestDma(src, dst, size, flags) */
            uint32_t s0 = p[0], s1 = p[1], s2 = p[2], s3 = p[3];
            ipc_begin(r, r->cmd, (r->cmd == 0x0007) ? 2 : 1, 0);
            ipc_put(r, 0);
            if (r->cmd == 0x0007) ipc_put(r, 0);
            if (g_log && r->cmd == 0x0007) {
                printf("[gsp] RequestDma p0=%08X p1=%08X p2=%08X p3=%08X\n",
                       s0, s1, s2, s3);
            }
            /* Echte DMA ausfuehren, falls die Parameter plausibel sind. */
            if (r->cmd == 0x0007 && g_cpu != NULL && s2 != 0u &&
                s2 <= 0x00100000u && s0 + s2 <= g_cpu->mem_size &&
                s1 + s2 <= g_cpu->mem_size) {
                memmove(g_cpu->mem + s1, g_cpu->mem + s0, s2);
                if (g_log) printf("[gsp] DMA copy %08X -> %08X (%u)\n",
                                  s0, s1, s2);
            }
            break;
        }
        case 0x0008: case 0x0009: case 0x000A:
        case 0x000B: { /* Cache / Events / LcdForceBlack */
            ipc_begin(r, r->cmd, 1, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x000C: { /* TriggerCmdReqQueue -> Shared-Memory-Kommandopuffer */
            /* Azahar (gsp_gpu.cpp): base = shmem + 0x800 + thread_id*0x200,
               Header {index:8, number_commands:8, status:8, should_stop:8},
               Command[15] ab +0x20 (je 0x20 B). id 0x01 = SubmitCmdList
               (Adresse +0x04, Laenge +0x08). */
            int qops = gsp_process_cmdqueue(g_cpu);
            {
                static int ring_only = -1;
                if (ring_only < 0)
                    ring_only = (getenv("HLE_RING_ONLY") != NULL);
                if (ring_only && qops > 0 && g_log) {
                    printf("[gsp] Trigger: %d Ops aus Shared-Memory-Ring\n", qops);
                }
                if (ring_only && qops > 0)
                    goto trigger_done;
            }
            if (g_cmdlist_addr != 0 && g_cmdlist_size != 0 && g_cpu != NULL) {
                int legacy = (g_cmdlist_addr == 0x08002000u ||
                              g_cmdlist_addr == 0x00410000u ||
                              g_cmdlist_addr == 0x0CFE2000u ||
                              g_cmdlist_addr == 0x0CFE2100u ||
                              g_cmdlist_addr == 0x0CFE6000u);
                int nops = legacy
                               ? gpu_run_command_list(g_cpu, g_cmdlist_addr, g_cmdlist_size)
                               : gpu_run_pica_list(g_cpu, g_cmdlist_addr, g_cmdlist_size);
                if (nops == 0 && legacy)
                    nops = gpu_run_pica_list(g_cpu, g_cmdlist_addr, g_cmdlist_size);
                if (g_log) printf("[gsp] TriggerCmdReqQueue -> %d Ops%s\n", nops,
                                          legacy ? " (legacy)" : " (pica)");
            }
            if (g_log) printf("[gsp] Trigger lr=%08X addr=%08X size=%X qops=%d\n",
                              g_cpu ? g_cpu->r[14] : 0,
                              g_cmdlist_addr, g_cmdlist_size, qops);
        trigger_done:
            ipc_begin(r, 0x000C, 1, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x000D: case 0x000E: case 0x000F: case 0x0010:
        case 0x0011: case 0x0012: case 0x0014: case 0x0015:
        case 0x0016: case 0x0017:
        case 0x0019: case 0x001A: case 0x001B: case 0x001C:
        case 0x001D: case 0x001E: case 0x001F: { /* Transfer / Perf / Right / Vram */
            ipc_begin(r, r->cmd, 1, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0013: { /* RegisterInterruptRelayQueue */
            static int gsp_relay_first = 1;
            uint32_t ev = (uint32_t)alloc_handle(SVC_OTHER, "gsp-ev");
            uint32_t shm = (uint32_t)alloc_handle(SVC_OTHER, "gsp-shmem");
            Handle *hh = get_handle(ev);
            if (hh) hh->signaled = 1;
            ipc_begin(r, 0x0013, 2, 2);
            /* Azahar: beim ERSTEN Aufruf muss FirstInitialization (nicht 0)
               zurueckkommen, sonst bricht die GSP-Initialisierung ab.
               Result = (GX << 10) | 519 = 0x00000E07 */
            ipc_put(r, gsp_relay_first ? 0x00000E07u : 0u);
            gsp_relay_first = 0;
            ipc_put(r, 0x100u);
            ipc_put_handle_copy(r, shm);
            if (g_log) printf("[gsp] RegisterInterruptRelayQueue -> ev=%u shm=%u\n", ev, shm);
            break;
        }
        case 0x0018: { /* ImportDisplayCaptureInfo */
            ipc_begin(r, 0x0018, 6, 0);
            ipc_put(r, 0);
            ipc_put(r, 0x1F000000u); ipc_put(r, 0);
            ipc_put(r, 0x1F000000u); ipc_put(r, 0);
            ipc_put(r, 0); ipc_put(r, 0);
            break;
        }
        default:
            generic_command(r, SVC_GSP_GPU);
            break;
    }
}

/* Y2R: die meisten Kommandos sind Setter (1 Result). Wichtig ist
 * 0x000F GetTransferEndEvent -> Event-Handle, sonst wartet das Spiel auf
 * Handle 0 und kommt nie zur Grafik-Initialisierung. */
static void y2r_command(Ipc *r, uint32_t *p) {
    switch (r->cmd) {
        case 0x0002: case 0x0004: case 0x0006: case 0x0008:
        case 0x000A: case 0x000C: case 0x000E: { /* Get* -> Result + Wert */
            ipc_begin(r, r->cmd, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x000D: { /* SetTransferEndInterrupt -> (Result, enabled) */
            ipc_begin(r, 0x000D, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x000F: { /* GetTransferEndEvent -> (Result) + CopyObject(Event) */
            uint32_t ev = (uint32_t)alloc_handle(SVC_OTHER, "y2r-ev");
            Handle *hh = get_handle(ev);
            if (hh) hh->signaled = 1;
            ipc_begin(r, 0x000F, 1, 2);
            ipc_put(r, 0);
            ipc_put_handle_copy(r, ev);
            if (g_log) printf("[y2r] GetTransferEndEvent -> %u\n", ev);
            break;
        }
        case 0x0014: case 0x0019: { /* IsFinished* -> true */
            ipc_begin(r, r->cmd, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 1);
            break;
        }
        default:
            ipc_begin(r, r->cmd, 1, 0);
            ipc_put(r, 0);
            if (g_log) printf("[y2r] cmd 0x%04X -> 0\n", r->cmd);
            break;
    }
}

static void generic_command(Ipc *r, int service) {
    if (g_log) {
        printf("[%s] cmd 0x%04X normal=%d -> 0\n",
               SERVICE_NAMES[service], r->cmd, r->normal);
    }
    /* Antwort: Header + so viele Nullwoerter wie Parameter erwartet werden */
    int n = r->normal > 0 ? r->normal : 1;
    if (n > 16) n = 16;
    ipc_begin(r, r->cmd, n, 0);
    for (int i = 0; i < n; i++) ipc_put(r, 0);
}

static void dispatch_ipc(uint32_t handle) {
    Handle *h = get_handle(handle);
    ArmCpu *c = g_cpu;
    uint32_t *buf = (uint32_t *)(c->mem + c->tpidruro + TLS_CMD_BUF);
    uint32_t header = buf[0];
    Ipc r;
    r.buf = buf;

    uint32_t cmd = (header >> 16) & 0xFFFFu;
    int normal = (int)((header >> 6) & 0x3Fu);
    int translate = (int)(header & 0x3Fu);
    uint32_t *params = buf + 1;

    r.cmd = cmd;
    r.normal = normal;
    r.translate = translate;
    r.written = 0;

    int service = h ? h->service : SVC_OTHER;
    if (service < 0 || service > 33) service = SVC_OTHER;
    /* Fallback: einige SDK-Wrapper senden ueber die srv-Session, obwohl das
       Kommando zu APT/cfg gehoert (im Trace beobachtet). */
    if (service == SVC_SRV) {
        if (cmd == 0x0050 || cmd == 0x004F || cmd == 0x000D || cmd == 0x000E)
            service = SVC_APT_U;
        else if ((cmd == 0x0001 || cmd == 0x0003) && normal == 2)
            service = SVC_CFG_U;
    }
    /* Fallback: Datei-/FS-Kommandos auch dann richtig routen, wenn das
       Handle im HLE einer anderen Kategorie zugeordnet ist (im Trace
       sendet das Spiel 0x0802/0x0803/0x0808 auf Handle 4 = "sync"). */
    /* 0x08xx IMMER nach Kommandoform routen: das Spiel schickt Datei-
       Kommandos auch auf Handles, die wir anders vergeben haben. */
    if ((cmd & 0xFF00u) == 0x0800u) {
        if (cmd == 0x080Cu || cmd == 0x080Du || cmd == 0x080Eu || cmd == 0x0812u)
            service = SVC_FS_USER;
        /* 0x0803 ist mehrdeutig: als fs:USER-Kommando ist es
           OpenFileDirectly (8 Parameter + 2 Puffer), als Datei-
           Kommando GetSize (4 Parameter). */
        else if (normal >= 6)
            service = SVC_FS_USER;
        else if (cmd == 0x0801u && normal >= 5)
            service = SVC_FS_USER;
        else
            service = SVC_FILE;
    }
    g_cur_handle = handle;
    g_cur_mem = c->mem;
    if (getenv("HLE_MUTEX_HACK") && !late_mutex_hack) {
        late_mutex_hack = 1;
        uint32_t v = 1;
        memcpy(c->mem + 0x40EE10u, &v, 4);
        printf("[hle] late Mutex-Hack: [0x40EE10] = 1\n");
    }
    ev(2, (uint32_t)service, cmd, normal);
    g_ipc_count++;
    if (cmd == 0x0001 && normal >= 32 && getenv("HLE_ERRCTX")) {
        static int ec = 0;
        if (ec < 10) {
            ec++;
            printf("[errctx] fatal code=%08X site=%08X th=%u blk=%08X lr=%08X\n",
                   params[1], params[2],
                   (uint32_t)((c->tpidruro >> 12) & 0xFu), arm_last_block, c->r[14]);
            arm_trace_thread_dump((int)((c->tpidruro >> 12) & 0xFu), 24);
            {
                uint32_t q;
                printf("[errctx] object 3D66C0:");
                for (q = 0; q < 8; q++) {
                    uint32_t w = 0;
                    memcpy(&w, c->mem + (0x3D66C0u & 0x1FFFFFFFu) + q * 4, 4);
                    printf(" %08X", w);
                }
                printf("\n");
            }
            fflush(stdout);
        }
    }
    if (cmd < 64 && service >= 0 && service < 64) g_cmd_seen[service][cmd]++;
    if (h && (g_ipc_count <= g_ipc_log_limit || g_log)) {
        printf("[ipc] th=%03X blk=%08X %-9s cmd=0x%04X n=%d t=%d h=%u", (uint32_t)((g_cpu ? g_cpu->tpidruro : 0) >> 12) & 0xFFFu, arm_last_block, SERVICE_NAMES[service], cmd,
               normal, translate, handle);
        for (int i = 0; i < normal && i < 8; i++) printf(" p%d=%08X", i, params[i]);
        printf("\n");
    }

    switch (service) {
        case SVC_SRV:
            srv_command(&r, params);
            break;
        case SVC_APT_U:
            apt_command(&r, params);
            break;
        case SVC_HID_USER:
            hid_command(&r, params);
            break;
        case SVC_FS_USER: case SVC_FS_LDR:
            fs_command(&r, params);
            break;
        case SVC_FILE:
            file_command(&r, params);
            break;
        case SVC_CFG_U:
            cfg_command(&r, params);
            break;
        case SVC_DSP:
            dsp_command(&r, params);
            break;
        case SVC_PTM_U:
            ptm_command(&r, params);
            break;
        case SVC_NDM:
            ndm_command(&r, params);
            break;
        case SVC_BOSS:
            boss_command(&r, params);
            break;
        case SVC_GSP_GPU:
            gsp_command(&r, params);
            break;
        case SVC_Y2R:
            y2r_command(&r, params);
            break;
        default:
            generic_command(&r, service);
            break;
    }
    ipc_finish(&r);
    /* Rest des Puffers nullen */
    for (int i = r.written; i < 0x40; i++) buf[i] = 0;
}

/* ---------- SVC ---------- */

static void write_u32(ArmCpu *c, uint32_t addr, uint32_t value) {
    if ((addr & 0x1FFFFFFFu) + 4 <= c->mem_size) {
        memcpy(c->mem + (addr & 0x1FFFFFFFu), &value, 4);
    }
}

static void write_u64(ArmCpu *c, uint32_t addr, uint64_t value) {
    write_u32(c, addr, (uint32_t)value);
    write_u32(c, addr + 4, (uint32_t)(value >> 32));
}

static void w32(ArmCpu *c, uint32_t addr, uint32_t v) {
    memcpy(c->mem + addr, &v, 4);
}

static void setup_config_mem(ArmCpu *c) {
    uint8_t *p = c->mem + 0x1FF80000u;
    memset(p, 0, 0x1000);
    p[0x01] = 0x3A;                 /* kernel version rev  */
    p[0x02] = 0x02;                 /* kernel version min  */
    p[0x03] = 0x02;                 /* kernel version maj  */
    w32(c, 0x1FF80008u, 0x00008002u);  /* ns_tid low  */
    w32(c, 0x1FF8000Cu, 0x00040130u);  /* ns_tid high */
    w32(c, 0x1FF80010u, 2u);           /* sys_core_ver */
    p[0x14] = 1;                 /* unit_info (retail) */
    p[0x16] = 1;                    /* prev_firm */
    w32(c, 0x1FF80018u, 0x0000F450u);  /* ctr_sdk_ver */
    w32(c, 0x1FF80030u, 0u);           /* app_mem_type */
    w32(c, 0x1FF80040u, 0x08000000u);  /* app_mem_alloc = 128 MiB */
    w32(c, 0x1FF80044u, 0x04000000u);  /* app/sys_mem_alloc: 64 MiB (Port) */
    w32(c, 0x1FF80048u, 0x00800000u);  /* base_mem_alloc */
    p[0x61] = 0x3A;
    p[0x62] = 0x02;
    p[0x63] = 0x02;
    w32(c, 0x1FF80064u, 2u);
    w32(c, 0x1FF80068u, 0x0000F450u);
    printf("[hle] config-memory initialisiert (app_mem_alloc=128MiB)\n");
}

static volatile int g_vblank_run = 0;
/* Warteschlange fuer svcArbitrateAddress (Adresse -> Anzahl Weckungen).
 * Die echte Semantik: type 0 = Signal weckt Waiter, ohne zu blockieren. */
static uint32_t g_sig_addr[64];
static int g_sig_cnt[64];
static int g_sig_n = 0;

static void arb_signal(uint32_t a, int32_t n) {
    int i;
    for (i = 0; i < g_sig_n; i++) if (g_sig_addr[i] == a) break;
    if (i == g_sig_n) { if (g_sig_n >= 64) return; g_sig_addr[g_sig_n] = a; g_sig_cnt[g_sig_n] = 0; g_sig_n++; }
    if (n < 0) g_sig_cnt[i] = 0x3FFFFFFF;
    else g_sig_cnt[i] += n;
}

static int arb_take(uint32_t a) {
    for (int i = 0; i < g_sig_n; i++) {
        if (g_sig_addr[i] == a && g_sig_cnt[i] > 0) { g_sig_cnt[i]--; return 1; }
    }
    return 0;
}

static void arb_unlock_short(void) {
    if (g_guest_lock_init && !g_no_lock) {
        if (t_lock_owned) {
            guest_lock_leave();
            Sleep(0);
            guest_lock_reacquire();
        } else {
            Sleep(0);
        }
    } else {
        Sleep(0);
    }
}

/* Handles, auf die tatsaechlich gewartet wird (fuer deterministisches
 * VBlank-Signalisieren statt "alle Handles fluten"). */
static unsigned char g_wait_seen[MAX_HANDLES];
static volatile uint32_t g_arb_seen[64];
static volatile int g_arb_seen_n = 0;
static unsigned g_vblank_frames = 0;

/* Simulierter VBlank: signalisiert periodisch alle Event-Handles. */
static DWORD WINAPI vblank_thread(LPVOID p) {
    (void)p;
    while (g_vblank_run) {
        Sleep(16);
        for (int k = 1; k < MAX_HANDLES; k++) {
            /* Nur Handles wecken, auf die wirklich gewartet wird. */
            if (g_handles[k].used &&
                (getenv("HLE_VB_ONLY") == NULL || g_wait_seen[k]) &&
                !((getenv("HLE_APT_ONLY") == NULL || getenv("HLE_APT_ONLY")[0] != '0') &&
                  (strcmp(g_handles[k].name, "apt-param") == 0 ||
                   strcmp(g_handles[k].name, "apt-notif") == 0)))
                g_handles[k].signaled = 1;
        }
        /* GSP-Interrupt-Relay-Queue: das Spiel mappt sie an 0x10002000.
           Ohne VBlank-Eintrag dort wartet die Render-Schleife ewig. */
        if (g_cpu != NULL && getenv("HLE_GSP_IRQ")) {
            /* Interrupt-Relay-Queue liegt bei SharedMem+0x1000 (Citra/Azahar:
               GetInterruptRelayQueue = 0x1000 + thread_id*0x40).
               Layout: index(u8), count(u8), error(u8), config(u8),
               missed_PDC0(u32), missed_PDC1(u32), slots[0x34] (InterruptId). */
            uint32_t base = (0x10002000u + 0x1000u) & 0x1FFFFFFFu;
            if (base + 0x10 <= g_cpu->mem_size) {
                g_cpu->mem[base + 0x00] = 0;   /* index */
                g_cpu->mem[base + 0x01] = 1;   /* number_interrupts */
                g_cpu->mem[base + 0x02] = 0;   /* error_code */
                g_cpu->mem[base + 0x03] = 0;   /* config */
                g_cpu->mem[base + 0x0C] = 0x02; /* slot[0] = PDC0 (VBlank) */
            }
        }
        /* HID-Shared-Memory: das Spiel mappt den Block an 0x10000000
           (svc 0x1F Handle 0x17). PadState.current_state liegt bei +0x1C:
           START (Bit 3) / A (Bit 0) periodisch druecken. */
        if (g_cpu != NULL && g_vblank_frames >= 0) {
            uint32_t addr = (0x10000000u + 0x1Cu) & 0x1FFFFFFFu;
            if (addr + 4 <= g_cpu->mem_size) {
                uint32_t btn = (((g_vblank_frames / 30) & 1) != 0) ? 0x9u : 0x0u;
                memcpy(g_cpu->mem + addr, &btn, 4);
            }
            g_vblank_frames++;
            /* Queue-Sperrflag (0x42B938, Byte +0x18C) offen halten, damit
               die App ihre Ringpuffer-Pushes ausfuehrt. */
            if (getenv("HLE_QPUSH")) g_cpu->mem[0x42B938u] = 0;
        }
        /* Alle tatsaechlich benutzten Arbitrate-Objekte posten. */
        if (g_cpu != NULL) {
            for (int k = 0; k < g_arb_seen_n; k++) {
                uint32_t off = g_arb_seen[k] & 0x1FFFFFFFu;
                if (off + 4 <= g_cpu->mem_size) {
                    int32_t *w = (int32_t *)(g_cpu->mem + off);
                    if (*w <= 0) *w = 1;
                }
            }
        }
        /* Bekannte Arbitrate-Objekte ebenfalls "posten". */
        if (g_cpu != NULL) {
            static const uint32_t arbs[] = { 0x090001D4u, 0x0042BD54u, 0x0042B864u,
                                          0x0040C628u, 0x0040C5B4u, 0x0040C5A8u,
                                          0x141EA4u, 0x141EA8u };
            for (unsigned a = 0; a < sizeof(arbs) / sizeof(arbs[0]); a++) {
                uint32_t off = arbs[a] & 0x1FFFFFFFu;
                if (off + 4 <= g_cpu->mem_size) {
                    int32_t *w = (int32_t *)(g_cpu->mem + off);
                    if (*w <= 0) *w = 1;
                }
            }
        }
    }
    return 0;
}

void hle_init(ArmCpu *c, const char *romfs_dir) {
    ensure_savedata();
    g_cpu = c;
    snprintf(g_romfs, sizeof(g_romfs), "%s", romfs_dir ? romfs_dir : "out/romfs");
    g_log = getenv("HLE_LOG") != NULL;
    if (getenv("HLE_SRV_LAYOUT")) g_srv_layout = atoi(getenv("HLE_SRV_LAYOUT"));
    const char *lim = getenv("HLE_IPC_LOG");
    if (lim) g_ipc_log_limit = atoi(lim);
    setup_config_mem(c);
    /* Boot-Speichergroessen fuer den Engine-Heap: 0x10FE9C/0x10FEAC lesen
       [0x3EC00C]/[0x3EC008] und fuettern damit 0x289B9C(heap=0x408448).
       Ohne sie bleibt der Heap leer und 0x27B49C liefert 0. */
    {
        uint32_t m0 = 0x08000000u;   /* Heap-Basis (ControlMemory op=3) */
        uint32_t m1 = 0x05700000u;   /* Groesse des App-Blocks */
        memcpy(c->mem + 0x003EC008u, &m0, 4);
        memcpy(c->mem + 0x003EC00Cu, &m1, 4);
    }
    arm_pump_hook = hle_pump;
    arm_tick_hook = hle_tick;
    g_frame_cpu = c;

    /* Der VBlank-Thread war nie gestartet (g_vblank_run blieb 0) ->
       keine Handle-Signalisierung, kein HID-Update, kein Relay-Flag. */
    if (getenv("HLE_NO_VBLANK") == NULL) {
        g_vblank_run = 1;
        CreateThread(NULL, 0, vblank_thread, NULL, 0, NULL);
        printf("[hle] VBlank-Thread gestartet\n");
    }
    if (getenv("HLE_QUEUE_INIT")) {
        uint32_t lim = 0x8000u;
        memcpy(c->mem + 0x42B934u, &lim, 4);
        printf("[hle] Queue-Limit 0x42B934 = 0x8000\n");
    }
    if (getenv("HLE_GSP_STATE")) {
        uint32_t st = 6;
        memcpy(c->mem + 0x3D9200u, &st, 4);
        printf("[hle] GSP-State 0x3D9200 = 6 gesetzt\n");
    }
    if (getenv("HLE_FS_PRESET") != NULL &&
        (getenv("HLE_FS_HANDLE") == NULL || getenv("HLE_FS_HANDLE")[0] != '0')) {
        /* Client-Handle-Slot 0x3D91D0 (Setter fn_00107AD4 wird nie gerufen):
           gueltigen fs:USER-Handle eintragen, sonst laufen alle "rom:"-
           Zugriffe mit Handle 0 und werfen FatalError. */
        /* Das Spiel sendet ueber [0x3D91D0] Datei-Kommandos
           (0x0801 Read / 0x0802 Write / 0x0803 GetSize / 0x0808
           SetPriority) -> es braucht einen DATEI-Client, nicht fs:USER. */
        uint32_t fsh = (uint32_t)alloc_handle(SVC_FILE, "file");
        memcpy(c->mem + 0x3D91D0u, &fsh, 4);
        /* Der Session-Werfer 0x212CA8 prueft [obj+4] (Objekt 0x3D66C0).
           Vorbelegung optional: sie erhoeht die Fatal-Zahl und bremst
           den GSP-Init -> nur mit HLE_FS_SLOT2 aktiv. */
        if (getenv("HLE_FS_SLOT2") == NULL || getenv("HLE_FS_SLOT2")[0] != '0')
            memcpy(c->mem + 0x3D66C4u, &fsh, 4);
        /* Der generische Session-Getter 0x212CA8 wird mit Objekt
           0x3D67C0 aufgerufen (per ARM_WATCH gemessen) und prueft +4. */
        memcpy(c->mem + 0x3D67C4u, &fsh, 4);
        printf("[hle] fs-Client-Handle 0x3D91D0/0x3D66C4 = %u\n", fsh);
    }
    /* Asset-Registry-Guard: [0x3EC028+0xA34] = 0x3ECA5C. Ist er 0,
       verwirft 0x2919DC jede .fa-Registrierung (Sprung nach 0x291A7C).
       Der Setter 0x285038 laeuft bei uns nicht -> Guard vorbelegen. */
    /* Diagnose: Frame-Callback-Slot des Pacing-Loops (0x3D7C74) vorbelegen. */
    /* GPU: vom Spiel gebaute Kommandodaten ausfuehren (Adresse via Env). */
    if (getenv("HLE_GPU_TEST_ADDR")) {
        uint32_t ca = (uint32_t)strtoul(getenv("HLE_GPU_TEST_ADDR"), NULL, 16);
        uint32_t cs = 0x2000u;
        if (getenv("HLE_GPU_TEST_SIZE"))
            cs = (uint32_t)strtoul(getenv("HLE_GPU_TEST_SIZE"), NULL, 16);
        {
            int nops2 = gpu_run_command_list(c, ca, cs);
            g_cmdlist_addr = ca;
            g_cmdlist_size = cs;   /* fuer spaetere TriggerCmdReqQueue */
            printf("[gputest2] addr=%08X size=%X ops=%d\n", ca, cs, nops2);
        }
    }
    /* GPU-Selbsttest: synthetische Kommandoliste ausfuehren. */
    if (getenv("HLE_GPU_TEST")) {
        uint32_t cl = 0x00410000u;
        uint32_t glist[7];
        glist[0] = 0x0400FF00u;      /* ClearColor gruen */
        glist[1] = 0x050FFFFFu;      /* FillRect, Anzahl folgt */
        glist[2] = 4u;
        glist[3] = 0u; glist[4] = 0u; glist[5] = 400u; glist[6] = 240u;
        memcpy(c->mem + cl, glist, sizeof(glist));
        {
            int nops = gpu_run_command_list(c, cl, (uint32_t)sizeof(glist));
            printf("[gputest] ops=%d fb=%08X px=%02X%02X%02X\n", nops,
                   gpu_get_fb(), c->mem[0x1F000000u + 2],
                   c->mem[0x1F000000u + 1], c->mem[0x1F000000u + 0]);
        }
    }
    if (getenv("HLE_FRAME_CB")) {
        uint32_t cb = (uint32_t)strtoul(getenv("HLE_FRAME_CB"), NULL, 16);
        memcpy(c->mem + 0x3D7C74u, &cb, 4);
        printf("[hle] Frame-Callback 0x3D7C74 = %08X\n", cb);
    }
    /* Diagnose: RM-Frame-Callback [0x40B370+0x15C] = [0x40B4CC] setzen.
       Wird von fn_00134818 (Tick-Slot 2) mit r0=0x400 aufgerufen. */
    if (getenv("HLE_FRAME_CB2")) {
        uint32_t cb = (uint32_t)strtoul(getenv("HLE_FRAME_CB2"), NULL, 16);
        memcpy(c->mem + 0x40B4CCu, &cb, 4);
        printf("[hle] RM-Frame-Callback 0x40B4CC = %08X\n", cb);
    }
    if (getenv("HLE_REG_ENABLE")) {
        /* Deskriptor {basis, stride, count} + Eintragsarray synthetisch anlegen. */
        uint32_t desc[3];
        uint32_t g = 0x00401000u;
        desc[0] = 0x00400000u;
        desc[1] = 0x000000C0u;
        desc[2] = 8u;
        memcpy(c->mem + g, desc, sizeof(desc));
        memcpy(c->mem + 0x3ECA5Cu, &g, 4);
        printf("[hle] Asset-Registry-Deskriptor 0x00401000 {0x00400000,0xC0,8}, Guard=0x401000\n");
    }
    /* Diagnose: FS-Fehlerhelfer 0x28EDCC liefert 0xE0A046DB, solange
       [0x3D8D8C] == 0. Objektzeiger vorbelegen, damit echte Resultate
       zurueckkommen. */
    if (getenv("HLE_FS_OBJ") == NULL || getenv("HLE_FS_OBJ")[0] != '0') {
        uint32_t obj = 0x00408000u;
        memset(c->mem + obj, 0, 0x40);
        memcpy(c->mem + 0x3D8D8Cu, &obj, 4);
        printf("[hle] FS-Objektzeiger 0x3D8D8C = %08X\n", obj);
    }
    if (getenv("HLE_GSP_CLIENT")) {
        /* GSP-Client-Objekt 0x3D91E8 vorinitialisieren: Statusfeld +0x18 = 6 und
         * ein echter gsp::Gpu-Handle in +0x10, damit die Spiel-Renderlogik den
         * Client ohne eigenen Open-Schritt benutzen kann. */
        uint32_t st = 6;
        uint32_t gh = (uint32_t)alloc_handle(SVC_GSP_GPU, "gsp::Gpu");
        memcpy(c->mem + 0x3D9200u, &st, 4);
        memcpy(c->mem + 0x3D91F8u, &gh, 4);
        printf("[hle] GSP-Client vorinitialisiert: state=6 handle=%u\n", gh);
        /* Denselben Log-Pfad wie ein echter srv:GetServiceHandle bedienen. */
        if (getenv("HLE_SRV_NAMES"))
            printf("[srvname] \"gsp::Gpu\" srvh=%u blk=%08X lr=%08X desc=%08X -> %u (gsp::Gpu)\n",
                   0u, 0u, 0u, 0x003D91E8u, gh);
    }
    /* TODO: folgt aus der Analyse des Mutex-Spinlocks (Wert 1 = frei). */
    if (getenv("HLE_MUTEX_HACK")) {
        uint32_t v = 1;
        memcpy(c->mem + 0x40EE10u, &v, 4);
        printf("[hle] Mutex-Hack: [0x40EE10] = 1\n");
    }
    memset(g_handles, 0, sizeof(g_handles));
    g_next_handle = 1;
    g_srv_handle = (uint32_t)alloc_handle(SVC_SRV, "srv:");
    if (getenv("ARM_WATCH")) {
        arm_watch_pc = (uint32_t)strtoul(getenv("ARM_WATCH"), NULL, 16);
    }
    if (getenv("HLE_TRACE")) {
        g_trace_file = fopen("out/hle_trace.txt", "w");
    }
    if (!g_guest_lock_init) {
        InitializeCriticalSection(&g_guest_lock);
        g_guest_lock_init = 1;
        /* Default: Guest-Lock AN (deterministisch, Speicher konsistent).
           HLE_NO_THREAD_LOCK=1 schaltet ihn ab. HLE_DET_SCHED=1 erzwingt
           festes Round-Robin (fuer reproduzierbare A/B-Matrizen). */
        g_no_lock = getenv("HLE_NO_THREAD_LOCK") != NULL;
        g_det_sched = getenv("HLE_DET_SCHED") != NULL;
        if (g_det_sched) g_turn = 1;   /* erster Gast-Thread ist id 1 */
        if (!g_no_lock) guest_lock_enter();
    }
    g_main_cpu = c;
    /* gsp::Gpu muss im Port immer als Dienst existieren. Der Gast oeffnet
       ihn im Normalfall beim Boot; im unvollstaendigen Bootpfad kommt der
       srv-Request nie an, dann sind SetCommandList/TriggerCmdReqQueue/
       SetBufferSwap fuer die HLE unerreichbar (g_hle_gsp_handle == 0).
       WICHTIG: erst nach dem Handle-Reset oben allozieren. */
    if (g_hle_gsp_handle == 0)
        g_hle_gsp_handle = (uint32_t)alloc_handle(SVC_GSP_GPU, "gsp::Gpu");
    g_hle_gsp_export = g_hle_gsp_handle;
    printf("[hle] romfs=%s log=%d srv-handle=%u gsp-handle=%u\n",
           g_romfs, g_log, g_srv_handle, g_hle_gsp_handle);
}

void hle_tick_stats(int *calls, unsigned long long *tick) {
    if (calls) *calls = g_tick_calls;
    if (tick) *tick = g_tick;
}

void hle_report(void) {
    printf("[hle] ipc=%d ticks=%llu tickcalls=%d\n", g_ipc_count, (unsigned long long)g_tick, g_tick_calls);
    arm_hook_report();
    arm_retforce_report();
    arm_perfmap_report();
    printf("[hle] letzte Ereignisse:\n");
    for (int i = 0; i < HLE_EV_RING; i++) {
        int idx = (g_ev_pos + i) % HLE_EV_RING;
        if (g_ev_ring[idx].kind == 0) continue;
        if (g_ev_ring[idx].kind == 1) {
            printf("   svc 0x%02X  r0=0x%08X r1=0x%08X\n",
                   g_ev_ring[idx].a, g_ev_ring[idx].b, g_ev_ring[idx].c);
        } else {
            printf("   ipc %-9s cmd=0x%04X n=%d\n",
                   SERVICE_NAMES[g_ev_ring[idx].a], g_ev_ring[idx].b, (int)g_ev_ring[idx].c);
        }
    }
}

/* Thread-Ende signalisieren: der zugehoerige Thread-Handle wird auf
 * "signaled" gesetzt, damit svcWaitSynchronization darauf zurueckkehrt. */
static void thread_signal_exit(ArmCpu *c) {
    if (getenv("HLE_THR_EXIT") == NULL) return;   /* A/B-Schalter */
    for (int i = 0; i < g_thread_count; i++) {
        if (g_threads[i].cpu == c) {
            Handle *hh = get_handle(g_threads[i].handle);
            if (hh != NULL) hh->signaled = 1;
            if (getenv("HLE_THR_LOG"))
                printf("[threxit] handle=%u\n", g_threads[i].handle);
            return;
        }
    }
}

uint32_t hle_svc(ArmCpu *c, uint32_t imm) {
    if (getenv("HLE_SVC_SEEN")) {
        static unsigned char seen[256];
        if (imm < 256 && !seen[imm]) {
            seen[imm] = 1;
            printf("[svcseen] 0x%02X\n", imm);
        }
    }
    if (imm == 0x32 && getenv("HLE_SVC32_LOG") != NULL) {
        static int n32 = 0;
        if (n32++ < 30)
            printf("[svc32] #%d r0=%08X r1=%08X lr=%08X blk=%08X\n",
                   n32, c->r[0], c->r[1], c->r[14], arm_last_block);
        fflush(stdout);
    }
    g_cpu = c;
    ev(1, imm, c->r[0], c->r[1]);
    svc_trace(imm, c);
    if (getenv("HLE_MUTEX_HACK") && !late_mutex_hack) {
        late_mutex_hack = 1;
        uint32_t v = 1;
        memcpy(c->mem + 0x40EE10u, &v, 4);
    }
    if (imm == 0x3C) {
        printf("[svc] Break bei Block 0x%08X: r0=%08X r1=%08X r2=%08X r3=%08X lr=%08X\n",
               arm_last_block, c->r[0], c->r[1], c->r[2], c->r[3], c->r[14]);
        if (g_log) {
            uint32_t base = 0x003D8CC0u;
            printf("      globals @0x%08X:", base);
            for (int i = 0; i < 16; i++) {
                printf(" %08X", (uint32_t)arm_mem_read(c, base + i * 4, 4));
            }
            printf("\n");
        }
    }
    switch (imm) {
        case 0x01: { /* ControlMemory(out, addr0, addr1, size, op, perm) */
            /* SDK-Wrapper (0x28ED30): r0=Operation, r4=Permissions,
               r1=addr0, r2=addr1, r3=size; Out-Pointer liegt auf dem Stack. */
            uint32_t out = (uint32_t)arm_mem_read(c, c->r[13], 4);
            uint32_t addr0 = c->r[1], size = c->r[3];
            uint32_t operation = c->r[0];
            uint32_t perms = c->r[4];
            (void)perms;
            uint32_t op = operation & 0xFFu;
            int linear = (operation & 0x10000u) != 0;
            uint32_t addr = addr0;

            if (op == 1) {                     /* MEMOP_FREE */
                addr = addr0;
            } else if (op == 3) {              /* MEMOP_COMMIT */
                if (addr == 0) {
                    if (linear) {
                        addr = g_lin_cursor;
                        g_lin_cursor += (size + 0xFFFu) & ~0xFFFu;
                    } else {
                        addr = g_heap_cursor;
                        g_heap_cursor += (size + 0xFFFu) & ~0xFFFu;
                    }
                }
            } else {                            /* RESERVE/MAP/UNMAP/PROTECT */
                addr = addr0;
            }
            if (out) write_u32(c, out, addr);
            c->r[1] = addr;          /* SDK-Wrapper speichert r1 nach *out */
            if (g_log) {
                printf("[svc] ControlMemory op=%u addr0=0x%08X size=0x%X linear=%d -> 0x%08X",
                       op, addr0, size, linear, addr);
                printf(" | r0=%08X r4=%08X stack:", c->r[0], c->r[4]);
                for (int i = 0; i < 6; i++) {
                    printf(" %08X", (uint32_t)arm_mem_read(c, c->r[13] + i * 4, 4));
                }
                printf("\n");
            }
            return 0;
        }
        case 0x02: { /* QueryMemory(info, page_info, addr) */
            uint32_t info = c->r[0], page = c->r[1], addr = c->r[2];
            uint32_t base = 0x08000000u, size = 0x08000000u;
            if (addr >= 0x14000000u) { base = 0x14000000u; size = 0x08000000u; }
            if (addr >= 0x1F000000u) { base = 0x1F000000u; size = 0x00600000u; }
            if (info) {
                write_u32(c, info + 0, base);
                write_u32(c, info + 4, size);
                write_u32(c, info + 8, 3);      /* state = committed */
                write_u32(c, info + 12, 3);     /* perm = rw */
            }
            if (page) write_u32(c, page, 0);
            return 0;
        }
        case 0x08: { /* CreateThread(out, entry, arg, stack, prio, ...) */
            uint32_t out = c->r[0], entry = c->r[1], arg = c->r[2];
            uint32_t stack = c->r[3];
            uint32_t h = (uint32_t)alloc_handle(SVC_OTHER, "thread");
            if (g_thread_count < 32) {
                GuestThread *t = &g_threads[g_thread_count];
                ArmCpu *tc = (ArmCpu *)calloc(1, sizeof(ArmCpu));
                if (tc != NULL) {
                    arm_cpu_init(tc, c->mem, c->mem_size);
                    tc->tpidruro = c->tpidruro + (uint32_t)(g_thread_count + 1) * 0x1000u;
                    tc->r[0] = arg;
                    tc->r[13] = stack;
                    tc->r[15] = entry;
                    t->used = 1;
                    t->cpu = tc;
                    t->entry = entry;
                    t->id = ((g_thread_count + 1) & 15);
                    t->handle = h;
                    if (getenv("HLE_THR_LOG"))
                        printf("[thrnew] handle=%u entry=%08X arg=%08X stack=%08X id=%d\n",
                               h, entry, arg, stack, t->id);
                    g_thread_count++;
                    CreateThread(NULL, 0, guest_thread_main, t, 0, NULL);
                    if (g_det_sched && t->id >= 0 && t->id < 16) {
                        /* Handshake: warten, bis der neue Thread registriert ist
                           (deterministische Startreihenfolge). */
                        for (unsigned spin = 0; !g_thread_started[t->id]; spin++) {
                            if (spin > 2000000u) break;
                            Sleep(0);
                        }
                    }
                }
            }
            if (out) write_u32(c, out, h);
            c->r[1] = h;
            return 0;
        }
        case 0x09: /* ExitThread */
            if (c != g_main_cpu) {
                thread_signal_exit(c);
                guest_lock_leave();
                if (g_guest_lock_init) ExitThread(0);
            }
            return 0;
        case 0x0C: /* SetThreadPriority(handle, prio) -> Result */
            return 0;
        case 0x0A: { /* SleepThread(ns): echte Wartezeit */
            uint64_t ns = (uint64_t)c->r[0] | ((uint64_t)c->r[1] << 32);
            uint32_t ms = (uint32_t)(ns / 1000000ull);
            if (ms > 50) ms = 50;
            if (g_guest_lock_init && !g_no_lock) {
                guest_lock_leave();
                Sleep(ms ? ms : 0);
                guest_lock_reacquire();
            } else {
                Sleep(ms ? ms : 0);
            }
            return 0;
        }
        case 0x13: /* CreateMutex */
        case 0x15: /* CreateSemaphore */
        case 0x17: { /* CreateEvent */
            uint32_t out = c->r[0];
            uint32_t h = (uint32_t)alloc_handle(SVC_OTHER, "sync");
            if (out) write_u32(c, out, h);
            c->r[1] = h;
            return 0;
        }
        case 0x18: { /* SignalEvent */
            Handle *hh = get_handle(c->r[0]);
            if (hh != NULL) hh->signaled = 1;
            return 0;
        }
        case 0x19: { /* ClearEvent */
            Handle *hh = get_handle(c->r[0]);
            if (hh != NULL) hh->signaled = 0;
            return 0;
        }
        case 0x14: case 0x16: /* ReleaseMutex / ReleaseSemaphore */
            return 0;
        case 0x1E: case 0x1F: case 0x20: /* MemoryBlock */
            if (getenv("HLE_MAP_LOG"))
                printf("[map] svc=%02X r0=%08X r1=%08X r2=%08X r3=%08X\n",
                       imm, c->r[0], c->r[1], c->r[2], c->r[3]);
            return 0;
        case 0x21: { /* CreateAddressArbiter(out) */
            uint32_t h = (uint32_t)alloc_handle(SVC_OTHER, "arbiter");
            if (c->r[0]) write_u32(c, c->r[0], h);
            c->r[1] = h;
            return 0;
        }
        case 0x22: { /* ArbitrateAddress(handle, addr, type, value, timeout) */
            static int arb_prints = 0;
            uint32_t addr = c->r[1] & ~3u;
            uint32_t type = c->r[2];
            int32_t value = (int32_t)c->r[3];
            uint32_t off = addr & 0x1FFFFFFFu;

            if (getenv("HLE_ARB_LOG") && arb_prints < 300) {
                int32_t adbg = 0;
                if (off + 4 <= c->mem_size) memcpy(&adbg, c->mem + off, 4);
                printf("[arb] th=%u addr=%08X type=%u val=%d mem=%08X\n",
                       (uint32_t)((c->tpidruro >> 12) & 0xFu),
                       addr, type, value, (uint32_t)adbg);
                arb_prints++;
            }

            /* Objekt fuer das VBlank-Posting registrieren. */
            {
                int known = 0;
                for (int k = 0; k < g_arb_seen_n; k++)
                    if (g_arb_seen[k] == addr) { known = 1; break; }
                if (!known && g_arb_seen_n < 64) {
                    g_arb_seen[g_arb_seen_n++] = addr;
                    if (getenv("HLE_ARB_LOG"))
                        printf("[arbnew] addr=%08X type=%u val=%d th=%u\n",
                               addr, type, value,
                               (uint32_t)((c->tpidruro >> 12) & 0xFu));
                }
            }

            if (off + 4 > c->mem_size) return 0;

            /* type 0 = SIGNAL: weckt Waiter, blockiert selbst nie. */
            if (type == 0) {
                arb_signal(addr, value);
                return 0;
            }

            /* Warte-Typen 1..4: blockieren, solange Wert < value. */
            {
                int decremented = 0;
                for (int spin = 0; spin < 200; spin++) {
                    int32_t cur = 0;
                    memcpy(&cur, c->mem + off, 4);
                    if (arb_take(addr)) break;          /* per Signal geweckt */
                    if (cur >= value) break;            /* Bedingung erfuellt */
                    /* Negativer Wert = Waiter eingetragen. Erst dem VBlank-
                       Posting ~8 ms Zeit geben, dann zwangsweise signalisieren
                       (sonst laeuft 0x288B90 endlos bzw. blockiert dauerhaft). */
                    if (cur < 0 && spin >= 0) {
                        int32_t nv = 1;
                        memcpy(c->mem + off, &nv, 4);
                        break;
                    }


                    if ((type == 2 || type == 4) && !decremented) {
                        int32_t nv = cur - 1;
                        memcpy(c->mem + off, &nv, 4);
                        decremented = 1;
                    }
                    arb_unlock_short();
                }
                /* Notausgang: nach Timeout als signalisiert markieren. */
                {
                    int32_t cur = 0;
                    memcpy(&cur, c->mem + off, 4);
                    if (cur < value) {
                        int32_t nv = 1;
                        memcpy(c->mem + off, &nv, 4);
                    }
                }
            }
            return 0;
        }
        case 0x23: /* CloseHandle */
            return 0;
        case 0x24: { /* WaitSynchronization1(handle, timeout) */
            uint32_t h = c->r[0];
            Handle *hh = get_handle(h);
            static unsigned vblank = 0;
            for (int i = 0; i < 8; i++) {
                if (hh != NULL && hh->signaled) break;
                guest_yield();
                if ((++vblank & 0x3FFu) == 0) {
                    /* Virtueller VBlank: alle Events signalisieren */
                    for (int k = 1; k < MAX_HANDLES; k++)
                        if (g_handles[k].used) g_handles[k].signaled = 1;
                }
            }
            if (hh != NULL) hh->signaled = 0;
            return 0;
        }
        case 0x25: { /* WaitSynchronizationN(out, handles, count, wait_all, timeout) */
            static int wl = 0;
            if (getenv("HLE_WAIT_NONBLOCK")) {
                uint32_t o = c->r[0];
                if (o != 0 && o != 0xFFFFFFFFu) write_u32(c, o, 0);
                return 0;
            }
            if (getenv("HLE_WAIT_LOG") && wl < 40) {
                wl++;
                printf("[wait25] th=%u r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X blk=%08X lr=%08X\n",
                       (uint32_t)((c->tpidruro >> 12) & 0xFu),
                       c->r[0], c->r[1], c->r[2], c->r[3], c->r[4],
                       arm_last_block, c->r[14]);
            }
            uint32_t out = c->r[0], hlist = c->r[1], cnt = c->r[2];
            if (getenv("HLE_WAIT_LOG") && cnt > 0 && cnt <= 8 && hlist != 0) {
                printf("[wait25 handles] th=%u n=%u:",
                       (uint32_t)((c->tpidruro >> 12) & 0xFu), cnt);
                for (uint32_t k = 0; k < cnt; k++) {
                    uint32_t hv = arm_mem_read(c, hlist + k * 4, 4);
                    Handle *hh2 = get_handle(hv);
                    printf(" %u(%s)", hv, hh2 ? hh2->name : "-");
                }
                printf("\n");
            }
            static unsigned vb2 = 0;
            int idx = 0;
            for (int i = 0; i < 64; i++) {
                int hit = -1;
                for (uint32_t k = 0; k < cnt && k < 16; k++) {
                    uint32_t hv = (hlist ? arm_mem_read(c, hlist + k * 4, 4) : 0);
                    if (hv < MAX_HANDLES) g_wait_seen[hv] = 1;
                    Handle *hh = get_handle(hv);
                    /* Handle 0 = ungueltig: als "sofort erfuellt" behandeln,
                       sonst blockiert der Waiter dauerhaft. */
                    /* Handle 0 = ungueltig: standardmaessig als sofort
                       erfuellt behandeln. Mit HLE_H0_WAIT dagegen als
                       "nie signalisiert", damit der Dispatcher die echten
                       Indizes (z. B. APT-Parameter = 2) sieht. */
                    int h0ok = (hv == 0) && getenv("HLE_H0_WAIT") == NULL;
                    if (h0ok || (hh != NULL && hh->signaled)) { hit = (int)k; break; }
                }
                if (hit >= 0) { idx = hit; break; }
                guest_yield();
                if ((++vb2 & 0x3FFu) == 0) {
                    for (int kk = 1; kk < MAX_HANDLES; kk++)
                        if (g_handles[kk].used) g_handles[kk].signaled = 1;
                }
            }
            if (cnt > 0) {
                for (uint32_t k = 0; k < cnt && k < 16; k++) {
                    uint32_t hv = (hlist ? arm_mem_read(c, hlist + k * 4, 4) : 0);
                    Handle *hh = get_handle(hv);
                    if (hh != NULL && hh->signaled) { hh->signaled = 0; break; }
                }
            }
            if (getenv("HLE_WAIT_LOG"))
                printf("[wait25 idx] th=%u out=%08X cnt=%u -> idx=%d\n",
                       (uint32_t)((c->tpidruro >> 12) & 0xFu), out, cnt, idx);
            if (out != 0 && out != 0xFFFFFFFFu) write_u32(c, out, (uint32_t)idx);
            /* Der SDK-Wrapper (0x287B90) erwartet den Index in r1 und
               schreibt ihn selbst nach *out; ohne r1 nimmt der
               Dispatcher den falschen Zweig. */
            c->r[1] = (uint32_t)idx;
            return 0;
        }
        case 0x27: { /* DuplicateHandle(out, handleO) */
            Handle *h = get_handle(c->r[1]);
            uint32_t nh = h ? (uint32_t)alloc_handle(h->service, h->name) : 0;
            if (c->r[0]) write_u32(c, c->r[0], nh);
            c->r[1] = nh;
            return 0;
        }
        case 0x28: { /* GetSystemTick -> 64 Bit in r0:r1 (echte Zeitbasis) */
            static uint64_t base_ms = 0;
            static uint64_t last_frame = 0;
            static int in_frame = 0;
            uint64_t now = GetTickCount64();
            if (base_ms == 0) base_ms = now;
            g_tick = (now - base_ms) * 268111ull;   /* ~268.111 MHz */
            g_tick_calls++;
            /* Frame-Pump: die Engine fragt die Zeit in ihrer Frame-Schleife
               dauerhaft ab. Genau dort treiben wir die registrierten
               Tick-/Frame-Tasks (0x400/0x401) an, damit die Frame-Zustellung
               nicht von einem bestimmten Gast-Thread abhaengt. */
            if (!in_frame && !getenv("HLE_TICK_FROM_SVC0") &&
                g_tick - last_frame >= 268111u / 60u) {
                uint32_t sp0 = c->r[13];
                in_frame = 1;
                last_frame = g_tick;
                c->r[13] = 0x0CF0F000u;
                arm_call_guest_pub(c, 0x00134818u);
                arm_call_guest_pub(c, 0x0013485Cu);
                c->r[13] = sp0;
                in_frame = 0;
            }
            c->r[1] = (uint32_t)(g_tick >> 32);
            return (uint32_t)g_tick;
        }
        case 0x2A: { /* GetSystemInfo(type, out u64*, param, process) */
            uint32_t type = c->r[0], out = c->r[1];
            uint64_t value = 0;
            switch (type) {
                case 0x10000: value = 0x08000000ull; break;  /* FCRAM gesamt */
                case 0x10001: value = 0x00800000ull; break;  /* belegt */
                case 0x10002: value = 0x1FF00000ull; break;  /* VRAM-Basis */
                case 0x10003: value = 0x00600000ull; break;  /* VRAM-Groesse */
                default: value = 0; break;
            }
            if (out) write_u64(c, out, value);
            if (g_log) printf("[svc] GetSystemInfo type=0x%X -> 0x%llX\n", type,
                              (unsigned long long)value);
            return 0;
        }
        case 0x2B: { /* GetProcessInfo(type, out, process) */
            uint32_t type = c->r[0], out = c->r[1];
            uint64_t value = 0;
            switch (type) {
                case 0: value = 0x00000020ull; break;
                case 2: value = 0x08000000ull; break;
                case 0x10000: value = 0x00000000ull; break;
                default: value = 0; break;
            }
            if (out) write_u64(c, out, value);
            return 0;
        }
        case 0x2D: { /* ConnectToPort(out_ptr=r0, name=r1) -> r1 = handle */
            const char *name = (const char *)(c->mem + (c->r[1] & 0x1FFFFFFF));
            int svc = service_from_name(name);
            uint32_t h = (uint32_t)alloc_handle(svc, name);
            if (g_log) printf("[svc] ConnectToPort(\"%s\") -> %u (lr=%08X sp=%08X)\n",
                              name, h, c->r[14], c->r[13]);
            c->r[1] = h;
            return 0;
        }
        case 0x32: { /* SendSyncRequest(handle) */
            uint32_t handle = c->r[0];
            if (getenv("HLE_IPC_RAW")) {
                uint32_t *rb = (uint32_t *)(c->mem + c->tpidruro + TLS_CMD_BUF);
                uint32_t hdr = rb[0];
                printf("[ipcraw] h=%u hdr=%08X cmd=%04X n=%d t=%d p0=%08X p1=%08X blk=%08X lr=%08X\n",
                       handle, hdr, (hdr >> 16) & 0xFFFFu,
                       (int)((hdr >> 6) & 0x3Fu), (int)(hdr & 0x3Fu),
                       rb[1], rb[2], arm_last_block, c->r[14]);
            }
            dispatch_ipc(handle);
            return 0;
        }
        case 0x35: /* GetProcessId(out, process) */
            if (c->r[0]) write_u32(c, c->r[0], 0x20);
            return 0;
        case 0x37: { /* GetThreadId(out_*, thread): pro Thread eindeutig */
            uint32_t tid = 0x100u + ((c->tpidruro >> 12) & 0xFu);
            if (c->r[0]) write_u32(c, c->r[0], tid);
            return tid;
        }
        case 0x38: { /* GetResourceLimit(out, process) */
            uint32_t out = c->r[0];
            uint32_t h = (uint32_t)alloc_handle(SVC_OTHER, "reslimit");
            if (out) write_u32(c, out, h);
            c->r[1] = h;
            return 0;
        }
        case 0x39: { /* GetResourceLimitLimitValues(vals, handle, names, count) */
            uint32_t vals = c->r[0], names = c->r[2], count = c->r[3];
            for (uint32_t i = 0; i < count && i < 16; i++) {
                write_u64(c, vals + i * 8, 0x08000000ull);   /* 128 MiB je Ressource */
            }
            return 0;
        }
        case 0x3A: { /* GetResourceLimitCurrentValues(vals, handle, names, count) */
            uint32_t vals = c->r[0], count = c->r[3];
            for (uint32_t i = 0; i < count && i < 16; i++) write_u64(c, vals + i * 8, 0ull);
            return 0;
        }
        case 0x3C: /* Break */
            return 0;
        default:
            if (g_log) {
                static unsigned char seen[256];
                if (imm < 256 && !seen[imm]) {
                    seen[imm] = 1;
                    printf("[svc] 0x%02X unbehandelt (r0=%08X r1=%08X)\n", imm, c->r[0], c->r[1]);
                }
            }
            return 0;
    }
}

/* Platzhalter, bis die GPU angebunden ist */
void hle_gpu_write_reg(uint32_t addr, uint32_t value) { (void)addr; (void)value; }
void hle_gpu_command_list(const uint8_t *data, uint32_t size, uint32_t flags) {
    (void)data; (void)size; (void)flags;
}
