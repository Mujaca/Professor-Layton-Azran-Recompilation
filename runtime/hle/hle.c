/* HLE: SVC-Dispatch, IPC und 3DS-Services fuer den recompilierten Code.
 *
 * IPC-Header (verifiziert an Azahar src/core/hle/ipc.h):
 *   bits 0-5   : translate_params_size (Woerter)
 *   bits 6-11  : normal_params_size   (Woerter)
 *   bits 16-31 : command_id
 */
#include "hle.h"
#include "../arm_trace.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define TLS_CMD_BUF 0x80u

/* ---- kooperatives Threading: es laeuft immer nur ein Gast-Thread ---- */
static CRITICAL_SECTION g_guest_lock;
static int g_guest_lock_init = 0;
static ArmCpu *g_main_cpu = NULL;

static int g_no_lock = 0;
static void guest_lock_enter(void) {
    if (g_guest_lock_init && !g_no_lock) EnterCriticalSection(&g_guest_lock);
}
static void guest_lock_leave(void) {
    if (g_guest_lock_init && !g_no_lock) LeaveCriticalSection(&g_guest_lock);
}
static void guest_yield(void) {
    if (!g_guest_lock_init) return;
    if (g_no_lock) { Sleep(0); return; }
    LeaveCriticalSection(&g_guest_lock);
    Sleep(0);
    EnterCriticalSection(&g_guest_lock);
}

extern void arm_call_guest_pub(ArmCpu *c, uint32_t addr);

typedef struct {
    int used;
    ArmCpu *cpu;
    uint32_t entry;
} GuestThread;
static GuestThread g_threads[32];
static int g_thread_count = 0;

static DWORD WINAPI guest_thread_main(LPVOID p) {
    GuestThread *t = (GuestThread *)p;
    if (!g_no_lock) EnterCriticalSection(&g_guest_lock);
    arm_call_guest_pub(t->cpu, t->entry);
    if (!g_no_lock) LeaveCriticalSection(&g_guest_lock);
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
            if (g_log) {
                printf("[srv] GetServiceHandle \"%s\" -> %d (%s)\n", name, h, SERVICE_NAMES[svc]);
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
            ipc_put(r, 1);            /* state = running */
            ipc_put_handle_copy(r, lock);
            break;
        }
        case 2: { /* Initialize -> { CopyHandle x2, h1, h2 } */
            uint32_t ev1 = (uint32_t)alloc_handle(SVC_OTHER, "apt-notif");
            uint32_t ev2 = (uint32_t)alloc_handle(SVC_OTHER, "apt-param");
            ipc_begin(r, 2, 1, 3);
            put_result(r);
            ipc_put(r, (1u << 26));   /* CopyHandle-Deskriptor, 2 Handles */
            ipc_put(r, ev1);
            ipc_put(r, ev2);
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
            if (!g_apt_wakeup_sent) {
                g_apt_wakeup_sent = 1;
                ipc_begin(r, r->cmd, 4, 2 + (r->translate >= 2 ? 2 : 0));
                ipc_put(r, 0);          /* ResultSuccess */
                ipc_put(r, 0);          /* sender_id = None */
                ipc_put(r, 1);          /* signal = Wakeup */
                ipc_put(r, 0);          /* Parameter-Groesse */
                ipc_put_handle_copy(r, 0);
                if (r->translate >= 2) {
                    ipc_put(r, dsc_flags);
                    ipc_put(r, dsc_addr);
                }
                if (g_log) printf("[apt] ReceiveParameter -> Wakeup\n");
            } else {
                ipc_begin(r, r->cmd, 1, 0);
                /* Result(NoData=1007, Applet=51, InvalidState=5, Status=25) */
                ipc_put(r, 0xC8A0CFEFu);
                if (g_log) printf("[apt] ReceiveParameter -> NoData\n");
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

static int open_guest_file(const char *guest_path, FileSlot **slot_out) {
    char path[256];
    const char *q = guest_path;
    if (strncmp(q, "romfs:", 6) == 0) q += 6;
    while (*q == '/') q++;
    snprintf(path, sizeof(path), "%s/%s", g_romfs, q);
    for (char *c = path; *c; c++) {
        if (*c == '/') *c = '\\';
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    for (int i = 0; i < MAX_FILES; i++) {
        if (!g_files[i].used) {
            g_files[i].used = 1;
            snprintf(g_files[i].path, sizeof(g_files[i].path), "%s", path);
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
                printf("[fsdbg] cmd=%04X n=%d t=%d type=%u size=%u addr=%08X len=%u",
                       r->cmd, r->normal, r->translate, ftype, fsize, addr, len);
                if (addr) {
                    const uint8_t *dbg = g_cur_mem + (addr & 0x1FFFFFFFu);
                    printf(" bytes:");
                    for (int i = 0; i < 16; i++) printf(" %02X", dbg[i]);
                }
                printf("\n");
            }
            if (addr >= 0x08000000u && addr < 0x20000000u) {
                const uint8_t *src = g_cur_mem + (addr & 0x1FFFFFFFu);
                uint32_t limit = len ? len : fsize;
                if (limit > 250) limit = 250;
                int n = 0;
                if (ftype == 4 || (ftype == 0 && limit >= 4 && src[1] == 0)) {
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
            FileSlot *slot = NULL;
            int opened = path[0] ? open_guest_file(path, &slot) : 0;
            if (!opened) {
                /* Fallback: leere Datei, damit die SDK weiterlaeuft */
                for (int i = 0; i < MAX_FILES; i++) {
                    if (!g_files[i].used) {
                        g_files[i].used = 1;
                        g_files[i].fp = NULL;
                        g_files[i].size = 0;
                        snprintf(g_files[i].path, sizeof(g_files[i].path), "%s", path);
                        slot = &g_files[i];
                        opened = 1;
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
            ipc_begin(r, r->cmd, 1, 2);
            ipc_put(r, 0);
            ipc_put_handle(r, h);
            break;
        }
        case 0x080C: { /* OpenArchive */
            char path[256];
            find_path(p, path, sizeof(path));
            uint32_t h = (uint32_t)alloc_handle(SVC_FS_USER, "archive");
            if (g_log) printf("[fs] OpenArchive -> handle %u\n", h);
            ipc_begin(r, 0x080C, 1, 2);
            ipc_put(r, 0);
            ipc_put_handle(r, h);
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

static void file_command(Ipc *r, uint32_t *p) {
    Handle *h = get_handle(g_cur_handle);
    if (h == NULL || h->file < 0) {
        ipc_begin(r, r->cmd, 1, 0);
        ipc_put(r, 0);
        return;
    }
    FileSlot *f = &g_files[h->file];
    switch (r->cmd) {
        case 0x0804: { /* GetSize */
            ipc_begin(r, 0x0804, 3, 0);
            ipc_put(r, 0);
            ipc_put(r, f->size);
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
        case 0x0802: { /* Read(offset u64, length u32, buffer) */
            uint64_t offset = (uint64_t)p[0] | ((uint64_t)p[1] << 32);
            uint32_t length = p[2];
            uint32_t addr = 0;
            int nn = r->normal;
            for (int i = 0; i < 8; i++) {
                uint32_t w = p[nn + i];
                if (w >= 0x08000000u && w < 0x20000000u) { addr = w; break; }
            }
            uint32_t done = 0;
            if (addr && length && f->fp) {
                fseek(f->fp, (long)offset, SEEK_SET);
                done = (uint32_t)fread(g_cur_mem + (addr & 0x1FFFFFFFu), 1, length, f->fp);
            }
            if (g_log) {
                printf("[fs] Read off=0x%llX len=0x%X -> %u Bytes\n",
                       (unsigned long long)offset, length, done);
            }
            ipc_begin(r, 0x0802, 3, 0);
            ipc_put(r, 0);
            ipc_put(r, done);
            ipc_put(r, 0);
            break;
        }
        default:
            generic_command(r, SVC_FILE);
            break;
    }
}

/* Schreibt Daten in den Antwort-Puffer eines IPC-Requests (Buffer-Scan). */
static uint32_t ipc_out_buffer(uint32_t *p, int normal, int translate) {
    for (int i = 0; i < translate; i++) {
        uint32_t w = p[normal + i];
        if (w >= 0x08000000u && w < 0x20000000u) return w;
    }
    return 0;
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

static void dsp_command(Ipc *r, uint32_t *p) {
    switch (r->cmd) {
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
        case 0x0012: { /* UnloadComponent */
            ipc_begin(r, 0x0012, 1, 0);
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
static uint32_t g_gsp_fb_addr[3] = {0x1F000000u, 0x1F000000u, 0x1F000000u};
static int g_gsp_last_screen = 0;

static void gsp_command(Ipc *r, uint32_t *p) {
    switch (r->cmd) {
        case 0x0001: { /* WriteHWRegs(addr, size, data...) */
            if (g_log) printf("[gsp] WriteHWRegs addr=0x%X size=%u\n", p[0], p[1]);
            ipc_begin(r, 0x0001, 1, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0004: { /* SetBufferSwap(screen, framebuffer_info) */
            uint32_t screen = p[0];
            uint32_t fb = p[1];
            if (screen < 3) g_gsp_fb_addr[screen] = fb;
            g_gsp_last_screen = (int)(screen & 1);
            if (g_log) printf("[gsp] SetBufferSwap screen=%u fb=0x%08X\n", screen, fb);
            ipc_begin(r, 0x0004, 1, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0006: { /* RegisterInterruptRelayQueue -> Event + SharedMem */
            uint32_t ev = (uint32_t)alloc_handle(SVC_OTHER, "gsp-ev");
            ipc_begin(r, 0x0006, 1, 2);
            ipc_put(r, 0);
            ipc_put_handle_copy(r, ev);
            break;
        }
        case 0x0008: { /* TriggerCmdReqQueue */
            ipc_begin(r, 0x0008, 1, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0009: { /* ImportDisplayCaptureInfo -> 2x Framebuffer-Info */
            ipc_begin(r, 0x0009, 6, 0);
            ipc_put(r, 0);
            ipc_put(r, 0x1F000000u); ipc_put(r, 0);
            ipc_put(r, 0x1F000000u); ipc_put(r, 0);
            ipc_put(r, 0); ipc_put(r, 0);
            break;
        }
        case 0x000B: { /* SetLcdForceBlack */
            ipc_begin(r, 0x000B, 1, 0);
            ipc_put(r, 0);
            break;
        }
        default:
            generic_command(r, SVC_GSP_GPU);
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
    /* Fallback: fs-Kommandos auch bei unbekanntem Handle als fs behandeln */
    if (h == NULL && (cmd & 0xFF00u) == 0x0800u) {
        service = SVC_FS_USER;
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
        case SVC_GSP_GPU:
            gsp_command(&r, params);
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
    w32(c, 0x1FF80044u, 0x01000000u);  /* sys_mem_alloc */
    w32(c, 0x1FF80048u, 0x00800000u);  /* base_mem_alloc */
    p[0x61] = 0x3A;
    p[0x62] = 0x02;
    p[0x63] = 0x02;
    w32(c, 0x1FF80064u, 2u);
    w32(c, 0x1FF80068u, 0x0000F450u);
    printf("[hle] config-memory initialisiert (app_mem_alloc=128MiB)\n");
}

void hle_init(ArmCpu *c, const char *romfs_dir) {
    g_cpu = c;
    snprintf(g_romfs, sizeof(g_romfs), "%s", romfs_dir ? romfs_dir : "out/romfs");
    g_log = getenv("HLE_LOG") != NULL;
    if (getenv("HLE_SRV_LAYOUT")) g_srv_layout = atoi(getenv("HLE_SRV_LAYOUT"));
    const char *lim = getenv("HLE_IPC_LOG");
    if (lim) g_ipc_log_limit = atoi(lim);
    setup_config_mem(c);
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
        g_no_lock = getenv("HLE_NO_THREAD_LOCK") != NULL;
        if (!g_no_lock) EnterCriticalSection(&g_guest_lock);
    }
    g_main_cpu = c;
    printf("[hle] romfs=%s log=%d srv-handle=%u\n", g_romfs, g_log, g_srv_handle);
}

void hle_report(void) {
    printf("[hle] ipc=%d ticks=%llu\n", g_ipc_count, (unsigned long long)g_tick);
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

uint32_t hle_svc(ArmCpu *c, uint32_t imm) {
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
                    g_thread_count++;
                    CreateThread(NULL, 0, guest_thread_main, t, 0, NULL);
                }
            }
            if (out) write_u32(c, out, h);
            c->r[1] = h;
            return 0;
        }
        case 0x09: /* ExitThread */
            if (c != g_main_cpu) {
                guest_lock_leave();
                if (g_guest_lock_init) ExitThread(0);
            }
            return 0;
        case 0x0A: /* SleepThread */
            guest_yield();
            return 0;
        case 0x13: /* CreateMutex */
        case 0x15: /* CreateSemaphore */
        case 0x17: { /* CreateEvent */
            uint32_t out = c->r[0];
            uint32_t h = (uint32_t)alloc_handle(SVC_OTHER, "sync");
            if (out) write_u32(c, out, h);
            c->r[1] = h;
            return 0;
        }
        case 0x18: case 0x19: case 0x14: case 0x16: /* Signal/Clear/Release */
            return 0;
        case 0x1E: case 0x1F: case 0x20: /* MemoryBlock */
            return 0;
        case 0x21: { /* CreateAddressArbiter(out) */
            uint32_t h = (uint32_t)alloc_handle(SVC_OTHER, "arbiter");
            if (c->r[0]) write_u32(c, c->r[0], h);
            c->r[1] = h;
            return 0;
        }
        case 0x22: { /* ArbitrateAddress(handle, addr, type, value, timeout) */
            guest_yield();
            uint32_t addr = c->r[1];
            uint32_t type = c->r[2];
            uint32_t value = c->r[3];
            if ((addr & 0x1FFFFFFFu) + 4 <= c->mem_size) {
                uint32_t cur;
                memcpy(&cur, c->mem + (addr & 0x1FFFFFFFu), 4);
                if (type == 1 || type == 3) { /* Decrement(AndWait)IfLessThan */
                    if ((int32_t)cur <= -2) cur = 1;
                    else cur -= 1;
                    memcpy(c->mem + (addr & 0x1FFFFFFFu), &cur, 4);
                } else if (type == 0 || type == 2) { /* WaitIfLessThan */
                    /* Ohne echte Interrupts wuerde ein nie geposteter Semaphor
                       ewig spinnen: hier einmalig "posten". */
                    if ((int32_t)cur <= 0) {
                        cur = 1;
                        memcpy(c->mem + (addr & 0x1FFFFFFFu), &cur, 4);
                    }
                }
                (void)value;
            }
            return 0;
        }
        case 0x23: /* CloseHandle */
            return 0;
        case 0x24: /* WaitSynchronization1 */
            for (int i = 0; i < 4; i++) guest_yield();
            return 0;
        case 0x25: /* WaitSynchronizationN(out, handles, count, wait_all, timeout) */
            for (int i = 0; i < 4; i++) guest_yield();
            if (c->r[0]) write_u32(c, c->r[0], 0);
            return 0;
        case 0x27: { /* DuplicateHandle(out, handleO) */
            Handle *h = get_handle(c->r[1]);
            uint32_t nh = h ? (uint32_t)alloc_handle(h->service, h->name) : 0;
            if (c->r[0]) write_u32(c, c->r[0], nh);
            c->r[1] = nh;
            return 0;
        }
        case 0x28: /* GetSystemTick -> 64 Bit in r0:r1 */
            /* Virtuelle Zeit: deutlich schneller als Echtzeit, damit
               Sleep-/Timeout-Schleifen nicht Millionen Iterationen brauchen. */
            g_tick += 0x20000;
            c->r[1] = (uint32_t)(g_tick >> 32);
            return (uint32_t)g_tick;
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
            if (g_log) printf("[svc] ConnectToPort(\"%s\") -> %u\n", name, h);
            c->r[1] = h;
            return 0;
        }
        case 0x32: { /* SendSyncRequest(handle) */
            uint32_t handle = c->r[0];
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
