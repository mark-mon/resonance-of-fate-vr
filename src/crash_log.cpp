// Crash logger: on an unhandled exception (or a fatal one seen first-chance in our own module), writes
// the exception, the registers and an unwound call stack (module+offset) to rofvr.log.
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include "log.h"

namespace so4vr {
namespace {

void Describe(DWORD64 a, char* out, size_t n) {
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m) {
        wchar_t p[MAX_PATH]; GetModuleFileNameW(m, p, MAX_PATH);
        const wchar_t* b = wcsrchr(p, L'\\'); b = b ? b + 1 : p;
        snprintf(out, n, "%ls+%llx", b, (unsigned long long)(a - (DWORD64)m));
    } else snprintf(out, n, "%llx", (unsigned long long)a);
}

void LogCrash(EXCEPTION_POINTERS* ep, const char* kind) {
    static volatile LONG count = 0;
    if (InterlockedIncrement(&count) > 4) return;   // a few, in case a harmless first-chance fault comes first
    EXCEPTION_RECORD* er = ep->ExceptionRecord; CONTEXT ctx = *ep->ContextRecord;
    char where[256]; Describe((DWORD64)er->ExceptionAddress, where, sizeof where);
    LOGE("CRASH (%s): code %08lx at %s thread %lu", kind, er->ExceptionCode, where, GetCurrentThreadId());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        LOGE("  access violation: %s address %llx", er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "execute",
             (unsigned long long)er->ExceptionInformation[1]);
    LOGE("  rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx r8 %llx r9 %llx rsp %llx",
         ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi, ctx.R8, ctx.R9, ctx.Rsp);
    for (int i = 0; i < 24 && ctx.Rip; ++i) {
        char d[256]; Describe(ctx.Rip, d, sizeof d);
        LOGE("  #%02d %s", i, d);
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn) { ctx.Rip = *(DWORD64*)ctx.Rsp; ctx.Rsp += 8; continue; }   // leaf function
        PVOID hd = nullptr; DWORD64 ef = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &hd, &ef, nullptr);
    }
}

LONG WINAPI Unhandled(EXCEPTION_POINTERS* ep) { LogCrash(ep, "unhandled"); return EXCEPTION_CONTINUE_SEARCH; }

// First-chance: only fatal codes raised inside our own DLL (the game may use SEH internally; a crash in our
// code inside a game-installed __try would otherwise be swallowed or reported without our frames).
HMODULE g_self = nullptr; MODULEINFO g_selfInfo{};
LONG WINAPI FirstChance(EXCEPTION_POINTERS* ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c != EXCEPTION_ACCESS_VIOLATION && c != EXCEPTION_ILLEGAL_INSTRUCTION && c != EXCEPTION_STACK_OVERFLOW && c != 0xC0000409) return EXCEPTION_CONTINUE_SEARCH;
    DWORD64 a = (DWORD64)ep->ExceptionRecord->ExceptionAddress, b = (DWORD64)g_selfInfo.lpBaseOfDll;
    // SO4: the exe's protection layer terminates the process on faults in game code too, so log every fatal one.
    LogCrash(ep, (a >= b && a < b + g_selfInfo.SizeOfImage) ? "first-chance in mod" : "first-chance (game/system)");
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void CrashLogInstall(HMODULE self) {
    g_self = self;
    GetModuleInformation(GetCurrentProcess(), self, &g_selfInfo, sizeof g_selfInfo);
    SetUnhandledExceptionFilter(Unhandled);
    AddVectoredExceptionHandler(1, FirstChance);   // first in the chain: log before anything else acts
}

} // namespace so4vr
