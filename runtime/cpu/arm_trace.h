/* Block-Hook + Diagnose fuer den recompilierten Code.
 * Trace-Format identisch zu tools/trace/azahar/ctr_trace.cpp: "CTRTRC01" + Records. */
#ifndef ARM_TRACE_H
#define ARM_TRACE_H

#include <stdint.h>

struct ArmCpu;

#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t arm_last_block;
extern uint32_t arm_thread_last_pc[16];
extern uint32_t arm_thread_last_lr[16];
/* Scheduler-Hook, einmal pro Block (runtime/kernel/hle.c). */
extern void (*arm_pump_hook)(void);
/* ARM_WATCH=<hex>: Register beim Erreichen des Blocks ausgeben (nur lesend). */
extern uint32_t arm_watch_pc;

void arm_trace_block(struct ArmCpu *c, uint32_t pc, uint32_t thumb);

/* Optionaler CTRTRC-Ausfuehrungstrace (tools/recomp/trace.py). */
void arm_trace_open(const char *path);
void arm_trace_close(void);
int arm_trace_is_open(void);
void arm_trace_svc(uint32_t imm);
void arm_trace_ipc(const char *service, uint32_t cmd_id);
void arm_trace_frame(void);
void arm_trace_event(uint32_t kind, uint32_t a, uint32_t b);

/* Diagnose bei Abbruch / Profiling (HLE_PERFMAP_FILE). */
void arm_trace_pc_ring_dump(unsigned count);
void arm_trace_thread_dump(int ti, unsigned n);
void arm_perfmap_report(void);

/* Vom Recompiler emittiert (ldr-pc-Sprungtabellen). */
int arm_ldrpc_log_on(void);
void arm_ldrpc_log(unsigned t);

#ifdef __cplusplus
}
#endif

#endif
