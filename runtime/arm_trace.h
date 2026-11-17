/* Trace-Runtime fuer den recompilierten Code.
 * Format identisch zu tools/trace/azahar/ctr_trace.cpp: "CTRTRC01" + Records. */
#ifndef ARM_TRACE_H
#define ARM_TRACE_H

#include <stdint.h>

struct ArmCpu;


#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t arm_last_block;
extern uint32_t arm_watch_pc;

void arm_trace_open(const char *path);
void arm_trace_close(void);
int arm_trace_is_open(void);

void arm_trace_block(struct ArmCpu *c, uint32_t pc, uint32_t thumb);
void arm_trace_svc(uint32_t imm);
void arm_trace_ipc(const char *service, uint32_t cmd_id);
void arm_trace_frame(void);
void arm_trace_event(uint32_t kind, uint32_t a, uint32_t b);
void arm_trace_pc_ring_dump(unsigned count);

#ifdef __cplusplus
}
#endif

#endif
