/* HLE: SVC-Dispatch, IPC und 3DS-Services fuer den recompilierten Code.
 *
 * IPC-Header (verifiziert an Azahar src/core/hle/ipc.h):
 *   bits 0-5   : translate_params_size (Woerter)
 *   bits 6-11  : normal_params_size   (Woerter)
 *   bits 16-31 : command_id
 */
#include "hle.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TLS_CMD_BUF 0x80u
#define MAX_HANDLES 512
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
};

typedef struct {
    int used;
    int service;     /* ServiceId */
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
static int g_ipc_count = 0;
static int g_ipc_log_limit = 60;
static unsigned g_cmd_seen[64][64];

/* Ringpuffer der letzten Ereignisse (fuer Absturzdiagnose) */
#define EV_RING 64
static struct { int kind; uint32_t a, b, c; } g_ev_ring[EV_RING];
static int g_ev_pos = 0;

static void ev(int kind, uint32_t a, uint32_t b, uint32_t c) {
    g_ev_ring[g_ev_pos].kind = kind;
    g_ev_ring[g_ev_pos].a = a;
    g_ev_ring[g_ev_pos].b = b;
    g_ev_ring[g_ev_pos].c = c;
    g_ev_pos = (g_ev_pos + 1) % EV_RING;
}   /* service x command */

static const char *SERVICE_NAMES[] = {
    "", "srv:", "fs:USER", "fs:LDR", "gsp::Gpu", "gsp::Lcd", "hid:USER",
    "APT:U", "cfg:u", "ptm:u", "dsp::DSP", "y2r:u", "ndsp:", "soc:U",
    "ir:USER", "mic:u", "cam:u", "ac:u", "boss:U", "cecd:u", "news:u",
    "ndm:u", "nwm::UDS", "frd:u", "am:u", "ldr:ro", "mvd:std", "nfc:u",
    "pxi:dev", "dlp:FKCL", "ssl:C", "http:C", "?",
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
            snprintf(g_handles[i].name, sizeof(g_handles[i].name), "%s", name ? name : "?");
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
            ipc_begin(r, 2, 1, 2);
            ipc_put(r, 0);
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
            ipc_begin(r, 5, 1, 2);
            ipc_put(r, 0);
            ipc_put_handle(r, (uint32_t)h);
            break;
        }
        default:
            if (g_log) printf("[srv] cmd %u (unbekannt) -> 0\n", r->cmd);
            ipc_begin(r, r->cmd, 1, 0);
            ipc_put(r, 0);
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
    ev(2, (uint32_t)service, cmd, normal);
    g_ipc_count++;
    if (cmd < 64 && service >= 0 && service < 64) g_cmd_seen[service][cmd]++;
    if (h && (g_ipc_count <= g_ipc_log_limit || g_log)) {
        printf("[ipc] %-9s cmd=0x%04X n=%d t=%d h=%u", SERVICE_NAMES[service], cmd,
               normal, translate, handle);
        for (int i = 0; i < normal && i < 8; i++) printf(" p%d=%08X", i, params[i]);
        printf("\n");
    }

    switch (service) {
        case SVC_SRV:
            srv_command(&r, params);
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

void hle_init(ArmCpu *c, const char *romfs_dir) {
    g_cpu = c;
    snprintf(g_romfs, sizeof(g_romfs), "%s", romfs_dir ? romfs_dir : "out/romfs");
    g_log = getenv("HLE_LOG") != NULL;
    const char *lim = getenv("HLE_IPC_LOG");
    if (lim) g_ipc_log_limit = atoi(lim);
    memset(g_handles, 0, sizeof(g_handles));
    g_next_handle = 1;
    g_srv_handle = (uint32_t)alloc_handle(SVC_SRV, "srv:");
    printf("[hle] romfs=%s log=%d srv-handle=%u\n", g_romfs, g_log, g_srv_handle);
}

void hle_report(void) {
    printf("[hle] ipc=%d ticks=%llu\n", g_ipc_count, (unsigned long long)g_tick);
    printf("[hle] letzte Ereignisse:\n");
    for (int i = 0; i < EV_RING; i++) {
        int idx = (g_ev_pos + i) % EV_RING;
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
    ev(1, imm, c->r[0], c->r[1]);
    switch (imm) {
        case 0x01: { /* ControlMemory(addr, size, op, perm) */
            uint32_t addr = c->r[1];
            if (addr != 0) write_u32(c, c->r[0], addr);
            return 0;
        }
        case 0x02: /* QueryMemory */
            return 0;
        case 0x08: { /* CreateThread(out, entry, arg, stack, prio>>..., ...) */
            uint32_t out = c->r[0];
            if (out) write_u32(c, out, (uint32_t)alloc_handle(SVC_OTHER, "thread"));
            return 0;
        }
        case 0x09: /* ExitThread */
            return 0;
        case 0x0A: /* SleepThread */
            return 0;
        case 0x13: /* CreateMutex */
        case 0x15: /* CreateSemaphore */
        case 0x17: { /* CreateEvent */
            uint32_t out = c->r[0];
            if (out) write_u32(c, out, (uint32_t)alloc_handle(SVC_OTHER, "sync"));
            return 0;
        }
        case 0x18: case 0x19: case 0x14: case 0x16: /* Signal/Clear/Release */
            return 0;
        case 0x1E: case 0x1F: case 0x20: /* MemoryBlock */
            return 0;
        case 0x22: /* ArbitrateAddress */
            return 0;
        case 0x23: /* CloseHandle */
            return 0;
        case 0x24: /* WaitSynchronization1 */
            return 0;
        case 0x25: /* WaitSynchronizationN(out, handles, count, wait_all, timeout) */
            if (c->r[0]) write_u32(c, c->r[0], 0);
            return 0;
        case 0x27: { /* DuplicateHandle(out, handleO) */
            Handle *h = get_handle(c->r[1]);
            uint32_t nh = h ? (uint32_t)alloc_handle(h->service, h->name) : 0;
            if (c->r[0]) write_u32(c, c->r[0], nh);
            return 0;
        }
        case 0x28: /* GetSystemTick */
            g_tick += 0x1000;
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
        case 0x37: /* GetThreadId(out_*, thread) */
            if (c->r[0]) write_u32(c, c->r[0], 0x100);
            return 0;
        case 0x38: { /* GetResourceLimit(out, process) */
            uint32_t out = c->r[0];
            if (out) write_u32(c, out, (uint32_t)alloc_handle(SVC_OTHER, "reslimit"));
            return 0;
        }
        case 0x39: { /* GetResourceLimitLimitValues(vals, handle, names, count) */
            uint32_t vals = c->r[0], names = c->r[2], count = c->r[3];
            for (uint32_t i = 0; i < count && i < 16; i++) {
                uint32_t name = names ? (uint32_t)arm_mem_read(c, names + i * 4, 4) : 0;
                uint64_t v = 0x40000000ull;
                if (name == 0) v = 0x40000000ull;
                write_u64(c, vals + i * 8, v);
            }
            return 0;
        }
        case 0x3A: { /* GetResourceLimitCurrentValues(vals, handle, names, count) */
            uint32_t vals = c->r[0], count = c->r[3];
            for (uint32_t i = 0; i < count && i < 16; i++) write_u64(c, vals + i * 8, 0x00100000ull);
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
