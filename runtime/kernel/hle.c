/* HLE: SVC-Dispatch, IPC und 3DS-Services fuer den recompilierten Code.
 *
 * IPC-Header (verifiziert an Azahar src/core/hle/ipc.h):
 *   bits 0-5   : translate_params_size (Woerter)
 *   bits 6-11  : normal_params_size   (Woerter)
 *   bits 16-31 : command_id
 */
#include "hle.h"
#include "gpu.h"

static uint32_t g_hle_gsp_handle;
/* Von generiertem Code (gen_boot.c) lesbarer gsp-Handle. */
uint32_t g_hle_gsp_export;
/* Interrupt-Event, das der Client bei RegisterInterruptRelayQueue uebergibt. */
static uint32_t g_gsp_irq_handle;
static uint32_t g_apt_param_ev = 0;
static void dispatch_ipc(uint32_t handle);
static int gsp_process_cmdqueue(ArmCpu *c);
#include "arm_trace.h"

void arm_call_guest_pub(ArmCpu *c, uint32_t addr);   /* gen_boot.c */

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
static uint32_t g_gsp_fb_stride[3] = {0, 0, 0};
static uint32_t g_gsp_fb_format[3] = {1, 1, 1};   /* 1 = BGR8 */
uint32_t hle_gsp_fb_addr(int screen) {
    return (screen >= 0 && screen < 3) ? g_gsp_fb_addr[screen] : 0u;
}
void hle_gsp_fb_layout(int screen, uint32_t *stride, uint32_t *format) {
    if (screen < 0 || screen > 2) screen = 0;
    if (stride) *stride = g_gsp_fb_stride[screen];
    if (format) *format = g_gsp_fb_format[screen];
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
static int gsp_process_cmdqueue(ArmCpu *c);

/* Pro Gast-Block: CPU-gebundene Gastschleifen ohne SVC duerfen den
 * kooperativen Lock nicht dauerhaft halten. Alle 0x4000 Bloecke abgeben. */
void hle_pump(void) {
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
    if (getenv("HLE_THR_LOG"))
        printf("[thrstart] id=%d entry=%08X\n", t_tid, t->entry);
    if (!g_no_lock) {
        sched_wait_turn();
        guest_lock_enter();
        if (getenv("HLE_THR_LOG")) printf("[thrlock] id=%d\n", t_tid);
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

/* Kernel-Synchronisationsobjekte (3dbrew/Azahar kernel/{mutex,semaphore,
 * event,timer,address_arbiter}.cpp). Der "signaled"-Zustand bleibt fuer
 * einfache HLE-Event-Handles (GSP/HID/DSP/Y2R) erhalten; eigene Objekte
 * tragen zusaetzlich die echte Semantik. */
typedef enum {
    HK_NONE = 0,     /* HLE-Event (Dienst-Handle), OneShot-artig */
    HK_MUTEX,
    HK_SEMAPHORE,
    HK_EVENT,
    HK_TIMER,
    HK_THREAD,
    HK_ARBITER
} HandleKind;

typedef struct {
    int used;
    int service;     /* ServiceId */
    int file;        /* Index in g_files oder -1 */
    volatile int signaled;   /* Event/Timer/Thread: Signalzustand */
    int kind;                /* HandleKind */
    uint32_t owner;          /* Mutex: Thread-Id des Besitzers */
    int lock_count;          /* Mutex: rekursive Sperren */
    int avail;               /* Semaphore: freie Plaetze */
    int max_count;           /* Semaphore: Maximum */
    int reset_type;          /* Event/Timer: 0 OneShot, 1 Sticky, 2 Pulse */
    uint64_t timer_due;      /* Timer: faellig (Host-us) */
    uint64_t timer_interval; /* Timer: Wiederholung (Host-us), 0 = einmalig */
    int timer_active;        /* Timer: laeuft */
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
            memset(&g_handles[i], 0, sizeof(g_handles[i]));
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

/* Typisierte Kernel-Objekte fuer HLE-Dienste (Azahar legt sie ebenfalls als
 * Mutex/Semaphore/Event an, z. B. APT_U:Lock, APT:Notification). */
static uint32_t new_event_handle(const char *name, int reset_type) {
    uint32_t h = (uint32_t)alloc_handle(SVC_OTHER, name);
    Handle *hh = get_handle(h);
    if (hh != NULL) { hh->kind = HK_EVENT; hh->reset_type = reset_type; }
    return h;
}

static uint32_t new_mutex_handle(const char *name, int initial_locked) {
    uint32_t h = (uint32_t)alloc_handle(SVC_OTHER, name);
    Handle *hh = get_handle(h);
    if (hh != NULL) {
        hh->kind = HK_MUTEX;
        if (initial_locked) { hh->owner = 0xFFFFFFFFu; hh->lock_count = 1; }
    }
    return h;
}

static uint32_t new_semaphore_handle(const char *name, int initial, int maxc) {
    uint32_t h = (uint32_t)alloc_handle(SVC_OTHER, name);
    Handle *hh = get_handle(h);
    if (hh != NULL) { hh->kind = HK_SEMAPHORE; hh->avail = initial; hh->max_count = maxc; }
    return h;
}

/* ---------- Kernel-Synchronisation ---------- */

/* Result-Codes nach Azahar core/hle/result.h */
#define RESULT_INVALID_HANDLE 0xD8E007F7u
#define RESULT_TIMEOUT        0x09401BFEu
#define RESULT_WRONG_THREAD   0xD8E0041Fu
#define RESULT_OUT_OF_RANGE   0xD8E007FDu

static uint64_t host_us(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (uint64_t)((t.QuadPart * 1000000ull) / (uint64_t)freq.QuadPart);
}

/* Thread-Id des aktuell laufenden Gast-Threads (0 = Hauptthread). */
static uint32_t cur_tid(ArmCpu *c) {
    if (c == g_main_cpu) return 0;
    for (int i = 0; i < 32; i++) {
        if (g_threads[i].used && g_threads[i].cpu == c) return (uint32_t)g_threads[i].id;
    }
    return 0;
}

static const char *handle_kind_name(const Handle *h) {
    switch (h->kind) {
    case HK_MUTEX: return "mutex";
    case HK_SEMAPHORE: return "semaphore";
    case HK_EVENT: return "event";
    case HK_TIMER: return "timer";
    case HK_THREAD: return "thread";
    case HK_ARBITER: return "arbiter";
    default: return "event(hle)";
    }
}

/* ShouldWait/Acquire nach Azahar kernel/{mutex,semaphore,event,timer}.cpp */
static int hs_should_wait(const Handle *h, uint32_t tid) {
    switch (h->kind) {
    case HK_MUTEX:
        return h->lock_count > 0 && h->owner != tid;
    case HK_SEMAPHORE:
        return h->avail <= 0;
    default:
        return !h->signaled;
    }
}

static void hs_acquire(Handle *h, uint32_t tid) {
    switch (h->kind) {
    case HK_MUTEX:
        if (h->lock_count == 0) {
            h->owner = tid;
            h->lock_count = 1;
        } else {
            h->lock_count++;
        }
        break;
    case HK_SEMAPHORE:
        if (h->avail > 0) h->avail--;
        break;
    case HK_EVENT:
    case HK_TIMER:
        if (h->reset_type != 1) h->signaled = 0;   /* OneShot/Pulse */
        break;
    case HK_THREAD:
        break;                                     /* bleibt signalisiert */
    default:
        h->signaled = 0;                           /* HLE-Event: OneShot */
        break;
    }
}

static void hs_signal(Handle *h) {
    h->signaled = 1;
}

/* Abgelaufene Timer signalisieren (Azahar Timer::Signal). Wird aus den
 * Warteschleifen und dem VBlank-Thread gepollt. */
static void kernel_poll_timers(void) {
    uint64_t now = host_us();
    for (int i = 1; i < MAX_HANDLES; i++) {
        Handle *h = &g_handles[i];
        if (!h->used || h->kind != HK_TIMER || !h->timer_active) continue;
        if (now < h->timer_due) continue;
        h->signaled = 1;
        if (h->timer_interval != 0) h->timer_due = now + h->timer_interval;
        else h->timer_active = 0;
    }
}

/* Wartet auf ein oder mehrere Objekte. Rueckgabe: 1 = erfolgreich,
 * 0 = Timeout/Fehler (result gesetzt). Genau ein Gast-Thread laeuft; das
 * Warten gibt den Gast-Lock ueber guest_yield() ab, damit Signalisierer
 * laufen koennen (kooperatives Modell des Ports). */
static int hs_wait(ArmCpu *c, Handle **objs, int cnt, int wait_all,
                   uint64_t timeout_ns, int *out_idx, uint32_t *result) {
    uint32_t tid = cur_tid(c);
    int infinite = (timeout_ns == UINT64_MAX);
    uint64_t timeout_us = infinite ? 0 : ((timeout_ns + 999ull) / 1000ull);
    uint64_t start = host_us();

    for (;;) {
        int hit = -1;
        kernel_poll_timers();
        if (wait_all) {
            int ok = 1;
            for (int k = 0; k < cnt; k++) {
                if (hs_should_wait(objs[k], tid)) { ok = 0; break; }
            }
            if (ok) {
                for (int k = 0; k < cnt; k++) hs_acquire(objs[k], tid);
                *out_idx = -1;
                *result = 0;
                return 1;
            }
        } else {
            for (int k = 0; k < cnt; k++) {
                if (!hs_should_wait(objs[k], tid)) { hit = k; break; }
            }
            if (hit >= 0) {
                hs_acquire(objs[hit], tid);
                *out_idx = hit;
                *result = 0;
                return 1;
            }
        }
        if (!infinite && timeout_us == 0) {
            *out_idx = -1;
            *result = RESULT_TIMEOUT;
            return 0;
        }
        if (!infinite && (host_us() - start) >= timeout_us) {
            *out_idx = -1;
            *result = RESULT_TIMEOUT;
            return 0;
        }
        guest_yield();
    }
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

/* Beginnt eine Antwort. Layout wie Azahar IPC::RequestBuilder:
 *   Wort 0 : Header (wird erst in ipc_finish geschrieben)
 *   Wort 1 : Result
 *   danach : Normal-Parameter
 *   zuletzt: Translate-Parameter (Puffer-/Handle-Deskriptoren)
 * ipc_begin() reserviert nur das Header-Wort; das erste ipc_put() ist
 * deshalb bereits der Result-Slot.  `normal`/`translate` sind Sollwerte
 * (normal inkl. Result, translate inkl. Deskriptor-Woerter). */
static void ipc_begin(Ipc *r, uint32_t cmd, int normal, int translate) {
    r->cmd = cmd;
    r->normal = normal;
    r->translate = translate;
    r->written = 1;
    r->buf[0] = 0;
}

static void ipc_finish(Ipc *r) {
    /* Header aus den tatsaechlich geschriebenen Woertern ableiten, damit
       Header und Nutzdaten nie auseinanderlaufen. */
    int normal = r->written - 1 - r->translate;
    if (normal < 0) normal = 0;
    if (normal != r->normal && g_log) {
        static unsigned char warned[4096];
        unsigned key = (uint32_t)((r->cmd & 0xFFu) << 4) ^ (uint32_t)((r->cmd >> 8) & 0xFu);
        if (key < sizeof(warned) && !warned[key]) {
            warned[key] = 1;
            printf("[ipc] LAYOUT cmd=0x%04X: Header normal=%d, geschrieben=%d (translate=%d)\n",
                   r->cmd, normal, r->normal, r->translate);
            fflush(stdout);
        }
    }
    r->buf[0] = ((r->cmd & 0xFFFFu) << 16) | ((uint32_t)(normal & 0x3F) << 6)
                | (uint32_t)(r->translate & 0x3F);
}

static void put_result(Ipc *r) {
    ipc_put(r, 0);      /* ResultSuccess */
}

static void generic_command(Ipc *r, int service);

/* ---------- Services ---------- */

static void srv_command(Ipc *r, uint32_t *p) {
    switch (r->cmd) {
        case 1: { /* RegisterClient: Result (Azahar MakeBuilder(1,0)) */
            ipc_begin(r, 1, 1, 0);
            put_result(r);
            break;
        }
        case 2: { /* EnableNotification: Result, CopyHandle(Semaphore) */
            uint32_t h = new_semaphore_handle("srv:notif", 0, 2);
            if (g_log) printf("[srv] EnableNotification -> %u\n", h);
            ipc_begin(r, 2, 1, 2);
            put_result(r);
            ipc_put_handle_copy(r, h);
            break;
        }
        case 3: { /* RegisterService(name[8], name_len, max_sessions): Result, MoveHandle */
            char name[9];
            memcpy(name, (const char *)p, 8);
            name[8] = 0;
            int h = alloc_handle(SVC_OTHER, name);
            if (g_log) printf("[srv] RegisterService \"%s\"\n", name);
            ipc_begin(r, 3, 1, 2);
            put_result(r);
            ipc_put_handle(r, (uint32_t)h);
            break;
        }
        case 4: { /* UnregisterService: Result */
            ipc_begin(r, 4, 1, 0);
            put_result(r);
            break;
        }
        case 5: { /* GetServiceHandle(name[8], name_len, flags): Result, MoveHandle */
            char name[9];
            memcpy(name, (const char *)p, 8);
            name[8] = 0;
            uint32_t name_len = p[2];
            if (name_len < 8) name[name_len] = 0;
            int svc = service_from_name(name);
            int h = alloc_handle(svc, name);
            if (svc == SVC_GSP_GPU) g_hle_gsp_handle = h;
            if (g_log || getenv("HLE_SRV_NAMES")) {
                printf("[srvname] \"%s\" srvh=%u blk=%08X lr=%08X desc=%08X -> %d (%s)\n",
                       name, g_cur_handle, arm_last_block, g_cpu ? g_cpu->r[14] : 0u,
                       0x003D91E8u, h, SERVICE_NAMES[svc]);
            }
            ipc_begin(r, 5, 1, 2);
            put_result(r);
            ipc_put_handle(r, (uint32_t)h);
            break;
        }
        default:
            generic_command(r, SVC_SRV);
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


static int g_apt_wakeup_sent = 0;

static void apt_command(Ipc *r, uint32_t *p) {
    switch (r->cmd) {
        case 1: { /* GetLockHandle: Result, AppletAttributes, State, CopyHandle(Lock) */
            uint32_t attrs = (r->normal >= 1) ? p[0] : 0u;
            uint32_t lock = new_mutex_handle("apt-lock", 0);
            /* Azahar (apt.cpp): MakeBuilder(3,2), Push(ResultSuccess,
               corrected_attributes, state), PushCopyObjects(lock). */
            ipc_begin(r, 1, 3, 2);
            put_result(r);
            ipc_put(r, attrs);      /* corrected AppletAttributes */
            ipc_put(r, 0u);         /* state */
            ipc_put_handle_copy(r, lock);
            break;
        }
        case 2: { /* Initialize: Result, CopyHandle(Notification), CopyHandle(Parameter) */
            uint32_t ev1 = new_event_handle("apt-notif", 0);
            uint32_t ev2 = new_event_handle("apt-param", 0);
            g_apt_param_ev = ev2;
            /* Azahar: MakeBuilder(1,3), PushCopyObjects(notif, param). */
            ipc_begin(r, 2, 1, 3);
            put_result(r);
            ipc_put(r, (1u << 26));   /* CopyHandle-Deskriptor, 2 Handles */
            ipc_put(r, ev1);
            ipc_put(r, ev2);
            break;
        }
        case 3: case 4: case 0x28: { /* Enable / Finalize / CloseLibraryApplet */
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            break;
        }
        case 5: { /* GetAppletManInfo: Result + 4 AppletIDs */
            ipc_begin(r, 5, 5, 0);
            put_result(r);
            ipc_put(r, 1);         /* active applet position */
            ipc_put(r, 0x101);     /* requested applet id */
            ipc_put(r, 0x101);     /* home menu applet id */
            ipc_put(r, 0x100);     /* active applet id */
            break;
        }
        case 9: { /* IsRegistered: Result + bool */
            ipc_begin(r, 9, 2, 0);
            put_result(r);
            ipc_put(r, 1);
            break;
        }
        case 0x0B: { /* InquireNotification: Result + SignalType */
            ipc_begin(r, 0x0B, 2, 0);
            put_result(r);
            ipc_put(r, 0);
            break;
        }
        case 0x0D: case 0x0E: { /* ReceiveParameter / GlanceParameter */
            /* Azahar (apt.cpp): MakeBuilder(4,4)
               Push(ResultSuccess, sender_id, signal, size),
               PushMoveObjects(object), PushStaticBuffer(buffer, 0). */
            uint32_t dsc_flags = 0, dsc_addr = 0;
            if (r->translate >= 2) {
                dsc_flags = p[r->normal];
                dsc_addr = p[r->normal + 1];
            }
            /* Hardware-Semantik: GlanceParameter (0x000E) konsumiert den
               Parameter NICHT, ReceiveParameter (0x000D) schon. Der Wakeup
               wird erst zugestellt, wenn die Main-Loop bereits pollt, danach
               NoData. */
            {
                static unsigned rx_calls = 0;
                unsigned wake_at = 1u;
                rx_calls++;
                if (!g_apt_wakeup_sent && rx_calls >= wake_at) {
                    if (r->cmd == 0x000D) g_apt_wakeup_sent = 1;
                    uint32_t obj = (uint32_t)alloc_handle(SVC_OTHER, "apt-parobj");
                    Handle *oh = get_handle(obj);
                    if (oh != NULL) oh->signaled = 1;
                    ipc_begin(r, r->cmd, 4, 4);
                    put_result(r);
                    ipc_put(r, 0u);         /* sender id */
                    ipc_put(r, 1u);         /* Signal: Wakeup */
                    ipc_put(r, 0);          /* Parameter-Groesse */
                    ipc_put_handle(r, obj); /* MoveHandle(Parameter-Objekt) */
                    ipc_put(r, (2u | (0u << 10) | (0u << 14)));  /* StaticBuffer, 0 Byte */
                    ipc_put(r, dsc_addr);
                    if (g_apt_param_ev != 0) {
                        Handle *ph = get_handle(g_apt_param_ev);
                        if (ph != NULL) ph->signaled = 1;
                    }
                    if (g_log) printf("[apt] ReceiveParameter -> Wakeup (#%u)\n", rx_calls);
                } else {
                    ipc_begin(r, r->cmd, 1, 0);
                    /* Result(NoData=1007, Applet=51, InvalidState=5, Status=25) */
                    ipc_put(r, 0xC8A303EFu);
                }
            }
            break;
        }
        case 0x43: { /* NotifyToWait: Result */
            ipc_begin(r, 0x43, 1, 0);
            put_result(r);
            break;
        }
        case 0x4B: { /* AppletUtility: Result, UtilityResult, StaticBuffer(out) */
            uint32_t out_size = (r->normal >= 3) ? p[2] : 0u;
            uint32_t dsc_flags = (r->translate >= 2) ? p[r->normal] : 2u;
            uint32_t dsc_addr = (r->translate >= 2) ? p[r->normal + 1] : 0u;
            (void)dsc_flags;
            if (dsc_addr != 0u && out_size != 0u && g_cpu != NULL) {
                uint32_t off = dsc_addr & 0x1FFFFFFFu;
                if (off + out_size <= g_cpu->mem_size) memset(g_cpu->mem + off, 0, out_size);
            }
            ipc_begin(r, 0x4B, 2, 2);
            put_result(r);
            ipc_put(r, 0);           /* Utility-Funktionsergebnis */
            ipc_put(r, dsc_flags);   /* StaticBuffer-Deskriptor */
            ipc_put(r, dsc_addr);
            break;
        }
        case 0x4F: { /* SetAppCpuTimeLimit: Result */
            ipc_begin(r, 0x4F, 1, 0);
            put_result(r);
            break;
        }
        case 0x50: { /* GetAppCpuTimeLimit: Result + Limit */
            ipc_begin(r, 0x50, 2, 0);
            put_result(r);
            ipc_put(r, 0x1Eu);      /* 30 % */
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
        case 0x000A: { /* GetIPCHandles: Result, CopyHandle(SharedMem + 5 Events) */
            /* Azahar (hid.cpp): MakeBuilder(1,7), PushCopyObjects(shared_mem,
               event_pad_or_touch_1, event_pad_or_touch_2, event_accelerometer,
               event_gyroscope, event_debug_pad). */
            uint32_t shmem = (uint32_t)alloc_handle(SVC_OTHER, "hid-shmem");
            uint32_t h[5];
            for (int i = 0; i < 5; i++) h[i] = new_event_handle("hid-ev", 0);
            ipc_begin(rp, 0x000A, 1, 7);
            put_result(rp);
            ipc_put(rp, (5u << 26));       /* CopyHandle-Deskriptor, 6 Handles */
            ipc_put(rp, shmem);
            for (int i = 0; i < 5; i++) ipc_put(rp, h[i]);
            break;
        }
        case 0x0011: case 0x0012: case 0x0013: case 0x0014: { /* Accelerometer/Gyro enable/disable */
            ipc_begin(rp, rp->cmd, 1, 0);
            put_result(rp);
            break;
        }
        case 0x0015: { /* GetGyroscopeLowRawToDpsCoefficient: Result + f32 */
            ipc_begin(rp, 0x0015, 2, 0);
            put_result(rp);
            ipc_put(rp, 0);        /* 0.0f */
            break;
        }
        case 0x0016: { /* GetGyroscopeLowCalibrateParam: Result + 5 Woerter */
            ipc_begin(rp, 0x0016, 6, 0);
            put_result(rp);
            for (int i = 0; i < 5; i++) ipc_put(rp, 0);
            break;
        }
        case 0x0017: { /* GetSoundVolume: Result + u8 */
            ipc_begin(rp, 0x0017, 2, 0);
            put_result(rp);
            ipc_put(rp, 0x3F);     /* volle Lautstaerke */
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
            /* Azahar (fs_user.cpp): MakeBuilder(1,2)
               Push(file_res.Code()), PushMoveObjects(file). */
            ipc_begin(r, r->cmd, 1, 2);
            ipc_put(r, 0);          /* ResultSuccess */
            ipc_put_handle(r, h);   /* MoveHandle(file) */
            break;
        }
        case 0x0861: case 0x0862: { /* InitializeWithSdkVersion / SetPriority: Result */
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            break;
        }
        case 0x0863: { /* GetPriority: Result + Priority */
            ipc_begin(r, 0x0863, 2, 0);
            put_result(r);
            ipc_put(r, 0);          /* priority (noch nicht gesetzt) */
            break;
        }
        case 0x080C: { /* OpenArchive: Result + u64 ArchiveHandle */
            char path[256];
            find_path(p, path, sizeof(path));
            uint32_t h = (uint32_t)alloc_handle(SVC_FS_USER, "archive");
            if (g_log) printf("[fs] OpenArchive id=0x%08X type=%u size=%u path=\"%s\" -> handle %u\n",
                              p[0], p[1], p[2], path, h);
            /* Azahar: MakeBuilder(3,0), Push(result), PushRaw<u64>(handle). */
            ipc_begin(r, 0x080C, 3, 0);
            put_result(r);
            ipc_put(r, h);          /* handle low  */
            ipc_put(r, 0);          /* handle high */
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
        case 0x0813: case 0x0821: { /* GetCardType / CardSlotIsInserted: Result + bool */
            ipc_begin(r, r->cmd, 2, 0);
            put_result(r);
            ipc_put(r, 0);
            break;
        }
        case 0x0817: case 0x0818: { /* IsSdmcDetected / IsSdmcWriteable: Result + bool */
            ipc_begin(r, r->cmd, 2, 0);
            put_result(r);
            ipc_put(r, 1);          /* virtuelle SD vorhanden/schreibbar */
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
            /* Azahar: MakeBuilder(2,2), Result + Grosse + MappedBuffer-Echo */
            ipc_begin(r, r->cmd, 2, 2);
            put_result(r);
            ipc_put(r, 0);          /* gelesene/geschriebene Bytes */
            for (int i = 0; i < 2; i++) ipc_put(r, (r->translate >= 2) ? p[r->normal + i] : 0u);
        } else if (r->cmd == 0x0804) {
            ipc_begin(r, r->cmd, 3, 0);
            put_result(r);
            ipc_put(r, 0);
            ipc_put(r, 0);
        } else {
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
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
                    if (f->path != NULL && strstr(f->path, "romfs_image.bin") != NULL) {
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
            if (r->cmd == 0x080A) { /* SetPriority: Result */
                ipc_begin(r, 0x080A, 1, 0);
                put_result(r);
                break;
            }
            if (r->cmd == 0x080B) { /* GetPriority: Result + Priority */
                ipc_begin(r, 0x080B, 2, 0);
                put_result(r);
                ipc_put(r, 0);
                break;
            }
            generic_command(r, SVC_FILE);
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
            /* Azahar (cfg.cpp): MakeBuilder(1,2), Push(result), PushMappedBuffer. */
            ipc_begin(r, 0x0001, 1, 2);
            put_result(r);
            for (int i = 0; i < 2; i++) ipc_put(r, (r->translate >= 2) ? p[r->normal + i] : 0u);
            break;
        }
        case 0x0002: { /* GetRegion: Result + Region-Code */
            ipc_begin(r, 0x0002, 2, 0);
            put_result(r);
            ipc_put(r, 2);      /* EUR */
            break;
        }
        case 0x0003: { /* GetTransferableId(salt): Result + u64 */
            /* Azahar: MakeBuilder(3,0), Push(result, low, high). */
            ipc_begin(r, 0x0003, 3, 0);
            put_result(r);
            ipc_put(r, 0x12345678u);    /* low  */
            ipc_put(r, 0x00000000u);    /* high */
            if (g_log) printf("[cfg] GetTransferableId salt=0x%X -> ok\n", p[0]);
            break;
        }
        case 0x0005: { /* GetSystemModel: Result + Model */
            ipc_begin(r, 0x0005, 2, 0);
            put_result(r);
            ipc_put(r, 0);      /* CTR (Old3DS) */
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
        case 0x0001:   /* EnterExclusiveState: Result */
        case 0x0002:   /* LeaveExclusiveState */
        case 0x0004:   /* LockState */
        case 0x0005:   /* UnlockState */
        case 0x0006:   /* SuspendDaemons */
        case 0x0007:   /* ResumeDaemons */
        case 0x0008:   /* SuspendScheduler */
        case 0x0009:   /* ResumeScheduler */
        case 0x0010:   /* SetScanInterval */
        case 0x0012:   /* SetRetryInterval */
        case 0x0014:   /* OverrideDefaultDaemons */
        case 0x0015:   /* ResetDefaultDaemons */
        case 0x0017: { /* ClearHalfAwakeMacFilter */
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            break;
        }
        case 0x0003:   /* QueryExclusiveMode: Result + State */
        case 0x000D:   /* QueryStatus: Result + Status */
        case 0x0011:   /* GetScanInterval: Result + Interval */
        case 0x0013:   /* GetRetryInterval: Result + Interval */
        case 0x0016: { /* GetDefaultDaemons: Result + Mask */
            ipc_begin(r, r->cmd, 2, 0);
            put_result(r);
            ipc_put(r, 0);
            break;
        }
        case 0x000E:   /* GetDaemonDisableCount: Result + 2 Counts */
        case 0x000F: { /* GetSchedulerDisableCount: Result + 2 Counts */
            ipc_begin(r, r->cmd, 3, 0);
            put_result(r);
            ipc_put(r, 0);
            ipc_put(r, 0);
            break;
        }
        default:
            generic_command(r, SVC_NDM);
            break;
    }
}

static void boss_command(Ipc *r, uint32_t *p) {
    (void)p;
    switch (r->cmd) {
        case 0x0001: { /* InitializeSession: Result */
            ipc_begin(r, 0x0001, 1, 0);
            put_result(r);
            break;
        }
        case 0x0006: { /* RegisterPrivateClientCert: Result + 2 MappedBuffer-Echos */
            /* Azahar (boss.cpp): MakeBuilder(1,4),
               PushMappedBuffer(buffer1), PushMappedBuffer(buffer2). */
            ipc_begin(r, 0x0006, 1, 4);
            put_result(r);
            for (int i = 0; i < 4; i++) ipc_put(r, (r->translate > i) ? p[r->normal + i] : 0u);
            break;
        }
        case 0x000A: { /* GetOptoutFlag: Result + Flag */
            ipc_begin(r, 0x000A, 2, 0);
            put_result(r);
            ipc_put(r, 0);      /* output_flag = 0 */
            break;
        }
        default:
            generic_command(r, SVC_BOSS);
            break;
    }
}

static void ptm_command(Ipc *r, uint32_t *p) {
    switch (r->cmd) {
        case 0x0005:   /* GetAdapterState: Result + bool */
        case 0x0006:   /* GetShellState: Result + bool */
        case 0x0007:   /* GetBatteryLevel: Result + Level */
        case 0x0008:   /* GetBatteryChargeState: Result + bool */
        case 0x0009: { /* GetPedometerState: Result + bool */
            ipc_begin(r, r->cmd, 2, 0);
            put_result(r);
            ipc_put(r, 0);
            break;
        }
        case 0x000B: { /* GetStepHistory: Result + StaticBuffer-Echo */
            /* Azahar (ptm.cpp): MakeBuilder(1,2), PushStaticBuffer. */
            ipc_begin(r, 0x000B, 1, 2);
            put_result(r);
            for (int i = 0; i < 2; i++) ipc_put(r, (r->translate >= 2) ? p[r->normal + i] : 0u);
            break;
        }
        case 0x000C: { /* GetTotalStepCount: Result + StepCount */
            ipc_begin(r, 0x000C, 2, 0);
            put_result(r);
            ipc_put(r, 0);      /* StepCount */
            break;
        }
        default:
            generic_command(r, SVC_PTM_U);
            break;
    }
}

static void dsp_command(Ipc *r, uint32_t *p) {
    /* Antwortlayouts nach Azahar src/core/hle/service/dsp/dsp_dsp.cpp. */
    switch (r->cmd) {
        case 0x0001: { /* RecvData: Result + Wert */
            ipc_begin(r, 0x0001, 2, 0);
            put_result(r);
            ipc_put(r, 1);
            break;
        }
        case 0x0002: { /* RecvDataIsReady: Result + bool */
            ipc_begin(r, 0x0002, 2, 0);
            put_result(r);
            ipc_put(r, 1);
            break;
        }
        case 0x0007:   /* SetSemaphore: Result */
        case 0x0017: { /* SetSemaphoreMask: Result */
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            break;
        }
        case 0x000C: { /* ConvertProcessAddressFromDspDram: Result + ARM-Adresse */
            uint32_t dsp_addr = p[0];
            uint32_t arm = 0x00408800u + (dsp_addr & 0xFFu) * 0x10u;
            ipc_begin(r, 0x000C, 2, 0);
            put_result(r);
            ipc_put(r, arm);
            if (g_log) printf("[dsp] ConvertProcessAddress 0x%X -> 0x%X\n", dsp_addr, arm);
            break;
        }
        case 0x000D: { /* WriteProcessPipe: Result */
            uint32_t size = p[1];
            uint32_t addr = 0;
            if (r->translate >= 2) addr = p[r->normal + 1];
            ipc_begin(r, 0x000D, 1, 0);
            put_result(r);
            if (g_log) printf("[dsp] WriteProcessPipe ch=%u size=%u addr=%08X\n",
                              p[0], size, addr);
            break;
        }
        case 0x000E: { /* ReadPipe: Result + StaticBuffer-Echo */
            uint32_t dsc_flags = 0, dsc_addr = 0;
            if (r->translate >= 2) {
                dsc_flags = p[r->normal];
                dsc_addr = p[r->normal + 1];
            }
            ipc_begin(r, 0x000E, 1, 2);
            put_result(r);
            ipc_put(r, dsc_flags);
            ipc_put(r, dsc_addr);
            if (g_log) printf("[dsp] ReadPipe ch=%u size=%u -> 0\n", p[0], p[1]);
            break;
        }
        case 0x000F: { /* GetPipeReadableSize: Result + u16 */
            ipc_begin(r, 0x000F, 2, 0);
            put_result(r);
            ipc_put(r, 0);
            break;
        }
        case 0x0010: { /* ReadPipeIfPossible: Result + Groesse + StaticBuffer-Echo */
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
            ipc_begin(r, 0x0010, 2, 2);
            put_result(r);
            ipc_put(r, 2);          /* gelesene Bytes */
            ipc_put(r, dsc_flags);
            ipc_put(r, dsc_addr);
            if (g_log) printf("[dsp] ReadPipeIfPossible ch=%u size=%u -> 2\n", p[0], p[2]);
            break;
        }
        case 0x0011: { /* LoadComponent: Result + bool + MappedBuffer-Echo */
            uint32_t dsc_flags = 0, dsc_addr = 0;
            if (r->translate >= 2) {
                dsc_flags = p[r->normal];
                dsc_addr = p[r->normal + 1];
            }
            ipc_begin(r, 0x0011, 2, 2);
            put_result(r);
            ipc_put(r, 1);          /* true = geladen */
            ipc_put(r, dsc_flags);
            ipc_put(r, dsc_addr);
            if (g_log) printf("[dsp] LoadComponent size=0x%X -> ok\n", p[0]);
            break;
        }
        case 0x0012: { /* UnloadComponent: Result */
            ipc_begin(r, 0x0012, 1, 0);
            put_result(r);
            break;
        }
        case 0x0013:   /* FlushDataCache */
        case 0x0014:   /* InvalidateDataCache */
        case 0x0015: { /* RegisterInterruptEvents: Result (kein Handle) */
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            break;
        }
        case 0x0016: { /* GetSemaphoreEventHandle: Result + CopyHandle(Event) */
            uint32_t ev = new_event_handle("dsp-ev", 0);
            Handle *hh = get_handle(ev);
            if (hh) hh->signaled = 1;
            ipc_begin(r, 0x0016, 1, 2);
            put_result(r);
            ipc_put_handle(r, ev);
            if (g_log) printf("[dsp] GetSemaphoreEventHandle -> %u\n", ev);
            break;
        }
        case 0x001F: { /* GetHeadphoneStatus: Result + bool */
            ipc_begin(r, 0x001F, 2, 0);
            put_result(r);
            ipc_put(r, 0);
            break;
        }
        case 0x0020: { /* ForceHeadphoneOut: Result */
            ipc_begin(r, 0x0020, 1, 0);
            put_result(r);
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

/* ---------- GSP-Interrupts und GX-Kommandos ----------
 * Layout nach 3dbrew "GSP Shared Memory" und Azahar (gsp_gpu.cpp,
 * gsp_command.h, gsp_interrupt.h). Das GSP-Shared-Memory wird vom Spiel
 * per svcMapMemoryBlock auf 0x10002000 gemappt (Groesse 0x1000):
 *   +0x000 + tid*0x40  InterruptRelayQueue (max. 4 Threads)
 *   +0x200 + tid*0x80  FrameBufferUpdate je Screen (0x40 pro Screen)
 *   +0x800 + tid*0x200 CommandBuffer (Header + 15 Kommandos)
 * Interrupt-IDs: PSC0=0, PSC1=1, PDC0=2, PDC1=3, PPF=4, P3D=5, DMA=6. */
#define GSP_SHMEM_BASE      0x10002000u
#define GSP_RELAY_SLOTS     0x34
#define GSP_RELAY_PDC_LIMIT 0x20

enum {
    GSP_IRQ_PSC0 = 0,
    GSP_IRQ_PSC1 = 1,
    GSP_IRQ_PDC0 = 2,
    GSP_IRQ_PDC1 = 3,
    GSP_IRQ_PPF  = 4,
    GSP_IRQ_P3D  = 5,
    GSP_IRQ_DMA  = 6,
};

/* Schuetzt die InterruptRelayQueue gegen gleichzeitige Zugriffe von
 * VBlank-Thread und GSP-IPC. Statisch initialisiert, keine Init noetig. */
static SRWLOCK g_gsp_srw = SRWLOCK_INIT;
static volatile int g_gsp_registered = 0;
static volatile int g_gsp_thread_id = 0;
static volatile int g_gsp_right_thread = -1;

static uint32_t gsp_rd32(ArmCpu *c, uint32_t off) {
    uint32_t v = 0;
    off &= 0x1FFFFFFFu;
    if (off + 4u <= c->mem_size) memcpy(&v, c->mem + off, 4);
    return v;
}

static void gsp_wr32(ArmCpu *c, uint32_t off, uint32_t v) {
    off &= 0x1FFFFFFFu;
    if (off + 4u <= c->mem_size) memcpy(c->mem + off, &v, 4);
}

/* Reiht einen Interrupt in die RelayQueue des registrierten Threads ein und
 * signalisiert dessen Event (Azahar SignalInterruptForThread /
 * ProcessPendingInterruptImpl). PDC0/PDC1 werden ab 0x20 Eintraegen nicht
 * mehr eingereiht, sondern als "missed" gezaehlt; andere Interrupts setzen
 * ab 0x34 den Fehlercode. */
static void gsp_signal_interrupt(int id) {
    ArmCpu *c = g_cpu;
    int tid = g_gsp_thread_id;
    uint32_t base;
    uint8_t *q;
    Handle *evh;

    if (!g_gsp_registered || c == NULL) return;
    base = (GSP_SHMEM_BASE + (uint32_t)tid * 0x40u) & 0x1FFFFFFFu;
    if (base + 0x40u > c->mem_size) return;
    q = c->mem + base;

    AcquireSRWLockExclusive(&g_gsp_srw);
    if (id == GSP_IRQ_PDC0 || id == GSP_IRQ_PDC1) {
        if ((q[3] & 1u) != 0u) {  /* Client ignoriert PDC */
            ReleaseSRWLockExclusive(&g_gsp_srw);
            return;
        }
        if (q[1] >= GSP_RELAY_PDC_LIMIT) {
            uint32_t mo = base + (id == GSP_IRQ_PDC0 ? 4u : 8u);
            gsp_wr32(c, mo, gsp_rd32(c, mo) + 1u);
            ReleaseSRWLockExclusive(&g_gsp_srw);
            return;
        }
    } else if (q[1] >= GSP_RELAY_SLOTS) {
        q[2] = 1u;  /* queue_full_error */
        ReleaseSRWLockExclusive(&g_gsp_srw);
        return;
    }
    q[0xC + ((q[0] + q[1]) % GSP_RELAY_SLOTS)] = (uint8_t)id;
    q[1] = (uint8_t)(q[1] + 1u);
    ReleaseSRWLockExclusive(&g_gsp_srw);
    MemoryBarrier();

    evh = get_handle(g_gsp_irq_handle);
    if (evh != NULL) evh->signaled = 1;
}


/* GSP wendet beim VBlank die vom Spiel im Shared-Memory gesetzte
 * FrameBufferInfo an (3dbrew "Framebuffer Info", Azahar
 * GSP_GPU::SignalInterrupt -> GetFrameBufferInfo + GPU::SetBufferSwap).
 * dirty-Flag im Update-Header: Bit0; Info-Eintrag = index & 1. */
static void gsp_vblank_fb_update(ArmCpu *c, int tid) {
    int screen;
    if (c == NULL) return;
    for (screen = 0; screen < 2; screen++) {
        uint32_t base = (GSP_SHMEM_BASE + 0x200u +
                         (uint32_t)(2 * tid + screen) * 0x40u) & 0x1FFFFFFFu;
        uint32_t info, idx, left = 0, right = 0, st = 0, fmt = 0, shown = 0;
        uint8_t *fbu;
        if (base + 0x40u > c->mem_size) return;
        fbu = c->mem + base;
        if ((fbu[1] & 1u) == 0u) continue;   /* nicht aktualisiert */
        idx = fbu[0] & 1u;
        info = base + 8u + idx * 0x20u;
        memcpy(&left, c->mem + info + 4u, 4);
        memcpy(&right, c->mem + info + 8u, 4);
        memcpy(&st, c->mem + info + 0xCu, 4);
        memcpy(&fmt, c->mem + info + 0x10u, 4);
        memcpy(&shown, c->mem + info + 0x14u, 4);
        fbu[1] = 0;
        (void)right; (void)shown;
        if (left == 0u || st == 0u) continue;
        g_gsp_fb_addr[screen] = left;
        g_gsp_fb_stride[screen] = st;
        g_gsp_fb_format[screen] = fmt;
        gpu_set_fb_size(left, (screen == 1) ? 320 : 400, 240);
        g_gsp_last_screen = screen;
        printf("[gsp] SetBufferSwap screen=%d fb=0x%08X stride=0x%X fmt=%X (VBlank)\n",
               screen, left, st, fmt);
    }
}

/* Verarbeitet den GSP-Shared-Memory-Kommandopuffer (Azahar-Layout) und
   fuehrt ProcessCommandList-Eintraege (id 0x01) als PICA-Liste aus. */
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





/* --- Pixelformate und Speichertransfers (Azahar renderer_software/
 * sw_blitter.cpp, video_core/utils.h) --- */
typedef struct { uint8_t r, g, b, a; } GspColor;

static int gsp_bpp(uint32_t fmt) {
    switch (fmt & 7u) {
        case 0: return 4;                 /* RGBA8 */
        case 1: return 3;                 /* RGB8  */
        case 2: case 3: case 4: return 2; /* RGB565, RGB5A1, RGBA4 */
        default: return 0;
    }
}

static uint8_t gsp_5to8(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static uint8_t gsp_6to8(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
static uint8_t gsp_4to8(uint32_t v) { return (uint8_t)((v << 4) | v); }

static GspColor gsp_decode_pixel(uint32_t fmt, const uint8_t *p) {
    GspColor col = {0, 0, 0, 0};
    switch (fmt & 7u) {
        case 0: /* RGBA8 */
            col.r = p[3]; col.g = p[2]; col.b = p[1]; col.a = p[0];
            break;
        case 1: /* BGR8 */
            col.r = p[2]; col.g = p[1]; col.b = p[0]; col.a = 255;
            break;
        case 2: { /* RGB565 */
            uint32_t v; memcpy(&v, p, 2);
            col.r = gsp_5to8((v >> 11) & 0x1Fu);
            col.g = gsp_6to8((v >> 5) & 0x3Fu);
            col.b = gsp_5to8(v & 0x1Fu); col.a = 255;
            break;
        }
        case 3: { /* RGB5A1 */
            uint32_t v; memcpy(&v, p, 2);
            col.r = gsp_5to8((v >> 11) & 0x1Fu);
            col.g = gsp_5to8((v >> 6) & 0x1Fu);
            col.b = gsp_5to8((v >> 1) & 0x1Fu);
            col.a = (uint8_t)((v & 1u) * 255u);
            break;
        }
        case 4: { /* RGBA4 */
            uint32_t v; memcpy(&v, p, 2);
            col.r = gsp_4to8((v >> 12) & 0xFu);
            col.g = gsp_4to8((v >> 8) & 0xFu);
            col.b = gsp_4to8((v >> 4) & 0xFu);
            col.a = gsp_4to8(v & 0xFu);
            break;
        }
        default: break;
    }
    return col;
}

static void gsp_encode_pixel(uint32_t fmt, uint8_t *p, GspColor col) {
    switch (fmt & 7u) {
        case 0:
            p[3] = col.r; p[2] = col.g; p[1] = col.b; p[0] = col.a;
            break;
        case 1:
            p[2] = col.r; p[1] = col.g; p[0] = col.b;
            break;
        case 2: {
            uint32_t v = ((uint32_t)(col.r >> 3) << 11) |
                         ((uint32_t)(col.g >> 2) << 5) | (col.b >> 3);
            memcpy(p, &v, 2);
            break;
        }
        case 3: {
            uint32_t v = ((uint32_t)(col.r >> 3) << 11) |
                         ((uint32_t)(col.g >> 3) << 6) |
                         ((uint32_t)(col.b >> 3) << 1) | (col.a >> 7);
            memcpy(p, &v, 2);
            break;
        }
        case 4: {
            uint32_t v = ((uint32_t)(col.r >> 4) << 12) |
                         ((uint32_t)(col.g >> 4) << 8) |
                         ((uint32_t)(col.b >> 4) << 4) | (col.a >> 4);
            memcpy(p, &v, 2);
            break;
        }
        default: break;
    }
}

/* 8x8-Morton-Offset (Z-Order) wie Azahar VideoCore::GetMortonOffset. */
static uint32_t gsp_morton_offset(uint32_t x, uint32_t y, uint32_t bpp) {
    static const uint32_t xlut[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    static const uint32_t ylut[8] = {0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A};
    uint32_t i = xlut[x & 7u] + ylut[y & 7u];
    return (i + (x & ~7u) * 8u) * bpp;
}

/* Command 0x02: MemoryFill. control = Control0 | (Control1 << 16), Bits
 * 0 Trigger, 8 = 24 Bit, 9 = 32 Bit (Azahar MemoryFillConfig). */
static void gsp_gx_memory_fill(ArmCpu *c, const uint32_t *w) {
    const uint32_t start1 = w[1], value1 = w[2], end1 = w[3];
    const uint32_t start2 = w[4], value2 = w[5], end2 = w[6];
    const uint32_t ctl_a = w[7] & 0xFFFFu, ctl_b = (w[7] >> 16) & 0xFFFFu;
    const int both = (start1 != 0u) && (start2 != 0u);
    int i;
    for (i = 0; i < 2; i++) {
        uint32_t start = i ? start2 : start1;
        uint32_t value = i ? value2 : value1;
        uint32_t end = i ? end2 : end1;
        uint32_t control = i ? ctl_b : ctl_a;
        uint32_t so, eo, off;
        if (start == 0u) continue;
        if ((control & 1u) == 0u) continue;   /* Trigger nicht gesetzt */
        so = start & 0x1FFFFFFFu;
        eo = end & 0x1FFFFFFFu;
        if (so >= eo || eo > c->mem_size) {
            printf("[gspq] MemoryFill ungueltig: %08X..%08X\n", start, end);
            continue;
        }
        if (control & 0x200u) {
            for (off = so; off + 4u <= eo; off += 4u) memcpy(c->mem + off, &value, 4);
        } else if (control & 0x100u) {
            for (off = so; off + 3u <= eo; off += 3u) {
                c->mem[off] = (uint8_t)(value & 0xFFu);
                c->mem[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
                c->mem[off + 2] = (uint8_t)((value >> 16) & 0xFFu);
            }
        } else {
            uint16_t v16 = (uint16_t)(value & 0xFFFFu);
            for (off = so; off + 2u <= eo; off += 2u) memcpy(c->mem + off, &v16, 2);
        }
    }
    /* PSC0 fuer Puffer 0; sind beide Puffer gesetzt, meldet nur Puffer 1
     * PSC0 (Azahar GPU::Execute / gpu.cpp). Puffer 1 allein meldet PSC1. */
    if (both || start1 != 0u) {
        gsp_signal_interrupt(GSP_IRQ_PSC0);
    } else if (start2 != 0u) {
        gsp_signal_interrupt(GSP_IRQ_PSC1);
    }
}
/* Command 0x03: DisplayTransfer (Azahar SwBlitter::DisplayTransfer):
 * Pixelformat-Konvertierung und Umsetzung zwischen linearer und gekachelter
 * (8x8-Morton) Anordnung. Flags: Bit0 flip, Bit1 input_linear,
 * Bit2 crop_input_lines, Bit5 dont_swizzle, Bits8..10 input_format,
 * Bits12..14 output_format, Bits24..25 scaling. */
static void gsp_gx_display_transfer(ArmCpu *c, const uint32_t *w) {
    const uint32_t in_va = w[1], out_va = w[2];
    const uint32_t in_size = w[3], out_size = w[4], flags = w[5];
    const uint32_t in_w = in_size & 0xFFFFu, in_h = (in_size >> 16) & 0xFFFFu;
    const uint32_t out_w = out_size & 0xFFFFu, out_h = (out_size >> 16) & 0xFFFFu;
    const uint32_t in_fmt = (flags >> 8) & 7u, out_fmt = (flags >> 12) & 7u;
    const int flip = (flags & 1u) != 0u;
    const int in_linear = (flags & 2u) != 0u;
    const int crop = (flags & 4u) != 0u;
    const int dont_swizzle = (flags & 0x20u) != 0u;
    const uint32_t scaling = (flags >> 24) & 3u;
    const int in_bpp = gsp_bpp(in_fmt), out_bpp = gsp_bpp(out_fmt);
    uint32_t src, dst, hs, vs, ow, oh, y, x;

    if (in_w == 0u || in_h == 0u || out_w == 0u || out_h == 0u) return;
    if (in_bpp == 0 || out_bpp == 0) {
        printf("[gspq] DisplayTransfer Format %u/%u ungueltig\n", in_fmt, out_fmt);
        return;
    }
    if (scaling > 2u || (in_linear && scaling != 0u)) {
        printf("[gspq] DisplayTransfer Scaling %u ungueltig (linear=%d)\n", scaling, in_linear);
        return;
    }

    src = in_va & 0x1FFFFFFFu;
    dst = out_va & 0x1FFFFFFFu;
    if (flip && crop)
        dst += (in_w - out_w) * (out_h - 1u) * (uint32_t)out_bpp;
    if ((uint64_t)src + (uint64_t)in_w * in_h * (uint32_t)in_bpp > c->mem_size ||
        (uint64_t)dst + (uint64_t)out_w * out_h * (uint32_t)out_bpp > c->mem_size) {
        printf("[gspq] DisplayTransfer ausserhalb: %08X -> %08X\n", in_va, out_va);
        return;
    }

    hs = scaling != 0u ? 1u : 0u;
    vs = scaling == 2u ? 1u : 0u;
    ow = out_w >> hs;
    oh = out_h >> vs;
    if (ow == 0u || oh == 0u) return;

    for (y = 0; y < oh; y++) {
        for (x = 0; x < ow; x++) {
            const uint32_t ix = x << hs, iy = y << vs;
            const uint32_t oy = flip ? (oh - y - 1u) : y;
            uint32_t so, dof;
            GspColor col;

            if (in_linear) {
                if (!dont_swizzle) {
                    so = (ix + iy * in_w) * (uint32_t)in_bpp;
                    dof = gsp_morton_offset(x, oy, (uint32_t)out_bpp) +
                          (oy & ~7u) * ow * (uint32_t)out_bpp;
                } else {
                    so = (ix + iy * in_w) * (uint32_t)in_bpp;
                    dof = (x + oy * ow) * (uint32_t)out_bpp;
                }
            } else {
                if (!dont_swizzle) {
                    so = gsp_morton_offset(ix, iy, (uint32_t)in_bpp) +
                         (iy & ~7u) * in_w * (uint32_t)in_bpp;
                    dof = (x + oy * ow) * (uint32_t)out_bpp;
                } else {
                    so = gsp_morton_offset(ix, iy, (uint32_t)in_bpp) +
                         (iy & ~7u) * in_w * (uint32_t)in_bpp;
                    dof = gsp_morton_offset(x, oy, (uint32_t)out_bpp) +
                          (oy & ~7u) * ow * (uint32_t)out_bpp;
                }
            }
            if ((uint64_t)src + so + (uint32_t)in_bpp > c->mem_size ||
                (uint64_t)dst + dof + (uint32_t)out_bpp > c->mem_size)
                continue;

            col = gsp_decode_pixel(in_fmt, c->mem + src + so);
            if (scaling == 1u) {
                GspColor p1 = gsp_decode_pixel(in_fmt, c->mem + src + so + (uint32_t)in_bpp);
                col.r = (uint8_t)((col.r + p1.r) / 2u);
                col.g = (uint8_t)((col.g + p1.g) / 2u);
                col.b = (uint8_t)((col.b + p1.b) / 2u);
                col.a = (uint8_t)((col.a + p1.a) / 2u);
            } else if (scaling == 2u) {
                const uint32_t b = (uint32_t)in_bpp;
                GspColor p1 = gsp_decode_pixel(in_fmt, c->mem + src + so + b);
                GspColor p2 = gsp_decode_pixel(in_fmt, c->mem + src + so + 2u * b);
                GspColor p3 = gsp_decode_pixel(in_fmt, c->mem + src + so + 3u * b);
                col.r = (uint8_t)((col.r + p1.r + p2.r + p3.r) / 4u);
                col.g = (uint8_t)((col.g + p1.g + p2.g + p3.g) / 4u);
                col.b = (uint8_t)((col.b + p1.b + p2.b + p3.b) / 4u);
                col.a = (uint8_t)((col.a + p1.a + p2.a + p3.a) / 4u);
            }
            gsp_encode_pixel(out_fmt, c->mem + dst + dof, col);
        }
    }
}

/* Command 0x04: TextureCopy (Azahar SwBlitter::TextureCopy): kopiert
 * 16-Byte-Zeilen mit optionalem Quell-/Ziel-Gap. */
static void gsp_gx_texture_copy(ArmCpu *c, const uint32_t *w) {
    uint32_t src = w[1] & 0x1FFFFFFFu;
    uint32_t dst = w[2] & 0x1FFFFFFFu;
    uint32_t size = w[3] & ~0xFu;   /* untere 4 Bit ignoriert */
    uint32_t in_gap = ((w[4] >> 16) & 0xFFFFu) * 16u;
    uint32_t out_gap = ((w[5] >> 16) & 0xFFFFu) * 16u;
    uint32_t in_width = (in_gap == 0u) ? size : (w[4] & 0xFFFFu) * 16u;
    uint32_t out_width = (out_gap == 0u) ? size : (w[5] & 0xFFFFu) * 16u;
    uint32_t rem = size, ri, ro;
    uint64_t need_in, need_out;

    if (size == 0u || in_width == 0u || out_width == 0u) return;
    need_in = (uint64_t)(size / in_width) * (in_width + in_gap);
    need_out = (uint64_t)(size / out_width) * (out_width + out_gap);
    if ((uint64_t)src + need_in > c->mem_size || (uint64_t)dst + need_out > c->mem_size) {
        printf("[gspq] TextureCopy ausserhalb: %08X -> %08X (%X)\n", w[1], w[2], w[3]);
        return;
    }

    ri = in_width;
    ro = out_width;
    while (rem > 0u) {
        uint32_t n = ri < ro ? ri : ro;
        if (n > rem) n = rem;
        memmove(c->mem + dst, c->mem + src, n);
        src += n; dst += n;
        ri -= n; ro -= n; rem -= n;
        if (ri == 0u) { ri = in_width; src += in_gap; }
        if (ro == 0u) { ro = out_width; dst += out_gap; }
    }
}

/* Verarbeitet den GX-Kommandopuffer des Threads mit GPU-Recht
 * (Azahar GSP_GPU::TriggerCmdReqQueue). Header: Index u8, Anzahl u8,
 * Status u8, ShouldStop u8; Kommandos ab +0x20 je 0x20 Byte. */
static int gsp_process_cmdqueue(ArmCpu *c) {
    int qops = 0;
    int tid = g_gsp_right_thread >= 0 ? g_gsp_right_thread : g_gsp_thread_id;
    uint32_t base = (GSP_SHMEM_BASE + 0x800u + (uint32_t)tid * 0x200u) & 0x1FFFFFFFu;
    uint32_t guard;

    if (c == NULL || base + 0x200u > c->mem_size) return 0;

    for (guard = 0; guard < 0x40u; guard++) {
        uint32_t hdr = gsp_rd32(c, base);
        uint32_t n = (hdr >> 8) & 0xFFu;
        uint32_t idx = hdr & 0xFFu;
        uint32_t status = (hdr >> 16) & 0xFFu;
        uint32_t stop_req = (hdr >> 24) & 0xFFu;
        uint32_t cb, w[8], id, i;
        uint32_t newhdr;
        int failed = 0;
        int stop_cmd = 0;

        if (n == 0u) break;
        if (status == 1u) break;                 /* STATUS_STOPPED */
        if (stop_req != 0u) {                    /* should_stop */
            hdr = (hdr & 0xFF00FFFFu) | (1u << 16);
            gsp_wr32(c, base, hdr);
            break;
        }

        cb = base + 0x20u + idx * 0x20u;
        if (cb + 0x20u > c->mem_size) break;
        for (i = 0; i < 8u; i++) w[i] = gsp_rd32(c, cb + i * 4u);

        newhdr = (((idx + 1u) % 0xFu) & 0xFFu) |
                 (((n - 1u) & 0xFFu) << 8) |
                 (status << 16) | (stop_req << 24);
        gsp_wr32(c, base, newhdr);

        id = w[0] & 0xFFu;
        stop_cmd = ((w[0] >> 16) & 0xFFu) != 0u;
        switch (id) {
            case 0x00: { /* RequestDma: Quelle -> Ziel, danach DMA-Interrupt */
                uint32_t s0 = w[1] & 0x1FFFFFFFu;
                uint32_t s1 = w[2] & 0x1FFFFFFFu;
                uint32_t sz = w[3];
                if (sz != 0u && sz <= 0x08000000u &&
                    (uint64_t)s0 + sz <= c->mem_size &&
                    (uint64_t)s1 + sz <= c->mem_size) {
                    memmove(c->mem + s1, c->mem + s0, sz);
                    printf("[gspq] RequestDma %08X -> %08X (%u)\n", w[1], w[2], sz);
                } else {
                    printf("[gspq] RequestDma ungueltig %08X -> %08X (%u)\n", w[1], w[2], sz);
                }
                gsp_signal_interrupt(GSP_IRQ_DMA);
                break;
            }
            case 0x01: { /* ProcessCommandList */
                uint32_t addr = w[1], size = w[2];
                if (addr != 0u && size != 0u &&
                    (uint64_t)(addr & 0x1FFFFFFFu) + size <= c->mem_size) {
                    int nops = gpu_run_pica_list(c, addr, size);
                    qops += nops;
                    printf("[gspq] ProcessCommandList list=%08X size=%X ops=%d\n",
                           addr, size, nops);
                } else {
                    printf("[gspq] ProcessCommandList ungueltig %08X/%X\n", addr, size);
                }
                gsp_signal_interrupt(GSP_IRQ_P3D);
                break;
            }
            case 0x02: /* MemoryFill */
                gsp_gx_memory_fill(c, w);
                break;
            case 0x03: /* DisplayTransfer */
                gsp_gx_display_transfer(c, w);
                gsp_signal_interrupt(GSP_IRQ_PPF);
                break;
            case 0x04: /* TextureCopy */
                gsp_gx_texture_copy(c, w);
                gsp_signal_interrupt(GSP_IRQ_PPF);
                break;
            case 0x05: /* FlushCacheRegions: kein CPU-Cache im Port */
                printf("[gspq] FlushCacheRegions %08X/%X %08X/%X %08X/%X\n",
                       w[1], w[2], w[3], w[4], w[5], w[6]);
                break;
            default:
                printf("[gspq] GX-Befehl 0x%02X unbekannt\n", id);
                failed = 1;
                break;
        }

        if (stop_cmd || failed) {
            newhdr = gsp_rd32(c, base) & 0xFF00FFFFu;
            newhdr |= (failed ? 0x80u : 1u) << 16;
            gsp_wr32(c, base, newhdr);
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
        case 0x0004: { /* ReadHWRegs: Result + StaticBuffer-Echo */
            /* Azahar (gsp_gpu.cpp): MakeBuilder(1,2), Push(result),
               PushStaticBuffer(buffer, 0). */
            uint32_t dsc_flags = (r->translate >= 2) ? p[r->normal] : 2u;
            uint32_t dsc_addr = (r->translate >= 2) ? p[r->normal + 1] : 0u;
            uint32_t size = (r->normal >= 2) ? p[1] : 0u;
            if (dsc_addr != 0u && size != 0u && g_cpu != NULL) {
                uint32_t off = dsc_addr & 0x1FFFFFFFu;
                if ((size > 0x80u ? 0x80u : size) + off <= g_cpu->mem_size)
                    memset(g_cpu->mem + off, 0, size > 0x80u ? 0x80u : size);
            }
            ipc_begin(r, 0x0004, 1, 2);
            put_result(r);
            ipc_put(r, dsc_flags);
            ipc_put(r, dsc_addr);
            break;
        }
        case 0x0005: { /* SetBufferSwap(screen_id, FrameBufferInfo) */
            /* Azahar: screen_id < 2 -> GPU().SetBufferSwap(screen_id, fb_info),
               sonst ignoriert; Antwort ist immer ein Result. */
            uint32_t screen = p[0];
            uint32_t left = (r->normal >= 3) ? p[2] : 0u;
            uint32_t st = (r->normal >= 5) ? p[4] : 0u;
            uint32_t fmt = (r->normal >= 6) ? p[5] : 0u;
            if (screen < 2u) {
                if (left == 0u) {
                    /* Noch keine FB-Adresse im Request: aus der
                       FrameBufferUpdate-Struktur im GSP-Shared-Memory lesen. */
                    uint32_t a = 0, s2 = 0, f2 = 0;
                    if (gsp_fb_info(g_cpu, (int)screen, &a, &s2, &f2)) {
                        left = a; st = s2; fmt = f2;
                    } else {
                        left = g_gsp_fb_addr[screen];
                    }
                }
                g_gsp_fb_addr[screen] = left;
                g_gsp_fb_stride[screen] = st;
                g_gsp_fb_format[screen] = fmt;
                gpu_set_fb_size(left, (screen == 1u) ? 320 : 400, 240);
                g_gsp_last_screen = (int)screen;
                if (g_log) printf("[gsp] SetBufferSwap screen=%u fb=0x%08X stride=0x%X fmt=%X\n",
                                  screen, left, st, fmt);
            } else if (g_log) {
                printf("[gsp] SetBufferSwap screen=%u ungueltig (ignoriert)\n", screen);
            }
            ipc_begin(r, 0x0005, 1, 0);
            put_result(r);
            break;
        }
        case 0x0006: { /* SetCommandList: in dieser Firmware nur Result */
            g_cmdlist_addr = p[0];
            g_cmdlist_size = p[1];
            if (g_log) printf("[gsp] SetCommandList addr=0x%08X size=0x%X lr=%08X\n",
                              g_cmdlist_addr, g_cmdlist_size,
                              g_cpu ? g_cpu->r[14] : 0);
            ipc_begin(r, 0x0006, 1, 0);
            put_result(r);
            break;
        }
        case 0x0007: { /* RequestDma(src, dst, size, flags): Result */
            uint32_t s0 = p[0], s1 = p[1], s2 = p[2], s3 = p[3];
            if (g_log) printf("[gsp] RequestDma p0=%08X p1=%08X p2=%08X p3=%08X\n",
                              s0, s1, s2, s3);
            /* DMA wirklich ausfuehren, falls die Parameter plausibel sind. */
            if (g_cpu != NULL && s2 != 0u && s2 <= 0x00100000u &&
                s0 + s2 <= g_cpu->mem_size && s1 + s2 <= g_cpu->mem_size) {
                memmove(g_cpu->mem + s1, g_cpu->mem + s0, s2);
                if (g_log) printf("[gsp] DMA copy %08X -> %08X (%u)\n", s0, s1, s2);
            }
            ipc_begin(r, 0x0007, 1, 0);
            put_result(r);
            break;
        }
        case 0x0008: case 0x0009: case 0x000A:
        case 0x000B: { /* Cache / Events / LcdForceBlack */
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            break;
        }
        case 0x000C: { /* TriggerCmdReqQueue -> Shared-Memory-Kommandopuffer */
            /* Azahar (gsp_gpu.cpp): base = shmem + 0x800 + thread_id*0x200,
               Header {index:8, number_commands:8, status:8, should_stop:8},
               Command[15] ab +0x20 (je 0x20 B). id 0x01 = SubmitCmdList
               (Adresse +0x04, Laenge +0x08). */
            int qops = gsp_process_cmdqueue(g_cpu);
            if (qops == 0 && g_cmdlist_addr != 0 && g_cmdlist_size != 0 && g_cpu != NULL) {
                int nops = gpu_run_pica_list(g_cpu, g_cmdlist_addr, g_cmdlist_size);
                if (g_log) printf("[gsp] TriggerCmdReqQueue list=%08X size=%X -> %d Ops\n",
                                  g_cmdlist_addr, g_cmdlist_size, nops);
            }
            ipc_begin(r, 0x000C, 1, 0);
            put_result(r);
            break;
        }
        case 0x000D: case 0x000E: case 0x000F: case 0x0010:
        case 0x0011:
        case 0x0019: case 0x001A: case 0x001B: case 0x001C:
        case 0x001D: case 0x001E: case 0x001F: { /* Transfer / Vram / Cache */
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            break;
        }
        case 0x0014: { /* UnregisterInterruptRelayQueue */
            g_gsp_registered = 0;
            ipc_begin(r, 0x0014, 1, 0);
            put_result(r);
            break;
        }
        case 0x0015: { /* TryAcquireRight: Recht geben, falls frei */
            if (g_gsp_right_thread < 0) g_gsp_right_thread = g_gsp_thread_id;
            ipc_begin(r, 0x0015, 1, 0);
            put_result(r);
            break;
        }
        case 0x0016: { /* AcquireRight */
            g_gsp_right_thread = g_gsp_thread_id;
            ipc_begin(r, 0x0016, 1, 0);
            put_result(r);
            break;
        }
        case 0x0017: { /* ReleaseRight */
            g_gsp_right_thread = -1;
            ipc_begin(r, 0x0017, 1, 0);
            put_result(r);
            break;
        }
        case 0x0012: { /* GetPerfLog: Result + 14 Woerter */
            ipc_begin(r, 0x0012, 15, 0);
            put_result(r);
            for (int i = 0; i < 14; i++) ipc_put(r, 0);
            break;
        }
        case 0x0013: { /* RegisterInterruptRelayQueue: Result, ThreadID, CopyHandle(ShMem) */
            static int gsp_relay_first = 1;
            /* Der Client uebergibt sein Interrupt-Event als CopyHandle im
               Translate-Bereich; dieses Handle wird bei Interrupts signalisiert. */
            uint32_t ev = (r->translate >= 2) ? p[r->normal + 1] : 0u;
            uint32_t relay_off;
            if (get_handle(ev) == NULL) {
                ev = new_event_handle("gsp-ev", 0);
            }
            g_gsp_irq_handle = ev;
            g_gsp_thread_id = 0;
            /* Azahar setzt die RelayQueue des Threads zurueck. */
            relay_off = (GSP_SHMEM_BASE + (uint32_t)g_gsp_thread_id * 0x40u) & 0x1FFFFFFFu;
            if (g_cpu != NULL && relay_off + 0x40u <= g_cpu->mem_size)
                memset(g_cpu->mem + relay_off, 0, 0x40);
            g_gsp_registered = 1;

            uint32_t shm = (uint32_t)alloc_handle(SVC_OTHER, "gsp-shmem");
            /* Azahar: MakeBuilder(2,2); Push(FirstInitialization/ResultSuccess),
               Push(thread_id), PushCopyObjects(shared_memory).
               ResultFirstInitialization = GX-Modul(10), Desc 519 -> 0x00002A07. */
            ipc_begin(r, 0x0013, 2, 2);
            ipc_put(r, gsp_relay_first ? 0x00002A07u : 0u);
            gsp_relay_first = 0;
            ipc_put(r, (uint32_t)g_gsp_thread_id);   /* Thread-Index im Shmem */
            ipc_put_handle_copy(r, shm);
            if (g_log) printf("[gsp] RegisterInterruptRelayQueue -> ev=%u shm=%u tid=%d\n",
                                    ev, shm, g_gsp_thread_id);
            break;
        }
        case 0x0018: { /* ImportDisplayCaptureInfo: Result + 2 x 4 Woerter */
            /* Azahar: MakeBuilder(9,0), PushRaw(top_entry), PushRaw(bottom_entry). */
            ipc_begin(r, 0x0018, 9, 0);
            put_result(r);
            ipc_put(r, g_gsp_fb_addr[0]); ipc_put(r, 0); ipc_put(r, 0); ipc_put(r, 0);
            ipc_put(r, g_gsp_fb_addr[1]); ipc_put(r, 0); ipc_put(r, 0); ipc_put(r, 0);
            break;
        }
        default:
            generic_command(r, SVC_GSP_GPU);
            break;
    }
}

/* Y2R: die meisten Kommandos sind Setter (1 Result). Wichtig ist
 * 0x000F GetTransferEndEvent -> Event-Handle, sonst wartet das Spiel auf
 * Handle 0 und kommt nie zur Grafik-Initialisierung.
 * Layouts nach Azahar src/core/hle/service/cam/y2r_u.cpp. */
static void y2r_command(Ipc *r, uint32_t *p) {
    switch (r->cmd) {
        case 0x0002: case 0x0004: case 0x0006: case 0x0008:
        case 0x000A: case 0x000C: case 0x000E: { /* Get*: Result + Wert */
            ipc_begin(r, r->cmd, 2, 0);
            put_result(r);
            ipc_put(r, 0);
            break;
        }
        case 0x000D: { /* SetTransferEndInterrupt: Result */
            ipc_begin(r, 0x000D, 1, 0);
            put_result(r);
            break;
        }
        case 0x000F: { /* GetTransferEndEvent: Result + CopyHandle(Event) */
            uint32_t ev = new_event_handle("y2r-ev", 0);
            Handle *hh = get_handle(ev);
            if (hh) hh->signaled = 1;
            ipc_begin(r, 0x000F, 1, 2);
            put_result(r);
            ipc_put_handle_copy(r, ev);
            if (g_log) printf("[y2r] GetTransferEndEvent -> %u\n", ev);
            break;
        }
        case 0x0014: case 0x0015: case 0x0016: case 0x0017:
        case 0x0019: { /* IsFinished*: Result + bool(true) */
            ipc_begin(r, r->cmd, 2, 0);
            put_result(r);
            ipc_put(r, 1);
            break;
        }
        default: {
            /* Setter (Result) bzw. nicht implementierte Kommandos. */
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            if (g_log) printf("[y2r] cmd 0x%04X -> 0\n", r->cmd);
            break;
        }
    }
}

static void generic_command(Ipc *r, int service) {
    if (g_log) {
        printf("[%s] cmd 0x%04X normal=%d -> 0\n",
               SERVICE_NAMES[service], r->cmd, r->normal);
    }
    /* Azahar ServiceFrameworkBase::ReportUnimplementedFunction: Header mit
       derselben Kommandonummer, normal_params_size = 1, Result = 0. */
    ipc_begin(r, r->cmd, 1, 0);
    put_result(r);
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
    if ((h != NULL || g_log) && (g_ipc_count <= g_ipc_log_limit || g_log)) {
        printf("[ipc] th=%03X blk=%08X %-9s cmd=0x%04X n=%d t=%d h=%u%s",
               (uint32_t)((g_cpu ? g_cpu->tpidruro : 0) >> 12) & 0xFFFu, arm_last_block,
               SERVICE_NAMES[service], cmd, normal, translate, handle,
               h ? "" : " (unbekannt)");
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
/* Warteschlange fuer svcArbitrateAddress nach Azahar
 * AddressArbiter::WaitThread/ResumeAllThreads/ResumeHighestPriorityThread.
 * Signale sammeln sich NICHT an: geweckt wird nur, wer tatsaechlich wartet. */
#define ARBQ_MAX 64
typedef struct {
    int used;
    uint32_t addr;
    uint32_t tid;
    int woken;
} ArbWaiter;
static ArbWaiter g_arbq[ARBQ_MAX];

/* Signal: value < 0 weckt alle, sonst die ersten value Waiter (FIFO statt
 * Prioritaetswahl, da der Port keine echten Thread-Prioritaeten plant). */
static int arb_resume(uint32_t addr, int32_t value) {
    int woken = 0;
    int32_t remaining = (value < 0) ? 0x7FFFFFFF : value;
    for (int i = 0; i < ARBQ_MAX && remaining > 0; i++) {
        if (!g_arbq[i].used || g_arbq[i].addr != addr || g_arbq[i].woken) continue;
        g_arbq[i].woken = 1;
        remaining--;
        woken++;
    }
    return woken;
}

static int arb_register(uint32_t addr, uint32_t tid) {
    for (int i = 0; i < ARBQ_MAX; i++) {
        if (!g_arbq[i].used) {
            g_arbq[i].used = 1;
            g_arbq[i].addr = addr;
            g_arbq[i].tid = tid;
            g_arbq[i].woken = 0;
            return i;
        }
    }
    return -1;
}

static void arb_unregister(int slot) {
    if (slot >= 0 && slot < ARBQ_MAX) {
        g_arbq[slot].used = 0;
        g_arbq[slot].woken = 0;
    }
}


/* Simulierter VBlank im Host-Takt (~60 Hz). Die fruehere pauschale
 * Event-Flut (wa-kernel-signal-all) ist durch echte GSP-Ereignisse ersetzt:
 * PDC0/PDC1 gehen ueber die InterruptRelayQueue an das bei
 * RegisterInterruptRelayQueue registrierte Spiel-Event. */
static DWORD WINAPI vblank_thread(LPVOID p) {
    (void)p;
    while (g_vblank_run) {
        Sleep(16);
        /* Echte VBlank-Quelle: GSP uebernimmt die FrameBufferInfo aus dem
         * Shared-Memory und meldet PDC0 (oben) / PDC1 (unten) per RelayQueue
         * und Event (Azahar GPU::VBlankCallback / GSP_GPU::SignalInterrupt). */
        gsp_vblank_fb_update(g_cpu, g_gsp_thread_id);
        gsp_signal_interrupt(GSP_IRQ_PDC0);
        gsp_signal_interrupt(GSP_IRQ_PDC1);
        kernel_poll_timers();
    }
    return 0;
}

void hle_init(ArmCpu *c, const char *romfs_dir) {
    ensure_savedata();
    g_cpu = c;
    g_main_cpu = c;
    snprintf(g_romfs, sizeof(g_romfs), "%s", romfs_dir ? romfs_dir : "out/romfs");
    g_log = getenv("HLE_LOG") != NULL;
    {
        const char *lim = getenv("HLE_IPC_LOG");
        if (lim) g_ipc_log_limit = atoi(lim);
    }
    setup_config_mem(c);
    arm_pump_hook = hle_pump;
    g_vblank_run = 1;
    CreateThread(NULL, 0, vblank_thread, NULL, 0, NULL);

    memset(g_handles, 0, sizeof(g_handles));
    g_next_handle = 1;
    g_srv_handle = (uint32_t)alloc_handle(SVC_SRV, "srv:");

    /* Rein lesende Diagnose. */
    if (getenv("ARM_WATCH"))
        arm_watch_pc = (uint32_t)strtoul(getenv("ARM_WATCH"), NULL, 16);
    if (getenv("HLE_TRACE"))
        g_trace_file = fopen("out/hle_trace.txt", "w");

    if (!g_guest_lock_init) {
        InitializeCriticalSection(&g_guest_lock);
        g_guest_lock_init = 1;
        /* Kooperatives Threading: genau ein Gast-Thread laeuft. */
        g_no_lock = getenv("HLE_NO_THREAD_LOCK") != NULL;
        g_det_sched = getenv("HLE_DET_SCHED") != NULL;
        if (g_det_sched) g_turn = 1;
    }
    printf("[hle] romfs=%s log=%d srv-handle=%u\n", g_romfs, g_log, g_srv_handle);
}

/* Der Gast-Lock wird NICHT hier (Host-Hauptthread/main) genommen, sondern im
 * game_thread vor dem Einstieg in den Gastcode (runtime/main.c). Sonst haelt
 * ein Thread den Lock, der nie Gastcode ausfuehrt, und alle per
 * svcCreateThread gestarteten Gast-Threads blockieren dauerhaft in
 * guest_lock_enter() (Befund 2026-11-24, Commit 60a3682). */
void hle_guest_enter(void) {
    guest_lock_enter();
}

void hle_guest_leave(void) {
    guest_lock_leave();
}

void hle_tick_stats(int *calls, unsigned long long *tick) {
    if (calls) *calls = g_tick_calls;
    if (tick) *tick = g_tick;
}

void hle_report(void) {
    printf("[hle] ipc=%d ticks=%llu tickcalls=%d\n", g_ipc_count, (unsigned long long)g_tick, g_tick_calls);
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

/* Thread-Ende: Thread-Handle signalisieren (Azahar Thread::Stop setzt den
 * Status Dead; ShouldWait ist dann dauerhaft falsch) und alle Mutexe
 * freigeben, die der Thread haelt (ReleaseThreadMutexes). */
static void thread_signal_exit(ArmCpu *c) {
    for (int i = 0; i < g_thread_count; i++) {
        if (!g_threads[i].used || g_threads[i].cpu != c) continue;
        uint32_t tid = (uint32_t)g_threads[i].id;
        Handle *hh = get_handle(g_threads[i].handle);
        if (hh != NULL) {
            hh->kind = HK_THREAD;
            hs_signal(hh);
        }
        for (int k = 1; k < MAX_HANDLES; k++) {
            Handle *m = &g_handles[k];
            if (m->used && m->kind == HK_MUTEX && m->lock_count > 0 && m->owner == tid) {
                m->lock_count = 0;
                m->owner = 0;
                if (getenv("HLE_THR_LOG"))
                    printf("[threxit] mutex %d freigegeben (tid=%u)\n", k, tid);
            }
        }
        if (getenv("HLE_THR_LOG"))
            printf("[threxit] id=%u handle=%u\n", tid, g_threads[i].handle);
        g_threads[i].used = 0;
        return;
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
            {
                Handle *th = get_handle(h);
                if (th != NULL) th->kind = HK_THREAD;   /* ShouldWait: lebt noch */
            }
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
        case 0x13: { /* CreateMutex(out, initial_locked) */
            uint32_t out = c->r[0];
            uint32_t h = (uint32_t)alloc_handle(SVC_OTHER, "mutex");
            Handle *hh = get_handle(h);
            if (hh != NULL) {
                hh->kind = HK_MUTEX;
                if (c->r[1] != 0) {   /* initial gesperrt */
                    hh->owner = cur_tid(c);
                    hh->lock_count = 1;
                }
            }
            if (out) write_u32(c, out, h);
            c->r[1] = h;
            return 0;
        }
        case 0x14: { /* ReleaseMutex(handle) */
            Handle *hh = get_handle(c->r[0]);
            if (hh == NULL || hh->kind != HK_MUTEX) return RESULT_INVALID_HANDLE;
            uint32_t tid = cur_tid(c);
            if (hh->lock_count <= 0 || hh->owner != tid) return RESULT_WRONG_THREAD;
            hh->lock_count--;
            if (hh->lock_count == 0) hh->owner = 0;
            return 0;
        }
        case 0x15: { /* CreateSemaphore(out, initial, max) */
            uint32_t out = c->r[0];
            int32_t initial = (int32_t)c->r[1], maxc = (int32_t)c->r[2];
            if (initial > maxc) return RESULT_OUT_OF_RANGE;
            uint32_t h = (uint32_t)alloc_handle(SVC_OTHER, "semaphore");
            Handle *hh = get_handle(h);
            if (hh != NULL) {
                hh->kind = HK_SEMAPHORE;
                hh->avail = initial;
                hh->max_count = maxc;
            }
            if (out) write_u32(c, out, h);
            c->r[1] = h;
            return 0;
        }
        case 0x16: { /* ReleaseSemaphore(out_count, handle, release_count) */
            uint32_t out = c->r[0];
            Handle *hh = get_handle(c->r[1]);
            int32_t rel = (int32_t)c->r[2];
            if (hh == NULL || hh->kind != HK_SEMAPHORE) return RESULT_INVALID_HANDLE;
            if (rel < 0 || hh->avail + rel > hh->max_count) return RESULT_OUT_OF_RANGE;
            if (out) write_u32(c, out, (uint32_t)hh->avail);
            hh->avail += rel;
            return 0;
        }
        case 0x17: { /* CreateEvent(out, reset_type) */
            uint32_t out = c->r[0];
            uint32_t h = (uint32_t)alloc_handle(SVC_OTHER, "event");
            Handle *hh = get_handle(h);
            if (hh != NULL) {
                hh->kind = HK_EVENT;
                hh->reset_type = (int)c->r[1];
                hh->signaled = 0;
            }
            if (out) write_u32(c, out, h);
            c->r[1] = h;
            return 0;
        }
        case 0x18: { /* SignalEvent(handle) */
            Handle *hh = get_handle(c->r[0]);
            if (hh == NULL || hh->kind != HK_EVENT) return RESULT_INVALID_HANDLE;
            hs_signal(hh);
            return 0;
        }
        case 0x19: { /* ClearEvent(handle) */
            Handle *hh = get_handle(c->r[0]);
            if (hh == NULL || hh->kind != HK_EVENT) return RESULT_INVALID_HANDLE;
            hh->signaled = 0;
            return 0;
        }
        case 0x1A: { /* CreateTimer(out, reset_type) */
            uint32_t out = c->r[0];
            uint32_t h = (uint32_t)alloc_handle(SVC_OTHER, "timer");
            Handle *hh = get_handle(h);
            if (hh != NULL) {
                hh->kind = HK_TIMER;
                hh->reset_type = (int)c->r[1];
                hh->signaled = 0;
                hh->timer_active = 0;
            }
            if (out) write_u32(c, out, h);
            c->r[1] = h;
            return 0;
        }
        case 0x1B: { /* SetTimer(handle, initial_ns, interval_ns) */
            Handle *hh = get_handle(c->r[0]);
            int64_t initial = (int64_t)((uint64_t)c->r[1] | ((uint64_t)c->r[2] << 32));
            int64_t interval = (int64_t)((uint64_t)c->r[3] | ((uint64_t)c->r[4] << 32));
            if (hh == NULL || hh->kind != HK_TIMER) return RESULT_INVALID_HANDLE;
            if (initial < 0 || interval < 0) return RESULT_OUT_OF_RANGE;
            hh->signaled = 0;
            hh->timer_interval = (uint64_t)interval / 1000ull;
            if (initial == 0) {
                hh->timer_active = 0;
                hh->signaled = 1;              /* sofort faellig */
            } else {
                hh->timer_due = host_us() + (uint64_t)initial / 1000ull;
                hh->timer_active = 1;
            }
            return 0;
        }
        case 0x1C: { /* ClearTimer(handle) */
            Handle *hh = get_handle(c->r[0]);
            if (hh == NULL || hh->kind != HK_TIMER) return RESULT_INVALID_HANDLE;
            hh->signaled = 0;
            hh->timer_active = 0;
            return 0;
        }
        case 0x1E: case 0x1F: case 0x20: /* MemoryBlock */
            if (getenv("HLE_MAP_LOG"))
                printf("[map] svc=%02X r0=%08X r1=%08X r2=%08X r3=%08X\n",
                       imm, c->r[0], c->r[1], c->r[2], c->r[3]);
            return 0;
        case 0x21: { /* CreateAddressArbiter(out) */
            uint32_t h = (uint32_t)alloc_handle(SVC_OTHER, "arbiter");
            Handle *hh = get_handle(h);
            if (hh != NULL) hh->kind = HK_ARBITER;
            if (c->r[0]) write_u32(c, c->r[0], h);
            c->r[1] = h;
            return 0;
        }
        case 0x22: { /* ArbitrateAddress(handle, addr, type, value, timeout) */
            static int arb_prints = 0;
            uint32_t addr = c->r[1] & ~3u;
            uint32_t type = c->r[2];
            int32_t value = (int32_t)c->r[3];
            uint64_t timeout_ns = (uint64_t)c->r[4] | ((uint64_t)c->r[5] << 32);
            uint32_t off = addr & 0x1FFFFFFFu;
            Handle *hh = get_handle(c->r[0]);
            if (hh == NULL || hh->kind != HK_ARBITER) return RESULT_INVALID_HANDLE;

            if (getenv("HLE_ARB_LOG") && arb_prints < 300) {
                int32_t adbg = 0;
                if (off + 4 <= c->mem_size) memcpy(&adbg, c->mem + off, 4);
                printf("[arb] tid=%u addr=%08X type=%u val=%d mem=%08X blk=%08X lr=%08X\n",
                       cur_tid(c), addr, type, value, (uint32_t)adbg,
                       arm_last_block, c->r[14]);
                arb_prints++;
            }
            if (off + 4 > c->mem_size) return 0;

            /* type 0 = Signal: weckt wartende Threads, blockiert nie. */
            if (type == 0) {
                arb_resume(addr, value);
                return 0;
            }
            if (type < 1 || type > 4) return 0xD8E093EDu;

            /* Warte-Typen blockieren nur, wenn [addr] < value. type 3/4
             * dekrementieren das Wort zuvor (Azahar
             * DecrementAndWaitIfLessThan). */
            {
                int32_t cur = 0;
                memcpy(&cur, c->mem + off, 4);
                if (cur >= value) return 0;
                if (type == 3 || type == 4) {
                    int32_t nv = cur - 1;
                    memcpy(c->mem + off, &nv, 4);
                }
            }
            {
                uint32_t tid = cur_tid(c);
                int slot = arb_register(addr, tid);
                uint64_t start = host_us();
                int infinite = (timeout_ns == UINT64_MAX);
                uint64_t timeout_us = infinite ? 0 : ((timeout_ns + 999ull) / 1000ull);
                if (getenv("HLE_KLOG"))
                    printf("[karb] tid=%u addr=%08X type=%u val=%d slot=%d\n",
                           tid, addr, type, value, slot);
                for (;;) {
                    if (slot < 0 || g_arbq[slot].woken) break;
                    if (!infinite && (host_us() - start) >= timeout_us) break;
                    guest_yield();
                }
                arb_unregister(slot);
            }
            /* Azahar: Timeout-Typen liefern immer ResultTimeout. */
            if (type == 2 || type == 4) return RESULT_TIMEOUT;
            return 0;
        }
        case 0x23: { /* CloseHandle(handle) */
            uint32_t h = c->r[0];
            Handle *hh = get_handle(h);
            if (hh == NULL) return RESULT_INVALID_HANDLE;
            if (hh->file >= 0 && hh->file < MAX_FILES) {
                if (g_files[hh->file].fp != NULL) fclose(g_files[hh->file].fp);
                g_files[hh->file].used = 0;
                g_files[hh->file].fp = NULL;
            }
            memset(hh, 0, sizeof(*hh));
            hh->file = -1;
            return 0;
        }
        case 0x24: { /* WaitSynchronization1(handle, timeout) */
            uint32_t h = c->r[0];
            uint64_t timeout_ns = (uint64_t)c->r[1] | ((uint64_t)c->r[2] << 32);
            Handle *hh = get_handle(h);
            if (hh == NULL) {
                if (getenv("HLE_KLOG")) printf("[kwait] WaitSync1 ungueltiges Handle %u\n", h);
                return RESULT_INVALID_HANDLE;
            }
            {
                Handle *objs[1];
                int idx = 0;
                uint32_t res = 0;
                objs[0] = hh;
                if (!hs_wait(c, objs, 1, 0, timeout_ns, &idx, &res)) return res;
            }
            return 0;
        }
        case 0x25: { /* WaitSynchronizationN(out, handles, count, wait_all, timeout) */
            static int wl = 0;
            uint32_t out = c->r[0], hlist = c->r[1], cnt = c->r[2];
            int32_t wait_all = (int32_t)c->r[3];
            uint64_t timeout_ns = (uint64_t)c->r[4] | ((uint64_t)c->r[5] << 32);
            Handle *objs[32];
            int idx = -1;
            uint32_t res = 0;

            if (getenv("HLE_WAIT_LOG") && wl < 40) {
                wl++;
                printf("[wait25] tid=%u r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X blk=%08X lr=%08X\n",
                       cur_tid(c), c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5],
                       arm_last_block, c->r[14]);
            }
            if (cnt > 32u) return RESULT_OUT_OF_RANGE;
            for (uint32_t k = 0; k < cnt; k++) {
                uint32_t hv = (hlist != 0) ? arm_mem_read(c, hlist + k * 4, 4) : 0;
                objs[k] = get_handle(hv);
                if (objs[k] == NULL) {
                    /* Echter Kernel: ResultInvalidHandle, kein Warten. */
                    if (getenv("HLE_KLOG"))
                        printf("[kwait] tid=%u WaitSyncN ungueltiges Handle %u (idx=%u/%u)\n",
                               cur_tid(c), hv, k, cnt);
                    return RESULT_INVALID_HANDLE;
                }
            }
            if (getenv("HLE_KLOG")) {
                int blocks = 0;
                for (uint32_t k = 0; k < cnt; k++)
                    if (hs_should_wait(objs[k], cur_tid(c))) blocks = 1;
                if (blocks && cnt <= 8) {
                    printf("[kblock] tid=%u n=%u timeout=%llX blk=%08X lr=%08X:", cur_tid(c), cnt,
                           (unsigned long long)timeout_ns, arm_last_block, c->r[14]);
                    for (uint32_t k = 0; k < cnt; k++)
                        printf(" %u(%s:%s)", (unsigned)(objs[k] - g_handles),
                               objs[k]->name, handle_kind_name(objs[k]));
                    printf("\n");
                }
            }
            if (!hs_wait(c, objs, (int)cnt, wait_all != 0, timeout_ns, &idx, &res)) {
                if (getenv("HLE_KLOG"))
                    printf("[kwait] tid=%u Timeout cnt=%u all=%d res=%08X\n",
                           cur_tid(c), cnt, wait_all, res);
                if (getenv("HLE_WAIT_LOG"))
                    printf("[wait25 idx] tid=%u cnt=%u -> timeout\n", cur_tid(c), cnt);
                return res;
            }
            if (getenv("HLE_WAIT_LOG"))
                printf("[wait25 idx] tid=%u out=%08X cnt=%u -> idx=%d\n",
                       cur_tid(c), out, cnt, idx);
            if (!wait_all && out != 0 && out != 0xFFFFFFFFu)
                write_u32(c, out, (uint32_t)idx);
            /* SDK-Wrapper (0x287B90) liefert den Index zusaetzlich in r1. */
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
            uint64_t now = GetTickCount64();
            if (base_ms == 0) base_ms = now;
            g_tick = (now - base_ms) * 268111ull;   /* ~268.111 MHz */
            g_tick_calls++;
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
