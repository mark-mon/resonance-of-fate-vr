#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>
#include <string>
#include "stack_trace.h"
#include "config.h"
#include "log.h"

namespace so4vr {

void LogStackOnce(const char* tag, int maxTimes) {
    if (!g_config.debugStacks) return;
    static std::mutex m;
    static std::map<std::string, int> counts;
    {
        std::lock_guard<std::mutex> lk(m);
        int& c = counts[tag];
        if (c >= maxTimes) return;
        ++c;
    }
    void* frames[40];
    USHORT n = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    char line[2048];
    int len = snprintf(line, sizeof(line), "stack[%s] thread %lu:", tag, GetCurrentThreadId());
    for (USHORT i = 0; i < n && len < (int)sizeof(line) - 64; ++i) {
        HMODULE mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(frames[i]), &mod);
        uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
        if (reinterpret_cast<uintptr_t>(mod) == exe) {
            len += snprintf(line + len, sizeof(line) - len, " SO6+%llx", (unsigned long long)(a - exe));
        } else if (mod) {
            char name[64] = "?";
            GetModuleBaseNameA(GetCurrentProcess(), mod, name, sizeof(name));
            len += snprintf(line + len, sizeof(line) - len, " %s+%llx", name, (unsigned long long)(a - reinterpret_cast<uintptr_t>(mod)));
        } else {
            len += snprintf(line + len, sizeof(line) - len, " %p", frames[i]);
        }
    }
    LOGI("%s", line);
}

} // namespace so4vr
