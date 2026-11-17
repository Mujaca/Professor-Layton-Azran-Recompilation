// CTR-Trace: schreibt Ausfuehrungs-Traces fuer das Recompiler-Harness.
// Format: "CTRTRC01" + Records (siehe tools/recomp/trace.py).
#pragma once

#include <cstdint>

extern "C" {
void ctr_trace_init(void);
void ctr_trace_shutdown(void);

void ctr_trace_block(std::uint32_t pc, std::uint32_t thumb);
void ctr_trace_svc(std::uint32_t imm);
void ctr_trace_ipc(const char* service, std::uint32_t cmd_id);
void ctr_trace_frame(void);
void ctr_trace_event(std::uint32_t kind, std::uint32_t a, std::uint32_t b);
}
