#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <share.h>

namespace so4vr {

// Simple thread-safe line logger writing to so4vr.log next to the game executable.
class Log {
public:
    static void Open(const wchar_t* path) {
        std::lock_guard<std::mutex> lk(mtx());
        if (file()) return;
        // Shared so the log can be read while the game is running.
        file() = _wfsopen(path, L"w", _SH_DENYNO);
    }
    static void Close() {
        std::lock_guard<std::mutex> lk(mtx());
        if (file()) { fclose(file()); file() = nullptr; }
    }
    static void Write(const char* level, const char* fmt, ...) {
        char msg[2048];
        va_list ap; va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        SYSTEMTIME t; GetLocalTime(&t);
        char line[2200];
        snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u][%5lu][%s] %s\n",
                 t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), level, msg);
        std::lock_guard<std::mutex> lk(mtx());
        if (file()) { fputs(line, file()); fflush(file()); }
        OutputDebugStringA(line);
    }
private:
    static FILE*& file() { static FILE* f = nullptr; return f; }
    static std::mutex& mtx() { static std::mutex m; return m; }
};

} // namespace so4vr

#define LOGI(...) ::so4vr::Log::Write("INFO", __VA_ARGS__)
#define LOGW(...) ::so4vr::Log::Write("WARN", __VA_ARGS__)
#define LOGE(...) ::so4vr::Log::Write("ERROR", __VA_ARGS__)
#define LOGD(...) do { if (::so4vr::g_config.verboseLog) ::so4vr::Log::Write("DEBUG", __VA_ARGS__); } while (0)
