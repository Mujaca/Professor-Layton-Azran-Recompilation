/* HLE: SVC-Dispatch, IPC und 3DS-Services fuer den recompilierten Code.
 *
 * IPC-Header (verifiziert an Azahar src/core/hle/ipc.h):
 *   bits 0-5   : translate_params_size (Woerter)
 *   bits 6-11  : normal_params_size   (Woerter)
 *   bits 16-31 : command_id
 */
#include "hle.h"
#include "fs_archive.h"
#include "gpu.h"
#include "gsp_fb.h"
#include "frame_clock.h"
#include "dsp_hle.h"
#include "host_output.h"
#include "hid_ring.h"
#include "y2r.h"

static uint32_t g_hle_gsp_handle;
/* Von generiertem Code (gen_boot.c) lesbarer gsp-Handle. */
uint32_t g_hle_gsp_export;
/* Interrupt-Event, das der Client bei RegisterInterruptRelayQueue uebergibt. */
static uint32_t g_gsp_irq_handle;
static uint32_t g_apt_param_ev = 0;
/* Azahar AppletManager haelt lock(), notification_event und parameter_event
 * als Mitglieder: GetLockHandle/Initialize liefern bei jedem Aufruf dieselben
 * Kernel-Objekte (applet_manager.cpp). */
static uint32_t g_apt_lock_handle = 0;
static uint32_t g_apt_notif_handle = 0;
static int g_apt_initialized = 0;
static void dispatch_ipc(uint32_t handle);
static int gsp_process_cmdqueue(ArmCpu *c);
static void gsp_pica_irq(int id);
#include "arm_trace.h"

void arm_call_guest_pub(ArmCpu *c, uint32_t addr);   /* gen_boot.c */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <direct.h>

#include "kernel_sched.h"

#define TLS_CMD_BUF 0x80u

/* ---- kooperatives Threading: es laeuft immer nur ein Gast-Thread ---- */
static CRITICAL_SECTION g_guest_lock;
static int g_guest_lock_init = 0;
static ArmCpu *g_main_cpu = NULL;

/* GSP-Framebuffer-Zustand. Die Register (GspFbRegs) werden durch
 * SetBufferSwap (gsp 0x0005) und die dirty-FrameBufferInfo im GSP-Shared-
 * Memory gesetzt; die Anzeige liest den beim VBlank uebernommenen Stand
 * (g_gsp_display). Damit wechselt das Bild genau im VBlank und nicht mitten
 * im Frame (Azahar GPU::SetBufferSwap + Renderer-Latch, siehe gsp_fb.h).
 * Screen 0 = oben (400x240), Screen 1 = unten (320x240). */
static GspFbRegs g_gsp_fb_regs[2];
static HleGspFrame g_gsp_display[2];
static volatile unsigned g_gsp_guest_frames = 0;
static SRWLOCK g_gsp_fb_srw = SRWLOCK_INIT;
static int g_gsp_fb_init_done = 0;

static void gsp_fb_init(void) {
    if (g_gsp_fb_init_done) return;
    /* Startwerte wie Azahar PicaCore::InitializeRegs, Adressen im Port-VRAM. */
    gsp_fb_regs_reset(&g_gsp_fb_regs[0], 0x1F000000u, 0x1F060000u, 3u * 240u, 1u);
    gsp_fb_regs_reset(&g_gsp_fb_regs[1], 0x1F0C0000u, 0x1F120000u, 3u * 240u, 1u);
    memset(g_gsp_display, 0, sizeof(g_gsp_display));
    g_gsp_fb_init_done = 1;
}

int hle_gsp_frame(int screen, HleGspFrame *out) {
    int ok;
    if (screen < 0 || screen > 1 || out == NULL) return 0;
    AcquireSRWLockShared(&g_gsp_fb_srw);
    *out = g_gsp_display[screen];
    ok = out->valid;
    ReleaseSRWLockShared(&g_gsp_fb_srw);
    return ok;
}

unsigned hle_gsp_guest_frames(void) {
    return g_gsp_guest_frames;
}

/* Uebernimmt den sichtbaren Registerstand in die Anzeige (VBlank-Latch). */
static void gsp_fb_latch_locked(int screen) {
    gsp_fb_latch(&g_gsp_fb_regs[screen], &g_gsp_display[screen]);
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

static uint64_t host_us(void);
/* Diagnose (HLE_YSTAT): Wandzeit in den kooperativen Lock-Abgaben sowie
 * Zeit, in der der Gast-Lock zwischen zwei Gast-Abschnitten frei lag.
 * Reine Zaehler, kein Gastzustand. */
static int g_ystat = 0;
/* Diagnose (HLE_WSTAT): DSP-Zustell- und Aufwachverzoegerungen > 10 ms. */
static int g_wstat = 0;
/* Diagnose (HLE_GTIME): Wandzeit der PICA-Kommandolisten-Ausfuehrung
 * (Software-Rasterizer) gegen die Gesamtlaufzeit. Reine Zaehler. */
static int g_gtime = 0;
static unsigned long long g_gtime_us, g_gtime_lists;
static unsigned long long g_yield_n, g_yield_us, g_pump_rel_n, g_pump_rel_us;
static unsigned long long g_lock_idle_us;
static volatile unsigned long long g_lock_free_us = 0;
static unsigned g_vblank_tick = 0;

/* Wartende Gast-Threads (hs_wait, ArbitrateAddress) schlafen auf g_kcv statt
 * im Millisekundentakt um die Gast-Sperre zu pollen. Jeder Thread, der die
 * Sperre nach echter Arbeit abgibt (guest_lock_leave ausserhalb eines
 * Warte-Polls), weckt alle Warter: jede Zustandsaenderung (Event, Mutex,
 * Arbiter, IPC, Timer-Poll im VBlank, DSP/GPU-Interrupts) passiert unter der
 * Sperre und endet mit einer Abgabe. Ein Timeout-Deckel (KWAIT_SLICE_MS)
 * haelt Timer und Zeitlimits ohne weiteren Weckruf aktuell. */
static CONDITION_VARIABLE g_kcv = CONDITION_VARIABLE_INIT;
static _Thread_local int t_wait_poll = 0;
#define KWAIT_SLICE_MS 2u

static void guest_lock_enter(void) {
    if (!g_guest_lock_init || g_no_lock) return;
    if (t_lock_owned) return;
    EnterCriticalSection(&g_guest_lock);
    t_lock_owned = 1;
    if (g_ystat) {
        unsigned long long free_at = g_lock_free_us;
        if (free_at != 0ull) {
            g_lock_idle_us += host_us() - free_at;
            g_lock_free_us = 0ull;
        }
    }
}
static void guest_lock_leave(void) {
    if (!g_guest_lock_init || g_no_lock) return;
    if (!t_lock_owned) return;
    t_lock_owned = 0;
    /* Threadwechsel: der lokale Exklusiv-Monitor eines abgeloesten Threads
     * darf nicht in einen anderen Thread hineinwirken (Azahar
     * ThreadManager::SwitchContext -> cpu->ClearExclusiveState()). Wir
     * invalidieren global, weil der naechste Thread beliebig ist. */
    arm_excl_clear_all();
    LeaveCriticalSection(&g_guest_lock);
    if (!t_wait_poll) WakeAllConditionVariable(&g_kcv);
    if (g_ystat) g_lock_free_us = host_us();
}
/* Nicht blockierend (DSP-Thread): 1 = Sperre erhalten. */
static int guest_lock_try(void) {
    if (!g_guest_lock_init || g_no_lock) return 1; /* ohne Sperre: sofort zustellen */
    if (t_lock_owned) return 0;
    if (!TryEnterCriticalSection(&g_guest_lock)) return 0;
    t_lock_owned = 1;
    return 1;
}
/* GPU-Abschnitte: GSP-GX-Befehle (RequestDma, SubmitCmdList, MemoryFill,
 * DisplayTransfer, TextureCopy) laufen synchron im aufrufenden Gast-Thread
 * (gx_run_inline) unter g_gpu_cs, das alle GPU-Zugriffe serialisiert. Waehrend
 * der GPU-Arbeit wird die Gast-Sperre abgegeben, wenn ein anderer Gast-Thread
 * bereit ist, den echte Hardware jetzt laufen liesse (Sound-Thread auf dem
 * SysCore). Frueher hielt der Software-Rasterizer die Sperre durchgehend und
 * hungerte den Sound-Thread aus (Musikluecken 35..140 ms). Interrupts aus dem
 * Abschnitt werden geordnet vorgemerkt (gpu_irq_push) und unter der
 * Gast-Sperre zugestellt (gsp_flush_pending_irqs). Sperr-Reihenfolge:
 * Gast-Sperre -> g_gpu_cs; wer g_gpu_cs haelt, nimmt die Gast-Sperre nie. */
static CRITICAL_SECTION g_gpu_cs;
static int g_gpu_cs_init = 0;
/* Vorgemerkte GPU-Interrupts als geordnete FIFO (Anzahl und Reihenfolge
 * zaehlen: das Spiel erwartet z. B. je DisplayTransfer einen PPF). */
#define GPU_IRQ_Q 1024u
static uint8_t g_gpu_irq_q[GPU_IRQ_Q];
static unsigned g_gpu_irq_head = 0, g_gpu_irq_tail = 0;
static volatile LONG g_gpu_irq_pending = 0;   /* Anzahl in der FIFO */
static SRWLOCK g_gpu_irq_srw = SRWLOCK_INIT;
static void gpu_irq_push(int id) {
    AcquireSRWLockExclusive(&g_gpu_irq_srw);
    if (g_gpu_irq_head - g_gpu_irq_tail < GPU_IRQ_Q) {
        g_gpu_irq_q[g_gpu_irq_head % GPU_IRQ_Q] = (uint8_t)id;
        g_gpu_irq_head++;
        InterlockedIncrement(&g_gpu_irq_pending);
    }
    ReleaseSRWLockExclusive(&g_gpu_irq_srw);
}
static _Thread_local int t_gpu_section = 0;
static void gsp_signal_interrupt(int id);

static void gpu_cs_enter(void) { if (g_gpu_cs_init) EnterCriticalSection(&g_gpu_cs); }
static void gpu_cs_leave(void) { if (g_gpu_cs_init) LeaveCriticalSection(&g_gpu_cs); }

/* Aufrufer haelt die Gast-Sperre (oder es gibt keine). */
static void gsp_flush_pending_irqs(void) {
    if (g_gpu_irq_pending == 0) return;
    for (;;) {
        int id;
        AcquireSRWLockExclusive(&g_gpu_irq_srw);
        if (g_gpu_irq_head == g_gpu_irq_tail) {
            ReleaseSRWLockExclusive(&g_gpu_irq_srw);
            break;
        }
        id = g_gpu_irq_q[g_gpu_irq_tail % GPU_IRQ_Q];
        g_gpu_irq_tail++;
        InterlockedDecrement(&g_gpu_irq_pending);
        ReleaseSRWLockExclusive(&g_gpu_irq_srw);
        gsp_signal_interrupt(id);
    }
}

typedef struct { uint32_t w[8]; } GxOp;
static void gx_exec(const GxOp *op);

/* Synchron im aufrufenden Thread: das Spiel sieht die Fertigmeldung wie
 * bisher direkt nach dem Befehl. Die Gast-Sperre wird fuer die Dauer der
 * GPU-Arbeit nur abgegeben, wenn ein Gast-Thread bereit ist, den echte
 * Hardware jetzt laufen liesse (hoeher priorisiert, oder auf dem anderen Kern:
 * der Sound-Thread dieses Spiels laeuft auf dem SysCore, processor_id 1). Sonst bleibt sie gehalten: parallel
 * laufende Gast-Threads bremsen den Software-Rasterizer messbar (CPU). */
static uint32_t cur_tid(ArmCpu *c);
static int cur_prio(ArmCpu *c);
static int kernel_other_should_run(uint32_t tid, int prio);
static void gx_run_inline(ArmCpu *c, const uint32_t w[8]) {
    GxOp op;
    int had = 0;
    memcpy(op.w, w, sizeof(op.w));
    if (t_lock_owned) {
        audio_dsp_flush_events();
        gsp_flush_pending_irqs();
        if (kernel_other_should_run(cur_tid(c), cur_prio(c))) {
            had = 1;
            guest_lock_leave();
        }
    }
    t_gpu_section = 1;
    gpu_cs_enter();
    gx_exec(&op);
    gpu_cs_leave();
    t_gpu_section = 0;
    if (had && g_guest_lock_init && !g_no_lock) {
        EnterCriticalSection(&g_guest_lock);
        t_lock_owned = 1;
    }
    gsp_flush_pending_irqs();
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
static void hid_poll(void);
/* Y2R-Fertigmeldung (StartConversion -> TransferEnd-Event) faellig? */
static void y2r_poll(void);

/* Pro Gast-Block: CPU-gebundene Gastschleifen ohne SVC duerfen den
 * kooperativen Lock nicht dauerhaft halten. Alle 0x4000 Bloecke abgeben. */
void hle_pump(void) {
    if ((++g_pump_count & 0x3FFFu) != 0) return;
    if (t_lock_owned) { audio_dsp_flush_events(); gsp_flush_pending_irqs(); }
    hid_poll();
    y2r_poll();
    if (!g_guest_lock_init || g_no_lock) return;
    if (!t_lock_owned) return;
    {
        uint64_t t0 = g_ystat ? host_us() : 0;
        guest_lock_leave();
        Sleep(0);
        guest_lock_reacquire();
        if (g_ystat) {
            g_pump_rel_n++;
            g_pump_rel_us += host_us() - t0;
        }
    }
}

static void guest_yield(void) {
    uint64_t t0;
    if (!g_guest_lock_init) return;
    if (g_no_lock) { Sleep(0); return; }
    if (!t_lock_owned) { Sleep(0); return; }
    t0 = g_ystat ? host_us() : 0;
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
    audio_dsp_flush_events();
    gsp_flush_pending_irqs();
    if (g_ystat) {
        g_yield_n++;
        g_yield_us += host_us() - t0;
    }
}

static void gsp_flush_pending_irqs(void);
/* Warte-Poll: Sperre abgeben und schlafen, bis ein anderer Thread nach
 * echter Arbeit abgibt (g_kcv) oder KWAIT_SLICE_MS vergeht. Ohne Sperre bzw.
 * im deterministischen Modus wie bisher guest_yield(). */
static void guest_wait_change(void) {
    if (!g_guest_lock_init || g_no_lock || g_det_sched || !t_lock_owned) {
        guest_yield();
        return;
    }
    arm_excl_clear_all();
    t_wait_poll = 1;
    SleepConditionVariableCS(&g_kcv, &g_guest_lock, KWAIT_SLICE_MS);
    t_wait_poll = 0;
    /* Sperre ist wieder gehalten (t_lock_owned blieb 1). */
    audio_dsp_flush_events();
    gsp_flush_pending_irqs();
}


typedef struct {
    int used;
    ArmCpu *cpu;
    uint32_t entry;
    int id;
    int prio;          /* Gast-Prioritaet aus svcCreateThread (kleiner = hoeher) */
    int proc_id;       /* svcCreateThread processor_id (0 AppCore, 1 SysCore) */
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
    int fixed_size;  /* ExtSaveData: Groesse bleibt fest (Azahar FixSizeDiskFile) */
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
    HK_ARBITER,
    HK_PROCESS
} HandleKind;

typedef struct {
    int used;
    int service;     /* ServiceId */
    int file;        /* Index in g_files oder -1 */
    int dir;         /* Directory-Stream (fs_archive.c) oder -1 */
    int alias;       /* >0: Slot verweist auf dieses kanonische Objekt */
    int refs;        /* kanonisch: lebende Handle-Slots (Azahar HandleTable) */
    volatile int signaled;   /* Event/Timer/Thread: Signalzustand */
    int kind;                /* HandleKind */
    uint32_t owner;          /* Mutex: Thread-Id des Besitzers */
    int lock_count;          /* Mutex: rekursive Sperren */
    int avail;               /* Semaphore: freie Plaetze */
    int max_count;           /* Semaphore: Maximum */
    int reset_type;          /* Event/Timer: 0 OneShot, 1 Sticky, 2 Pulse */
    uint64_t timer_due;      /* Timer: faellig (Host-ns) */
    uint64_t sig_us;         /* Diagnose HLE_WSTAT: Zeitpunkt der DSP-Signalisierung */
    uint64_t timer_interval; /* Timer: Wiederholung (Host-ns), 0 = einmalig */
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
static uint64_t g_boot_ns = 0;   /* Nullpunkt fuer svcGetSystemTick */
static int g_tick_calls = 0;
static int g_ticklog_n = 0;
static int g_sleeplog_n = 0;
static int g_waittmo_n = 0;
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
    if (strcmp(name, "hid:SPVR") == 0) return SVC_HID_USER;   /* gleiches Modul */
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
            g_handles[i].dir = -1;
            g_handles[i].alias = 0;
            g_handles[i].refs = 1;
            snprintf(g_handles[i].name, sizeof(g_handles[i].name), "%s", name ? name : "?");
            if (getenv("HLE_HDL_LOG")) printf("[hdl] %d = %s\n", i, g_handles[i].name);
            return i;
        }
    }
    return 0;
}

/* Pseudo-Handles des Kernels (Azahar kernel/handle_table.h):
 * 0xFFFF8000 = aktueller Thread, 0xFFFF8001 = aktueller Prozess. Sie werden
 * als permanente Slots abgebildet, damit DuplicateHandle sie teilen kann. */
static uint32_t g_pseudo_thread = 0;
static uint32_t g_pseudo_process = 0;

static Handle *get_handle(uint32_t h) {
    if (h == 0xFFFF8000u || h == 0xFFFF8001u) {
        uint32_t *slot = (h == 0xFFFF8000u) ? &g_pseudo_thread : &g_pseudo_process;
        if (*slot == 0) {
            *slot = (uint32_t)alloc_handle(SVC_OTHER,
                                           (h == 0xFFFF8000u) ? "cur-thread" : "cur-proc");
            if (*slot == 0) return NULL;
            g_handles[*slot].kind = (h == 0xFFFF8000u) ? HK_THREAD : HK_PROCESS;
            g_handles[*slot].signaled = 1;
        }
        return &g_handles[*slot];
    }
    if (h == 0 || h >= (uint32_t)MAX_HANDLES) return NULL;
    if (!g_handles[h].used) return NULL;
    /* DuplicateHandle legt in Azahar (kernel/handle_table.cpp Duplicate) einen
       zweiten Slot auf dasselbe Kernel-Objekt. Der Alias forwardet deshalb
       auf den kanonischen Slot. */
    if (g_handles[h].alias != 0) {
        uint32_t canon = (uint32_t)g_handles[h].alias;
        if (canon >= (uint32_t)MAX_HANDLES || !g_handles[canon].used) return NULL;
        return &g_handles[canon];
    }
    return &g_handles[h];
}

/* Kanonischen Slot eines Handle-Werts liefern (ohne Alias-Aufloesung). */
static int canonical_slot(uint32_t h) {
    if (h == 0xFFFF8000u || h == 0xFFFF8001u) {
        Handle *ph = get_handle(h);
        return ph ? (int)(ph - g_handles) : -1;
    }
    if (h == 0 || h >= (uint32_t)MAX_HANDLES) return -1;
    if (!g_handles[h].used) return -1;
    if (g_handles[h].alias != 0) return g_handles[h].alias;
    return (int)h;
}

/* Gibt einen Handle-Slot frei. Das Objekt (Datei, Event-Zustand, Mutex ...)
   wird erst zerstoert, wenn kein anderer Slot mehr darauf verweist
   (Azahar HandleTable::Close). */
static void close_handle_slot(uint32_t h) {
    Handle *s;
    if (h == 0 || h >= (uint32_t)MAX_HANDLES) return;
    s = &g_handles[h];
    if (!s->used) return;
    if (s->alias != 0) {
        int canon = s->alias;
        memset(s, 0, sizeof(*s));
        s->file = -1;
        s->dir = -1;
        if (canon > 0 && canon < MAX_HANDLES && g_handles[canon].used &&
            g_handles[canon].refs > 1)
            g_handles[canon].refs--;
        return;
    }
    if (s->refs > 1) {
        /* Ein Alias wird zum neuen kanonischen Slot; die uebrigen Aliase
           werden umgebogen (Azahar: das Objekt bleibt am Leben). */
        int p = -1;
        for (int i = 1; i < MAX_HANDLES; i++) {
            if (g_handles[i].used && g_handles[i].alias == (int)h) { p = i; break; }
        }
        if (p > 0) {
            int refs = s->refs - 1;
            g_handles[p] = *s;
            g_handles[p].alias = 0;
            g_handles[p].refs = refs;
            for (int i = 1; i < MAX_HANDLES; i++) {
                if (i != p && g_handles[i].used && g_handles[i].alias == (int)h)
                    g_handles[i].alias = p;
            }
            memset(s, 0, sizeof(*s));
            s->file = -1;
            s->dir = -1;
            return;
        }
    }
    if (s->file >= 0 && s->file < MAX_FILES) {
        if (g_files[s->file].fp != NULL) fclose(g_files[s->file].fp);
        g_files[s->file].used = 0;
        g_files[s->file].fp = NULL;
    }
    if (s->dir >= 0) {
        fs_archive_dir_close(s->dir);
        s->dir = -1;
    }
    memset(s, 0, sizeof(*s));
    s->file = -1;
    s->dir = -1;
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
#define RESULT_NOT_AUTHORIZED 0xD9001BEAu

static uint64_t host_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    /* Zerlegung, damit t * 1e9 nicht ueberlaeuft (QPC-Takt ~10 MHz). */
    return (uint64_t)(t.QuadPart / freq.QuadPart) * 1000000000ull +
           (uint64_t)(t.QuadPart % freq.QuadPart) * 1000000000ull /
               (uint64_t)freq.QuadPart;
}

static uint64_t host_us(void) {
    return host_ns() / 1000ull;
}

/* Systemticks wie svcGetSystemTick: ARM11-Kern 268.111856 MHz
 * (Azahar core_timing.h BASE_CLOCK_RATE_ARM11). QPC liefert die noetige
 * Aufloesung; GetTickCount64 (~15,6 ms) ist fuer HID-Zeitstempel zu grob. */
#define SYSCLOCK_ARM11 268111856ull
/* Gemeinsame Zeitbasis mit svcGetSystemTick (Nullpunkt g_boot_ns). */
static uint64_t host_system_ticks(void) {
    return ks_tick_from_ns(host_ns() - g_boot_ns);
}

/* Thread-Id des aktuell laufenden Gast-Threads (0 = Hauptthread). */
static uint32_t cur_tid(ArmCpu *c) {
    if (c == g_main_cpu) return 0;
    for (int i = 0; i < 32; i++) {
        if (g_threads[i].used && g_threads[i].cpu == c) return (uint32_t)g_threads[i].id;
    }
    return 0;
}

/* Hauptthread: Prioritaet aus dem ExHeader (ARM11 SystemLocalCaps.priority,
 * ExHeader+0x20F dieses Titels = 0x30; Azahar ThreadPrioDefault = 48, das ist
 * zugleich der Wert den Azahar fuer fehlende ExHeader einsetzt). Ideal-
 * Prozessor laut ExHeader-Flags0 Bits0..1 = Core 0 (AppCore). */
static int g_main_prio = (int)KS_PRIO_DEFAULT;
static int g_main_proc = 0;

/* Gast-Prioritaet des aktuell laufenden Threads. Hauptthread und unbekannte
 * CPUs laufen mit der Prioritaet des Hauptthreads. */
static int cur_prio(ArmCpu *c) {
    if (c == g_main_cpu) return g_main_prio;
    for (int i = 0; i < 32; i++) {
        if (g_threads[i].used && g_threads[i].cpu == c) return g_threads[i].prio;
    }
    return g_main_prio;
}

/* Prozessor-Id (0 = AppCore, 1 = SysCore) des aktuell laufenden Threads.
 * Der Port fuehrt alle Gast-Threads auf einem Host-Thread aus; die Id ist
 * deshalb nur informativ (svcGetCurrentProcessorNumber/GetThreadIdealProcessor)
 * und wird nicht zur Auswahl eines zweiten Kerns benutzt. */
static int cur_proc(ArmCpu *c) {
    if (c == g_main_cpu) return g_main_proc;
    for (int i = 0; i < 32; i++) {
        if (g_threads[i].used && g_threads[i].cpu == c) return g_threads[i].proc_id;
    }
    return g_main_proc;
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

/* Warteliste fuer WaitSynchronization (Azahar kernel/wait_object.cpp
 * AddWaitingThread/GetHighestPriorityReadyThread). Jeder wartende Thread
 * traegt seine Handle-Liste ein. Ein freigegebenes Objekt wird dem
 * hoechstprioren bereiten Waiter zugesprochen (kleinster Prioritaetswert);
 * bei Gleichstand gewinnt der zuerst eingetragene, also der laenger
 * wartende Thread. Das ersetzt das fruehere "wer zuerst nachsieht, gewinnt"
 * des kooperativen Ports. */
#define KWAIT_MAX 64
#define KWAIT_MAX_OBJS 32
typedef struct {
    volatile int used;
    int tid;
    int prio;
    int wait_all;
    int nobj;
    uint32_t seq;            /* Eintragsreihenfolge */
    int objs[KWAIT_MAX_OBJS];  /* kanonische Handle-Slots */
} KernelWaiter;
static KernelWaiter g_kwait[KWAIT_MAX];
static uint32_t g_kwait_seq = 0;

static int kwait_register(uint32_t tid, int prio, int wait_all, const Handle **objs, int cnt) {
    for (int i = 0; i < KWAIT_MAX; i++) {
        if (!g_kwait[i].used) {
            g_kwait[i].tid = (int)tid;
            g_kwait[i].prio = prio;
            g_kwait[i].wait_all = wait_all;
            g_kwait[i].nobj = cnt > KWAIT_MAX_OBJS ? KWAIT_MAX_OBJS : cnt;
            g_kwait[i].seq = ++g_kwait_seq;
            for (int k = 0; k < g_kwait[i].nobj; k++)
                g_kwait[i].objs[k] = (int)(objs[k] - g_handles);
            g_kwait[i].used = 1;   /* zuletzt veroeffentlichen */
            return i;
        }
    }
    return -1;
}

static void kwait_unregister(int slot) {
    if (slot >= 0 && slot < KWAIT_MAX) g_kwait[slot].used = 0;
}

static void kwait_unregister_tid(uint32_t tid) {
    for (int i = 0; i < KWAIT_MAX; i++)
        if (g_kwait[i].used && (uint32_t)g_kwait[i].tid == tid) g_kwait[i].used = 0;
}

/* Ist der Wunsch dieses Waiters jetzt erfuellbar? (Azahar ShouldWait fuer
 * jedes Objekt; bei wait_all muessen alle bereit sein.) */
static int kwait_ready(const KernelWaiter *w) {
    if (w->wait_all) {
        for (int k = 0; k < w->nobj; k++)
            if (hs_should_wait(&g_handles[w->objs[k]], (uint32_t)w->tid)) return 0;
        return 1;
    }
    for (int k = 0; k < w->nobj; k++)
        if (!hs_should_wait(&g_handles[w->objs[k]], (uint32_t)w->tid)) return 1;
    return 0;
}

static int kwait_shares(const KernelWaiter *w, const int *slots, int n) {
    for (int i = 0; i < n; i++)
        for (int k = 0; k < w->nobj; k++)
            if (w->objs[k] == slots[i]) return 1;
    return 0;
}

/* 1 = dieser Thread darf das Objekt nehmen; 0 = ein anderer registrierter
 * Waiter hat Vorrang. */
static int kwait_self_wins(uint32_t tid, const int *slots, int n) {
    int best = -1;
    for (int i = 0; i < KWAIT_MAX; i++) {
        KernelWaiter *w = &g_kwait[i];
        if (!w->used) continue;
        if (!kwait_shares(w, slots, n)) continue;
        if (!kwait_ready(w)) continue;
        if (best < 0 || w->prio < g_kwait[best].prio ||
            (w->prio == g_kwait[best].prio && w->seq < g_kwait[best].seq))
            best = i;
    }
    return best < 0 || (uint32_t)g_kwait[best].tid == tid;
}

/* Ist ein hoeher priorisierter registrierter Thread bereit? Wird nach einem
 * SVC als Preemptionspunkt benutzt (Azahar PrepareReschedule/Reschedule). */
static int kernel_higher_ready(uint32_t tid, int prio) {
    for (int i = 0; i < KWAIT_MAX; i++) {
        KernelWaiter *w = &g_kwait[i];
        if (!w->used || (uint32_t)w->tid == tid || w->prio >= prio) continue;
        if (kwait_ready(w)) return 1;
    }
    return 0;
}

/* Kern (processor_id) eines Gast-Threads: 0 AppCore, 1 SysCore. */
static int proc_of_tid(uint32_t tid) {
    if (tid == 0) return g_main_proc;
    for (int i = 0; i < 32; i++)
        if (g_threads[i].used && (uint32_t)g_threads[i].id == tid) return g_threads[i].proc_id;
    return 0;
}

/* Ist ein Thread bereit, den echte Hardware jetzt laufen liesse, waehrend
 * tid beschaeftigt ist? Hoeher priorisiert auf demselben Kern, oder beliebig
 * auf einem anderen Kern (3DS: AppCore und SysCore laufen parallel). */
static int kernel_other_should_run(uint32_t tid, int prio) {
    int mine = proc_of_tid(tid);
    for (int i = 0; i < KWAIT_MAX; i++) {
        KernelWaiter *w = &g_kwait[i];
        if (!w->used || (uint32_t)w->tid == tid) continue;
        if (w->prio >= prio && proc_of_tid((uint32_t)w->tid) == mine) continue;
        if (kwait_ready(w)) return 1;
    }
    return 0;
}

/* Abgelaufene Timer signalisieren (Azahar Timer::Signal). Wird aus den
 * Warteschleifen und dem VBlank-Thread gepollt. */
static void kernel_poll_timers(void) {
    uint64_t now = host_ns();
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
    int prio = cur_prio(c);
    int infinite = (timeout_ns == UINT64_MAX);
    uint64_t timeout_ns_eff = infinite ? 0 : timeout_ns;
    uint64_t start = host_ns();
    int yielded = 0;
    int slots[KWAIT_MAX_OBJS];
    int wslot;
    int n = cnt > KWAIT_MAX_OBJS ? KWAIT_MAX_OBJS : cnt;

    for (int k = 0; k < n; k++) slots[k] = (int)(objs[k] - g_handles);
    /* Azahar AddWaitingThread: der Thread steht ab jetzt in der Warteliste
       jedes Objekts; ein spaeterer Signalisierer weckt den hoechstprioren
       bereiten Waiter, nicht den zufaellig zuerst pollenden. */
    wslot = kwait_register(tid, prio, wait_all, (const Handle **)objs, n);

    for (;;) {
        int hit = -1;
        kernel_poll_timers();
        hid_poll();
        y2r_poll();
        if (wait_all) {
            int ok = 1;
            for (int k = 0; k < n; k++) {
                if (hs_should_wait(objs[k], tid)) { ok = 0; break; }
            }
            if (ok && kwait_self_wins(tid, slots, n)) {
                for (int k = 0; k < n; k++) hs_acquire(objs[k], tid);
                kwait_unregister(wslot);
                *out_idx = -1;
                *result = 0;
                return 1;
            }
        } else {
            for (int k = 0; k < n; k++) {
                if (!hs_should_wait(objs[k], tid)) { hit = k; break; }
            }
            if (hit >= 0 && kwait_self_wins(tid, slots, n)) {
                if (g_wstat && objs[hit]->sig_us != 0) {
                    uint64_t lat = host_us() - objs[hit]->sig_us;
                    objs[hit]->sig_us = 0;
                    if (lat > 10000)
                        printf("[wstat] t=%llu tid=%u proc=%d aufwachen %llu ms nach DSP-Signal\n",
                               (unsigned long long)((host_ns() - g_boot_ns) / 1000000ull),
                               tid, proc_of_tid(tid), (unsigned long long)(lat / 1000));
                }
                hs_acquire(objs[hit], tid);
                kwait_unregister(wslot);
                /* Azahar: sofortiger Treffer meldet den ersten Index
                   (find_if), ein Aufwachen aus dem Warten den letzten Index
                   desselben Objekts (GetWaitObjectIndex via rbegin). */
                *out_idx = yielded ? ks_wait_index_last((const void *const *)objs, n, hit)
                                   : hit;
                *result = 0;
                return 1;
            }
        }
        if (!infinite && timeout_ns_eff == 0) {
            kwait_unregister(wslot);
            *out_idx = -1;
            *result = RESULT_TIMEOUT;
            return 0;
        }
        if (!infinite && (host_ns() - start) >= timeout_ns_eff) {
            kwait_unregister(wslot);
            *out_idx = -1;
            *result = RESULT_TIMEOUT;
            if (getenv("HLE_WAITTMO_LOG") && g_waittmo_n < 200) {
                g_waittmo_n++;
                printf("[wtmo] t=%llu tid=%u blk=%08X lr=%08X ns=%llu elapsed_us=%llu\n",
                       (unsigned long long)((host_ns() - g_boot_ns) / 1000000ull),
                       tid, arm_last_block, c->r[14],
                       (unsigned long long)timeout_ns_eff,
                       (unsigned long long)((host_ns() - start) / 1000ull));
            }
            return 0;
        }
        yielded = 1;
        guest_wait_change();
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
        case 2: { /* EnableNotification: Result, CopyHandle(Semaphore)
                     Azahar sm/srv.cpp: MAX_PENDING_NOTIFICATIONS = 16. */
            uint32_t h = new_semaphore_handle("srv:notif", 0, 16);
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
            /* Azahar sm/srv.cpp: kMaxPortSize = 8 -> ResultInvalidNameSize
               (0xD9006405). */
            if (name_len > 8) {
                if (g_log) printf("[srv] GetServiceHandle name_len=%u -> 0xD9006405\n", name_len);
                ipc_begin(r, 5, 1, 0);
                ipc_put(r, 0xD9006405u);
                break;
            }
            {
                int svc = service_from_name(name);
                int h;
                if (svc == SVC_OTHER) {
                    /* Azahar sm/sm.cpp GetServicePort: ResultServiceNotRegistered
                       (0xD0406401). Azahar wartet nur mit flags-Bit0 auf eine
                       spaetere Registrierung; der Port bildet keinen
                       Registrierungsdienst ab und meldet den Fehler sofort. */
                    if (g_log || getenv("HLE_SRV_NAMES")) {
                        printf("[srvname] \"%s\" unbekannt -> 0xD0406401\n", name);
                    }
                    ipc_begin(r, 5, 1, 0);
                    ipc_put(r, 0xD0406401u);
                    break;
                }
                h = alloc_handle(svc, name);
                if (svc == SVC_GSP_GPU) g_hle_gsp_handle = h;
                if (g_log || getenv("HLE_SRV_NAMES")) {
                    printf("[srvname] \"%s\" len=%u name8=%08X%08X blk=%08X lr=%08X -> %d (%s)\n",
                           name, name_len, p[1], p[0],
                           arm_last_block, g_cpu ? g_cpu->r[14] : 0u,
                           h, SERVICE_NAMES[svc]);
                }
                ipc_begin(r, 5, 1, 2);
                put_result(r);
                ipc_put_handle(r, (uint32_t)h);
            }
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


/* APT-Parameterwarteschlange (Azahar AppletManager::CancelAndSendParameter):
 * APT::Initialize legt fuer den ersten registrierten Applet einen
 * Wakeup-Parameter (Sender 0, Signal 1) an und signalisiert das
 * Parameter-Event. ReceiveParameter konsumiert ihn, GlanceParameter nicht. */
typedef struct {
    int used;
    uint32_t sender;
    uint32_t dest;
    uint32_t signal;
    uint32_t size;
} AptParam;
static AptParam g_apt_next_param;

static void apt_command(Ipc *r, uint32_t *p) {
    switch (r->cmd) {
        case 1: { /* GetLockHandle: Result, AppletAttributes, State, CopyHandle(Lock) */
            uint32_t attrs = (r->normal >= 1) ? p[0] : 0u;
            if (g_apt_lock_handle == 0)
                g_apt_lock_handle = new_mutex_handle("apt-lock", 0);
            /* Azahar (apt.cpp): MakeBuilder(3,2), Push(ResultSuccess,
               corrected_attributes, state), PushCopyObjects(lock). */
            ipc_begin(r, 1, 3, 2);
            put_result(r);
            ipc_put(r, attrs);      /* corrected AppletAttributes */
            ipc_put(r, 0u);         /* state */
            ipc_put_handle_copy(r, g_apt_lock_handle);
            break;
        }
        case 2: { /* Initialize: Result, CopyHandle(Notification), CopyHandle(Parameter) */
            uint32_t app_id = (r->normal >= 1) ? p[0] : 0x300u;
            if (!g_apt_initialized) {
                g_apt_notif_handle = new_event_handle("apt-notif", 0);
                g_apt_param_ev = new_event_handle("apt-param", 0);
                /* Azahar AppletManager::Initialize -> SendParameter(Wakeup):
                   der erste registrierte Applet bekommt sofort einen
                   Wakeup-Parameter (Sender 0, Signal 1, Groesse 0), das
                   Parameter-Event wird signalisiert. */
                g_apt_next_param.used = 1;
                g_apt_next_param.sender = 0;
                g_apt_next_param.dest = app_id;
                g_apt_next_param.signal = 1;   /* SignalType::Wakeup */
                g_apt_next_param.size = 0;
                {
                    Handle *ph = get_handle(g_apt_param_ev);
                    if (ph != NULL) ph->signaled = 1;
                }
                g_apt_initialized = 1;
            }
            /* Azahar: MakeBuilder(1,3), PushCopyObjects(notif, param). */
            ipc_begin(r, 2, 1, 3);
            put_result(r);
            ipc_put(r, (1u << 26));   /* CopyHandle-Deskriptor, 2 Handles */
            ipc_put(r, g_apt_notif_handle);
            ipc_put(r, g_apt_param_ev);
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
            /* GlanceParameter (0x000E) konsumiert den Parameter NICHT,
               ReceiveParameter (0x000D) schon. */
            {
                uint32_t app_id = (r->normal >= 1) ? p[0] : 0u;
                uint32_t buffer_size = (r->normal >= 2) ? p[1] : 0u;
                (void)dsc_flags;
                if (!g_apt_next_param.used || g_apt_next_param.dest != app_id) {
                    /* Kein passender Parameter: NoData */
                    ipc_begin(r, r->cmd, 1, 0);
                    ipc_put(r, 0xC8A303EFu);
                    break;
                }
                {
                    AptParam par = g_apt_next_param;
                    uint32_t size = (par.size < buffer_size) ? par.size : buffer_size;
                    if (r->cmd == 0x000D) {
                        g_apt_next_param.used = 0;
                        if (g_apt_param_ev != 0) {
                            Handle *ph = get_handle(g_apt_param_ev);
                            if (ph != NULL && ph->reset_type != 1) ph->signaled = 0;
                        }
                    }
                    ipc_begin(r, r->cmd, 4, 4);
                    put_result(r);
                    ipc_put(r, par.sender);
                    ipc_put(r, par.signal);
                    ipc_put(r, size);
                    ipc_put_handle(r, 0);  /* MoveHandle: kein Objekt (Azahar: nullptr) */
                    ipc_put(r, (2u | ((buffer_size & 0x3FFFFu) << 14)));
                    ipc_put(r, dsc_addr);
                    if (g_log) printf("[apt] %s -> sender=%u signal=%u\n",
                                      r->cmd == 0x0D ? "ReceiveParameter" : "GlanceParameter",
                                      par.sender, par.signal);
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
            uint32_t utility = (r->normal >= 1) ? p[0] : 0u;
            uint32_t out_size = (r->normal >= 3) ? p[2] : 0u;
            uint32_t dsc_flags = (r->translate >= 2) ? p[r->normal] : 2u;
            uint32_t dsc_addr = (r->translate >= 2) ? p[r->normal + 1] : 0u;
            (void)dsc_flags;
            if (dsc_addr != 0u && out_size != 0u && g_cpu != NULL) {
                uint32_t off = dsc_addr & 0x1FFFFFFFu;
                if (off + out_size <= g_cpu->mem_size) {
                    memset(g_cpu->mem + off, 0, out_size);
                    /* Azahar (apt.cpp, AppletUtility): Kommando 0x6
                       (TryLockTransition) meldet Erfolg in out[0], damit
                       die App nicht in den Fehlerpfad laeuft. */
                    if (utility == 0x6u)
                        g_cpu->mem[off] = 1;
                }
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

/* ---------- HID (hid:USER / hid:SPVR) ----------
 * Layout und reine Ringpuffer-Logik stehen in runtime/platform/hid_ring.h
 * (Azahar core/hle/service/hid/hid.h bzw. hid.cpp). Die Ringpuffer werden in
 * einem Host-Schattenpuffer gefuehrt. Sobald der Gast den Shared-Memory-Block
 * per svcMapMemoryBlock einblendet, werden nach jedem Update nur die von
 * Azahar geschriebenen Ringbereiche (HID_RING_RANGES) dorthin kopiert. */
/* Azahar hid.h: BASE_CLOCK_RATE_ARM11 / 234, /104, /101 (empirisch). */
#define HID_PAD_TICKS_PERIOD   (SYSCLOCK_ARM11 / 234u)
#define HID_ACC_TICKS_PERIOD   (SYSCLOCK_ARM11 / 104u)
#define HID_GYRO_TICKS_PERIOD  (SYSCLOCK_ARM11 / 101u)

/* Ruhelage des Motion-Emulators (Azahar input_common/motion_emu.cpp: gravity
 * (0,-1,0) in 3DS-Koordinaten, hid.cpp multipliziert mit accelerometer_coef
 * 512). Ohne Bewegungseingabe liegt das Geraet flach, also y = -512. */
#define HID_ACCEL_REST_X  (0)
#define HID_ACCEL_REST_Y  (-512)
#define HID_ACCEL_REST_Z  (0)

typedef struct {
    int initialized;
    uint32_t shmem;                 /* Handle des Shared-Memory-Blocks */
    uint32_t events[5];             /* Pad0, Pad1, Accel, Gyro, DebugPad */
    HidRing ring;                   /* Indizes/Ticks/Mittelung (hid_ring.h) */
    uint64_t pad_due, acc_due, gyro_due;
    int enable_acc, enable_gyro;
    int guest_mapped;
    uint32_t guest_addr;
    uint64_t pad_updates;
    uint8_t shadow[HID_SHMEM_SIZE];
} HidState;

static HidState g_hid;
static SRWLOCK g_hid_srw = SRWLOCK_INIT;
/* Eigener Mutex nur fuer den Host-Eingabepuffer: Fenster-/Hauptthread
 * schreiben hier, hid_poll liest ihn unter dem Gast-Lock. */
static SRWLOCK g_hid_in_srw = SRWLOCK_INIT;
static HleHidInput g_hid_input;
static int g_hid_log = -1;

static uint32_t hid_rd32(const HidState *s, uint32_t off) {
    uint32_t v = 0;
    memcpy(&v, s->shadow + off, 4);
    return v;
}

static void hid_signal_event(int which) {
    Handle *hh = get_handle(g_hid.events[which]);
    if (hh != NULL) hh->signaled = 1;
}

/* Signalisiert die Ereignisse, die ein Ring-Update gemeldet hat. */
static void hid_signal_events(uint32_t ev) {
    if (ev & HID_RING_EV_PAD0) hid_signal_event(0);
    if (ev & HID_RING_EV_PAD1) hid_signal_event(1);
    if (ev & HID_RING_EV_ACCEL) hid_signal_event(2);
    if (ev & HID_RING_EV_GYRO) hid_signal_event(3);
}

static void hid_update_pad_locked(uint64_t tick) {
    HidRingInput in;
    AcquireSRWLockShared(&g_hid_in_srw);
    in = g_hid_input;
    ReleaseSRWLockShared(&g_hid_in_srw);
    g_hid.pad_updates++;
    hid_signal_events(hid_ring_write_pad(&g_hid.ring, g_hid.shadow,
                                        &in, (int64_t)tick));
}

static void hid_update_acc_locked(uint64_t tick) {
    /* Ohne Bewegungseingabe: Erdbeschleunigung in Ruhelage (Azahar). */
    hid_signal_events(hid_ring_write_accel(&g_hid.ring, g_hid.shadow,
                                           HID_ACCEL_REST_X, HID_ACCEL_REST_Y,
                                           HID_ACCEL_REST_Z, (int64_t)tick));
}

static void hid_update_gyro_locked(uint64_t tick) {
    hid_signal_events(hid_ring_write_gyro(&g_hid.ring, g_hid.shadow,
                                          0, 0, 0, (int64_t)tick));
}

/* Kopiert genau die Ringbereiche, die Azahar im Shared Memory aktualisiert.
 * Der Aufrufer haelt den Gast-Lock und g_hid_srw: Gast-Speicher wird nur
 * unter dem Gast-Lock geschrieben. */
static void hid_copy_to_guest_locked(void) {
    uint32_t off;
    if (!g_hid.guest_mapped || g_cpu == NULL) return;
    off = g_hid.guest_addr & 0x1FFFFFFFu;
    if ((uint64_t)off + HID_SHMEM_SIZE > g_cpu->mem_size) return;
    for (size_t i = 0; i < HID_RING_RANGE_COUNT; i++) {
        uint32_t o = HID_RING_RANGES[i].offset;
        memcpy(g_cpu->mem + off + o, g_hid.shadow + o, HID_RING_RANGES[i].length);
    }
}

/* Regelt die Update-Zeitpunkte (Azahar CoreTiming: pad_update_ticks usw.).
 * Wird aus hle_pump, den Warteschleifen und dem VBlank-Thread aufgerufen;
 * verpasste Perioden werden nachgeholt. Der Gast-Lock wird hier genommen,
 * falls der Aufrufer ihn nicht schon haelt, weil der Schatten in den
 * Gast-Shared-Memory kopiert wird. */
static void hid_poll(void) {
    uint64_t now;
    int guard;
    int had_lock;
    if (!g_hid.initialized) return;
    now = host_system_ticks();

    /* Der Lock ist nicht rekursiv: nur abgeben, wenn hid_poll ihn selbst
       genommen hat (VBlank-Thread nimmt ihn bereits aussen). */
    had_lock = t_lock_owned;
    if (!had_lock) guest_lock_enter();
    AcquireSRWLockExclusive(&g_hid_srw);
    guard = 0;
    while (now >= g_hid.pad_due && guard++ < 64) {
        hid_update_pad_locked(g_hid.pad_due);
        g_hid.pad_due += HID_PAD_TICKS_PERIOD;
    }
    if (g_hid.pad_due < now) g_hid.pad_due = now + HID_PAD_TICKS_PERIOD;

    guard = 0;
    while (g_hid.enable_acc > 0 && now >= g_hid.acc_due && guard++ < 64) {
        hid_update_acc_locked(g_hid.acc_due);
        g_hid.acc_due += HID_ACC_TICKS_PERIOD;
    }
    if (g_hid.acc_due < now) g_hid.acc_due = now + HID_ACC_TICKS_PERIOD;

    guard = 0;
    while (g_hid.enable_gyro > 0 && now >= g_hid.gyro_due && guard++ < 64) {
        hid_update_gyro_locked(g_hid.gyro_due);
        g_hid.gyro_due += HID_GYRO_TICKS_PERIOD;
    }
    if (g_hid.gyro_due < now) g_hid.gyro_due = now + HID_GYRO_TICKS_PERIOD;

    hid_copy_to_guest_locked();
    if (g_hid_log < 0) g_hid_log = getenv("HLE_HID_LOG") ? 1 : 0;
    if (g_hid_log && g_hid.pad_updates != 0u && (g_hid.pad_updates % 234u) == 0u) {
        static uint64_t last_logged = 0;
        if (g_hid.pad_updates != last_logged) {
            uint32_t ti = hid_rd32(&g_hid, HID_TOUCH_INDEX) % HID_TOUCH_ENTRIES;
            uint32_t te = HID_TOUCH_ENTRIES_OFF + ti * 8u;
            uint32_t vx = hid_rd32(&g_hid, te + 4u);
            uint16_t tx = 0, ty = 0;
            last_logged = g_hid.pad_updates;
            memcpy(&tx, g_hid.shadow + te, 2);
            memcpy(&ty, g_hid.shadow + te + 2, 2);
            printf("[hid] pads=%llu pad_idx=%u state=%08X pad_ticks=%lld touch_idx=%u "
                   "valid=%u pos=(%u,%u) acc_idx=%u gyro_idx=%u\n",
                   (unsigned long long)g_hid.pad_updates,
                   hid_rd32(&g_hid, HID_PAD_INDEX), hid_rd32(&g_hid, HID_PAD_CURRENT),
                   (long long)g_hid.ring.pad_ticks, ti, vx & 1u, tx, ty,
                   hid_rd32(&g_hid, HID_ACC_INDEX), hid_rd32(&g_hid, HID_GYRO_INDEX));
            fflush(stdout);
        }
    }
    ReleaseSRWLockExclusive(&g_hid_srw);
    if (!had_lock) guest_lock_leave();
}

/* Host-Threads (Fenster/Hauptthread) schreiben nur in diesen Host-Puffer,
 * nie in Gast-Speicher. Angewendet wird er ausschliesslich in hid_poll unter
 * dem Gast-Lock. */
void hle_hid_set_input(const HleHidInput *in) {
    if (in == NULL) return;
    AcquireSRWLockExclusive(&g_hid_in_srw);
    g_hid_input = *in;
    ReleaseSRWLockExclusive(&g_hid_in_srw);
}

static void hid_map_guest(uint32_t addr) {
    AcquireSRWLockExclusive(&g_hid_srw);
    g_hid.guest_addr = addr;
    g_hid.guest_mapped = 1;
    hid_copy_to_guest_locked();
    ReleaseSRWLockExclusive(&g_hid_srw);
    if (getenv("HLE_MAP_LOG"))
        printf("[hid] Shared-Memory gemappt auf 0x%08X\n", addr);
}

static void hid_unmap_guest(uint32_t addr) {
    AcquireSRWLockExclusive(&g_hid_srw);
    if (g_hid.guest_mapped && (g_hid.guest_addr & 0x1FFFFFFFu) == (addr & 0x1FFFFFFFu))
        g_hid.guest_mapped = 0;
    ReleaseSRWLockExclusive(&g_hid_srw);
}

/* Azahar legt Shared-Memory und die fuenf Events beim Modulaufbau an; alle
 * hid-Sessions (hid:USER und hid:SPVR) teilen sie. */
static void hid_init_once(void) {
    static const char *names[5] = {
        "hid-pad0", "hid-pad1", "hid-accel", "hid-gyro", "hid-debugpad"
    };
    uint64_t now;
    if (g_hid.initialized) return;
    memset(&g_hid, 0, sizeof(g_hid));
    g_hid.shmem = (uint32_t)alloc_handle(SVC_OTHER, "hid-shmem");
    for (int i = 0; i < 5; i++) g_hid.events[i] = new_event_handle(names[i], 0);
    now = host_system_ticks();
    g_hid.pad_due = now + HID_PAD_TICKS_PERIOD;
    g_hid.acc_due = now + HID_ACC_TICKS_PERIOD;
    g_hid.gyro_due = now + HID_GYRO_TICKS_PERIOD;
    g_hid.initialized = 1;
}

static void hid_command(Ipc *rp, uint32_t *p) {
    (void)p;
    switch (rp->cmd) {
        case 0x000A: { /* GetIPCHandles: Result, CopyHandle(SharedMem + 5 Events) */
            /* Azahar (hid.cpp): MakeBuilder(1,7), PushCopyObjects(shared_mem,
               event_pad_or_touch_1, event_pad_or_touch_2, event_accelerometer,
               event_gyroscope, event_debug_pad). Die Objekte existieren
               einmal pro HID-Modul; jede Session bekommt dieselben Handles. */
            hid_init_once();
            ipc_begin(rp, 0x000A, 1, 7);
            put_result(rp);
            ipc_put(rp, (5u << 26));       /* CopyHandle-Deskriptor, 6 Handles */
            ipc_put(rp, g_hid.shmem);
            for (int i = 0; i < 5; i++) ipc_put(rp, g_hid.events[i]);
            if (g_log) printf("[hid] GetIPCHandles -> shm=%u ev=%u/%u/%u/%u/%u\n",
                              g_hid.shmem, g_hid.events[0], g_hid.events[1],
                              g_hid.events[2], g_hid.events[3], g_hid.events[4]);
            break;
        }
        case 0x0011: { /* EnableAccelerometer (Azahar: Zaehler, ab 1 geplant) */
            hid_init_once();
            ipc_begin(rp, rp->cmd, 1, 0);
            put_result(rp);
            AcquireSRWLockExclusive(&g_hid_srw);
            if (g_hid.enable_acc++ == 0)
                g_hid.acc_due = host_system_ticks() + HID_ACC_TICKS_PERIOD;
            ReleaseSRWLockExclusive(&g_hid_srw);
            break;
        }
        case 0x0012: { /* DisableAccelerometer */
            ipc_begin(rp, rp->cmd, 1, 0);
            put_result(rp);
            AcquireSRWLockExclusive(&g_hid_srw);
            if (g_hid.enable_acc > 0) g_hid.enable_acc--;
            ReleaseSRWLockExclusive(&g_hid_srw);
            break;
        }
        case 0x0013: { /* EnableGyroscopeLow */
            hid_init_once();
            ipc_begin(rp, rp->cmd, 1, 0);
            put_result(rp);
            AcquireSRWLockExclusive(&g_hid_srw);
            if (g_hid.enable_gyro++ == 0)
                g_hid.gyro_due = host_system_ticks() + HID_GYRO_TICKS_PERIOD;
            ReleaseSRWLockExclusive(&g_hid_srw);
            break;
        }
        case 0x0014: { /* DisableGyroscopeLow */
            ipc_begin(rp, rp->cmd, 1, 0);
            put_result(rp);
            AcquireSRWLockExclusive(&g_hid_srw);
            if (g_hid.enable_gyro > 0) g_hid.enable_gyro--;
            ReleaseSRWLockExclusive(&g_hid_srw);
            break;
        }
        case 0x0015: { /* GetGyroscopeLowRawToDpsCoefficient: Result + f32 */
            float coef = 14.375f;   /* Azahar hid.cpp gyroscope_coef */
            uint32_t bits;
            memcpy(&bits, &coef, 4);
            ipc_begin(rp, 0x0015, 2, 0);
            put_result(rp);
            ipc_put(rp, bits);
            break;
        }
        case 0x0016: { /* GetGyroscopeLowCalibrateParam: 3x {0,+6700,-6700} */
            /* Azahar hid.h GyroscopeCalibrateParam, param_unit = 6700. */
            uint16_t pos = (uint16_t)6700, neg = (uint16_t)(int16_t)-6700;
            ipc_begin(rp, 0x0016, 6, 0);
            put_result(rp);
            ipc_put(rp, ((uint32_t)pos << 16) | 0u);   /* x.zero, x.pos */
            ipc_put(rp, ((uint32_t)0u << 16) | neg);   /* x.neg, y.zero */
            ipc_put(rp, ((uint32_t)neg << 16) | pos);  /* y.pos, y.neg */
            ipc_put(rp, ((uint32_t)pos << 16) | 0u);   /* z.zero, z.pos */
            ipc_put(rp, (uint32_t)neg);                /* z.neg + Padding */
            break;
        }
        case 0x0017: { /* GetSoundVolume: Result + u8 */
            /* Azahar: (u8)(0x3F * Settings::volume) = 0x3F bei voller Lautstaerke. */
            ipc_begin(rp, 0x0017, 2, 0);
            put_result(rp);
            ipc_put(rp, 0x3F);
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

/* Pfadpuffer in fs:USER-Requests. Das SDK dieses Spiels legt FS_Path in den
 * Translate-Bereich (zwei Woerter {flags, adresse}); Azahar liest dieselben
 * Bytes ueber PopStaticBuffer. Fallback: StaticBuffer-Deskriptor bei 0x40. */
static uint32_t fs_req_buffer_addr(uint32_t *p, int normal, int translate, int index) {
    int off = normal + index * 2 + 1;
    if (index * 2 + 1 < translate) {
        uint32_t a = p[off];
        if (a >= 0x00100000u && a < 0x20000000u) return a;
    }
    {
        uint32_t a = p[0x10 + index * 2];
        if (a >= 0x00100000u && a < 0x20000000u) return a;
    }
    return 0;
}

static int fs_req_path(uint32_t *p, int normal, int translate, int index, uint32_t type,
                       uint32_t size, uint8_t *out, size_t cap) {
    uint32_t addr = fs_req_buffer_addr(p, normal, translate, index);
    if (addr == 0) {
        if (cap) out[0] = 0;
        return 0;
    }
    return fs_archive_decode_path(g_cur_mem, g_cpu ? g_cpu->mem_size : 0, type, size, addr,
                                  out, cap);
}

static int fs_open_host_file(const char *host, const char *label, FileSlot **slot_out) {
    FILE *fp = fopen(host, "r+b");
    long sz;
    if (fp == NULL) fp = fopen(host, "rb");
    if (fp == NULL) return 0;
    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    for (int i = 0; i < MAX_FILES; i++) {
        if (!g_files[i].used) {
            g_files[i].used = 1;
            snprintf(g_files[i].path, sizeof(g_files[i].path), "%s", label);
            g_files[i].fp = fp;
            g_files[i].size = (uint32_t)sz;
            *slot_out = &g_files[i];
            return 1;
        }
    }
    fclose(fp);
    return 0;
}

static void fs_reply_result(Ipc *r, uint32_t cmd, uint32_t res) {
    ipc_begin(r, cmd, 1, 0);
    ipc_put(r, res);
}

static void fs_command(Ipc *r, uint32_t *p) {
    if (getenv("HLE_FS_PATHS")) {
        printf("[fscmd] cmd=0x%04X n=%d t=%d", r->cmd, r->normal, r->translate);
        for (int i = 0; i < r->normal && i < 10; i++) printf(" p%d=%08X", i, p[i]);
        printf("\n");
    }
    switch (r->cmd) {
        case 0x0801: { /* Initialize */
            ipc_begin(r, 0x0801, 1, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0802: { /* OpenFile */
            uint64_t archive = (uint64_t)p[1] | ((uint64_t)p[2] << 32);
            uint32_t type = p[3], size = p[4], mode = p[5], attrs = p[6];
            uint8_t raw[FS_ARCHIVE_PATH_MAX];
            char path[FS_ARCHIVE_PATH_MAX];
            char host[FS_ARCHIVE_PATH_MAX];
            FileSlot *slot = NULL;
            uint32_t h = 0;
            int fixed = 0;
            uint32_t res = FS_RESULT_INVALID_PATH;
            int len = fs_req_path(p, r->normal, r->translate, 0, type, size, raw, sizeof(raw));

            path[0] = 0;
            if (len > 0 && (type == 3 || type == 4)) snprintf(path, sizeof(path), "%s", raw);
            if (archive != 0 && path[0]) {
                res = fs_archive_open_file(archive, path, mode, attrs, host, sizeof(host), &fixed);
                if (res == FS_RESULT_SUCCESS) {
                    if (fs_open_host_file(host, host, &slot)) {
                        slot->fixed_size = fixed;
                        h = (uint32_t)alloc_handle(SVC_FILE, "file");
                        if (h && h < MAX_HANDLES) g_handles[h].file = (int)(slot - g_files);
                    } else {
                        res = FS_RESULT_FILE_NOT_FOUND;
                    }
                }
            } else if (archive == 0) {
                if (path[0] && open_guest_file(path, &slot)) {
                    res = FS_RESULT_SUCCESS;
                    h = (uint32_t)alloc_handle(SVC_FILE, "file");
                    if (h && h < MAX_HANDLES) g_handles[h].file = (int)(slot - g_files);
                }
            }
            if (g_log) {
                printf("[fs] OpenFile \"%s\" archive=%llu result=%08X handle=%u\n",
                       path, (unsigned long long)archive, res, h);
            }
            ipc_begin(r, 0x0802, 1, 2);
            ipc_put(r, res);
            ipc_put_handle(r, h);
            break;
        }
        case 0x0803: { /* OpenFileDirectly */
            uint32_t aid = p[1];
            uint32_t atype = p[2], asize = p[3], ftype = p[4], fsize = p[5];
            uint32_t mode = p[6], attrs = p[7];
            uint8_t araw[FS_ARCHIVE_PATH_MAX];
            uint8_t fraw[FS_ARCHIVE_PATH_MAX];
            char fpath[FS_ARCHIVE_PATH_MAX];
            char host[FS_ARCHIVE_PATH_MAX];
            FileSlot *slot = NULL;
            uint32_t h = 0;
            int fixed = 0;
            uint32_t res = FS_RESULT_INVALID_PATH;
            int alen = fs_req_path(p, r->normal, r->translate, 0, atype, asize, araw, sizeof(araw));
            int flen = fs_req_path(p, r->normal, r->translate, 1, ftype, fsize, fraw, sizeof(fraw));

            fpath[0] = 0;
            if (aid == 3) {
                uint32_t kind = 0;
                if (flen >= 4) memcpy(&kind, fraw, 4);
                if (kind == 0 || kind == 5) {
                    if (fs_open_host_file("out/romfs_image.bin", "out/romfs_image.bin", &slot))
                        res = FS_RESULT_SUCCESS;
                } else if (kind == 2) {
                    char nm[9];
                    memcpy(nm, fraw + 4, 8);
                    nm[8] = 0;
                    snprintf(host, sizeof(host), "out/exefs/%s.bin", nm);
                    if (fs_open_host_file(host, host, &slot)) res = FS_RESULT_SUCCESS;
                }
            } else {
                int64_t ah = fs_archive_open(aid, araw, (uint32_t)(alen > 0 ? alen : 0),
                                             fs_archive_program_id(), &res);
                if (ah > 0) {
                    if (flen > 0 && (ftype == 3 || ftype == 4))
                        snprintf(fpath, sizeof(fpath), "%s", fraw);
                    res = fs_archive_open_file((uint64_t)ah, fpath, mode, attrs, host, sizeof(host),
                                               &fixed);
                    fs_archive_close((uint64_t)ah);
                    if (res == FS_RESULT_SUCCESS && !fs_open_host_file(host, host, &slot))
                        res = FS_RESULT_FILE_NOT_FOUND;
                    if (res == FS_RESULT_SUCCESS && slot != NULL) slot->fixed_size = fixed;
                }
            }
            if (res == FS_RESULT_SUCCESS && slot != NULL) {
                h = (uint32_t)alloc_handle(SVC_FILE, "file");
                if (h && h < MAX_HANDLES) g_handles[h].file = (int)(slot - g_files);
            }
            if (g_log) {
                printf("[fs] OpenFileDirectly id=%u result=%08X handle=%u\n", aid, res, h);
            }
            ipc_begin(r, 0x0803, 1, 2);
            ipc_put(r, res);
            ipc_put_handle(r, h);
            break;
        }
        case 0x0804: { /* DeleteFile */
            uint64_t a = (uint64_t)p[1] | ((uint64_t)p[2] << 32);
            uint8_t raw[FS_ARCHIVE_PATH_MAX];
            char path[FS_ARCHIVE_PATH_MAX];
            int len = fs_req_path(p, r->normal, r->translate, 0, p[3], p[4], raw, sizeof(raw));
            uint32_t res = FS_RESULT_INVALID_PATH;
            path[0] = 0;
            if (len > 0 && (p[3] == 3 || p[3] == 4)) snprintf(path, sizeof(path), "%s", raw);
            if (path[0]) res = fs_archive_delete_file(a, path);
            if (g_log) printf("[fs] DeleteFile \"%s\" -> %08X\n", path, res);
            fs_reply_result(r, 0x0804, res);
            break;
        }
        case 0x0805: { /* RenameFile */
            uint64_t src = (uint64_t)p[1] | ((uint64_t)p[2] << 32);
            uint64_t dst = (uint64_t)p[5] | ((uint64_t)p[6] << 32);
            uint8_t sraw[FS_ARCHIVE_PATH_MAX], draw[FS_ARCHIVE_PATH_MAX];
            char sp[FS_ARCHIVE_PATH_MAX], dp[FS_ARCHIVE_PATH_MAX];
            uint32_t res = FS_RESULT_INVALID_PATH;
            int slen = fs_req_path(p, r->normal, r->translate, 0, p[3], p[4], sraw, sizeof(sraw));
            int dlen = fs_req_path(p, r->normal, r->translate, 1, p[7], p[8], draw, sizeof(draw));
            sp[0] = dp[0] = 0;
            if (slen > 0 && (p[3] == 3 || p[3] == 4)) snprintf(sp, sizeof(sp), "%s", sraw);
            if (dlen > 0 && (p[7] == 3 || p[7] == 4)) snprintf(dp, sizeof(dp), "%s", draw);
            if (sp[0] && dp[0]) res = fs_archive_rename_file(src, sp, dst, dp);
            if (g_log) printf("[fs] RenameFile \"%s\" -> \"%s\" : %08X\n", sp, dp, res);
            fs_reply_result(r, 0x0805, res);
            break;
        }
        case 0x0806: case 0x0807: { /* DeleteDirectory */
            uint64_t a = (uint64_t)p[1] | ((uint64_t)p[2] << 32);
            uint8_t raw[FS_ARCHIVE_PATH_MAX];
            char path[FS_ARCHIVE_PATH_MAX];
            int len = fs_req_path(p, r->normal, r->translate, 0, p[3], p[4], raw, sizeof(raw));
            uint32_t res = FS_RESULT_INVALID_PATH;
            path[0] = 0;
            if (len > 0 && (p[3] == 3 || p[3] == 4)) snprintf(path, sizeof(path), "%s", raw);
            if (path[0]) res = fs_archive_delete_directory(a, path, r->cmd == 0x0807);
            if (g_log) printf("[fs] DeleteDirectory \"%s\" rec=%d -> %08X\n", path,
                              r->cmd == 0x0807, res);
            fs_reply_result(r, r->cmd, res);
            break;
        }
        case 0x0808: { /* CreateFile */
            uint64_t a = (uint64_t)p[1] | ((uint64_t)p[2] << 32);
            uint64_t fsize = (uint64_t)p[6] | ((uint64_t)p[7] << 32);
            uint8_t raw[FS_ARCHIVE_PATH_MAX];
            char path[FS_ARCHIVE_PATH_MAX];
            int len = fs_req_path(p, r->normal, r->translate, 0, p[3], p[4], raw, sizeof(raw));
            uint32_t res = FS_RESULT_INVALID_PATH;
            path[0] = 0;
            if (len > 0 && (p[3] == 3 || p[3] == 4)) snprintf(path, sizeof(path), "%s", raw);
            if (path[0]) res = fs_archive_create_file(a, path, fsize, p[5]);
            if (g_log) printf("[fs] CreateFile \"%s\" size=%llu -> %08X\n", path,
                              (unsigned long long)fsize, res);
            fs_reply_result(r, 0x0808, res);
            break;
        }
        case 0x0809: { /* CreateDirectory */
            uint64_t a = (uint64_t)p[1] | ((uint64_t)p[2] << 32);
            uint8_t raw[FS_ARCHIVE_PATH_MAX];
            char path[FS_ARCHIVE_PATH_MAX];
            int len = fs_req_path(p, r->normal, r->translate, 0, p[3], p[4], raw, sizeof(raw));
            uint32_t res = FS_RESULT_INVALID_PATH;
            path[0] = 0;
            if (len > 0 && (p[3] == 3 || p[3] == 4)) snprintf(path, sizeof(path), "%s", raw);
            if (path[0]) res = fs_archive_create_directory(a, path, p[5]);
            if (g_log) printf("[fs] CreateDirectory \"%s\" -> %08X\n", path, res);
            fs_reply_result(r, 0x0809, res);
            break;
        }
        case 0x080A: { /* RenameDirectory */
            uint64_t src = (uint64_t)p[1] | ((uint64_t)p[2] << 32);
            uint64_t dst = (uint64_t)p[5] | ((uint64_t)p[6] << 32);
            uint8_t sraw[FS_ARCHIVE_PATH_MAX], draw[FS_ARCHIVE_PATH_MAX];
            char sp[FS_ARCHIVE_PATH_MAX], dp[FS_ARCHIVE_PATH_MAX];
            uint32_t res = FS_RESULT_INVALID_PATH;
            int slen = fs_req_path(p, r->normal, r->translate, 0, p[3], p[4], sraw, sizeof(sraw));
            int dlen = fs_req_path(p, r->normal, r->translate, 1, p[7], p[8], draw, sizeof(draw));
            sp[0] = dp[0] = 0;
            if (slen > 0 && (p[3] == 3 || p[3] == 4)) snprintf(sp, sizeof(sp), "%s", sraw);
            if (dlen > 0 && (p[7] == 3 || p[7] == 4)) snprintf(dp, sizeof(dp), "%s", draw);
            if (sp[0] && dp[0]) res = fs_archive_rename_directory(src, sp, dst, dp);
            if (g_log) printf("[fs] RenameDirectory \"%s\" -> \"%s\" : %08X\n", sp, dp, res);
            fs_reply_result(r, 0x080A, res);
            break;
        }
        case 0x080B: { /* OpenDirectory */
            uint64_t a = (uint64_t)p[0] | ((uint64_t)p[1] << 32);
            uint8_t raw[FS_ARCHIVE_PATH_MAX];
            char path[FS_ARCHIVE_PATH_MAX];
            char host[FS_ARCHIVE_PATH_MAX];
            uint32_t h = 0;
            uint32_t res;
            int slot = -1;
            int len = fs_req_path(p, r->normal, r->translate, 0, p[2], p[3], raw, sizeof(raw));
            path[0] = 0;
            if (len > 0 && (p[2] == 3 || p[2] == 4)) snprintf(path, sizeof(path), "%s", raw);
            res = path[0] ? fs_archive_open_directory(a, path, host, sizeof(host))
                          : FS_RESULT_INVALID_PATH;
            if (res == FS_RESULT_SUCCESS) slot = fs_archive_dir_open(host, &res);
            if (slot >= 0) {
                h = (uint32_t)alloc_handle(SVC_FILE, "dir");
                if (h && h < MAX_HANDLES) {
                    g_handles[h].dir = slot;
                    g_handles[h].file = -1;
                }
                res = FS_RESULT_SUCCESS;
            }
            if (g_log) printf("[fs] OpenDirectory \"%s\" -> %08X handle=%u\n", path, res, h);
            ipc_begin(r, 0x080B, 1, 2);
            ipc_put(r, res);
            ipc_put_handle(r, h);
            break;
        }
        case 0x080C: { /* OpenArchive */
            uint32_t id = p[0];
            uint8_t raw[FS_ARCHIVE_PATH_MAX];
            uint32_t res = FS_RESULT_SUCCESS;
            int64_t ah;
            int len = fs_req_path(p, r->normal, r->translate, 0, p[1], p[2], raw, sizeof(raw));
            ah = fs_archive_open(id, raw, (uint32_t)(len > 0 ? len : 0), fs_archive_program_id(),
                                 &res);
            if (g_log) {
                printf("[fs] OpenArchive id=0x%08X type=%u size=%u len=%d -> %08X handle=%lld\n",
                       id, p[1], p[2], len, res, (long long)ah);
            }
            ipc_begin(r, 0x080C, 3, 0);
            ipc_put(r, res);
            ipc_put(r, ah > 0 ? (uint32_t)(uint64_t)ah : 0u);
            ipc_put(r, ah > 0 ? (uint32_t)((uint64_t)ah >> 32) : 0u);
            break;
        }
        case 0x080D: { /* ControlArchive */
            uint64_t a = (uint64_t)p[0] | ((uint64_t)p[1] << 32);
            uint32_t res = fs_archive_control(a, p[2], p[3], p[4]);
            if (g_log) printf("[fs] ControlArchive h=%llu action=%u -> %08X\n",
                              (unsigned long long)a, p[2], res);
            fs_reply_result(r, 0x080D, res);
            break;
        }
        case 0x080E: { /* CloseArchive */
            uint64_t a = (uint64_t)p[0] | ((uint64_t)p[1] << 32);
            uint32_t res = fs_archive_close(a);
            if (g_log) printf("[fs] CloseArchive h=%llu -> %08X\n", (unsigned long long)a, res);
            fs_reply_result(r, 0x080E, res);
            break;
        }
        case 0x080F: { /* FormatThisUserSaveData */
            FsFormatInfo info;
            info.total_size = p[0] * 512u;
            info.number_directories = p[1];
            info.number_files = p[2];
            info.duplicate_data = (uint8_t)p[5];
            if (g_log) printf("[fs] FormatThisUserSaveData\n");
            fs_reply_result(r, 0x080F,
                            fs_archive_format(4, NULL, 0, fs_archive_program_id(), &info));
            break;
        }
        case 0x0812: { /* GetFreeBytes */
            uint64_t a = (uint64_t)p[0] | ((uint64_t)p[1] << 32);
            uint64_t bytes = 0;
            uint32_t res = fs_archive_get_free_bytes(a, &bytes);
            ipc_begin(r, 0x0812, 3, 0);
            ipc_put(r, res);
            ipc_put(r, (uint32_t)bytes);
            ipc_put(r, (uint32_t)(bytes >> 32));
            break;
        }
        case 0x0813: case 0x0821: {
            ipc_begin(r, r->cmd, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 0);
            break;
        }
        case 0x0817: case 0x0818: {
            ipc_begin(r, r->cmd, 2, 0);
            ipc_put(r, 0);
            ipc_put(r, 1);
            break;
        }
        case 0x0814: {
            ipc_begin(r, 0x0814, 5, 0);
            ipc_put(r, 0);
            ipc_put(r, 512);
            ipc_put(r, 0x8000);
            ipc_put(r, 0x100000);
            ipc_put(r, 0x080000);
            break;
        }
        case 0x0831: { /* CreateSharedExtSaveData: in Azahar ohne Handler
                         (fs_user.cpp CommandInfo 0x0831 = nullptr). */
            generic_command(r, SVC_FS_USER);
            break;
        }
        case 0x0851: { /* CreateExtSaveData: Info, dirs, files, size u64, icon */
            uint32_t media = p[0] & 0xFFu;
            uint8_t bin[12];
            FsFormatInfo info;
            uint32_t res;
            memcpy(bin + 0, &p[0], 4);
            memcpy(bin + 4, &p[1], 4);
            memcpy(bin + 8, &p[2], 4);
            info.number_directories = p[3];
            info.number_files = p[4];
            info.total_size = p[5];
            info.duplicate_data = 0;
            if (g_log) printf("[fs] CreateExtSaveData cmd=%04X low=%08X high=%08X media=%u\n",
                              r->cmd, p[1], p[2], media);
            res = fs_archive_create_ext(6u, bin, sizeof(bin), &info, fs_archive_program_id());
            ipc_begin(r, r->cmd, 1, 2);
            ipc_put(r, res);
            ipc_put(r, (r->translate >= 2) ? p[r->normal] : 0u);
            ipc_put(r, (r->translate >= 2) ? p[r->normal + 1] : 0u);
            break;
        }
        case 0x0845: { /* GetFormatInfo */
            FsFormatInfo info;
            uint8_t raw[FS_ARCHIVE_PATH_MAX];
            uint32_t res;
            int len = fs_req_path(p, r->normal, r->translate, 0, p[1], p[2], raw, sizeof(raw));
            memset(&info, 0, sizeof(info));
            res = fs_archive_get_format_info(p[0], raw, (uint32_t)(len > 0 ? len : 0),
                                             fs_archive_program_id(), &info);
            if (g_log) printf("[fs] GetFormatInfo id=%u -> %08X\n", p[0], res);
            ipc_begin(r, 0x0845, 5, 0);
            ipc_put(r, res);
            ipc_put(r, res == FS_RESULT_SUCCESS ? info.total_size : 0u);
            ipc_put(r, res == FS_RESULT_SUCCESS ? info.number_directories : 0u);
            ipc_put(r, res == FS_RESULT_SUCCESS ? info.number_files : 0u);
            ipc_put(r, res == FS_RESULT_SUCCESS ? (uint32_t)info.duplicate_data : 0u);
            break;
        }
        case 0x084C: { /* FormatSaveData */
            FsFormatInfo info;
            uint8_t raw[FS_ARCHIVE_PATH_MAX];
            uint32_t res;
            int len = fs_req_path(p, r->normal, r->translate, 0, p[1], p[2], raw, sizeof(raw));
            (void) len;
            info.total_size = p[3] * 512u;
            info.number_directories = p[4];
            info.number_files = p[5];
            info.duplicate_data = (uint8_t)p[8];
            if (p[0] != 4 || p[1] != 1) {
                res = FS_RESULT_INVALID_PATH;
            } else {
                res = fs_archive_format(4, NULL, 0, fs_archive_program_id(), &info);
            }
            if (g_log) printf("[fs] FormatSaveData id=%u type=%u -> %08X\n", p[0], p[1], res);
            fs_reply_result(r, 0x084C, res);
            break;
        }
        case 0x0856: { /* CreateSystemSaveData */
            FsFormatInfo info;
            info.total_size = p[2];
            info.number_directories = p[4];
            info.number_files = p[5];
            info.duplicate_data = (uint8_t)p[8];
            if (g_log) printf("[fs] CreateSystemSaveData high=%08X low=%08X\n", p[0], p[1]);
            fs_reply_result(r, 0x0856, fs_archive_create_system(p[0], p[1], &info));
            break;
        }
        case 0x0861: case 0x0862: {
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            break;
        }
        case 0x0863: {
            ipc_begin(r, 0x0863, 2, 0);
            put_result(r);
            ipc_put(r, 0);
            break;
        }
        default:
            generic_command(r, SVC_FS_USER);
            break;
    }
}

/* Zieladresse eines Antwort-StaticBuffers. Die Kernel-Übersetzung
   (Azahar kernel/hle_ipc.cpp, DescriptorType::StaticBuffer) liest sie aus dem
   Static-Buffer-Bereich hinter dem Kommandopuffer:
   buf[0x40 + 2*id + 1]. Nicht aus den Translate-Parametern des Requests. */
static uint32_t ipc_resp_static_buffer_addr(uint32_t *buf, int id) {
    uint32_t off = 0x40u + 2u * (uint32_t)id;
    return buf[off + 1u];
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
    if (h != NULL && h->dir >= 0) {
        /* Directory-Objekt (Azahar service/fs/directory.cpp): 0x0801 Read,
         * 0x0802 Close.  Der Puffer ist ein MappedBuffer im Translate-Teil. */
        switch (r->cmd) {
            case 0x0801: {
                uint32_t count = p[0];
                uint32_t addr = (r->translate >= 2) ? p[r->normal + 1] : 0u;
                uint32_t got = 0;
                uint32_t res;
                int in_range = count == 0 ||
                    (addr != 0 && g_cpu != NULL &&
                     (uint64_t)(addr & 0x1FFFFFFFu) + (uint64_t)count * 0x228u <=
                         (uint64_t)g_cpu->mem_size);
                res = in_range ? fs_archive_dir_read(h->dir,
                                                     addr ? g_cur_mem + (addr & 0x1FFFFFFFu) : NULL,
                                                     count, &got)
                               : FS_RESULT_INVALID_PATH;
                if (g_log) printf("[fs] DirRead count=%u -> %08X got=%u\n", count, res, got);
                ipc_begin(r, 0x0801, 2, 2);
                ipc_put(r, res);
                ipc_put(r, got);
                ipc_put(r, (r->translate >= 2) ? p[r->normal] : 0u);
                ipc_put(r, (r->translate >= 2) ? p[r->normal + 1] : 0u);
                break;
            }
            case 0x0802: {
                fs_archive_dir_close(h->dir);
                h->dir = -1;
                ipc_begin(r, 0x0802, 1, 0);
                ipc_put(r, 0);
                break;
            }
            default:
                generic_command(r, SVC_FILE);
                break;
        }
        return;
    }
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
            /* Azahar file.cpp (File::Read) parst (u64 offset, u32 length)
               eindeutig; es gibt dort keinen Heuristik-Zweig fuer grosse
               Laengen. Der frueher hier stehende Fallback
               (length > 16 MiB -> offset=0, length=p[0]) haette jeden echten
               Read ueber 16 MiB (z. B. eine 19,7-MB-Moflex am Stueck) still-
               schweigend ab Offset 0 gelesen. */
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
            /* Azahar file.cpp (File::Write): (u64 offset, u32 length) ohne
               Heuristik, siehe Read. */
            uint32_t addr = 0;
            /* Translate-Deskriptor: {size|flags, adresse} -> Adresse ist das ZWEITE Wort. */
            if (r->translate >= 2) addr = p[r->normal + 1];
            uint32_t done = 0;
            if (addr && length && f->fixed_size) {
                /* Azahar FixSizeDiskFile::Write: offset > size -> WriteBeyondEnd,
                 * offset == size -> 0 Bytes, sonst auf die Groesse kappen. */
                if (offset > f->size) {
                    ipc_begin(r, 0x0803, 2, 2);
                    ipc_put(r, FS_RESULT_WRITE_BEYOND_END);
                    ipc_put(r, 0);
                    ipc_put(r, p[r->normal]);
                    ipc_put(r, p[r->normal + 1]);
                    break;
                }
                if (offset == f->size) {
                    ipc_begin(r, 0x0803, 2, 2);
                    ipc_put(r, 0);
                    ipc_put(r, 0);
                    ipc_put(r, p[r->normal]);
                    ipc_put(r, p[r->normal + 1]);
                    break;
                }
                if (offset + length > f->size) length = (uint32_t)(f->size - offset);
            }
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
        case 0x080C: { /* OpenLinkFile: neue Dateisitzung auf denselben Backend */
            /* Azahar file.cpp: MakeBuilder(1,2), Push(Result), PushMoveObjects.
               Die neue Sitzung hat eigene Position/Groesse, teilt aber die
               Datei (hier: eigener Host-Handle auf denselben Pfad). */
            FileSlot *ns = NULL;
            for (int i = 0; i < MAX_FILES; i++) {
                if (!g_files[i].used) { ns = &g_files[i]; break; }
            }
            uint32_t nh = 0;
            if (ns != NULL) {
                memset(ns, 0, sizeof(*ns));
                ns->used = 1;
                ns->size = f->size;
                snprintf(ns->path, sizeof(ns->path), "%s", f->path);
                if (f->fp != NULL && f->path[0] != 0) {
                    ns->fp = fopen(f->path, "r+b");
                    if (ns->fp == NULL) ns->fp = fopen(f->path, "rb");
                }
                nh = (uint32_t)alloc_handle(SVC_FILE, "file-link");
                if (nh != 0 && nh < MAX_HANDLES) g_handles[nh].file = (int)(ns - g_files);
            }
            if (g_log) printf("[file] OpenLinkFile %s -> handle %u\n", f->path, nh);
            ipc_begin(r, 0x080C, 1, 2);
            put_result(r);
            ipc_put_handle(r, nh);
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


/* ---- cfg:u: echte Config-Bloecke einer EUR-Konsole ---------------------- *
 * Referenz: Azahar core/hle/service/cfg/cfg.{h,cpp} und cfg_defaults.cpp.
 * Konsole: Old3DS, Region EUR, Sprache Englisch, Land GB (Code 110).      */
enum {
    CFG_REGION_EUR = 2,
    CFG_LANGUAGE_EN = 1,
    CFG_COUNTRY_GB = 110,
};

static int cfg_write_block(uint32_t block, uint8_t *dst, uint32_t size) {
    memset(dst, 0, size);
    switch (block) {
        case 0x00030001:  /* UserTimeOffsetBlock */
        case 0x000B0003:  /* LatitudeLongitudeBlock */
        case 0x00130000:  /* DebugModeBlock */
        case 0x00150000:  /* ClockSequenceBlock */
            break;
        case 0x00050001:  /* BacklightControls { power_saving, brightness } */
            if (size >= 2) dst[1] = 2;
            break;
        case 0x00050005: { /* StereoCameraSettings, 8 floats (Azahar-Default) */
            static const float cam[8] = {
                62.0f, 289.0f, 76.80000305175781f, 46.08000183105469f,
                10.0f, 5.0f, 55.58000183105469f, 21.56999969482422f,
            };
            uint32_t n = (size < sizeof(cam)) ? size : (uint32_t)sizeof(cam);
            memcpy(dst, cam, n);
            break;
        }
        case 0x00070001:  /* SoundOutputModeBlock: stereo */
            if (size >= 1) dst[0] = 1;
            break;
        case 0x00090000:  /* ConsoleUniqueID1 */
        case 0x00090001: { /* ConsoleUniqueID2: fester, deterministischer Wert */
            static const uint8_t id[8] = {1, 0, 0, 0, 1, 0, 0, 0};
            uint32_t n = (size < sizeof(id)) ? size : (uint32_t)sizeof(id);
            memcpy(dst, id, n);
            break;
        }
        case 0x000A0000: { /* UsernameBlock: 10 UTF-16-Zeichen, zero, ng_word */
            static const char name[] = "LAYTON";
            uint32_t i;
            for (i = 0; name[i] && i < 10 && (i + 1) * 2 <= size; i++)
                dst[i * 2] = (uint8_t)name[i];
            break;
        }
        case 0x000A0001:  /* BirthdayBlock { month, day } */
            if (size >= 2) { dst[0] = 11; dst[1] = 7; }
            break;
        case 0x000A0002:  /* LanguageBlock: Englisch */
            if (size >= 1) dst[0] = (uint8_t)CFG_LANGUAGE_EN;
            break;
        case 0x000B0000:  /* CountryInfoBlock { unknown[2], state, country } */
            if (size >= 4) { dst[2] = 2; dst[3] = (uint8_t)CFG_COUNTRY_GB; }
            break;
        case 0x000B0001:  /* CountryNameBlock: 16 Slots a 0x40 Byte UTF-16 */
        case 0x000B0002: { /* StateNameBlock */
            static const char country[] = "United Kingdom";
            static const char state[] = "England";
            const char *text = (block == 0x000B0001) ? country : state;
            uint32_t slot, i;
            for (slot = 0; slot < 16; slot++) {
                uint32_t base = slot * 0x40;
                if (base >= size) break;
                for (i = 0; text[i] && i < 0x1F; i++) {
                    if (base + i * 2 + 1 >= size) break;
                    dst[base + i * 2] = (uint8_t)text[i];
                }
            }
            break;
        }
        case 0x000D0000:  /* EULAVersionBlock { minor, major } */
            if (size >= 2) { dst[0] = 0x7F; dst[1] = 0x7F; }
            break;
        case 0x000F0004:  /* ConsoleModelBlock: Old3DS */
            if (size >= 1) dst[0] = 0;
            break;
        case 0x00110000:  /* SystemSetupRequiredBlock: Setup erledigt */
            if (size >= 1) dst[0] = 1;
            break;
        default:
            /* Unbekannter Block: Azahar cfg.cpp GetConfigBlockPointer ->
               ResultNotFound (0xD9010004), kein Erfolg mit Nullen. */
            return 0;
    }
    return 1;
}

static void cfg_command(Ipc *r, uint32_t *p) {
    switch (r->cmd) {
        case 0x0001: { /* GetConfig(size, block_id) + Puffer-Deskriptor */
            uint32_t size = p[0];
            uint32_t block = p[1];
            uint32_t addr = ipc_out_buffer(p, r->normal, r->translate);
            uint32_t limit = size;

            /* Geschuetzte Puffergroesse aus dem Translate-Deskriptor lesen,
               damit nie hinter den Gastpuffer geschrieben wird. */
            if (r->translate >= 2) {
                uint32_t d0 = p[r->normal];
                uint32_t type = d0 & 0xFu;
                uint32_t cap = 0;
                if (type == 0x2u) cap = d0 >> 14;        /* StaticBuffer */
                else if (d0 & 0x8u) cap = d0 >> 4;       /* MappedBuffer */
                if (cap != 0 && limit > cap) limit = cap;
            }
            if (addr == 0) limit = 0;
            if (limit > 0x1000) limit = 0x1000;
            {
                uint32_t result = 0;
                if (limit != 0 &&
                    !cfg_write_block(block, g_cur_mem + (addr & 0x1FFFFFFFu), limit)) {
                    /* Azahar GetConfigBlockPointer: unbekannter Block ohne
                       Default -> ResultNotFound (0xD9010004). */
                    printf("[cfg] GetConfig unbekannter Block 0x%08X size=%u -> 0xD9010004\n",
                           block, size);
                    fflush(stdout);
                    result = 0xD9010004u;
                }

                if (g_log) printf("[cfg] GetConfig block=0x%08X size=%u buf=0x%08X\n", block, size, addr);
                /* Azahar (cfg.cpp): MakeBuilder(1,2), Push(result), PushMappedBuffer. */
                ipc_begin(r, 0x0001, 1, 2);
                ipc_put(r, result);
                for (int i = 0; i < 2; i++) ipc_put(r, (r->translate >= 2) ? p[r->normal + i] : 0u);
            }
            break;
        }
        case 0x0002:   /* GetRegion */
        case 0x0406:   /* cfg:s GetRegion */
        case 0x0816: { /* cfg:i GetRegion (SecureInfo) */
            ipc_begin(r, r->cmd, 2, 0);
            put_result(r);
            ipc_put(r, CFG_REGION_EUR);   /* EUR, auch in SecureInfo so hinterlegt */
            break;
        }
        case 0x0003: { /* GetTransferableId(salt): Result + u64 */
            /* Azahar: MakeBuilder(3,0), Push(result, low, high).
             * p[0] vor der Antwort puffern: ipc_put() schreibt in denselben
             * Kommandopuffer, den p zeigt (Diagnose las sonst 0). */
            uint32_t salt = (r->normal >= 1) ? p[0] : 0u;
            ipc_begin(r, 0x0003, 3, 0);
            put_result(r);
            ipc_put(r, 0x12345678u);    /* low  */
            ipc_put(r, 0x00000000u);    /* high */
            if (g_log) printf("[cfg] GetTransferableId salt=0x%X -> ok\n", salt);
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

/* DSP-Interrupt-Ereignisse: RegisterInterruptEvents (cmd 0x0015) bindet pro
 * (InterruptType, Pipe) ein Gast-Ereignis (Azahar dsp_dsp.cpp GetInterruptEvent). */
#define DSP_INT_TYPES 3
#define DSP_NUM_PIPES 8
static uint32_t g_dsp_events[DSP_INT_TYPES][DSP_NUM_PIPES];

/* DSP-Pipe-Daten und Semaphore-Event. Azahar haelt pro Pipe einen Puffer
 * (audio_core/hle/hle.cpp PipeRead/PipeWrite) und ein OneShot-Semaphore-Event
 * (service/dsp/dsp_dsp.cpp), das nicht vorab signalisiert wird. */
#define DSP_PIPE_CAP 0x10000
static uint8_t g_dsp_pipe[DSP_NUM_PIPES][DSP_PIPE_CAP];
static uint32_t g_dsp_pipe_len[DSP_NUM_PIPES];
static uint32_t g_dsp_semaphore_ev = 0;

static void dsp_signal_events(int type, int pipe) {
    if (type < 0 || type >= DSP_INT_TYPES || pipe < 0 || pipe >= DSP_NUM_PIPES)
        return;
    {
        uint32_t ev = g_dsp_events[type][pipe];
        Handle *hh = get_handle(ev);
        if (hh != NULL) {
            hh->signaled = 1;
            if (g_wstat) {
                long long fd = audio_dsp_last_flush_delay_us();
                hh->sig_us = host_us();
                if (fd > 10000)
                    printf("[wstat] t=%llu dsp-zustellung %lld ms spaet\n",
                           (unsigned long long)((host_ns() - g_boot_ns) / 1000000ull), fd / 1000);
            }
        }
    }
}

static void dsp_interrupt_cb(int type, int pipe) {
    dsp_signal_events(type, pipe);
}

static void dsp_pipe_append(int ch, const void *src, uint32_t n) {
    if (ch < 0 || ch >= DSP_NUM_PIPES) return;
    if (n > DSP_PIPE_CAP - g_dsp_pipe_len[ch]) n = DSP_PIPE_CAP - g_dsp_pipe_len[ch];
    if (n == 0) return;
    memcpy(g_dsp_pipe[ch] + g_dsp_pipe_len[ch], src, n);
    g_dsp_pipe_len[ch] += n;
}

static uint32_t dsp_pipe_read(int ch, uint8_t *dst, uint32_t n) {
    if (ch < 0 || ch >= DSP_NUM_PIPES) return 0;
    if (n > g_dsp_pipe_len[ch]) n = g_dsp_pipe_len[ch];
    if (n == 0) return 0;
    memcpy(dst, g_dsp_pipe[ch], n);
    memmove(g_dsp_pipe[ch], g_dsp_pipe[ch] + n, g_dsp_pipe_len[ch] - n);
    g_dsp_pipe_len[ch] -= n;
    return n;
}

/* Azahar audio_core/hle/hle.cpp AudioPipeWriteStructAddresses: 15 DSP-DRAM-
 * Adressen (shared_memory.h), Basis 0x8000, danach Pipe-Interrupt. */
static void dsp_audio_struct_addresses(void) {
    static const uint16_t addrs[15] = {
        0xBFFF, 0x9E92, 0x8680, 0xA792, 0x9430, 0x8400, 0x8540, 0x9492,
        0x8710, 0x8410, 0xA912, 0xAA12, 0xAAD2, 0xAC52, 0xAC5C,
    };
    uint8_t buf[2 + 15 * 2];
    buf[0] = 15; buf[1] = 0;
    for (int i = 0; i < 15; i++) {
        buf[2 + i * 2] = (uint8_t)(addrs[i] & 0xFF);
        buf[2 + i * 2 + 1] = (uint8_t)(addrs[i] >> 8);
    }
    /* Azahar AudioPipeWriteStructAddresses haengt an die Pipe an; geleert wird
     * sie nur bei Initialize/Wakeup (ResetPipes), das macht WriteProcessPipe. */
    dsp_pipe_append(2, buf, sizeof(buf));   /* DspPipe::Audio */
    dsp_signal_events(2, 2);                /* InterruptType::Pipe, Audio */}

static void dsp_command(Ipc *r, uint32_t *p) {
    /* Antwortlayouts nach Azahar src/core/hle/service/dsp/dsp_dsp.cpp. */
    switch (r->cmd) {
        case 0x0001: { /* RecvData: Result + Wert (Azahar DspHle::RecvData) */
            ipc_begin(r, 0x0001, 2, 0);
            put_result(r);
            ipc_put(r, audio_dsp_recv_data());
            break;
        }
        case 0x0002: { /* RecvDataIsReady: Result + bool */
            ipc_begin(r, 0x0002, 2, 0);
            put_result(r);
            ipc_put(r, (uint32_t)audio_dsp_recv_data_ready());
            break;
        }
        case 0x0007: { /* SetSemaphore: Result
                          Azahar DspHle::SetSemaphore ist ein No-op; die
                          Pipe-Interrupts kommen je Audio-Frame aus
                          runtime/audio/dsp_hle.c. */
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            break;
        }
        case 0x0017: { /* SetSemaphoreMask: Result */
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            break;
        }
        case 0x000C: { /* ConvertProcessAddressFromDspDram: Result + ARM-Adresse */
            uint32_t dsp_addr = p[0];
            /* Azahar dsp_dsp.cpp: (addr << 1) + DSP_RAM_VADDR (0x1FF00000) + 0x40000 */
            uint32_t arm = (dsp_addr << 1) + 0x1FF00000u + 0x40000u;
            ipc_begin(r, 0x000C, 2, 0);
            put_result(r);
            ipc_put(r, arm);
            if (g_log) printf("[dsp] ConvertProcessAddress 0x%X -> 0x%X\n", dsp_addr, arm);
            break;
        }
        case 0x000D: { /* WriteProcessPipe: Result
                          Daten in den Pipe-Puffer (Azahar PipeWrite). */
            uint32_t ch = p[0];
            uint32_t size = p[1];
            uint32_t addr = 0;
            if (r->translate >= 2) addr = p[r->normal + 1];
            if (addr != 0 && size != 0 && g_cpu != NULL &&
                (addr & 0x1FFFFFFFu) + size <= g_cpu->mem_size) {
                const uint8_t *src = g_cur_mem + (addr & 0x1FFFFFFFu);
                if (ch == 2) {
                    /* Audio-Pipe: StateChange (Initialize/Shutdown/Wakeup/
                       Sleep) an das DSP-HLE-Modul; die Struktur-Adressen
                       schreibt der Callback (Azahar PipeWrite).  Initialize
                       und Wakeup leeren alle Pipe-Puffer (ResetPipes). */
                    if (size == 4 && (src[0] == 0 || src[0] == 2)) {
                        for (int i = 0; i < DSP_NUM_PIPES; i++) g_dsp_pipe_len[i] = 0;
                    }
                    audio_dsp_pipe_write(src, size);
                } else {
                    dsp_pipe_append((int)ch, src, size);
                    dsp_signal_events(2, (int)ch);
                }
            }
            ipc_begin(r, 0x000D, 1, 0);
            put_result(r);
            if (g_log) printf("[dsp] WriteProcessPipe ch=%u size=%u addr=%08X\n",
                              ch, size, addr);
            break;
        }
        case 0x000E: { /* ReadPipe: Result + StaticBuffer (echte Pipe-Daten) */
            uint32_t ch = p[0];
            uint32_t size = p[1];
            uint32_t dsc_addr = ipc_resp_static_buffer_addr(r->buf, 0);
            uint32_t n = 0;
            if (size != 0 && dsc_addr != 0 && g_cpu != NULL &&
                (dsc_addr & 0x1FFFFFFFu) + size <= g_cpu->mem_size) {
                n = dsp_pipe_read((int)ch, g_cur_mem + (dsc_addr & 0x1FFFFFFFu), size);
            }
            ipc_begin(r, 0x000E, 1, 2);
            put_result(r);
            ipc_put(r, 0x2u | ((n & 0x3FFFFu) << 14));
            ipc_put(r, dsc_addr);
            if (g_log) printf("[dsp] ReadPipe ch=%u size=%u -> %u\n", ch, size, n);
            break;
        }
        case 0x000F: { /* GetPipeReadableSize: Result + u16 */
            uint32_t ch = p[0];
            uint32_t avail = (ch < DSP_NUM_PIPES) ? g_dsp_pipe_len[ch] : 0;
            ipc_begin(r, 0x000F, 2, 0);
            put_result(r);
            ipc_put(r, avail > 0xFFFFu ? 0xFFFFu : avail);
            break;
        }
        case 0x0010: { /* ReadPipeIfPossible: Result + Groesse + StaticBuffer */
            uint32_t ch = p[0];
            uint32_t size = p[2];
            uint32_t dsc_addr = ipc_resp_static_buffer_addr(r->buf, 0);
            uint32_t n = 0;
            /* Azahar: nur lesen, wenn genug Daten da sind. */
            if (ch < DSP_NUM_PIPES && size != 0 && dsc_addr != 0 &&
                g_dsp_pipe_len[ch] >= size && g_cpu != NULL &&
                (dsc_addr & 0x1FFFFFFFu) + size <= g_cpu->mem_size) {
                n = dsp_pipe_read((int)ch, g_cur_mem + (dsc_addr & 0x1FFFFFFFu), size);
            }
            ipc_begin(r, 0x0010, 2, 2);
            put_result(r);
            ipc_put(r, n);
            ipc_put(r, 0x2u | ((n & 0x3FFFFu) << 14));
            ipc_put(r, dsc_addr);
            if (g_log) printf("[dsp] ReadPipeIfPossible ch=%u size=%u -> %u\n", ch, size, n);
            break;
        }
        case 0x0011: { /* LoadComponent: Result + bool + MappedBuffer-Echo */
            uint32_t dsc_flags = 0, dsc_addr = 0;
            /* Parameter vor der Antwort puffern (ipc_put() ueberschreibt p). */
            uint32_t size = (r->normal >= 1) ? p[0] : 0u;
            if (r->translate >= 2) {
                dsc_flags = p[r->normal];
                dsc_addr = p[r->normal + 1];
            }
            ipc_begin(r, 0x0011, 2, 2);
            put_result(r);
            ipc_put(r, 1);          /* true = geladen */
            ipc_put(r, dsc_flags);
            ipc_put(r, dsc_addr);
            if (g_log) printf("[dsp] LoadComponent size=0x%X -> ok\n", size);
            break;
        }
        case 0x0012: { /* UnloadComponent: Result */
            ipc_begin(r, 0x0012, 1, 0);
            put_result(r);
            break;
        }
        case 0x0013:   /* FlushDataCache */
        case 0x0014:   /* InvalidateDataCache */
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            break;
        case 0x0015: { /* RegisterInterruptEvents: Result
                          Params (Azahar): interrupt(type), channel(pipe),
                          CopyHandle(event). */
            uint32_t type = p[0];
            uint32_t chan = p[1];
            uint32_t ev = (r->translate >= 2) ? p[r->normal + 1] : 0u;
            if (type < DSP_INT_TYPES && chan < DSP_NUM_PIPES) {
                g_dsp_events[type][chan] = ev;
                if (g_log) printf("[dsp] RegisterInterruptEvents type=%u pipe=%u ev=%u\n",
                                  type, chan, ev);
            }
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            break;
        }
        case 0x0016: { /* GetSemaphoreEventHandle: Result + CopyHandle(Event)
                          Azahar: OneShot-Event, wird hier nicht vorab
                          signalisiert. */
            if (g_dsp_semaphore_ev == 0)
                g_dsp_semaphore_ev = new_event_handle("dsp-sem", 0);
            ipc_begin(r, 0x0016, 1, 2);
            put_result(r);
            ipc_put_handle_copy(r, g_dsp_semaphore_ev);
            if (g_log) printf("[dsp] GetSemaphoreEventHandle -> %u\n", g_dsp_semaphore_ev);
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

/* GSP-Service: Kommandonummern wie auf echter Hardware / in Azahar
 * (gsp_gpu.cpp): 0x0001-0x0003 WriteHWRegs*, 0x0004 ReadHWRegs,
 * 0x0005 SetBufferSwap, 0x0006 SetCommandList, 0x0007 RequestDma,
 * 0x000C TriggerCmdReqQueue, 0x0013 RegisterInterruptRelayQueue,
 * 0x0018 ImportDisplayCaptureInfo. */

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

/* GPU-Hardware-Interrupts (PSC0/PSC1 aus MemoryFill, PPF aus
 * DisplayTransfer/TextureCopy, P3D aus dem Kommandostrom) kommen mit der
 * GSP-Interrupt-ID aus runtime/gpu (Azahar GPU::SetInterruptHandler ->
 * GSP_GPU::SignalInterrupt). */
static void gsp_pica_irq(int id) {
    if (id < 0 || id > GSP_IRQ_DMA) return;
    if (t_gpu_section) {
        gpu_irq_push(id);
        return;
    }
    gsp_signal_interrupt(id);
}


/* GSP wendet beim VBlank die vom Spiel im Shared-Memory gesetzte
 * FrameBufferInfo an (3dbrew "Framebuffer Info", Azahar
 * GSP_GPU::SignalInterrupt -> GetFrameBufferInfo + GPU::SetBufferSwap).
 * dirty-Flag im Update-Header: Bit0; Info-Eintrag = index & 1. */
static void gsp_vblank_fb_update(ArmCpu *c, int tid) {
    int screen;
    int guest_updated = 0;
    if (c == NULL) return;
    gsp_fb_init();
    for (screen = 0; screen < 2; screen++) {
        uint32_t base = (GSP_SHMEM_BASE + 0x200u +
                         (uint32_t)(2 * tid + screen) * 0x40u) & 0x1FFFFFFFu;
        uint32_t info, idx, active = 0, left = 0, right = 0, st = 0, fmt = 0, shown = 0;
        uint8_t *fbu;
        if (base + 0x40u > c->mem_size) continue;
        fbu = c->mem + base;
        if ((fbu[1] & 1u) == 0u) continue;   /* nicht aktualisiert */
        guest_updated = 1;
        idx = fbu[0] & 1u;
        /* Azahar gsp_gpu.h: FrameBufferUpdate = {u8 index, u8 dirty, u16 pad,
           FrameBufferInfo[2] (je 0x1C Byte) ab +4} -> [1] bei +0x20. */
        info = base + 4u + idx * 0x1Cu;
        memcpy(&active, c->mem + info + 0u, 4);
        memcpy(&left, c->mem + info + 4u, 4);
        memcpy(&right, c->mem + info + 8u, 4);
        memcpy(&st, c->mem + info + 0xCu, 4);
        memcpy(&fmt, c->mem + info + 0x10u, 4);
        memcpy(&shown, c->mem + info + 0x14u, 4);
        fbu[1] = 0;
        AcquireSRWLockExclusive(&g_gsp_fb_srw);
        gsp_fb_regs_apply(&g_gsp_fb_regs[screen], screen, active, left, right,
                          st, fmt, shown);
        ReleaseSRWLockExclusive(&g_gsp_fb_srw);
        if (left != 0u) {
            gpu_cs_enter();
            gpu_set_fb_size(left, (screen == 1) ? 320 : 400, 240);
            gpu_cs_leave();
            if (g_log)
                printf("[gsp] SetBufferSwap screen=%d fb=0x%08X stride=0x%X fmt=%X (VBlank)\n",
                       screen, left, st, fmt);
        }
    }
    /* Anzeige-Wechsel genau im VBlank: der sichtbare Puffer (active_fb aus
     * shown_fb) wird jetzt uebernommen. */
    AcquireSRWLockExclusive(&g_gsp_fb_srw);
    gsp_fb_latch_locked(0);
    gsp_fb_latch_locked(1);
    ReleaseSRWLockExclusive(&g_gsp_fb_srw);
    if (guest_updated) g_gsp_guest_frames++;
}


/* Verarbeitet den GSP-Shared-Memory-Kommandopuffer (Azahar-Layout) und
   fuehrt ProcessCommandList-Eintraege (id 0x01) als PICA-Liste aus. */



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





/* Farb-/Speichertransfers (DisplayTransfer, TextureCopy, MemoryFill) und
 * das GPU-MMIO-Registerfeld liegen in runtime/gpu/gpu.c (Azahar
 * SwBlitter/GPU::ReadReg/WriteReg). */

/* Verarbeitet den GX-Kommandopuffer des Threads mit GPU-Recht
 * (Azahar GSP_GPU::TriggerCmdReqQueue). Header: Index u8, Anzahl u8,
 * Status u8, ShouldStop u8; Kommandos ab +0x20 je 0x20 Byte. */
/* GPU-Worker: einen GX-Befehl ausfuehren (ohne Gast-Sperre, unter g_gpu_cs).
 * Interrupts nur vormerken (t_gpu_section / gpu_irq_push). */
static void gx_exec(const GxOp *op) {
    ArmCpu *c = g_cpu;
    uint32_t w[8];
    uint32_t id;
    memcpy(w, op->w, sizeof(w));
    id = w[0] & 0xFFu;
    if (c == NULL) return;
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
                gpu_irq_push(GSP_IRQ_DMA);
                break;
            }
            case 0x01: { /* SubmitCmdList (Azahar GPU::Execute) */
                uint32_t addr = w[1], size = w[2];
                /* GSP reicht eine Virtualadresse; PICA-Register halten wie
                   auf echter Hardware physische Adressen (Azahar
                   GPU::Execute -> VirtualToPhysicalAddress). */
                uint32_t paddr = gpu_pica_virt_to_phys(addr);
                /* Azahar: eine Laenge von 0 ist zulaessig (er schreibt die
                   Register und fuehrt nichts aus); nur ein nicht abbildbarer
                   Bereich meldet P3D. */
                if (addr != 0u &&
                    (size == 0u ||
                     (uint64_t)gpu_pica_phys_to_guest(paddr) + size <= c->mem_size)) {
                    unsigned long long d0 = gpu_pica_draw_count();
                    uint64_t gt0 = g_gtime ? host_us() : 0;
                    int nops = gpu_gsp_submit_cmdlist_cmd(c, paddr, size);
                    if (g_gtime) {
                        g_gtime_us += host_us() - gt0;
                        g_gtime_lists++;
                    }
                    /* Frontier-M3: Liste mit Draws ([gpu] list ... draws=N). */
                    printf("[gpu] list addr=%08X paddr=%08X size=%X ops=%d draws=%llu\n",
                           addr, paddr, size, nops, gpu_pica_draw_count() - d0);
                } else {
                    /* Azahar GPU::SubmitCmdList: ungueltige Liste -> P3D. */
                    printf("[gspq] ProcessCommandList ungueltig %08X/%X\n", addr, size);
                    gpu_irq_push(GSP_IRQ_P3D);
                }
                break;
            }
            case 0x02: { /* MemoryFill */
                gpu_gsp_memory_fill_cmd(c, w);
                break;
            }
            case 0x03: { /* DisplayTransfer */
                gpu_gsp_display_transfer_cmd(c, w);
                break;
            }
            case 0x04: { /* TextureCopy */
                gpu_gsp_texture_copy_cmd(c, w);
                break;
            }
            default:
                break;
    }
}

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
            case 0x00: /* RequestDma */
            case 0x01: /* SubmitCmdList */
            case 0x02: /* MemoryFill */
            case 0x03: /* DisplayTransfer */
            case 0x04: /* TextureCopy */
                gx_run_inline(c, w);
                qops++;
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

        /* Azahar GSP_GPU::TriggerCmdReqQueue: ein fehlgeschlagenes Kommando
         * legt seinen Result-Code in unk[0] (Header+4) ab und setzt
         * STATUS_CMD_FAILED (0x80); das Stop-Bit des Kommandos setzt
         * STATUS_STOPPED (0x1) und hat danach Vorrang. */
        if (failed) {
            gsp_wr32(c, base + 4u, 0xE0C02BF4u);   /* ResultUnknownGxCommand */
            hdr = gsp_rd32(c, base) & 0xFF00FFFFu;
            gsp_wr32(c, base, hdr | (0x80u << 16));
        }
        if (stop_cmd) {
            hdr = gsp_rd32(c, base) & 0xFF00FFFFu;
            gsp_wr32(c, base, hdr | (1u << 16));
        }
    }
    return qops;
}

/* Request-StaticBuffer aus dem Translate-Teil: Deskriptor
 * (0x2 | id<<10 | size<<14, Gastadresse). Azahar gsp_gpu.cpp liest den
 * Puffer per rp.PopStaticBuffer(); die Groesse stammt aus dem Deskriptor
 * (der size-Parameter der Kommandos wird von echter Hardware ignoriert). */
static uint32_t gsp_req_buf_flags(const Ipc *r, const uint32_t *p, int id) {
    int off = r->normal + id * 2;
    if (r->translate < id * 2 + 2) return 0u;
    return p[off];
}

static uint32_t gsp_req_buf_addr(const Ipc *r, const uint32_t *p, int id) {
    int off = r->normal + id * 2;
    if (r->translate < id * 2 + 2) return 0u;
    return p[off + 1];
}

static void gsp_command(Ipc *r, uint32_t *p) {
    /* Kommandonummern wie auf echter Hardware / in Azahar (gsp_gpu.cpp),
     * NICHT die alte Citra-Nummerierung. */
    switch (r->cmd) {
        case 0x0001: case 0x0002: case 0x0003: { /* WriteHWRegs* */
            /* Azahar GSP::WriteHWRegs/WriteHWRegsWithMask/WriteHWRegRepeat:
               reg_addr = p[0], size = p[1] (von Hardware ignoriert, es gilt
               die StaticBuffer-Groesse), Daten als erster Translate-Puffer,
               Maske als zweiter. Geschrieben wird ueber das GPU-Registerfeld
               (runtime/gpu/gpu.c), Trigger-Bits starten die Operation. */
            uint32_t addr = (r->normal >= 1) ? p[0] : 0u;
            uint32_t size = gsp_req_buf_flags(r, p, 0) >> 14;
            uint32_t src = gsp_req_buf_addr(r, p, 0);
            uint32_t result = 0;
            uint32_t i;

            if (g_gsp_right_thread < 0) {
                result = 0xD8202A06u;               /* NoGpuRight */
            } else if ((addr & 3u) != 0u || addr >= 0x420000u) {
                result = 0xE0E02A01u;               /* OutOfRangeOrMisaligned */
            } else if (size > 0x80u) {
                result = 0xE0E02BECu;               /* InvalidSize */
            } else if ((size & 3u) != 0u) {
                result = 0xE0E02BF2u;               /* MisalignedSize */
            } else if (size != 0u &&
                       (src == 0u || g_cpu == NULL ||
                        (uint64_t)(src & 0x1FFFFFFFu) + size > g_cpu->mem_size)) {
                result = 0xE0E02BF5u;               /* InvalidBuffer */
            }

            if (g_log) {
                uint32_t v0 = 0u, m0 = 0xFFFFFFFFu;
                if (result == 0u && size >= 4u) {
                    memcpy(&v0, g_cpu->mem + ((src + 0u) & 0x1FFFFFFFu), 4);
                    if (r->cmd == 0x0002) {
                        uint32_t ma = gsp_req_buf_addr(r, p, 1);
                        uint32_t ms = gsp_req_buf_flags(r, p, 1) >> 14;
                        if (ma != 0u && ms >= 4u)
                            memcpy(&m0, g_cpu->mem + ((ma + 0u) & 0x1FFFFFFFu), 4);
                    }
                }
                printf("[gsp] WriteHWRegs cmd=0x%04X addr=0x%X size=%u value=0x%08X mask=0x%08X res=0x%08X\n",
                       r->cmd, addr, size, v0, m0, result);
            }

            if (result == 0u && g_cpu != NULL) {
                gpu_cs_enter();
                for (i = 0u; i + 4u <= size; i += 4u) {
                    uint32_t val = 0u;
                    memcpy(&val, g_cpu->mem + ((src + i) & 0x1FFFFFFFu), 4);
                    if (r->cmd == 0x0003) {
                        /* WriteHWRegRepeat (Azahar): jeder Wert geht an
                         * dieselbe Registeradresse. */
                        gpu_gsp_write_reg(g_cpu, 0x1EB00000u + addr, val);
                    } else {
                        uint32_t mask = 0xFFFFFFFFu;
                        if (r->cmd == 0x0002) {
                            uint32_t ma = gsp_req_buf_addr(r, p, 1);
                            uint32_t ms = gsp_req_buf_flags(r, p, 1) >> 14;
                            mask = 0u;
                            if (ma != 0u && ms >= i + 4u)
                                memcpy(&mask, g_cpu->mem + ((ma + i) & 0x1FFFFFFFu), 4);
                        }
                        gpu_gsp_write_reg_masked(g_cpu, 0x1EB00000u + addr + i,
                                                 val, mask);
                    }
                }
                gpu_cs_leave();
            }
            ipc_begin(r, r->cmd, 1, 0);
            ipc_put(r, result);
            break;
        }
        case 0x0004: { /* ReadHWRegs: Result + StaticBuffer */
            /* Azahar (gsp_gpu.cpp): MakeBuilder(1,2), Push(result),
               PushStaticBuffer(buffer, 0). Zieladresse ist der Static-Buffer-
               Bereich des Kommandopuffers (Kernel-Translation). */
            uint32_t addr = (r->normal >= 1) ? p[0] : 0u;
            uint32_t in_size = (r->normal >= 2) ? p[1] : 0u;
            uint32_t size = in_size > 0x80u ? 0x80u : in_size;
            uint32_t dsc_addr = ipc_resp_static_buffer_addr(r->buf, 0);
            uint32_t dsc_flags;
            uint32_t result = 0;
            uint32_t i;

            if (dsc_addr == 0u && r->translate >= 2 &&
                p[r->normal + 1] != 0u) {
                dsc_addr = p[r->normal + 1];
            }
            dsc_flags = 0x2u | ((size & 0x3FFFFu) << 14);
            if (g_gsp_right_thread < 0) {
                result = 0xD8202A06u;
            } else if ((addr & 3u) != 0u || addr >= 0x420000u) {
                result = 0xE0E02A01u;
            } else if ((size & 3u) != 0u) {
                result = 0xE0E02BF2u;
            }
            if (result == 0u && dsc_addr >= 0x08000000u && g_cpu != NULL &&
                (uint64_t)(dsc_addr & 0x1FFFFFFFu) + size <= g_cpu->mem_size) {
                for (i = 0u; i < size; i += 4u) {
                    uint32_t v;
                    gpu_cs_enter();   /* wartet auf eine laufende GPU-Operation */
                    v = gpu_gsp_read_reg(g_cpu, 0x1EB00000u + addr + i);
                    gpu_cs_leave();
                    memcpy(g_cpu->mem + ((dsc_addr + i) & 0x1FFFFFFFu), &v, 4);
                }
            }
            ipc_begin(r, 0x0004, 1, 2);
            ipc_put(r, result);
            ipc_put(r, dsc_flags);
            ipc_put(r, dsc_addr);
            break;
        }
        case 0x0005: { /* SetBufferSwap(screen_id, FrameBufferInfo) */
            /* Azahar (gsp_gpu.cpp SetBufferSwap -> GPU::SetBufferSwap):
               FrameBufferInfo = {active_fb, address_left, address_right,
               stride, format, shown_fb, unknown}. Die Register werden sofort
               gesetzt; sichtbar wird der Puffer (active_fb aus shown_fb) erst
               beim naechsten VBlank (gsp_fb_latch_locked). Fuer screen_id >= 2
               gibt es trotzdem ein Result (echte Hardware ignoriert es). */
            uint32_t screen = p[0];
            uint32_t active = (r->normal >= 2) ? p[1] : 0u;
            uint32_t left = (r->normal >= 3) ? p[2] : 0u;
            uint32_t right = (r->normal >= 4) ? p[3] : 0u;
            uint32_t st = (r->normal >= 5) ? p[4] : 0u;
            uint32_t fmt = (r->normal >= 6) ? p[5] : 0u;
            uint32_t shown = (r->normal >= 7) ? p[6] : 0u;
            if (screen < 2u) {
                gsp_fb_init();
                if (left != 0u) {
                    AcquireSRWLockExclusive(&g_gsp_fb_srw);
                    gsp_fb_regs_apply(&g_gsp_fb_regs[screen], (int)screen, active,
                                      left, right, st, fmt, shown);
                    ReleaseSRWLockExclusive(&g_gsp_fb_srw);
                    gpu_cs_enter();
                    gpu_set_fb_size(gsp_fb_regs_visible_left(&g_gsp_fb_regs[screen]),
                                    (screen == 1u) ? 320 : 400, 240);
                    gpu_cs_leave();
                }
                if (g_log) printf("[gsp] SetBufferSwap screen=%u fb=0x%08X stride=0x%X fmt=%X active=%u shown=%u\n",
                                  screen, left, st, fmt, active, shown);
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
        case 0x0008: case 0x0009: case 0x000A: { /* Cache / Events */
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            break;
        }
        case 0x000B: { /* SetLcdForceBlack -> Azahar GPU::SetColorFill */
            uint32_t enable = (r->normal >= 1) ? p[0] : 0u;
            gpu_gsp_set_lcd_color_fill(g_cpu, enable != 0u);
            ipc_begin(r, 0x000B, 1, 0);
            put_result(r);
            break;
        }
        case 0x000C: { /* TriggerCmdReqQueue -> Shared-Memory-Kommandopuffer */
            /* Azahar (gsp_gpu.cpp): base = shmem + 0x800 + thread_id*0x200,
               Header {index:8, number_commands:8, status:8, should_stop:8},
               Command[15] ab +0x20 (je 0x20 B). id 0x01 = SubmitCmdList
               (Adresse +0x04, Laenge +0x08). */
            /* SetCommandList ist in Azahar ein Stub; die zuletzt gesetzte
               Liste wird hier nicht erneut abgespielt. */
            (void)gsp_process_cmdqueue(g_cpu);
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
            ipc_put(r, gsp_fb_regs_visible_left(&g_gsp_fb_regs[0]));
            ipc_put(r, 0); ipc_put(r, 0); ipc_put(r, 0);
            ipc_put(r, gsp_fb_regs_visible_left(&g_gsp_fb_regs[1]));
            ipc_put(r, 0); ipc_put(r, 0); ipc_put(r, 0);
            break;
        }
        default:
            generic_command(r, SVC_GSP_GPU);
            break;
    }
}

/* Y2R: vollstaendige Konfiguration wie Azahar src/core/hle/service/cam/y2r_u.*,
 * Umwandlung in runtime/platform/y2r.h (Azahar core/hw/y2r.cpp).
 *  - DriverInitialize setzt die Azahar-Startwerte (YUV422_Indiv8, RGBA8,
 *    keine Rotation, Linear, Koeffizienten 0, 1024 Zeilen/Spalten, alpha 0)
 *    und loescht das completion_event (OneShot).
 *  - StartConversion rechnet synchron in die per SetSendingY/U/V/YUYV und
 *    SetReceiving gesetzten Gast-Puffer und plant danach die Fertigmeldung
 *    (Azahar MinY2RDelay = 50000 Zyklen ≈ 186 us bei 268 MHz).
 *  - IsFinished* sind wie in Azahar mit 1 belegt (stub).
 *  - GetPackageParameter liefert die 12-Byte-ConversionParameters
 *    (MakeBuilder(4,0)); Azahar selbst schiebt dort versehentlich die ganze
 *    Struktur in den Kommandopuffer. */
static Y2rConversion g_y2r;
static uint32_t g_y2r_event = 0;
static int g_y2r_interrupt_enabled = 0;
static int g_y2r_spacial_dithering = 0;
static int g_y2r_temporal_dithering = 0;
static uint16_t g_y2r_dither[16];
static uint8_t g_y2r_std_coefficient = 0;
static int g_y2r_busy = 0;
static uint64_t g_y2r_due_us = 0;

static void y2r_poll(void) {
    if (g_y2r_busy && host_us() >= g_y2r_due_us) {
        g_y2r_busy = 0;
        {
            Handle *hh = get_handle(g_y2r_event);
            if (hh != NULL) hh->signaled = 1;
        }
    }
}

static void y2r_put_coefficients(Ipc *r) {
    int i;
    for (i = 0; i < 8; i += 2)
        ipc_put(r, (uint32_t)(uint16_t)g_y2r.coefficients[i] |
                    ((uint32_t)(uint16_t)g_y2r.coefficients[i + 1] << 16));
}

static void y2r_set_sending(Y2rBuffer *buf, const uint32_t *p) {
    buf->address = p[0];
    buf->image_size = p[1];
    buf->transfer_unit = (uint16_t)p[2];
    buf->gap = (uint16_t)p[3];
}

static uint32_t y2r_set_line_width(uint32_t width) {
    if (width == 0 || width > 1024u || (width % 8u) != 0)
        return 0xE0E053FD;      /* OutOfRange/CAM/InvalidArgument/Usage */
    g_y2r.input_line_width = (uint16_t)width;
    return 0;
}

static uint32_t y2r_set_lines(uint32_t lines) {
    if (lines == 0 || lines > 1024u)
        return 0xE0E053FD;
    /* Azahar-Bug der camera-Moduls: 1024 setzt das Register nicht. */
    if (lines != 1024u) g_y2r.input_lines = (uint16_t)lines;
    return 0;
}

static uint32_t y2r_set_standard_coefficient(uint32_t index) {
    if (index >= (uint32_t)Y2R_STD_COUNT)
        return 0xE0E053ED;      /* InvalidEnumValue/CAM/InvalidArgument/Usage */
    memcpy(g_y2r.coefficients, y2r_standard_coefficients[index],
           sizeof(g_y2r.coefficients));
    g_y2r_std_coefficient = (uint8_t)index;
    return 0;
}

static void y2r_command(Ipc *r, uint32_t *p) {
    y2r_poll();
    switch (r->cmd) {
        case 0x0001: g_y2r.input_format = (int)(p[0] & 0xFFu); goto result1;
        case 0x0003: g_y2r.output_format = (int)(p[0] & 0xFFu); goto result1;
        case 0x0005: g_y2r.rotation = (int)(p[0] & 0xFFu); goto result1;
        case 0x0007: g_y2r.block_alignment = (int)(p[0] & 0xFFu); goto result1;
        case 0x0009: g_y2r_spacial_dithering = (r->normal >= 1 && p[0] != 0); goto result1;
        case 0x000B: g_y2r_temporal_dithering = (r->normal >= 1 && p[0] != 0); goto result1;
        case 0x000D:
            g_y2r_interrupt_enabled = (r->normal >= 1 && p[0] != 0);
            goto result1;
        case 0x0010: case 0x0011: case 0x0012: case 0x0013: case 0x0018: {
            Y2rBuffer *b = r->cmd == 0x0010 ? &g_y2r.src_y
                         : r->cmd == 0x0011 ? &g_y2r.src_u
                         : r->cmd == 0x0012 ? &g_y2r.src_v
                         : r->cmd == 0x0013 ? &g_y2r.src_yuyv : &g_y2r.dst;
            y2r_set_sending(b, p);
            if (g_log) {
                printf("[y2r] cmd 0x%04X addr=%08X size=%08X unit=%u gap=%u\n",
                       r->cmd, b->address, b->image_size,
                       (unsigned)b->transfer_unit, (unsigned)b->gap);
                fflush(stdout);
            }
            goto result1;
        }
        case 0x001E: { /* SetCoefficient: 8 x s16 in 4 Woertern */
            int i;
            for (i = 0; i < 8; i += 2) {
                g_y2r.coefficients[i] = (int16_t)(p[i / 2] & 0xFFFFu);
                g_y2r.coefficients[i + 1] = (int16_t)(p[i / 2] >> 16);
            }
            goto result1;
        }
        case 0x0020: {
            uint32_t res = y2r_set_standard_coefficient(p[0]);
            ipc_begin(r, r->cmd, 1, 0);
            ipc_put(r, res);
            break;
        }
        case 0x0021: { /* GetStandardCoefficient: gueltiger Index -> 4 Woerter */
            uint32_t index = p[0];
            if (index >= (uint32_t)Y2R_STD_COUNT) {
                ipc_begin(r, r->cmd, 1, 0);
                ipc_put(r, 0xE0E053ED);
            } else {
                ipc_begin(r, r->cmd, 5, 0);
                put_result(r);
                ipc_put(r, (uint32_t)(uint16_t)y2r_standard_coefficients[index][0] |
                            ((uint32_t)(uint16_t)y2r_standard_coefficients[index][1] << 16));
                ipc_put(r, (uint32_t)(uint16_t)y2r_standard_coefficients[index][2] |
                            ((uint32_t)(uint16_t)y2r_standard_coefficients[index][3] << 16));
                ipc_put(r, (uint32_t)(uint16_t)y2r_standard_coefficients[index][4] |
                            ((uint32_t)(uint16_t)y2r_standard_coefficients[index][5] << 16));
                ipc_put(r, (uint32_t)(uint16_t)y2r_standard_coefficients[index][6] |
                            ((uint32_t)(uint16_t)y2r_standard_coefficients[index][7] << 16));
            }
            break;
        }
        case 0x0022: g_y2r.alpha = (uint16_t)p[0]; goto result1;
        case 0x0024: { /* SetDitheringWeightParams: 16 x u16 */
            int i;
            for (i = 0; i < 16; ++i)
                g_y2r_dither[i] = (uint16_t)(i & 1 ? (p[i / 2] >> 16) : (p[i / 2] & 0xFFFFu));
            goto result1;
        }
        case 0x001A: { /* SetInputLineWidth */
            uint32_t res = y2r_set_line_width(p[0]);
            ipc_begin(r, r->cmd, 1, 0);
            ipc_put(r, res);
            break;
        }
        case 0x001C: { /* SetInputLines */
            uint32_t res = y2r_set_lines(p[0]);
            ipc_begin(r, r->cmd, 1, 0);
            ipc_put(r, res);
            break;
        }
        case 0x0029: { /* SetPackageParameter: 12 Byte = 3 Woerter */
            uint32_t res;
            g_y2r.input_format = (int)(p[0] & 0xFFu);
            g_y2r.output_format = (int)((p[0] >> 8) & 0xFFu);
            g_y2r.rotation = (int)((p[0] >> 16) & 0xFFu);
            g_y2r.block_alignment = (int)((p[0] >> 24) & 0xFFu);
            res = y2r_set_line_width(p[1] & 0xFFFFu);
            if (res == 0) res = y2r_set_lines(p[1] >> 16);
            if (res == 0) res = y2r_set_standard_coefficient(p[2] & 0xFFu);
            if (res == 0) {
                g_y2r.padding = (uint8_t)((p[2] >> 8) & 0xFFu);
                g_y2r.alpha = (uint16_t)(p[2] >> 16);
            }
            ipc_begin(r, r->cmd, 1, 0);
            ipc_put(r, res);
            break;
        }
        case 0x0026: { /* StartConversion */
            Y2rMemory mem;
            mem.mem = g_cpu != NULL ? g_cpu->mem : NULL;
            mem.size = g_cpu != NULL ? g_cpu->mem_size : 0;
            if (mem.mem == NULL) {
                ipc_begin(r, r->cmd, 1, 0);
                put_result(r);
                break;
            }
            {
                int conv = y2r_perform_conversion(&mem, &g_y2r);
                if (getenv("HLE_Y2R_LOG")) {
                    /* Nur lesend: Mittelwerte der ersten 4096 Y-Bytes, der
                     * zugehoerigen U/V-Bytes und der Ausgabe (Farbdiagnose). */
                    double my = 0, mu = 0, mv = 0, mo = 0;
                    uint32_t k, n = 4096u;
                    for (k = 0; k < n; k++) {
                        uint32_t ay = (g_y2r.src_y.address & 0x1FFFFFFFu) + k;
                        uint32_t au = (g_y2r.src_u.address & 0x1FFFFFFFu) + k / 4u;
                        uint32_t av = (g_y2r.src_v.address & 0x1FFFFFFFu) + k / 4u;
                        uint32_t ao = (g_y2r.dst.address & 0x1FFFFFFFu) + k;
                        if (ay < mem.size) my += mem.mem[ay];
                        if (au < mem.size) mu += mem.mem[au];
                        if (av < mem.size) mv += mem.mem[av];
                        if (ao < mem.size) mo += mem.mem[ao];
                    }
                    fprintf(stderr, "[y2rsc] t=%llu conv=%d fmt=%d/%d %ux%u Y=%.0f U=%.0f V=%.0f "
                            "out=%.0f c=%d,%d,%d,%d,%d,%d,%d,%d alpha=%u\n",
                            (unsigned long long)((host_ns() - g_boot_ns) / 1000000ull),
                            conv, g_y2r.input_format, g_y2r.output_format,
                            (unsigned)g_y2r.input_line_width,
                            (unsigned)g_y2r.input_lines, my / n, mu / n, mv / n, mo / n,
                            g_y2r.coefficients[0], g_y2r.coefficients[1],
                            g_y2r.coefficients[2], g_y2r.coefficients[3],
                            g_y2r.coefficients[4], g_y2r.coefficients[5],
                            g_y2r.coefficients[6], g_y2r.coefficients[7],
                            (unsigned)g_y2r.alpha);
                }
                if (conv != 0) {
                    /* Azahar-ASSERT-Faelle; die Hardware meldet dort kein
                     * Completion-Interrupt. */
                    printf("[y2r] ungueltige Konfiguration (%d): fmt=%d/%d rot=%d "
                           "align=%d %ux%u\n", conv, g_y2r.input_format,
                           g_y2r.output_format, g_y2r.rotation, g_y2r.block_alignment,
                           (unsigned)g_y2r.input_line_width, (unsigned)g_y2r.input_lines);
                    fflush(stdout);
                    g_y2r_busy = 0;
                } else {
                    g_y2r_busy = 1;
                    g_y2r_due_us = host_us() + 186;
                    /* Y2R schreibt direkt in Gastpuffer: der Hardware-
                       Renderer muss sein Ziel/Texturen neu aus dem Gast
                       laden (sonst bleibt ein von Y2R beschriebener Puffer
                       auf dem alten D3D-Stand). */
                    gpu_cs_enter();   /* nicht parallel zu einer laufenden Liste */
                    gpu_pica_invalidate_guest();
                    gpu_cs_leave();
                }
            }
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            break;
        }
        case 0x0027: { /* StopConversion */
            if (g_y2r_busy) g_y2r_busy = 0;
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            break;
        }
        case 0x002A: { /* PingProcess: Result + u8 0 */
            ipc_begin(r, r->cmd, 2, 0);
            put_result(r);
            ipc_put(r, 0);
            break;
        }
        case 0x002B: { /* DriverInitialize: Azahar MakeBuilder(1,0) + Result */
            Handle *hh = get_handle(g_y2r_event);
            g_y2r.input_format = Y2R_IN_YUV422_INDIV8;
            g_y2r.output_format = Y2R_OUT_RGBA8;
            g_y2r.rotation = Y2R_ROT_NONE;
            g_y2r.block_alignment = Y2R_ALIGN_LINEAR;
            memset(g_y2r.coefficients, 0, sizeof(g_y2r.coefficients));
            y2r_set_line_width(1024);
            y2r_set_lines(1024);
            g_y2r.alpha = 0;
            memset(&g_y2r.src_y, 0, sizeof(g_y2r.src_y));
            memset(&g_y2r.src_u, 0, sizeof(g_y2r.src_u));
            memset(&g_y2r.src_v, 0, sizeof(g_y2r.src_v));
            memset(&g_y2r.dst, 0, sizeof(g_y2r.dst));
            if (hh != NULL) hh->signaled = 0;
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            break;
        }
        case 0x002C: { /* DriverFinalize */
            g_y2r_busy = 0;
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            break;
        }
        case 0x002D: { /* GetPackageParameter: Result + 12 Byte */
            ipc_begin(r, r->cmd, 4, 0);
            put_result(r);
            ipc_put(r, (uint32_t)g_y2r.input_format |
                        ((uint32_t)g_y2r.output_format << 8) |
                        ((uint32_t)g_y2r.rotation << 16) |
                        ((uint32_t)g_y2r.block_alignment << 24));
            ipc_put(r, (uint32_t)g_y2r.input_line_width |
                        ((uint32_t)g_y2r.input_lines << 16));
            ipc_put(r, (uint32_t)g_y2r_std_coefficient |
                        ((uint32_t)g_y2r.padding << 8) |
                        ((uint32_t)g_y2r.alpha << 16));
            break;
        }
        case 0x0002: case 0x0004: case 0x0006: case 0x0008:
        case 0x000A: case 0x000C: case 0x000E: case 0x001B: case 0x001D:
        case 0x0023: { /* Get*: Result + Wert */
            uint32_t v = 0;
            switch (r->cmd) {
                case 0x0002: v = (uint32_t)g_y2r.input_format; break;
                case 0x0004: v = (uint32_t)g_y2r.output_format; break;
                case 0x0006: v = (uint32_t)g_y2r.rotation; break;
                case 0x0008: v = (uint32_t)g_y2r.block_alignment; break;
                case 0x000A: v = (uint32_t)g_y2r_spacial_dithering; break;
                case 0x000C: v = (uint32_t)g_y2r_temporal_dithering; break;
                case 0x000E: v = (uint32_t)g_y2r_interrupt_enabled; break;
                case 0x001B: v = (uint32_t)g_y2r.input_line_width; break;
                case 0x001D: v = (uint32_t)g_y2r.input_lines; break;
                default: v = (uint32_t)g_y2r.alpha; break;
            }
            ipc_begin(r, r->cmd, 2, 0);
            put_result(r);
            ipc_put(r, v);
            break;
        }
        case 0x001F: { /* GetCoefficient */
            ipc_begin(r, r->cmd, 5, 0);
            put_result(r);
            y2r_put_coefficients(r);
            break;
        }
        case 0x0025: { /* GetDitheringWeightParams */
            int i;
            ipc_begin(r, r->cmd, 9, 0);
            put_result(r);
            for (i = 0; i < 16; i += 2)
                ipc_put(r, (uint32_t)g_y2r_dither[i] |
                            ((uint32_t)g_y2r_dither[i + 1] << 16));
            break;
        }
        case 0x000F: { /* GetTransferEndEvent: Result + CopyHandle(Event) */
            if (g_y2r_event == 0)
                g_y2r_event = new_event_handle("y2r-ev", 0);
            ipc_begin(r, 0x000F, 1, 2);
            put_result(r);
            ipc_put_handle_copy(r, g_y2r_event);
            if (g_log) printf("[y2r] GetTransferEndEvent -> %u\n", g_y2r_event);
            break;
        }
        case 0x0014: case 0x0015: case 0x0016: case 0x0017:
        case 0x0019: { /* IsFinished*: Azahar-Stub liefert immer 1 */
            ipc_begin(r, r->cmd, 2, 0);
            put_result(r);
            ipc_put(r, 1);
            break;
        }
        case 0x0028: { /* IsBusyConversion */
            ipc_begin(r, r->cmd, 2, 0);
            put_result(r);
            ipc_put(r, (uint32_t)g_y2r_busy);
            break;
        }
        default: {
            /* Unbekanntes Kommando laut melden (Azahar
             * ReportUnimplementedFunction), Antwort bleibt Result 0. */
            ipc_begin(r, r->cmd, 1, 0);
            put_result(r);
            if (g_log) printf("[y2r] UNIMPLEMENTIERT cmd 0x%04X\n", r->cmd);
            break;
        }
    }
    return;
result1:
    ipc_begin(r, r->cmd, 1, 0);
    put_result(r);
}

static void generic_command(Ipc *r, int service) {
    /* Azahar ServiceFrameworkBase::ReportUnimplementedFunction meldet das
       Kommando laut und liefert Header (normal=1, translate=0) mit Result 0. */
    static int logged = 0;
    if (logged < 200) {
        logged++;
        printf("[ipc] UNIMPLEMENTIERT %s cmd=0x%04X n=%d t=%d",
               SERVICE_NAMES[service], r->cmd, r->normal, r->translate);
        for (int i = 0; i < r->normal && i < 12; i++)
            printf(" p%d=%08X", i, r->buf[1 + i]);
        printf("\n");
        fflush(stdout);
    }
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
    /* 0x08xx: bei bekanntem Datei-/Verzeichnis-/FS-Handle entscheidet das
       Handle (Azahar: der Dienst hinter der Session waehlt den Handler;
       0x080C ist auf einer Datei OpenLinkFile, auf fs:USER OpenArchive).
       Nur bei unbekanntem Handle nach Kommandoform routen. */
    if ((cmd & 0xFF00u) == 0x0800u) {
        int handle_is_fs = (service == SVC_FS_USER || service == SVC_FS_LDR);
        int handle_is_file = (h != NULL && h->file >= 0);
        int handle_is_dir = (h != NULL && h->dir >= 0);
        if (handle_is_dir || handle_is_file) {
            service = SVC_FILE;
        } else if (handle_is_fs) {
            /* Echte fs:USER-Session: 0x0802/0x0803/0x0808 sind hier
             * OpenFile/OpenFileDirectly/CreateFile, nicht Datei-Kommandos. */
        } else if (cmd == 0x080Cu || cmd == 0x080Du || cmd == 0x080Eu || cmd == 0x0812u)
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
    int prio;            /* Gast-Prioritaet, kleiner = hoeher */
    int woken;
} ArbWaiter;
static ArbWaiter g_arbq[ARBQ_MAX];

/* Signal: value < 0 weckt alle, sonst value Threads in Prioritaetsreihenfolge
 * (Azahar AddressArbiter::ResumeHighestPriorityThread: kleinster
 * Prioritaetswert zuerst, bei Gleichstand der zuerst eingetragene Thread). */
static int arb_resume(uint32_t addr, int32_t value) {
    int woken = 0;
    int32_t remaining = (value < 0) ? 0x7FFFFFFF : value;
    while (remaining > 0) {
        int best = -1;
        for (int i = 0; i < ARBQ_MAX; i++) {
            if (!g_arbq[i].used || g_arbq[i].addr != addr) continue;
            if (g_arbq[i].woken) continue;
            if (best < 0 || g_arbq[i].prio < g_arbq[best].prio) best = i;
        }
        if (best < 0) break;
        g_arbq[best].woken = 1;
        remaining--;
        woken++;
    }
    return woken;
}

static int arb_register(uint32_t addr, uint32_t tid, int prio) {
    for (int i = 0; i < ARBQ_MAX; i++) {
        if (!g_arbq[i].used) {
            g_arbq[i].used = 1;
            g_arbq[i].addr = addr;
            g_arbq[i].tid = tid;
            g_arbq[i].prio = prio;
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
    FrameClock clock;
    (void)p;
    frame_clock_start(&clock);
    while (g_vblank_run) {
        frame_clock_wait(&clock);
        /* Echte VBlank-Quelle: GSP uebernimmt die FrameBufferInfo aus dem
         * Shared-Memory und meldet PDC0 (oben) / PDC1 (unten) per RelayQueue
         * und Event (Azahar GPU::VBlankCallback / GSP_GPU::SignalInterrupt).
         * Lesen + dirty-Flag loeschen und das Schreiben in Gast-Speicher
         * (RelayQueue) muessen atomar unter dem Gast-Lock passieren; sonst
         * kann ein Buffer-Swap des Spiels verloren gehen. */
        guest_lock_enter();
        gsp_vblank_fb_update(g_cpu, g_gsp_thread_id);
        gsp_signal_interrupt(GSP_IRQ_PDC0);
        gsp_signal_interrupt(GSP_IRQ_PDC1);
        kernel_poll_timers();
        hid_poll();
        audio_dsp_flush_events();
        gsp_flush_pending_irqs();
        if (g_ystat && (++g_vblank_tick % 62u) == 0u) {
            printf("[ystat] vbl=%u t=%llus yields=%llu yield_ms=%llu pumps=%llu pump_ms=%llu idle_ms=%llu\n",
                   g_vblank_tick,
                   (unsigned long long)((host_us() - g_boot_ns / 1000ull) / 1000000ull),
                   g_yield_n, g_yield_us / 1000ull,
                   g_pump_rel_n, g_pump_rel_us / 1000ull,
                   g_lock_idle_us / 1000ull);
            fflush(stdout);
        }
        guest_lock_leave();
    }
    frame_clock_stop(&clock);
    return 0;
}

void hle_init(ArmCpu *c, const char *romfs_dir) {
    ensure_savedata();
    fs_archive_init("out/savedata");
    /* SharedExtSaveData ist auf echter Hardware ein vorhandenes Systemarchiv.
     * Der Host stellt es beim Start bereit (nicht beim OpenArchive, siehe
     * Azahar ArchiveFactory_ExtSaveData::Open -> NotFormatted).  Die ID stammt
     * aus der Azahar-Referenzspur (OpenArchive 7, Path 000000000b0000f0...):
     * 00048000/F000000B, darin /gamecoin.dat. */
    fs_archive_provision_shared(0xF000000Bu);
    g_cpu = c;
    g_main_cpu = c;
    g_boot_ns = host_ns();
    snprintf(g_romfs, sizeof(g_romfs), "%s", romfs_dir ? romfs_dir : "out/romfs");
    g_log = getenv("HLE_LOG") != NULL;
    g_ystat = getenv("HLE_YSTAT") != NULL;
    g_wstat = getenv("HLE_WSTAT") != NULL;
    g_gtime = getenv("HLE_GTIME") != NULL;
    {
        /* Paralleles Rasterisieren grosser Dreiecke (Ergebnis pixelgleich).
         * HLE_RASTER_THREADS=0 schaltet es fuer Messungen ab. */
        const char *rt = getenv("HLE_RASTER_THREADS");
        gpu_pica_set_raster_threads(rt ? atoi(rt) : 3);
    }
    /* Diagnose-Laeufe werden per Timeout beendet; ungepufferte Ausgabe
       verhindert, dass die letzten Zeilen im stdio-Puffer verloren gehen. */
    if (getenv("HLE_ARB_LOG") || getenv("HLE_KLOG")) setvbuf(stdout, NULL, _IONBF, 0);
    {
        const char *lim = getenv("HLE_IPC_LOG");
        if (lim) g_ipc_log_limit = atoi(lim);
    }
    setup_config_mem(c);
    arm_pump_hook = hle_pump;
    gpu_pica_reset();
    gpu_pica_set_irq_handler(gsp_pica_irq);
    gsp_fb_init();

    memset(g_handles, 0, sizeof(g_handles));
    g_next_handle = 1;
    g_srv_handle = (uint32_t)alloc_handle(SVC_SRV, "srv:");
    /* Azahar legt das HID-Modul beim Dienstaufbau an: Shared-Memory und die
       fuenf Events existieren, und der Pad-Update-Takt laeuft ab jetzt. */
    hid_init_once();

    /* Rein lesende Diagnose. */
    if (getenv("ARM_WATCH"))
        arm_watch_pc = (uint32_t)strtoul(getenv("ARM_WATCH"), NULL, 16);
    if (getenv("HLE_TRACE"))
        g_trace_file = fopen("out/hle_trace.txt", "w");

    if (!g_guest_lock_init) {
        InitializeCriticalSection(&g_guest_lock);
        InitializeCriticalSection(&g_gpu_cs);
        g_gpu_cs_init = 1;
        g_guest_lock_init = 1;
        /* Kooperatives Threading: genau ein Gast-Thread laeuft. */
        g_no_lock = getenv("HLE_NO_THREAD_LOCK") != NULL;
        g_det_sched = getenv("HLE_DET_SCHED") != NULL;
        if (g_det_sched) g_turn = 1;
    }
    /* VBlank-Thread erst nach Lock- und HID-Initialisierung starten: er
       nimmt fuer jeden VBlank den Gast-Lock. */
    g_vblank_run = 1;
    CreateThread(NULL, 0, vblank_thread, NULL, 0, NULL);

    /* DSP-HLE-Audio: Regionen 0x1FF50000/0x1FF70000 im Gast-Speicher,
       Gast-Lock fuer den Audio-Thread, Pipe-2-Interrupts je Frame. */
    audio_dsp_init(c->mem, c->mem_size);
    audio_dsp_set_lock(guest_lock_enter, guest_lock_leave);
    audio_dsp_set_trylock(guest_lock_try);
    audio_dsp_set_interrupt(dsp_interrupt_cb);
    audio_dsp_set_struct_addresses(dsp_audio_struct_addresses);
    audio_dsp_start();
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
    if (g_gtime) {
        unsigned long long wall_ms = (host_us() - g_boot_ns / 1000ull) / 1000ull;
        unsigned long long frag = 0, tex = 0, lit = 0, fb = 0;
        unsigned long long pc = 0, sc = 0, pr = 0, sr = 0;
        gpu_pica_work_stats(&frag, &tex, &lit, &fb);
        gpu_pica_par_stats(&pc, &sc, &pr, &sr);
        printf("[gtime] lists=%llu raster_ms=%llu wall_ms=%llu (%.1f%%) "
               "frag=%llu tex=%llu lit=%llu fb=%llu par=%llu/%llu ser=%llu/%llu\n",
               g_gtime_lists, g_gtime_us / 1000ull, wall_ms,
               wall_ms ? 100.0 * (double)g_gtime_us / 1000.0 / (double)wall_ms : 0.0,
               frag, tex, lit, fb, pc, pr, sc, sr);
    }
    if (g_log) printf("[audio] frames=%llu host_dropped=%u underruns=%u\n", audio_dsp_frames(),
                      host_audio_dropped(), host_audio_underruns());
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
        kwait_unregister_tid(tid);
        Handle *hh = get_handle(g_threads[i].handle);
        /* Nur signalisieren, wenn der Slot noch dieser Thread ist: nach
           CloseHandle kann er fuer ein anderes Objekt wiederverwendet sein. */
        if (hh != NULL && hh->used && hh->kind == HK_THREAD)
            hs_signal(hh);
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

static uint32_t hle_svc_dispatch(ArmCpu *c, uint32_t imm) {
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
        printf("[svc] Break bei Block 0x%08X: r0=%08X r1=%08X r2=%08X r3=%08X lr=%08X th=%u\n",
               arm_last_block, c->r[0], c->r[1], c->r[2], c->r[3], c->r[14],
               (uint32_t)((c->tpidruro >> 12) & 0xFu));
        arm_trace_thread_dump((int)((c->tpidruro >> 12) & 0xFu), 16);
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
        case 0x08: { /* CreateThread: SVC-ABI (Azahar svc_wrapper.h):
                        r0 = priority, r1 = entry, r2 = arg, r3 = stack_top,
                        r4 = processor_id; Rueckgabe Result in r0, Handle in r1.
                        (Das `out`-Pointer-Argument der C-Signatur ist ein
                        Output-Parameter und wird ueber r1 zurueckgegeben.) */
            uint32_t prio = c->r[0];
            uint32_t entry = c->r[1], arg = c->r[2], stack = c->r[3];
            int32_t proc = (int32_t)c->r[4];
            uint32_t h;
            /* Azahar svc.cpp/CreateThread: ThreadProcessorIdDefault (-2) nimmt
               den Ideal-Prozessor des Prozesses (ExHeader), All (-1) laeuft auf
               Core 0. Prioritaet > ThreadPrioLowest = OutOfRange. */
            if (proc == -2) proc = g_main_proc;
            if (proc == -1) proc = 0;
            if (getenv("HLE_THR_LOG"))
                printf("[thrarg] prio=%u entry=%08X arg=%08X stack=%08X proc=%d"
                       " lr=%08X\n", prio, entry, arg, stack, (int)proc, c->r[14]);
            if (prio > KS_PRIO_LOWEST) return RESULT_OUT_OF_RANGE;
            if (proc < 0 || proc > 3) return RESULT_OUT_OF_RANGE;
            h = (uint32_t)alloc_handle(SVC_OTHER, "thread");
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
                    t->prio = (int)prio;
                    t->proc_id = (int)proc;
                    t->handle = h;
                    if (getenv("HLE_THR_LOG"))
                        printf("[thrnew] handle=%u entry=%08X arg=%08X stack=%08X id=%d"
                               " prio=%u proc=%d\n",
                               h, entry, arg, stack, t->id, prio, (int)proc);
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
        case 0x0B: { /* GetThreadPriority(out*, handle) */
            uint32_t out = c->r[0], handle = c->r[1];
            if (handle == 0xFFFF8000u) {
                if (out) write_u32(c, out, (uint32_t)g_main_prio);
                c->r[1] = (uint32_t)g_main_prio;   /* SVC-ABI: Output in r1 */
                return 0;
            }
            for (int i = 0; i < 32; i++) {
                if (g_threads[i].used && g_threads[i].handle == handle) {
                    if (out) write_u32(c, out, (uint32_t)g_threads[i].prio);
                    c->r[1] = (uint32_t)g_threads[i].prio;
                    return 0;
                }
            }
            return RESULT_INVALID_HANDLE;
        }
        case 0x0C: { /* SetThreadPriority(handle, prio) -> Result */
            Handle *hh;
            if (c->r[1] > KS_PRIO_LOWEST) return RESULT_OUT_OF_RANGE;
            /* Azahar: ResourceLimit Priority der Kategorie Application = 0x18;
               hoeher (kleinerer Wert) darf eine App nicht setzen. */
            if (c->r[1] < KS_PRIO_APP_MAX) return RESULT_NOT_AUTHORIZED;
            if (c->r[0] == 0xFFFF8000u) { g_main_prio = (int)c->r[1]; return 0; }
            hh = get_handle(c->r[0]);
            if (hh == NULL || hh->kind != HK_THREAD) return RESULT_INVALID_HANDLE;
            for (int i = 0; i < 32; i++) {
                if (g_threads[i].used && g_threads[i].handle == c->r[0]) {
                    g_threads[i].prio = (int)c->r[1];
                    break;
                }
            }
            return 0;
        }
        case 0x0F: { /* GetThreadIdealProcessor(out*, handle) */
            uint32_t out = c->r[0], handle = c->r[1];
            if (handle == 0xFFFF8000u) {
                if (out) write_u32(c, out, (uint32_t)g_main_proc);
                c->r[1] = (uint32_t)g_main_proc;
                return 0;
            }
            for (int i = 0; i < 32; i++) {
                if (g_threads[i].used && g_threads[i].handle == handle) {
                    if (out) write_u32(c, out, (uint32_t)g_threads[i].proc_id);
                    c->r[1] = (uint32_t)g_threads[i].proc_id;
                    return 0;
                }
            }
            return RESULT_INVALID_HANDLE;
        }
        case 0x11: /* GetCurrentProcessorNumber -> r0 = Core (0 App, 1 Sys) */
            return (uint32_t)cur_proc(c);
        case 0x0A: { /* SleepThread(ns): echte Wartezeit (Azahar svc.cpp:
                        WakeAfterDelay; ns <= 0 = sofort weiter, Yield) */
            int64_t ns = (int64_t)((uint64_t)c->r[0] | ((uint64_t)c->r[1] << 32));
            uint32_t sleep_ms;
            if (getenv("HLE_SLEEP_LOG") && g_sleeplog_n < 200) {
                g_sleeplog_n++;
                printf("[sleep] tid=%u ns=%lld blk=%08X lr=%08X\n",
                       cur_tid(c), (long long)ns, arm_last_block, c->r[14]);
            }
            if (ns <= 0) {
                sleep_ms = 0;
            } else {
                uint64_t ms = (uint64_t)ns / 1000000ull;
                if (ms > 0xFFFFFFFFull) ms = 0xFFFFFFFFull;
                sleep_ms = (uint32_t)ms;
            }
            if (g_guest_lock_init && !g_no_lock) {
                guest_lock_leave();
                Sleep(sleep_ms);
                guest_lock_reacquire();
            } else {
                Sleep(sleep_ms);
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
            /* Azahar Timer::Set laesst den Signalzustand unveraendert.
               Zeitbasis sind Host-Nanosekunden (CoreTiming-Takt ~3,7 ns). */
            hh->timer_interval = (uint64_t)interval;
            if (initial == 0) {
                hh->timer_active = 0;
                hh->signaled = 1;              /* sofort faellig */
            } else {
                hh->timer_due = host_ns() + (uint64_t)initial;
                hh->timer_active = 1;
            }
            return 0;
        }
        case 0x1C: { /* CancelTimer(handle): nur austragen (Azahar Timer::Cancel) */
            Handle *hh = get_handle(c->r[0]);
            if (hh == NULL || hh->kind != HK_TIMER) return RESULT_INVALID_HANDLE;
            hh->timer_active = 0;
            return 0;
        }
        case 0x1D: { /* ClearTimer(handle): Signal loeschen (Azahar Timer::Clear) */
            Handle *hh = get_handle(c->r[0]);
            if (hh == NULL || hh->kind != HK_TIMER) return RESULT_INVALID_HANDLE;
            hh->signaled = 0;
            return 0;
        }
        case 0x1E: { /* CreateMemoryBlock(out, addr, size, myperm, otherperm) */
            uint32_t out = c->r[0], addr = c->r[1], size = c->r[2];
            uint32_t h = (uint32_t)alloc_handle(SVC_OTHER, "memoryblock");
            if (out) write_u32(c, out, h);
            c->r[1] = h;
            if (getenv("HLE_MAP_LOG"))
                printf("[map] CreateMemoryBlock addr=%08X size=%08X -> h=%u\n",
                       addr, size, h);
            return 0;
        }
        case 0x1F: { /* MapMemoryBlock(handle, addr, myperm, otherperm) */
            uint32_t h = c->r[0], addr = c->r[1];
            if (g_hid.initialized &&
                canonical_slot(h) == canonical_slot(g_hid.shmem))
                hid_map_guest(addr);
            if (getenv("HLE_MAP_LOG"))
                printf("[map] MapMemoryBlock h=%u addr=%08X perm=%08X/%08X\n",
                       h, addr, c->r[2], c->r[3]);
            return 0;
        }
        case 0x20: { /* UnmapMemoryBlock(handle, addr) */
            if (g_hid.initialized &&
                canonical_slot(c->r[0]) == canonical_slot(g_hid.shmem))
                hid_unmap_guest(c->r[1]);
            return 0;
        }
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
                printf("[arb] tid=%u addr=%08X type=%u val=%d mem=%08X to=%llu blk=%08X lr=%08X\n",
                       cur_tid(c), addr, type, value, (uint32_t)adbg,
                       (unsigned long long)timeout_ns, arm_last_block, c->r[14]);
                arb_prints++;
            }
            if (off + 4 > c->mem_size) return 0;

            /* Azahar ArbitrationType: 0 Signal, 1 WaitIfLessThan,
             * 2 DecrementAndWaitIfLessThan, 3 WaitIfLessThanWithTimeout,
             * 4 DecrementAndWaitIfLessThanWithTimeout.
             * Signal blockiert nie. 1/2 warten ohne Grenze, 3/4 werten den
             * Timeout aus und liefern immer ResultTimeout (auch ohne Warten).
             * Dekrementiert wird nur bei 2/4 und nur, wenn gewartet wird. */
            if (type == 0) {
                arb_resume(addr, value);
                return 0;
            }
            if (type > 4) return 0xD8E093EDu;

            {
                int32_t cur = 0;
                memcpy(&cur, c->mem + off, 4);
                if (cur < value) {
                    if (type == 2 || type == 4) {
                        int32_t nv = cur - 1;
                        memcpy(c->mem + off, &nv, 4);
                    }
                    uint32_t tid = cur_tid(c);
                    int slot = arb_register(addr, tid, cur_prio(c));
                    uint64_t start = host_us();
                    /* 1/2 warten ohne Grenze; nur 3/4 werten den Timeout aus. */
                    int infinite = (type == 1 || type == 2) ||
                                   timeout_ns == UINT64_MAX;
                    uint64_t timeout_us =
                        infinite ? 0 : ((timeout_ns + 999ull) / 1000ull);
                    if (getenv("HLE_KLOG"))
                        printf("[karb] tid=%u addr=%08X type=%u val=%d slot=%d"
                               " mem=%d\n", tid, addr, type, value, slot, cur);
                    for (;;) {
                        if (slot < 0 || g_arbq[slot].woken) break;
                        if (!infinite && (host_us() - start) >= timeout_us) break;
                        guest_wait_change();
                    }
                    arb_unregister(slot);
                }
            }
            /* Azahar: die WithTimeout-Typen liefern immer ResultTimeout. */
            if (type == 3 || type == 4) return RESULT_TIMEOUT;
            return 0;
        }
        case 0x23: { /* CloseHandle(handle) */
            uint32_t h = c->r[0];
            Handle *hh = get_handle(h);
            if (hh == NULL) return RESULT_INVALID_HANDLE;
            close_handle_slot(h);
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
        case 0x25: { /* WaitSynchronizationN: SVC-ABI (Azahar svc_wrapper.h)
                        r1 = Handle-Liste, r2 = Anzahl, r3 = wait_all,
                        Timeout (s64) in r0:r4 (Stack-Argumente werden in die
                        freien Register r0/r4 umgehaengt). Der Index geht als
                        Output-Parameter in r1 zurueck. */
            static int wl = 0;
            uint32_t hlist = c->r[1], cnt = c->r[2];
            int32_t wait_all = (int32_t)c->r[3];
            uint64_t timeout_ns = (uint64_t)c->r[0] | ((uint64_t)c->r[4] << 32);
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
                printf("[wait25 idx] tid=%u cnt=%u -> idx=%d\n", cur_tid(c), cnt, idx);
            /* SVC-ABI: der Index ist Output-Parameter in r1; der SDK-Stub
               (0x287B90) speichert r1 nach *out. */
            c->r[1] = (uint32_t)idx;
            return 0;
        }
        case 0x27: { /* DuplicateHandle(out, handleO) */
            Handle *h = get_handle(c->r[1]);
            int canon = canonical_slot(c->r[1]);
            if (h == NULL || canon <= 0) return RESULT_INVALID_HANDLE;
            {
                /* Azahar kernel/handle_table.cpp Duplicate: neuer Handle-Slot
                   auf dasselbe Objekt (gleicher Zustand, gleiche Datei). */
                int nh = alloc_handle(h->service, h->name);
                Handle *n;
                if (nh <= 0) return RESULT_OUT_OF_RANGE;
                n = &g_handles[nh];
                memset(n, 0, sizeof(*n));
                n->used = 1;
                n->file = -1;
                n->alias = canon;
                n->refs = 0;
                g_handles[canon].refs++;
                if (c->r[0]) write_u32(c, c->r[0], (uint32_t)nh);
                c->r[1] = (uint32_t)nh;
            }
            return 0;
        }
        case 0x28: { /* GetSystemTick -> 64 Bit in r0:r1.
                        Azahar: GetTimer().GetTicks() mit
                        BASE_CLOCK_RATE_ARM11 = 268111856 Hz; dieselbe
                        Zeitbasis wie die hid-Zeitstempel. */
            g_tick = host_system_ticks();
            g_tick_calls++;
            if (getenv("HLE_TICK_LOG") && g_ticklog_n < 200) {
                g_ticklog_n++;
                printf("[tick] tid=%u blk=%08X lr=%08X t=%llu\n",
                       cur_tid(c), arm_last_block, c->r[14],
                       (unsigned long long)g_tick);
            }
            c->r[1] = (uint32_t)(g_tick >> 32);
            return (uint32_t)g_tick;
        }
        case 0x2A: { /* GetSystemInfo: out-Pointer r0, type r1;
                        Ausgabe (s64) als Output-Parameter in r1:r2. */
            uint32_t type = c->r[1], out = c->r[0];
            uint64_t value = 0;
            switch (type) {
                case 0x10000: value = 0x08000000ull; break;  /* FCRAM gesamt */
                case 0x10001: value = 0x00800000ull; break;  /* belegt */
                case 0x10002: value = 0x1FF00000ull; break;  /* VRAM-Basis */
                case 0x10003: value = 0x00600000ull; break;  /* VRAM-Groesse */
                default: value = 0; break;
            }
            if (out) write_u64(c, out, value);
            c->r[1] = (uint32_t)value;
            c->r[2] = (uint32_t)(value >> 32);
            if (g_log) printf("[svc] GetSystemInfo type=0x%X -> 0x%llX\n", type,
                              (unsigned long long)value);
            return 0;
        }
        case 0x2B: { /* GetProcessInfo: out-Pointer r0, type r1 */
            uint32_t type = c->r[1], out = c->r[0];
            uint64_t value = 0;
            switch (type) {
                case 0: value = 0x00000020ull; break;
                case 2: value = 0x08000000ull; break;
                case 0x10000: value = 0x00000000ull; break;
                default: value = 0; break;
            }
            if (out) write_u64(c, out, value);
            c->r[1] = (uint32_t)value;
            c->r[2] = (uint32_t)(value >> 32);
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
        case 0x35: /* GetProcessId: Prozess-Handle r1, PID als Output in r1 */
            if (c->r[0]) write_u32(c, c->r[0], 0x20);
            c->r[1] = 0x20;
            return 0;
        case 0x37: { /* GetThreadId: Handle in r1, Thread-Id als Output in r1
                        (SVC-ABI; der SDK-Stub 0x283354 speichert r1). */
            uint32_t handle = c->r[1];
            uint32_t tid;
            if (handle == 0xFFFF8000u) {
                tid = 0x100u + cur_tid(c);      /* aktueller Thread */
            } else {
                int found = 0;
                tid = 0;
                for (int i = 0; i < 32; i++)
                    if (g_threads[i].used && g_threads[i].handle == handle) {
                        tid = 0x100u + (uint32_t)g_threads[i].id;
                        found = 1;
                        break;
                    }
                if (!found) return RESULT_INVALID_HANDLE;
            }
            c->r[1] = tid;
            return 0;
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

/* Oeffentlicher SVC-Einstieg: nach dem eigentlichen Kommando folgt der
 * Preemptionspunkt. Azahar ruft nach jedem SVC system.PrepareReschedule();
 * dort laeuft ein durch das SVC bereiter Thread mit hoeherer Prioritaet vor
 * der Rueckkehr in den Gastcode. Im kooperativen Port ist das ein
 * guest_yield(), das den Gast-Lock kurz abgibt. */
uint32_t hle_svc(ArmCpu *c, uint32_t imm) {
    uint32_t r = hle_svc_dispatch(c, imm);
    if (g_guest_lock_init && !g_no_lock && !g_det_sched &&
        kernel_higher_ready(cur_tid(c), cur_prio(c)))
        guest_yield();
    return r;
}

/* Platzhalter, bis die GPU angebunden ist */
void hle_gpu_write_reg(uint32_t addr, uint32_t value) { (void)addr; (void)value; }
void hle_gpu_command_list(const uint8_t *data, uint32_t size, uint32_t flags) {
    (void)data; (void)size; (void)flags;
}
