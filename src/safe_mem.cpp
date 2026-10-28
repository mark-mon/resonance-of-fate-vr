// Guarded memory reads/writes: a wrong offset must log a failure, not crash the game.
// SO4: the exe's protection layer terminates the process on access violations even inside our own
// __try blocks, so every access is checked with VirtualQuery first and never faults in practice.
// The __try stays as a last resort. NOTE: VirtualQuery costs ~1 ms per call in SO4 (hooked by the
// protection layer), so SafeRead/SafeWrite are for occasional reads of unknown pointers only; read objects
// the game hands us (hook arguments) directly.
#include <windows.h>
#include <cstring>
#include "safe_mem.h"

namespace so4vr {
namespace {

bool RangeOk(const void* p, size_t n, bool write) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (!a || a + n < a || a + n > 0x7FFFFFFFFFFFull || a < 0x10000) return false;
    // No caching: a region freed after it was cached would fault, and a fault is fatal in this game.
    uintptr_t cur = a, end = a + n;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(reinterpret_cast<void*>(cur), &m, sizeof m)) return false;
        if (m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        DWORD rw = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        DWORD ro = rw | PAGE_READONLY | PAGE_EXECUTE_READ;
        if (!(m.Protect & (write ? rw : ro))) return false;
        uintptr_t hi = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
        cur = hi;
    }
    return true;
}

} // namespace

bool SafeRead(void* dst, const void* src, size_t n) {
    if (!RangeOk(src, n, false)) return false;
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeWrite(void* dst, const void* src, size_t n) {
    if (!RangeOk(dst, n, true)) return false;
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace so4vr
