/* Trace-Runtime fuer den recompilierten Code.
 * Format identisch zu tools/trace/azahar/ctr_trace.cpp: "CTRTRC01" + Records. */
#ifndef ARM_TRACE_H
#define ARM_TRACE_H

#include <stdint.h>

int arm_ldrpc_log_on(void);
void arm_ldrpc_log(unsigned t);
struct ArmCpu;


#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t arm_last_block;
int hle_fa_read(struct ArmCpu *c);
extern void (*arm_pump_hook)(void);
/* Pro Block mit der laufenden CPU aufgerufen: dient als VBlank-/Frame-
   Dispatcher (die Engine registriert ihre Tick-Tasks in 0x3D7C50). */
extern void (*arm_tick_hook)(struct ArmCpu *c);
extern uint32_t arm_watch_pc;

void arm_trace_open(const char *path);
void arm_trace_close(void);
int arm_trace_is_open(void);

void arm_trace_block(struct ArmCpu *c, uint32_t pc, uint32_t thumb);
/* HLE_RETFORCE=<fnaddr>=<r0>[,<fnaddr>=<r0>...]: laesst die generierte
   Funktion sofort mit r0=<wert> zurueckkehren (generischer Gate-Force-
   Hook, ohne Rebuild pro Adresse). Rueckgabe 1 = Funktion wurde ersetzt. */
int arm_retforce(struct ArmCpu *c, uint32_t addr);
void arm_retforce_report(void);
void arm_trace_svc(uint32_t imm);
void arm_trace_ipc(const char *service, uint32_t cmd_id);
void arm_trace_frame(void);
void arm_trace_event(uint32_t kind, uint32_t a, uint32_t b);
void arm_trace_pc_ring_dump(unsigned count);

/* Diagnose ohne Rebuild: Block-Histogramm (HLE_PERFMAP_FILE) und
   Adresse->Callback-Tabelle (HLE_HOOK). Beide sind env-gated. */
void arm_perfmap_report(void);
void arm_hook_report(void);

#ifdef __cplusplus
}
#endif

#endif
