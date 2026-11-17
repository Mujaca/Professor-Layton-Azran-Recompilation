#include "core/trace/ctr_trace.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {
std::mutex g_mutex;
std::FILE* g_file = nullptr;
bool g_tried = false;

std::FILE* File() {
    if (!g_tried) {
        g_tried = true;
        const char* path = std::getenv("CTR_TRACE_FILE");
        if (path == nullptr || *path == '\0') {
            path = "trace.ctrt";
        }
        g_file = std::fopen(path, "wb");
        if (g_file != nullptr) {
            std::fwrite("CTRTRC01", 1, 8, g_file);
        }
    }
    return g_file;
}

void U8(std::uint8_t v) { std::fwrite(&v, 1, 1, g_file); }

void U16(std::uint16_t v) {
    std::uint8_t b[2] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8)};
    std::fwrite(b, 1, 2, g_file);
}

void U32(std::uint32_t v) {
    std::uint8_t b[4] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8),
                         static_cast<std::uint8_t>(v >> 16),
                         static_cast<std::uint8_t>(v >> 24)};
    std::fwrite(b, 1, 4, g_file);
}
} // namespace

extern "C" {

void ctr_trace_init(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    File();
}

void ctr_trace_shutdown(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr) {
        std::fflush(g_file);
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void ctr_trace_block(std::uint32_t pc, std::uint32_t thumb) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (File() == nullptr) {
        return;
    }
    U8('B');
    U32(pc);
    U8(static_cast<std::uint8_t>(thumb != 0));
}

void ctr_trace_svc(std::uint32_t imm) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (File() == nullptr) {
        return;
    }
    U8('S');
    U32(imm);
}

void ctr_trace_ipc(const char* service, std::uint32_t cmd_id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (File() == nullptr) {
        return;
    }
    const char* name = service != nullptr ? service : "";
    std::size_t len = std::strlen(name);
    if (len > 64) {
        len = 64;
    }
    U8('I');
    U8(static_cast<std::uint8_t>(len));
    std::fwrite(name, 1, len, g_file);
    U16(static_cast<std::uint16_t>(cmd_id));
}

void ctr_trace_frame(void) {
    static std::uint32_t counter = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (File() == nullptr) {
        return;
    }
    U8('F');
    U32(counter++);
}

void ctr_trace_event(std::uint32_t kind, std::uint32_t a, std::uint32_t b) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (File() == nullptr) {
        return;
    }
    U8('E');
    U32(kind);
    U32(a);
    U32(b);
}

} // extern "C"
