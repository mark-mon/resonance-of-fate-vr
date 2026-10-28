#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include "native_stereo.h"
#include "temporal.h"
#include "reticle.h"
#include "hud_layer.h"
#include "render_res.h"
#include "config.h"
#include "log.h"
#include "vrmath.h"
#include "stack_trace.h"
#include "hook_util.h"
#include "safe_mem.h"
#include "ctx_stats.h"
#include "head_tracking.h"
#include "xr_runtime.h"
#include <d3d11.h>

namespace so4vr {
namespace {

// Render thread command processor (0x161D7C), command type 0:
//   161E84: mov rax,[r15] / mov r8d,r9d / mov rdx,r10 / mov rcx,r15 / call [rax+0x240]
// The 6-byte call is replaced by `call [rip+disp32]` to a pointer slot holding JobThunk.
constexpr uintptr_t kJobCallSite = 0x161E90;
constexpr uint8_t   kJobCallBytes[6] = {0xFF, 0x90, 0x40, 0x02, 0x00, 0x00};
constexpr int       kJobVtIndex = 0x240 / 8;

using FnJob = void (__fastcall*)(void* obj, void* arg1, uint32_t arg2);

struct Job { void* obj; void* arg1; uint32_t arg2; };
constexpr int kMaxJobs = 8192;
Job  g_jobs[kMaxJobs];
int  g_jobCount = 0;
bool g_overflow = false;
bool g_installed = false;
bool g_replayBroken = false;

std::atomic<uint64_t> s_jobs{0}, s_replays{0}, s_replayedJobs{0}, s_frames{0};
std::atomic<uint32_t> s_lastJobsPerFrame{0};
std::atomic<uint64_t> s_gameplayFrames{0};
double g_replayMsSum = 0; uint64_t g_replayMsN = 0;

bool g_inReplay = false;   // true while a frame is being executed a second time
void SnapshotGlobals();
extern std::atomic<uintptr_t> g_queue;
extern uint8_t* g_fakeQueue;
void __fastcall JobThunk(void* obj, void* arg1, uint32_t arg2) {
    if (!g_inReplay && g_jobCount == 0) SnapshotGlobals();   // first job of the frame
    if (g_inReplay) {} else if (g_jobCount < kMaxJobs) g_jobs[g_jobCount++] = Job{obj, arg1, arg2};
    else g_overflow = true;
    s_jobs++;
    FnJob fn = reinterpret_cast<FnJob>((*reinterpret_cast<void***>(obj))[kJobVtIndex]);
    static int hdrLogs = 0;
    bool watch = g_config.debugStacks && hdrLogs < 60 && s_gameplayFrames.load() >= 430 && s_gameplayFrames.load() <= 431;
    uintptr_t base = watch ? reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) : 0;
    float before[8] = {0};
    if (watch) memcpy(before, reinterpret_cast<void*>(base + 0x252DF30), 32);
    if (watch) {
        uintptr_t rva = reinterpret_cast<uintptr_t>(fn) - base;
        if (rva == 0x471164 || rva == 0x419100) {
            uintptr_t o = 0;
            if (rva == 0x471164) SafeLoad(reinterpret_cast<uintptr_t>(obj) + 0x330, o); else o = reinterpret_cast<uintptr_t>(obj);
            uint8_t flag = 0; float ex[2] = {0, 0};
            if (o) { SafeRead(&flag, reinterpret_cast<void*>(o + 0x8630), 1); SafeRead(ex, reinterpret_cast<void*>(o + 0x8648), 8); }
            ++hdrLogs;
            LOGI("exposure job %s SO6+%llx obj %p state %p flag %u saved (%.5f, %.5f)", g_inReplay ? "REPLAY" : "pass-1",
                 (unsigned long long)rva, obj, (void*)o, flag, ex[0], ex[1]);
        }
    }
    // During the replay, keep the exposure fields of the real queue (read by jobs through the game's global
    // queue pointer) in step with our private copy (written by the command processor), in both directions.
    uintptr_t realQ = g_inReplay ? g_queue.load(std::memory_order_relaxed) : 0;
    if (realQ && g_fakeQueue) memcpy(reinterpret_cast<void*>(realQ + 0x501A0), g_fakeQueue + 0x501A0, 0x20);
    fn(obj, arg1, arg2);
    if (realQ && g_fakeQueue) memcpy(g_fakeQueue + 0x501A0, reinterpret_cast<void*>(realQ + 0x501A0), 0x20);
    if (watch) {
        float after[8]; memcpy(after, reinterpret_cast<void*>(base + 0x252DF30), 32);
        static float last[8] = {0};
        if (memcmp(before, last, 32) != 0) {
            ++hdrLogs;
            LOGI("hdr globals before %s job #%d (vt fn SO6+%llx): df30 (%.5f, %.5f) df40 (%.5f, %.5f)", g_inReplay ? "REPLAY" : "pass-1",
                 g_jobCount, (unsigned long long)(reinterpret_cast<uintptr_t>(fn) - base), before[0], before[1], before[4], before[5]);
        }
        if (memcmp(before, after, 32) != 0) {
            ++hdrLogs;
            LOGI("hdr globals changed IN %s job #%d (vt fn SO6+%llx): df30 -> (%.5f, %.5f) df40 -> (%.5f, %.5f)", g_inReplay ? "REPLAY" : "pass-1",
                 g_jobCount, (unsigned long long)(reinterpret_cast<uintptr_t>(fn) - base), after[0], after[1], after[4], after[5]);
        }
        memcpy(last, after, 32);
    }
}

void* AllocNear(uintptr_t target, size_t size) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    for (uintptr_t d = gran; d < 0x70000000ull; d += gran) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            uintptr_t a = sgn < 0 ? target - d : target + d;
            a &= ~(gran - 1);
            void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (p) return p;
        }
    }
    return nullptr;
}

// Rotates the view in a ViewInfo about view-space Y (same as head_tracking's diagnostic).
void YawViewInfo(uintptr_t vi, float deg) {
    float* V = reinterpret_cast<float*>(vi + 0x000);
    float* IV = reinterpret_cast<float*>(vi + 0x040);
    float* P = reinterpret_cast<float*>(vi + 0x080);
    float* IP = reinterpret_cast<float*>(vi + 0x0C0);
    float* VP = reinterpret_cast<float*>(vi + 0x100);
    float* IVP = reinterpret_cast<float*>(vi + 0x140);
    float a = deg * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
    const float R[16] = { c, 0, s, 0,  0, 1, 0, 0,  -s, 0, c, 0,  0, 0, 0, 1 };
    const float Rt[16] = { c, 0, -s, 0,  0, 1, 0, 0,  s, 0, c, 0,  0, 0, 0, 1 };
    float nV[16], nIV[16], nVP[16], nIVP[16];
    vm::Mul4(R, V, nV); vm::Mul4(IV, Rt, nIV); vm::Mul4(P, nV, nVP); vm::Mul4(nIV, IP, nIVP);
    memcpy(V, nV, 64); memcpy(IV, nIV, 64); memcpy(VP, nVP, 64); memcpy(IVP, nIVP, 64);
}

// Command processor: void(queue, int* jobCounter, bool* flag). Runs commands from read index (+0xC4,
// last consumed) until it reaches the write index (+0xC0, next to be written).
constexpr uintptr_t kFnProcess = 0x161D7C;
constexpr uintptr_t kQRead = 0xC4, kQWrite = 0xC0, kQRing = 0xC8, kQEntry = 0x28, kQBusy = 0x98, kQWaitMode = 0xB2;
constexpr int       kQEntries = 0x2001;
constexpr size_t    kQueueCopySize = 0x50200;
using FnProcess = uint64_t (__fastcall*)(void* q, int* counter, uint8_t* flag);
FnProcess o_Process = nullptr;
std::atomic<uintptr_t> g_queue{0};
int  g_prevEnd = -1;
uint8_t* g_fakeQueue = nullptr;

// Per-view constant setup (vtable slot +0x20 of the render-view manager, called from the scene job 0xB8B44):
// builds the CPU-side view constants (per-pass cb, frustum, ...) from the ViewInfo read slot.
constexpr uintptr_t kFnViewSetup = 0x51520;
using FnViewSetup = void (__fastcall*)(void* self, void* p, uint32_t mode, uint32_t extra);
FnViewSetup o_ViewSetup = nullptr;
struct { void* self; void* p; uint32_t mode, extra; bool have; } g_vsArgs{};
std::atomic<uint64_t> s_vsCalls{0}, s_vsReplayCalls{0};
void __fastcall hk_ViewSetup(void* self, void* p, uint32_t mode, uint32_t extra) {
    if (g_inReplay) s_vsReplayCalls++;
    else { s_vsCalls++; g_vsArgs = {self, p, mode, extra, true}; }
    o_ViewSetup(self, p, mode, extra);
}
bool CallViewSetup() {
    if (!g_vsArgs.have) return false;
    __try { o_ViewSetup(g_vsArgs.self, g_vsArgs.p, g_vsArgs.mode, g_vsArgs.extra); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Stateful post-process pass (0x1B13E4, renders the full-res R16F target from depth): keeps ping-pong
// indices and "done" flags in its object (+0x66E..+0x677), so a second run in the same frame takes a
// different path. Snapshot the object at its first call each frame and restore it before the replay.
constexpr uintptr_t kFnStatefulPass = 0x1B13E4;
constexpr size_t    kStatefulSnap = 0x700;
using FnStatefulPass = uint64_t (__fastcall*)(void* self, void* a, void* b, void* c);
FnStatefulPass o_StatefulPass = nullptr;
struct StateSnap { void* obj; uint8_t bytes[kStatefulSnap]; bool have; };
StateSnap g_snaps[4]; int g_snapCount = 0;
uint64_t __fastcall hk_StatefulPass(void* self, void* a, void* b, void* c) {
    if (!g_inReplay && g_config.replayRestoreState) {
        int k = 0;
        for (; k < g_snapCount; ++k) if (g_snaps[k].obj == self) break;
        if (k == g_snapCount && g_snapCount < 4) { g_snaps[k].obj = self; g_snaps[k].have = false; ++g_snapCount; }
        if (k < 4 && !g_snaps[k].have) { g_snaps[k].have = SafeRead(g_snaps[k].bytes, self, kStatefulSnap); }
    }
    if (g_config.debugStacks) {
        static int logged = 0;
        if (logged < 24 && s_gameplayFrames.load() > 600) {
            ++logged;
            const uint8_t* p = static_cast<const uint8_t*>(self);
            LOGI("stateful pass %s: obj %p flags +66E..+677: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", g_inReplay ? "REPLAY" : "first ",
                 self, p[0x66e], p[0x66f], p[0x670], p[0x671], p[0x672], p[0x673], p[0x674], p[0x675], p[0x676], p[0x677]);
        }
    }
    uint64_t r = o_StatefulPass(self, a, b, c);
    if (g_config.debugStacks) {
        static int logged2 = 0;
        if (logged2 < 24 && s_gameplayFrames.load() > 600) {
            ++logged2;
            const uint8_t* p = static_cast<const uint8_t*>(self);
            LOGI("stateful pass %s after: flags +66E..+677: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", g_inReplay ? "REPLAY" : "first ",
                 p[0x66e], p[0x66f], p[0x670], p[0x671], p[0x672], p[0x673], p[0x674], p[0x675], p[0x676], p[0x677]);
        }
    }
    return r;
}
// Per-frame "already done" bitmaps in the game's globals, cleared before each frame's jobs start.
struct GlobalRegion { uintptr_t rva; size_t size; uint8_t snap[256]; bool have; };
GlobalRegion g_globals[] = {
    {0x252F3A0, 0x40, {0}, false},   // per-view bitmap set after the full-res R16F pass (0x1B06D5)
    {0x252DF30, 0x40, {0}, false},   // render globals incl. 0x252DF50 = cvHDRTransform (exposure, 1/exposure, 0, 1),
                                     // reset to (1,1,0,1) by a later command, so the replay saw no exposure
};
// Exposure kept in the render queue object (+0x501A8 = exposure, +0x501AC = scale), written by the
// "set exposure" command (0x162F28) and read by jobs through the global queue pointer (the real queue,
// not our private copy). Snapshot at frame start, restore into both queues before the replay.
constexpr uintptr_t kQExposure = 0x501A0; constexpr size_t kQExposureSize = 0x20;
uint8_t g_qExpSnap[kQExposureSize]; bool g_qExpHave = false;
void SnapshotGlobals() {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    for (auto& g : g_globals) g.have = SafeRead(g.snap, reinterpret_cast<void*>(base + g.rva), g.size);
    uintptr_t q = g_queue.load(std::memory_order_relaxed);
    g_qExpHave = q && SafeRead(g_qExpSnap, reinterpret_cast<void*>(q + kQExposure), kQExposureSize);
}
void RestoreGlobals() {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    for (auto& g : g_globals) if (g.have) memcpy(reinterpret_cast<void*>(base + g.rva), g.snap, g.size);
    uintptr_t q = g_queue.load(std::memory_order_relaxed);
    if (g_qExpHave && q) {
        memcpy(reinterpret_cast<void*>(q + kQExposure), g_qExpSnap, kQExposureSize);
        if (g_fakeQueue) memcpy(g_fakeQueue + kQExposure, g_qExpSnap, kQExposureSize);
    }
}

void RestoreStatefulPasses() {
    for (int k = 0; k < g_snapCount; ++k) if (g_snaps[k].have) memcpy(g_snaps[k].obj, g_snaps[k].bytes, kStatefulSnap);
}
// TAA depth state only (ReplayRestoreMask bit 8). Of the whole object, two bytes decide what the right eye's TAA
// does with its linear-depth pair: +0x673 picks which texture of the pair is "current" (flipped once per pass)
// and +0x66F bit 0x10 means "linear depth already made this frame" (set by 0x150298, so the replay skipped
// it and its TAA compared against a stale texture: dithered characters were not resolved in the right eye).
// Before the replay: put both back to their frame-start values, so the right eye writes its own linear depth
// into the same "current" texture; after it: back to the values pass 1 left, so the frame advances once.
// The per-eye history copies (temporal.cpp) then give each eye its own "previous" depth.
struct TaaDepthPost { uint8_t b673, b66f; bool have; };
TaaDepthPost g_taaPost[4];
void TaaDepthBeforeReplay() {
    for (int k = 0; k < g_snapCount; ++k) {
        g_taaPost[k].have = false;
        if (!g_snaps[k].have) continue;
        uint8_t* p = static_cast<uint8_t*>(g_snaps[k].obj);
        g_taaPost[k] = {p[0x673], p[0x66f], true};
        p[0x673] = g_snaps[k].bytes[0x673];
        p[0x66f] = g_snaps[k].bytes[0x66f];
    }
}
void TaaDepthAfterReplay() {
    for (int k = 0; k < g_snapCount; ++k) {
        if (!g_taaPost[k].have) continue;
        uint8_t* p = static_cast<uint8_t*>(g_snaps[k].obj);
        p[0x673] = g_taaPost[k].b673;
        p[0x66f] = g_taaPost[k].b66f;
        g_taaPost[k].have = false;
    }
}
void NewFrameStatefulPasses() { for (int k = 0; k < g_snapCount; ++k) g_snaps[k].have = false; }

// "Set exposure" (0x162F28): void(queue, exposure, scale). Called by command type 0x10 with the queue being
// processed (our private copy during the replay), while the exposure save/restore jobs read it back through
// the game's global queue pointer (the real queue). Mirror replay writes into the real queue.
constexpr uintptr_t kFnSetExposure = 0x162F28;
using FnSetExposure = void (__fastcall*)(void* q, float exposure, float scale);
FnSetExposure o_SetExposure = nullptr;
void __fastcall hk_SetExposure(void* q, float exposure, float scale) {
    o_SetExposure(q, exposure, scale);
    uintptr_t real = g_queue.load(std::memory_order_relaxed);
    if (g_inReplay && real && q == g_fakeQueue) o_SetExposure(reinterpret_cast<void*>(real), exposure, scale);
}

uint64_t __fastcall hk_Process(void* q, int* counter, uint8_t* flag) {
    if (!g_inReplay) g_queue.store(reinterpret_cast<uintptr_t>(q), std::memory_order_relaxed);
    return o_Process(q, counter, flag);
}

bool CallProcess(void* q, int* c, uint8_t* f) {
    __try { o_Process(q, c, f); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void LogFrameCommandsOnce(uintptr_t q, int from, int to) {
    static int logged = 0;
    if (logged >= 2) return;
    ++logged;
    int hist[256] = {0}; int total = 0;
    for (int i = from; i != to; ) {
        i = (i + 1 < kQEntries) ? i + 1 : 0;
        if (i == to) break;
        uint8_t t = *reinterpret_cast<uint8_t*>(q + kQRing + uintptr_t(i) * kQEntry);
        hist[t]++; total++;
    }
    char line[1024]; int n = snprintf(line, sizeof(line), "native stereo: frame commands %d (%d..%d):", total, from, to);
    for (int t = 0; t < 256; ++t) if (hist[t]) n += snprintf(line + n, sizeof(line) - n, " type%d=%d", t, hist[t]);
    LOGI("%s", line);
}

bool ReplayJobs(int n) {
    __try {
        for (int i = 0; i < n; ++i) {
            const Job& j = g_jobs[i];
            FnJob fn = reinterpret_cast<FnJob>((*reinterpret_cast<void***>(j.obj))[kJobVtIndex]);
            fn(j.obj, j.arg1, j.arg2);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- ViewInfo read watch (guard page) ----
uintptr_t g_watchLo = 0, g_watchHi = 0, g_watchPage = 0;
volatile LONG g_watchArmed = 0;
struct WatchHit { uintptr_t rip; uint32_t off; DWORD tid; uintptr_t rva[6]; int count; };
WatchHit g_watchHits[64]; volatile LONG g_watchN = 0;
PVOID g_veh = nullptr;
thread_local bool t_rearm = false;

LONG CALLBACK WatchVeh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == STATUS_GUARD_PAGE_VIOLATION && g_watchPage) {
        uintptr_t addr = ep->ExceptionRecord->ExceptionInformation[1];
        if (addr >= g_watchPage && addr < g_watchPage + 0x2000) {
            if (addr >= g_watchLo && addr < g_watchHi) {
                uintptr_t rip = ep->ContextRecord->Rip;
                LONG n = g_watchN; bool found = false;
                for (LONG i = 0; i < n; ++i) if (g_watchHits[i].rip == rip) { g_watchHits[i].count++; found = true; break; }
                if (!found && n < 64) {
                    WatchHit& h = g_watchHits[n];
                    h.rip = rip; h.off = uint32_t(addr - g_watchLo); h.tid = GetCurrentThreadId(); h.count = 1;
                    void* fr[20]; USHORT k = RtlCaptureStackBackTrace(0, 20, fr, nullptr);
                    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                    int j = 0;
                    for (USHORT q = 0; q < k && j < 6; ++q) { uintptr_t a = (uintptr_t)fr[q]; if (a >= base && a < base + 0x3000000) h.rva[j++] = a - base; }
                    for (; j < 6; ++j) h.rva[j] = 0;
                    g_watchN = n + 1;
                }
            }
            if (g_watchArmed) { ep->ContextRecord->EFlags |= 0x100; t_rearm = true; }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code == STATUS_SINGLE_STEP && t_rearm) {
        t_rearm = false;
        if (g_watchArmed) { DWORD old; VirtualProtect((void*)g_watchPage, 0x2000, PAGE_READWRITE | PAGE_GUARD, &old); }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void WatchArm(uintptr_t vi) {
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, WatchVeh);
    g_watchLo = vi; g_watchHi = vi + 0x530; g_watchPage = vi & ~uintptr_t(0xFFF); g_watchN = 0;
    g_watchArmed = 1;
    DWORD old; VirtualProtect((void*)g_watchPage, 0x2000, PAGE_READWRITE | PAGE_GUARD, &old);
}

void WatchDisarmAndReport() {
    g_watchArmed = 0;
    DWORD old; VirtualProtect((void*)g_watchPage, 0x2000, PAGE_READWRITE, &old);
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    LOGI("ViewInfo read watch: %ld distinct code locations read the read slot during one frame", g_watchN);
    for (LONG i = 0; i < g_watchN; ++i) {
        const WatchHit& h = g_watchHits[i];
        LOGI("  rip %s+%llx  (ViewInfo +0x%03X, thread %lu, x%d)  stack SO6+%llx SO6+%llx SO6+%llx SO6+%llx SO6+%llx SO6+%llx",
             (h.rip >= base && h.rip < base + 0x3000000) ? "SO6" : "?", (unsigned long long)(h.rip >= base ? h.rip - base : h.rip),
             h.off, h.tid, h.count, (unsigned long long)h.rva[0], (unsigned long long)h.rva[1], (unsigned long long)h.rva[2],
             (unsigned long long)h.rva[3], (unsigned long long)h.rva[4], (unsigned long long)h.rva[5]);
    }
    g_watchPage = 0;
}
bool g_watchPending = false, g_watchDone = false;

// ---- camera matrix replacement during replay ----
float g_origM[6][16], g_newM[6][16];
float g_delta[16];   // VP_new * InvVP_orig: maps this frame's clip space to the other eye's
bool  g_patching = false;
std::atomic<uint64_t> s_patchHits{0};

void CaptureMatrices(uintptr_t vi, float out[6][16]) { memcpy(out, reinterpret_cast<void*>(vi), 6 * 64); }

// ---- camera-matrix search (diagnostic) ----
struct Upload { uint32_t off, bytes, bufBytes; const char* how; uintptr_t rva[6]; };
std::vector<Upload> g_ups, g_ups2;
std::vector<uint8_t> g_upData, g_upData2;
bool g_cmpArmed = false, g_cmpDone = false;   // compare pass-1 vs replay uploads in one frame
bool g_scanArmed = false, g_scanDone = false;

bool Near(float a, float b) { return std::fabs(a - b) <= 1e-4f + 2e-3f * std::fabs(b); }
bool Interesting(const float* v) {
    int nontriv = 0;
    for (int i = 0; i < 4; ++i) { float a = std::fabs(v[i]); if (a > 1e-3f && std::fabs(a - 1.0f) > 1e-3f) ++nontriv; }
    return nontriv >= 2;
}

void RunScan(uintptr_t vi) {
    const char* names[6] = {"View", "InvView", "Proj", "InvProj", "ViewProj", "InvViewProj"};
    struct Key { int m, t, r; uint32_t buf; const char* how; uint32_t off; uintptr_t r0, r1; };
    std::vector<std::pair<Key, int>> agg;
    LOGI("camera search: %zu uploads, %zu bytes in the frame", g_ups.size(), g_upData.size());
    int hits = 0;
    for (int m = 0; m < 6; ++m) {
        const float* M = reinterpret_cast<const float*>(vi + m * 0x40);
        for (int t = 0; t < 2; ++t) for (int r = 0; r < 4; ++r) {
            float v[4];
            for (int c = 0; c < 4; ++c) v[c] = t == 0 ? M[r * 4 + c] : M[c * 4 + r];
            if (!Interesting(v)) continue;
            for (const Upload& u : g_ups) {
                const float* d = reinterpret_cast<const float*>(g_upData.data() + u.off);
                int nf = int(u.bytes / 4);
                for (int i = 0; i + 4 <= nf; ++i) {
                    if (Near(d[i], v[0]) && Near(d[i + 1], v[1]) && Near(d[i + 2], v[2]) && Near(d[i + 3], v[3])) {
                        ++hits;
                        Key k{m, t, r, u.bufBytes, u.how, uint32_t(i * 4), u.rva[0], u.rva[2]};
                        bool found = false;
                        for (auto& a : agg) {
                            const Key& q = a.first;
                            if (q.m == k.m && q.t == k.t && q.r == k.r && q.buf == k.buf && q.how == k.how && q.off == k.off && q.r0 == k.r0) { a.second++; found = true; break; }
                        }
                        if (!found && agg.size() < 400) agg.push_back({k, 1});
                    }
                }
            }
        }
    }
    LOGI("camera search: %d hits in %zu distinct places", hits, agg.size());
    for (auto& a : agg) {
        const Key& k = a.first;
        LOGI("camera search: %-11s %s %d  in %4u-byte buffer (%s) at +0x%03X  x%d  (writer SO6+%llx, via SO6+%llx)",
             names[k.m], k.t ? "col" : "row", k.r, k.buf, k.how, k.off, a.second, (unsigned long long)k.r0, (unsigned long long)k.r1);
    }
}

} // namespace

bool NativePatchActive() { return g_patching; }
bool NativeInReplay() { return g_inReplay; }
bool NativeReplayEnabled() { return g_config.debugReplay || g_config.activeMode == Config::Native; }

// ---- native stereo (RenderMode = Native) ----
// The game renders the head camera (centre between the eyes, union FOV, which also drives culling).
// Pass 1 is turned into the left eye and the replay into the right eye: ViewInfo gets the eye's view and
// projection (lighting / per-pass constants), and every clip-space matrix in the constant uploads is mapped
// from the head camera's clip space to the eye's.
constexpr size_t kVIMats = 0x180;      // V, InvV, P, InvP, VP, InvVP
constexpr uintptr_t kVIPos = 0x4D0;    // camera position (float3)
struct StereoState {
    bool active = false;
    HeadFrame hf{};
    float center[kVIMats / 4];
    float eye[2][kVIMats / 4];
    float centerPos[3];
    float eyePos[2][3];
} g_st;
ID3D11Texture2D* g_eyeTex[2] = {};
void DumpSeqFrame();
bool g_monoCaptured = false;

// ---- previous-frame scene targets (one frame of latency in the final composite) ----
bool g_latTrack = false;
static bool s_patchStopsAtComposite = true;
bool g_latWantSrv = false;
ID3D11Resource* g_finalRT = nullptr;         // identity only (the target bound at the end of the frame)
ID3D11Resource* g_finalRTPrev = nullptr;     // identity only (the game may alternate two final targets)
bool NativeIsFinalRT(ID3D11Resource* r) { return r && (r == g_finalRT || r == g_finalRTPrev); }
static void SetFinalRT(ID3D11Resource* r) { if (r && r != g_finalRT) { g_finalRTPrev = g_finalRT; g_finalRT = r; } }
static ID3D11Resource* s_rtCur = nullptr;    // referenced
static ID3D11Resource* s_rtPrev = nullptr;   // referenced
struct LatPass { ID3D11Texture2D* xCur = nullptr; ID3D11Texture2D* xPrev = nullptr; bool armed = false; bool found = false; };
static LatPass s_lat[2];
static ID3D11Texture2D* s_saveL = nullptr;
static ID3D11Texture2D* s_saveR = nullptr;
static bool s_saveRValid = false;
static ID3D11Texture2D* s_lastXCur = nullptr; // identity only, for the alternation check
static std::atomic<unsigned long long> s_rtBinds{0}, s_finalBinds{0};
static std::atomic<unsigned long long> s_latApplied{0}, s_latSkipped{0}, s_latAltOk{0}, s_latAltBad{0};
static HeadFrame s_prevHf{};
static bool s_prevHfValid = false;

static void LatReset(LatPass& l) {
    if (l.xCur) { l.xCur->Release(); l.xCur = nullptr; }
    if (l.xPrev) { l.xPrev->Release(); l.xPrev = nullptr; }
    l.armed = l.found = false;
}

void NativeOnSetRT(ID3D11Resource* rt0) {
    // Hold references: transient targets can be released between frames.
    if (rt0 && rt0 != s_rtCur) {   // ignore unbinds (null targets between passes)
        if (s_rtPrev) s_rtPrev->Release();
        s_rtPrev = s_rtCur;
        s_rtCur = rt0;
        if (s_rtCur) s_rtCur->AddRef();
    }
    LatPass& l = s_lat[g_inReplay ? 1 : 0];
    s_rtBinds++;
    if (NativeIsFinalRT(rt0)) {
        s_finalBinds++;
    }
    if (NativeIsFinalRT(rt0) && !l.armed && s_rtPrev) {
        D3D11_RESOURCE_DIMENSION dim; s_rtPrev->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            l.xCur = static_cast<ID3D11Texture2D*>(s_rtPrev); l.xCur->AddRef();
            l.armed = true; g_latWantSrv = true;
        }
    }
}

static bool s_passFullRes = true;
void NativeOnPassTarget(ID3D11Resource* rt0, ID3D11DepthStencilView* dsv) {
    if (!g_config.patchFullResOnly) { s_passFullRes = true; return; }
    ID3D11Resource* r = rt0;
    bool release = false;
    if (!r && dsv) { dsv->GetResource(&r); release = r != nullptr; }
    UINT w = 0;
    if (r) {
        D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC d; static_cast<ID3D11Texture2D*>(r)->GetDesc(&d); w = d.Width; }
    }
    if (release) r->Release();
    uint32_t rw = 0, rh = 0;
    RenderResCurrent(rw, rh);
    s_passFullRes = !r || rw == 0 || w * 100 >= rw * 45;
}

void NativeOnDraw(ID3D11DeviceContext* ctx) {
    g_latWantSrv = false;
    // This draw is the final composite: the 3D scene is done. Stop rewriting camera matrices in uploads,
    // so the HUD/UI that follows (minimap, menus) gets its constants untouched.
    if (s_patchStopsAtComposite) g_patching = false;
    LatPass& l = s_lat[g_inReplay ? 1 : 0];
    if (!l.armed || l.found) return;
    l.found = true;
    ID3D11ShaderResourceView* v = nullptr;
    ctx->PSGetShaderResources(0, 1, &v);
    if (!v) return;
    ID3D11Resource* r = nullptr; v->GetResource(&r); v->Release();
    if (!r) return;
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) l.xPrev = static_cast<ID3D11Texture2D*>(r);
    else r->Release();
}

static bool LatValid(const LatPass& l) {
    if (!l.found || !l.xCur || !l.xPrev) return false;
    if (l.xCur == l.xPrev) return true;   // composite reads this frame's scene (it also serves as TAA history)
    D3D11_TEXTURE2D_DESC a, b; l.xCur->GetDesc(&a); l.xPrev->GetDesc(&b);
    return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format && a.SampleDesc.Count == 1 && a.ArraySize == 1 && a.MipLevels == b.MipLevels;
}

static bool EnsureLike(ID3D11Texture2D*& t, ID3D11Texture2D* like) {
    D3D11_TEXTURE2D_DESC d; like->GetDesc(&d);
    if (t) { D3D11_TEXTURE2D_DESC h; t->GetDesc(&h); if (h.Width == d.Width && h.Height == d.Height && h.Format == d.Format && h.MipLevels == d.MipLevels) return true; t->Release(); t = nullptr; }
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = 0; d.MiscFlags = 0;
    ID3D11Device* dev = nullptr; like->GetDevice(&dev);
    if (dev) { dev->CreateTexture2D(&d, nullptr, &t); dev->Release(); }
    if (t) TemporalMarkModTexture(t);
    return t != nullptr;
}
// Field of view each eye is rendered with this frame (Native stereo), for the foveation pattern.
// Cached when a stereo frame starts (render-target binds see g_st between frames too, when it is not active).
static float s_eyeTan[2][4];
static bool s_eyeTanValid = false;
bool NativeIsHistoryFixTexture(ID3D11Resource* r) {
    // Also the game's final image targets: the composite reads last frame's, but it is what we capture per eye.
    return r && (r == s_lat[0].xCur || r == s_lat[0].xPrev || r == s_lat[1].xCur || r == s_lat[1].xPrev || NativeIsFinalRT(r));
}
bool NativeEyeFov(int e, float tanLRUD[4]) {
    if (!s_eyeTanValid || e < 0 || e > 1 || g_config.activeMode != Config::Native) return false;
    memcpy(tanLRUD, s_eyeTan[e], sizeof(s_eyeTan[e]));
    return true;
}
std::atomic<uint64_t> s_stereoFrames{0}, s_stereoNoFrame{0}, s_eyeCaptureFail{0};

void BuildEye(const StereoState& st, int e, float out[kVIMats / 4], float outPos[3]) {
    const float* Vc = st.center;
    const float* IVc = st.center + 16;
    const float* Pc = st.center + 32;
    // Eye -> head transform in the game's view space, and its inverse.
    vm::Quat q{st.hf.eyeRot[e * 4 + 0], st.hf.eyeRot[e * 4 + 1], st.hf.eyeRot[e * 4 + 2], st.hf.eyeRot[e * 4 + 3]};
    float r[9]; vm::QuatToMat3(q, r);
    const float* o = st.hf.eyeOff[e];
    float M[16]; vm::MakeTransform(r, {o[0], o[1], o[2]}, M);
    float E[16] = { r[0], r[3], r[6], 0,  r[1], r[4], r[7], 0,  r[2], r[5], r[8], 0,  0, 0, 0, 1 };
    E[3]  = -(r[0] * o[0] + r[3] * o[1] + r[6] * o[2]);
    E[7]  = -(r[1] * o[0] + r[4] * o[1] + r[7] * o[2]);
    E[11] = -(r[2] * o[0] + r[5] * o[1] + r[8] * o[2]);
    float V[16], IV[16], P[16], IP[16], VP[16], IVP[16];
    vm::Mul4(E, Vc, V);
    vm::Mul4(IVc, M, IV);
    // Projection: the head camera's with the eye's frustum (keeps near plane, reversed-Z and TAA jitter).
    auto tn = [](float a) { return std::tan(a); };
    const XrFovf& u = st.hf.fov; const XrFovf& f = st.hf.eyeFov[e];
    float uhw = (tn(u.angleRight) - tn(u.angleLeft)) * 0.5f, ucx = (tn(u.angleRight) + tn(u.angleLeft)) * 0.5f;
    float uhh = (tn(u.angleUp) - tn(u.angleDown)) * 0.5f,    ucy = (tn(u.angleUp) + tn(u.angleDown)) * 0.5f;
    float ehw = (tn(f.angleRight) - tn(f.angleLeft)) * 0.5f, ecx = (tn(f.angleRight) + tn(f.angleLeft)) * 0.5f;
    float ehh = (tn(f.angleUp) - tn(f.angleDown)) * 0.5f,    ecy = (tn(f.angleUp) + tn(f.angleDown)) * 0.5f;
    memcpy(P, Pc, 64);
    P[0] = Pc[0] * (uhw / ehw);
    P[5] = Pc[5] * (uhh / ehh);
    P[2] = Pc[2] + (ucx / uhw - ecx / ehw);
    P[6] = Pc[6] + (ucy / uhh - ecy / ehh) * g_config.lensShiftYSign;
    if (!vm::Inverse4(P, IP)) memcpy(IP, st.center + 48, 64);
    vm::Mul4(P, V, VP);
    vm::Mul4(IV, IP, IVP);
    memcpy(out + 0, V, 64); memcpy(out + 16, IV, 64); memcpy(out + 32, P, 64);
    memcpy(out + 48, IP, 64); memcpy(out + 64, VP, 64); memcpy(out + 80, IVP, 64);
    outPos[0] = IV[3]; outPos[1] = IV[7]; outPos[2] = IV[11];
}

void WriteEyeToViewInfo(uintptr_t vi, int e) {
    memcpy(reinterpret_cast<void*>(vi), g_st.eye[e], kVIMats);
    memcpy(reinterpret_cast<void*>(vi + kVIPos), g_st.eyePos[e], 12);
}

void StereoBegin(uintptr_t vi) {
    g_st.active = false;
    g_patching = false;
    if (!vi || g_config.activeMode != Config::Native) return;
    const float* V = reinterpret_cast<const float*>(vi);
    if (V[0] == 1.0f && V[5] == 1.0f && V[10] == 1.0f && V[3] == 0.0f && V[7] == 0.0f) return; // title/menu camera
    if (!HeadTrackFindFrame(V, g_st.hf)) { s_stereoNoFrame++; return; }
    memcpy(g_st.center, reinterpret_cast<void*>(vi), kVIMats);
    memcpy(g_st.centerPos, reinterpret_cast<void*>(vi + kVIPos), 12);
    for (int e = 0; e < 2; ++e) BuildEye(g_st, e, g_st.eye[e], g_st.eyePos[e]);
    for (int e = 0; e < 2; ++e) {
        const XrFovf& f = g_st.hf.eyeFov[e];
        s_eyeTan[e][0] = std::tan(f.angleLeft); s_eyeTan[e][1] = std::tan(f.angleRight);
        s_eyeTan[e][2] = std::tan(f.angleUp);   s_eyeTan[e][3] = std::tan(f.angleDown);
    }
    s_eyeTanValid = true;
    static int logged = 0;
    if (logged < 2) {
        ++logged;
        const HeadFrame& h = g_st.hf;
        LOGI("native stereo: eye offsets L(%.2f %.2f %.2f) R(%.2f %.2f %.2f) cm, rot L(%.3f %.3f %.3f %.3f) R(%.3f %.3f %.3f %.3f)",
             h.eyeOff[0][0], h.eyeOff[0][1], h.eyeOff[0][2], h.eyeOff[1][0], h.eyeOff[1][1], h.eyeOff[1][2],
             h.eyeRot[0], h.eyeRot[1], h.eyeRot[2], h.eyeRot[3], h.eyeRot[4], h.eyeRot[5], h.eyeRot[6], h.eyeRot[7]);
        LOGI("native stereo: centre V row0 (%.4f %.4f %.4f %.1f) | left V row0 (%.4f %.4f %.4f %.1f) | right V row0 (%.4f %.4f %.4f %.1f)",
             g_st.center[0], g_st.center[1], g_st.center[2], g_st.center[3], g_st.eye[0][0], g_st.eye[0][1], g_st.eye[0][2], g_st.eye[0][3],
             g_st.eye[1][0], g_st.eye[1][1], g_st.eye[1][2], g_st.eye[1][3]);
        LOGI("native stereo: centre P (%.4f %.4f %.4f) left P (%.4f %.4f %.4f) right P (%.4f %.4f %.4f) [m00 m02 m11]",
             g_st.center[32], g_st.center[34], g_st.center[37], g_st.eye[0][32], g_st.eye[0][34], g_st.eye[0][37],
             g_st.eye[1][32], g_st.eye[1][34], g_st.eye[1][37]);
    }
    // Pass 1 becomes the left eye.
    WriteEyeToViewInfo(vi, 0);
    memcpy(g_origM, g_st.center, sizeof(g_origM));
    memcpy(g_newM, g_st.eye[0], sizeof(g_newM));
    vm::Mul4(g_newM[4], g_origM[5], g_delta);
    g_patching = true;
    g_st.active = true;
}

// Copies the frame's final image (the render target bound at the end of the frame) into an eye texture.
bool CaptureEye(int e) {
    ID3D11DeviceContext* ctx = CtxImmediate();
    if (!ctx) return false;
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return false;
    ID3D11Resource* res = nullptr; rtv->GetResource(&res); rtv->Release();
    if (!res) return false;
    bool ok = false;
    D3D11_RESOURCE_DIMENSION dim; res->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        ID3D11Texture2D* src = static_cast<ID3D11Texture2D*>(res);
        SetFinalRT(res);
        D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
        static bool logged = false;
        if (!logged) { logged = true; LOGI("native stereo: eye image %ux%u format %d", d.Width, d.Height, d.Format); }
        if (d.SampleDesc.Count == 1 && d.Width >= 640 && d.ArraySize == 1) {
            D3D11_TEXTURE2D_DESC have{};
            if (g_eyeTex[e]) g_eyeTex[e]->GetDesc(&have);
            if (!g_eyeTex[e] || have.Width != d.Width || have.Height != d.Height || have.Format != d.Format) {
                if (g_eyeTex[e]) { g_eyeTex[e]->Release(); g_eyeTex[e] = nullptr; }
                D3D11_TEXTURE2D_DESC n = d;
                n.MipLevels = 1; n.Usage = D3D11_USAGE_DEFAULT; n.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;   // RT: the aim crosshair is drawn in
                n.CPUAccessFlags = 0; n.MiscFlags = 0;
                ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
                if (dev) { dev->CreateTexture2D(&n, nullptr, &g_eyeTex[e]); dev->Release(); }
                if (g_eyeTex[e]) TemporalMarkModTexture(g_eyeTex[e]);
            }
            if (g_eyeTex[e]) { ctx->CopySubresourceRegion(g_eyeTex[e], 0, 0, 0, 0, src, 0, nullptr); ok = true; }
        }
    }
    res->Release();
    if (!ok) s_eyeCaptureFail++;
    return ok;
}

// Diagnostic: write both eye images (half size) as BMP files into the game folder.
void DumpEyeBmp(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const wchar_t* path, bool alpha = false) {
    if (!ctx || !tex) return;
    D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    if (sd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    ID3D11Texture2D* st = nullptr;
    if (dev) { dev->CreateTexture2D(&sd, nullptr, &st); dev->Release(); }
    if (!st) return;
    ctx->CopyResource(st, tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        UINT w = d.Width / 2, h = d.Height / 2;
        UINT rowBytes = (w * 3 + 3) & ~3u;
        std::vector<uint8_t> px(size_t(rowBytes) * h);
        bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        for (UINT y = 0; y < h; ++y) {
            const uint8_t* src = static_cast<const uint8_t*>(m.pData) + size_t(y * 2) * m.RowPitch;
            uint8_t* dst = px.data() + size_t(h - 1 - y) * rowBytes;
            for (UINT x = 0; x < w; ++x) {
                const uint8_t* p4 = src + size_t(x * 2) * 4;
                if (alpha) { dst[x * 3 + 0] = dst[x * 3 + 1] = dst[x * 3 + 2] = p4[3]; continue; }
                dst[x * 3 + 0] = bgra ? p4[0] : p4[2];
                dst[x * 3 + 1] = p4[1];
                dst[x * 3 + 2] = bgra ? p4[2] : p4[0];
            }
        }
        ctx->Unmap(st, 0);
        BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
        fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = DWORD(fh.bfOffBits + px.size());
        ih.biSize = sizeof(ih); ih.biWidth = LONG(w); ih.biHeight = LONG(h); ih.biPlanes = 1; ih.biBitCount = 24; ih.biCompression = BI_RGB;
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"wb") == 0 && f) { fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f); fwrite(px.data(), 1, px.size(), f); fclose(f); }
        LOGI("native stereo: eye image written to %ls", path);
    }
    st->Release();
}

void NativeAfterAdvance(uintptr_t vi) {
    {
        const float* V = vi ? reinterpret_cast<const float*>(vi) : nullptr;
        bool gameplay = V && !(V[0] == 1.0f && V[5] == 1.0f && V[10] == 1.0f && V[3] == 0.0f && V[7] == 0.0f);
        HudFrameStart(gameplay);
    }
    StereoBegin(vi);
    TemporalBeginFrame(g_st.active);
    if (!g_config.debugStacks || g_watchDone || !vi) return;
    const float* V = reinterpret_cast<const float*>(vi);
    if (V[0] == 1.0f && V[5] == 1.0f && V[10] == 1.0f && V[3] == 0.0f && V[7] == 0.0f) return;
    if (s_gameplayFrames.load() < 500) return;
    WatchArm(vi);
    g_watchPending = true;
}

void NativePatchUpload(void* data, unsigned bytes) {
    if (!g_patching || !data || bytes < 64 || !s_passFullRes) return;
    float* d = static_cast<float*>(data);
    int nf = int(bytes / 4);
    // Clip-from-object matrices (World*ViewProj, ViewProj, previous-frame versions): InvVP * M is affine.
    const float* r3 = &g_origM[5][12];   // InvViewProj row 3
    for (int i = 0; i + 16 <= nf; i += 4) {
        const float* M = d + i;
        float x[4];
        for (int c = 0; c < 4; ++c) x[c] = r3[0] * M[c] + r3[1] * M[4 + c] + r3[2] * M[8 + c] + r3[3] * M[12 + c];
        if (std::fabs(x[0]) < 1e-3f && std::fabs(x[1]) < 1e-3f && std::fabs(x[2]) < 1e-3f && std::fabs(x[3] - 1.0f) < 1e-3f) {
            bool finite = true;
            for (int k = 0; k < 16; ++k) if (!std::isfinite(M[k])) { finite = false; break; }
            if (!finite) continue;
            float out[16];
            vm::Mul4(g_delta, M, out);
            memcpy(d + i, out, 64);
            s_patchHits++;
            i += 12;
        }
    }
    for (int i = 0; i + 16 <= nf; ++i) {
        for (int m = 0; m < 6; ++m) {
            const float* o = g_origM[m];
            // row-major as stored by the game
            if (d[i] == o[0] && d[i + 1] == o[1] && d[i + 2] == o[2] && d[i + 3] == o[3]) {
                bool all = true;
                for (int k = 4; k < 16 && all; ++k) all = Near(d[i + k], o[k]);
                if (all) { memcpy(d + i, g_newM[m], 64); s_patchHits++; i += 15; goto next; }
            }
            // transposed (column-major) copy
            if (d[i] == o[0] && d[i + 1] == o[4] && d[i + 2] == o[8] && d[i + 3] == o[12]) {
                bool all = true;
                for (int k = 0; k < 16 && all; ++k) all = Near(d[i + k], o[(k & 3) * 4 + (k >> 2)]);
                if (all) {
                    for (int k = 0; k < 16; ++k) d[i + k] = g_newM[m][(k & 3) * 4 + (k >> 2)];
                    s_patchHits++; i += 15; goto next;
                }
            }
        }
    next:;
    }
}

void NativeScanUpload(const void* data, unsigned bytes, unsigned bufferBytes, const char* how) {
    if (!(g_scanArmed || g_cmpArmed) || !data || bytes == 0 || bytes > 1u << 20) return;
    if (g_scanArmed && g_inReplay) return;
    auto& ups = (g_cmpArmed && g_inReplay) ? g_ups2 : g_ups;
    auto& upData = (g_cmpArmed && g_inReplay) ? g_upData2 : g_upData;
    if (upData.size() + bytes > (64u << 20)) return;
    Upload u{uint32_t(upData.size()), bytes, bufferBytes, how, {0}};
    void* fr[16]; USHORT n = RtlCaptureStackBackTrace(2, 16, fr, nullptr);
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    int k = 0;
    for (USHORT i = 0; i < n && k < 6; ++i) {
        uintptr_t a = reinterpret_cast<uintptr_t>(fr[i]);
        if (a >= base && a < base + 0x3000000) u.rva[k++] = a - base;
    }
    upData.insert(upData.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
    ups.push_back(u);
}

bool NativeInstall() {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uint8_t* site = reinterpret_cast<uint8_t*>(base + kJobCallSite);
    if (memcmp(site, kJobCallBytes, 6) != 0) {
        LOGE("native stereo: render job call site has unexpected bytes; not patched");
        return false;
    }
    void** slot = reinterpret_cast<void**>(AllocNear(base + kJobCallSite, 4096));
    if (!slot) { LOGE("native stereo: no memory near the game module"); return false; }
    *slot = reinterpret_cast<void*>(&JobThunk);
    DWORD oldProt = 0;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProt)) { LOGE("native stereo: VirtualProtect failed"); return false; }
    int32_t disp = int32_t(reinterpret_cast<intptr_t>(slot) - reinterpret_cast<intptr_t>(site + 6));
    uint8_t patch[6] = {0xFF, 0x15, 0, 0, 0, 0};
    memcpy(patch + 2, &disp, 4);
    memcpy(site, patch, 6);
    VirtualProtect(site, 6, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    g_installed = true;
    LOGI("native stereo: render job call site patched (slot %p)", slot);
    if (!Hook(reinterpret_cast<void*>(base + kFnProcess), (void*)&hk_Process, (void**)&o_Process, "render command processor"))
        return false;
    Hook(reinterpret_cast<void*>(base + kFnViewSetup), (void*)&hk_ViewSetup, (void**)&o_ViewSetup, "render view setup");
    Hook(reinterpret_cast<void*>(base + kFnSetExposure), (void*)&hk_SetExposure, (void**)&o_SetExposure, "set exposure");
    Hook(reinterpret_cast<void*>(base + kFnStatefulPass), (void*)&hk_StatefulPass, (void**)&o_StatefulPass, "stateful post pass");
    g_fakeQueue = static_cast<uint8_t*>(VirtualAlloc(nullptr, kQueueCopySize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return true;
}

namespace { void FindHdrTransform(const std::vector<Upload>& ups, const std::vector<uint8_t>& data, const char* which) {
    int found = 0;
    for (const Upload& u : ups) {
        const float* f = reinterpret_cast<const float*>(data.data() + u.off);
        int nf = int(u.bytes / 4);
        for (int i = 0; i + 4 <= nf && found < 30; i += 4) {   // float4-aligned
            float a = f[i], b = f[i + 1];
            if (a > 0.01f && a < 0.999f && b > 1.001f && std::fabs(a * b - 1.0f) < 1e-3f && f[i + 2] == 0.0f && f[i + 3] == 1.0f) {
                ++found;
                LOGI("hdr transform (%s): (%.5f, %.5f, 0, 1) in %u-byte buffer (%s) at +0x%03X, writer SO6+%llx SO6+%llx SO6+%llx SO6+%llx",
                     which, a, b, u.bufBytes, u.how, i * 4, (unsigned long long)u.rva[0], (unsigned long long)u.rva[1],
                     (unsigned long long)u.rva[2], (unsigned long long)u.rva[3]);
            }
        }
    }
    LOGI("hdr transform (%s): %d places", which, found);
}

void RunCompare() {
    FindHdrTransform(g_ups, g_upData, "pass 1");
    FindHdrTransform(g_ups2, g_upData2, "replay");
    LOGI("upload compare: pass 1 %zu uploads, replay %zu uploads", g_ups.size(), g_ups2.size());
    int reported = 0, skipped1 = 0, skipped2 = 0;
    size_t i1 = 0, i2 = 0;
    while (i1 < g_ups.size() && i2 < g_ups2.size() && reported < 120) {
        if (g_ups[i1].bufBytes != g_ups2[i2].bufBytes || g_ups[i1].bytes != g_ups2[i2].bytes) {
            size_t f = i1;
            while (f < g_ups.size() && f < i1 + 64 && (g_ups[f].bufBytes != g_ups2[i2].bufBytes || g_ups[f].bytes != g_ups2[i2].bytes)) ++f;
            if (f < g_ups.size() && f < i1 + 64) { skipped1 += int(f - i1); i1 = f; }
            else { ++i2; ++skipped2; continue; }
        }
        size_t k = i1;
        const Upload& a = g_ups[i1++]; const Upload& b = g_ups2[i2++];
        const float* fa = reinterpret_cast<const float*>(g_upData.data() + a.off);
        const float* fb = reinterpret_cast<const float*>(g_upData2.data() + b.off);
        for (unsigned i = 0; i < a.bytes / 4 && reported < 80; ++i) {
            if (memcmp(&fa[i], &fb[i], 4) != 0) {
                LOGI("upload compare: #%zu %u-byte buffer (%s) +0x%03X: %.6g vs %.6g  writer SO6+%llx via SO6+%llx",
                     k, a.bufBytes, a.how, i * 4, fa[i], fb[i], (unsigned long long)a.rva[0], (unsigned long long)a.rva[2]);
                ++reported;
            }
        }
    }
    LOGI("upload compare: done (%d uploads only in pass 1, %d only in replay)", skipped1, skipped2);
}

}

void NativeEndOfFrame(uintptr_t vi) {
    if (g_inReplay) return;
    if (g_watchPending) { g_watchPending = false; g_watchDone = true; WatchDisarmAndReport(); }
    struct FrameReset { ~FrameReset() { NewFrameStatefulPasses(); CtxNewFrameUavSnapshots(); } } frameReset;
    const bool stereoFrame = g_st.active;
    struct LatGuard {
        bool st;
        ~LatGuard() {
            if (st) { s_prevHf = g_st.hf; s_prevHfValid = true; } else s_prevHfValid = false;
            LatReset(s_lat[0]); LatReset(s_lat[1]); g_latWantSrv = false;
        }
    } latGuard{stereoFrame};
    int n = g_jobCount;
    g_jobCount = 0;
    s_frames++;
    s_lastJobsPerFrame.store(uint32_t(n), std::memory_order_relaxed);
    uintptr_t q = g_queue.load(std::memory_order_relaxed);
    if (!g_installed || !q) return;
    int curEnd = *reinterpret_cast<volatile int*>(q + kQRead); // index of the end-of-frame command being run
    int prevEnd = g_prevEnd;
    g_prevEnd = curEnd;
    if (g_overflow) { g_overflow = false; return; }
    if (vi) {
        const float* W = reinterpret_cast<const float*>(vi);
        if (!(W[0] == 1.0f && W[5] == 1.0f && W[10] == 1.0f && W[3] == 0.0f && W[7] == 0.0f)) s_gameplayFrames++;
    }
    if (g_config.debugLatTrack) {
        ID3D11DeviceContext* ctx = CtxImmediate();
        if (ctx) {
            ID3D11RenderTargetView* rtv = nullptr; ctx->OMGetRenderTargets(1, &rtv, nullptr);
            if (rtv) { ID3D11Resource* r = nullptr; rtv->GetResource(&r); rtv->Release(); if (r) { SetFinalRT(r); r->Release(); } }
        }
        g_latTrack = true;
        static int logs = 0;
        if (s_gameplayFrames.load() > 300 && logs < 10) {
            ++logs;
            LOGI("lat frame %llu (%s): pass-1 scene %p, composite reads %p", s_gameplayFrames.load(), g_st.active ? "stereo" : "mono",
                 (void*)s_lat[0].xCur, (void*)s_lat[0].xPrev);
        }
    }
    if (g_config.debugStacks && s_gameplayFrames.load() == 300 && vi) {
        const float* f = reinterpret_cast<const float*>(vi);
        LOGI("ViewInfo (render read slot) at gameplay frame 300, 0x530 bytes as floats:");
        for (int i = 0; i < 0x530 / 4; i += 16) {
            char line[512]; int k = snprintf(line, sizeof(line), "  +%03X:", i * 4);
            for (int j = 0; j < 16 && i + j < 0x530 / 4; ++j) k += snprintf(line + k, sizeof(line) - k, " %.5g", f[i + j]);
            LOGI("%s", line);
        }
    }
    if (g_config.debugStacks && vi && !g_cmpDone) {
        if (g_cmpArmed) { RunCompare(); g_cmpArmed = false; g_cmpDone = true; g_ups.clear(); g_ups2.clear(); g_upData.clear(); g_upData2.clear(); }
        else if (s_gameplayFrames.load() == 430) { g_cmpArmed = true; }
    }
    if (g_config.debugStacks && vi && !g_scanDone && !g_cmpArmed) {
        if (g_scanArmed) { RunScan(vi); g_scanArmed = false; g_scanDone = true; g_ups.clear(); g_upData.clear(); g_upData.shrink_to_fit(); }
        else if (s_gameplayFrames.load() == 400) g_scanArmed = true;
    }
    const bool stereo = g_st.active;
    g_st.active = false;
    TemporalEndPass1();
    g_patching = false;                       // pass 1 (left eye) is done
    HudEndPass(CtxImmediate());               // HUD draws of pass 1 are done: final RT bound again
    bool eyeL = stereo && CaptureEye(0);
    // Mono: hand the headset the game's final image at render resolution instead of the (letterboxed,
    // downscaled) back buffer.
    g_monoCaptured = !stereo && g_config.activeMode == Config::Mono &&
                     (g_config.renderResMode != Config::ResGame || HudFrameHasHud()) && CaptureEye(0);
    if (!stereo && g_monoCaptured) { DumpSeqFrame(); HudCompositeToDesktop(CtxImmediate()); }
    g_latTrack = (g_config.activeMode == Config::Native && g_config.stereoHistoryFix) || g_config.debugLatTrack;
    bool latOk = stereo && eyeL && LatValid(s_lat[0]);
    const bool latDelay = latOk && s_lat[0].xCur != s_lat[0].xPrev;   // composite shows the previous frame
    if (stereo && eyeL) {
        if (s_lat[0].found) {
            if (s_lastXCur) { if (s_lat[0].xPrev == s_lastXCur) s_latAltOk++; else s_latAltBad++; }
            s_lastXCur = s_lat[0].xCur;
            static bool logged = false;
            if (!logged && s_lat[0].xCur && s_lat[0].xPrev) {
                logged = true;
                D3D11_TEXTURE2D_DESC a, b; s_lat[0].xCur->GetDesc(&a); s_lat[0].xPrev->GetDesc(&b);
                LOGI("native stereo: composite reads %ux%u fmt %d, scene written to %ux%u fmt %d (%s, %s)",
                     b.Width, b.Height, b.Format, a.Width, a.Height, a.Format, latDelay ? "previous frame" : "same target", latOk ? "per-eye history on" : "not usable");
            }
        }
        if (latOk && EnsureLike(s_saveL, s_lat[0].xCur)) {
            ID3D11DeviceContext* ctx = CtxImmediate();
            ctx->CopyResource(s_saveL, s_lat[0].xCur);                     // this frame's left scene
            if (s_saveRValid) ctx->CopyResource(s_lat[0].xPrev, s_saveR);  // the right eye composites the previous right scene
            s_latApplied++;
        } else { latOk = false; s_latSkipped++; }
    }
    if (g_replayBroken || n == 0 || !vi || prevEnd < 0 || !g_fakeQueue) return;
    if (!(g_config.debugReplay || stereo) || !HeadTrackedMode()) return;
    if (stereo && !eyeL) return;
    const float* V = reinterpret_cast<const float*>(vi);
    if (V[0] == 1.0f && V[5] == 1.0f && V[10] == 1.0f && V[3] == 0.0f && V[7] == 0.0f) return; // title/menu camera
    if (g_config.debugStacks) LogFrameCommandsOnce(q, prevEnd, curEnd);

    LARGE_INTEGER t0, t1, f; QueryPerformanceCounter(&t0);
    float saved[0x180 / 4];
    memcpy(saved, reinterpret_cast<void*>(vi), 0x180);
    if (stereo) {
        memcpy(g_origM, g_st.center, sizeof(g_origM));   // uploads hold the head camera's matrices
        WriteEyeToViewInfo(vi, 1);                         // the replay is the right eye
    } else {
        CaptureMatrices(vi, g_origM);
        if (g_config.debugReplayYawDeg != 0.0f) YawViewInfo(vi, g_config.debugReplayYawDeg);
    }
    CaptureMatrices(vi, g_newM);
    vm::Mul4(g_newM[4], g_origM[5], g_delta);
    bool ok;
    if (stereo) TemporalBeforeReplay(CtxImmediate());   // the right eye gets its own history textures
    g_inReplay = true;
    if (g_config.replayRestoreState) {
        if (g_config.replayRestoreMask & 1) RestoreStatefulPasses();
        if (g_config.replayRestoreMask & 2) RestoreGlobals();
        if (g_config.replayRestoreMask & 4) CtxRestoreUavSnapshots(CtxImmediate());
        if (g_config.replayRestoreMask & 8) TaaDepthBeforeReplay();
    }
    if (g_config.replayReuploadBuffers) CtxReapplyBufferUploads(CtxImmediate());
    bool vsChanged = memcmp(g_origM, g_newM, sizeof(g_origM)) != 0 && g_config.debugReplayViewSetup;
    if (vsChanged) CallViewSetup();   // rebuild the CPU-side view constants for the other eye
    g_patching = memcmp(g_origM, g_newM, sizeof(g_origM)) != 0;
    if (g_config.debugReplay == 2) {
        ok = ReplayJobs(n); // render jobs only (misses render-target/state commands)
    } else {
        // Re-run this frame's whole command range on a private copy of the queue object.
        ok = SafeRead(g_fakeQueue, reinterpret_cast<void*>(q), kQueueCopySize);
        if (ok) {
            *reinterpret_cast<int*>(g_fakeQueue + kQRead) = prevEnd;
            *reinterpret_cast<int*>(g_fakeQueue + kQWrite) = curEnd;
            *reinterpret_cast<uint64_t*>(g_fakeQueue + kQBusy) = 0;
            g_fakeQueue[kQWaitMode] = 0;
            int counter = 0; uint8_t flag = 1;
            if (g_config.replayGpuSync) {
                // Diagnostic: let the GPU finish pass 1 before the replay writes into the game's dynamic buffers.
                static ID3D11Query* q = nullptr;
                ID3D11DeviceContext* ctx = CtxImmediate();
                if (!q && ctx) { ID3D11Device* dev = nullptr; ctx->GetDevice(&dev); D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0}; if (dev) { dev->CreateQuery(&qd, &q); dev->Release(); } }
                if (q && ctx) { ctx->End(q); BOOL done = FALSE; for (int i = 0; i < 200000 && ctx->GetData(q, &done, sizeof(done), 0) != S_OK; ++i) YieldProcessor(); }
            }
            ok = CallProcess(g_fakeQueue, &counter, &flag);
        }
    }
    g_patching = false;
    HudEndPass(CtxImmediate());   // the replay skipped its HUD draws
    if (stereo && ok && latOk && EnsureLike(s_saveR, s_lat[0].xCur)) {
        ID3D11DeviceContext* ctx = CtxImmediate();
        ctx->CopyResource(s_saveR, s_lat[0].xCur);   // this frame's right scene (written by the replay)
        ctx->CopyResource(s_lat[0].xCur, s_saveL);   // next frame's left eye composites the left scene,
        if (s_lat[0].xPrev != s_lat[0].xCur) ctx->CopyResource(s_lat[0].xPrev, s_saveL);  // whichever of the two targets it reads
        s_saveRValid = true;
        static int logs = 0;
        if (g_config.debugLatTrack && logs < 10 && s_gameplayFrames.load() > 300) { ++logs; LOGI("lat replay: scene %p, composite reads %p", (void*)s_lat[1].xCur, (void*)s_lat[1].xPrev); }
    } else if (stereo) s_saveRValid = false;
    if (stereo && ok && CaptureEye(1)) {
        // If the composite shows the scene rendered one frame earlier, submit that frame's head pose.
        const HeadFrame& pubHf = latDelay && s_prevHfValid ? s_prevHf : g_st.hf;
        {
            const float* eyeVI[2] = {g_st.eye[0], g_st.eye[1]};
            ReticleOnStereoFrame(CtxImmediate(), HudTexture(), g_eyeTex, g_st.center, eyeVI);
        }
        XrPublishStereo(g_eyeTex[0], g_eyeTex[1], pubHf, HudTakeFrame());
        s_stereoFrames++;
        DumpSeqFrame();
        static bool dumped = false;
        if (g_config.debugDumpKey) {   // diagnostic: F11 writes both eyes (numbered) plus a camera line to the log
            static bool was = false; static int n = 0;
            bool down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
            if (down && !was) {
                wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
                std::wstring dir = exe; dir = dir.substr(0, dir.find_last_of(L'\\'));
                wchar_t nm[64];
                swprintf(nm, 64, L"\\rofvr_key%02d_left.bmp", n);  DumpEyeBmp(CtxImmediate(), g_eyeTex[0], (dir + nm).c_str());
                swprintf(nm, 64, L"\\rofvr_key%02d_right.bmp", n); DumpEyeBmp(CtxImmediate(), g_eyeTex[1], (dir + nm).c_str());
                if (ID3D11Texture2D* h = HudTexture()) { swprintf(nm, 64, L"\\rofvr_key%02d_hud.bmp", n); DumpEyeBmp(CtxImmediate(), h, (dir + nm).c_str()); }
                const float* c = g_st.center;
                LOGI("F11 dump %d: centre P m00 %.4f m11 %.4f m02 %.4f m12 %.4f | eye L P m00 %.4f m02 %.4f | eye R P m00 %.4f m02 %.4f | eyeOff L %.2f R %.2f",
                     n, c[32], c[37], c[34], c[38], g_st.eye[0][32], g_st.eye[0][34], g_st.eye[1][32], g_st.eye[1][34],
                     g_st.hf.eyeOff[0][0], g_st.hf.eyeOff[1][0]);
                ++n;
            }
            was = down;
        }
        if (!dumped && g_config.debugDumpEyesAt > 0 && s_gameplayFrames.load() >= (unsigned long long)g_config.debugDumpEyesAt) {
            dumped = true;
            wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring dir = exe; dir = dir.substr(0, dir.find_last_of(L'\\'));
            DumpEyeBmp(CtxImmediate(), g_eyeTex[0], (dir + L"\\rofvr_eye_left.bmp").c_str());
            DumpEyeBmp(CtxImmediate(), g_eyeTex[1], (dir + L"\\rofvr_eye_right.bmp").c_str());
            if (ID3D11Texture2D* h = HudTexture()) {
                DumpEyeBmp(CtxImmediate(), h, (dir + L"\\rofvr_hud_rgb.bmp").c_str());
                DumpEyeBmp(CtxImmediate(), h, (dir + L"\\rofvr_hud_alpha.bmp").c_str(), true);
            }
        }
        if (g_config.debugDesktopEye == 0) {
            // Show the left eye on the monitor instead of the right (diagnostic).
            ID3D11DeviceContext* ctx = CtxImmediate();
            ID3D11RenderTargetView* rtv = nullptr;
            if (ctx) ctx->OMGetRenderTargets(1, &rtv, nullptr);
            if (rtv) {
                ID3D11Resource* res = nullptr; rtv->GetResource(&res); rtv->Release();
                if (res) { ctx->CopySubresourceRegion(res, 0, 0, 0, 0, g_eyeTex[0], 0, nullptr); res->Release(); }
            }
        }
        HudCompositeToDesktop(CtxImmediate());
    }
    // After the right eye has been captured: keep its history, restore the left eye's for the next frame.
    TemporalAfterReplay(CtxImmediate(), ok);
    TaaDepthAfterReplay();
    memcpy(reinterpret_cast<void*>(vi), saved, 0x180);
    if (stereo) {   // leave the head camera (not the left eye) in the ViewInfo for anything that reads it later
        memcpy(reinterpret_cast<void*>(vi), g_st.center, kVIMats);
        memcpy(reinterpret_cast<void*>(vi + kVIPos), g_st.centerPos, 12);
    }
    if (vsChanged) CallViewSetup();   // and back to this frame's own view
    g_inReplay = false;
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f);
    if (!ok) {
        g_replayBroken = true;
        LOGE("native stereo: exception while replaying the frame; replay disabled");
        return;
    }
    s_replays++; s_replayedJobs += uint64_t(n);
    g_replayMsSum += double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart); g_replayMsN++;
}

// Diagnostic: a few consecutive frames of the HUD (or the eye image when there is no HUD layer).
void DumpSeqFrame() {
    static int n = 0;
    if (g_config.debugDumpEyesAt <= 0 || n >= 12 || s_gameplayFrames.load() < (unsigned long long)g_config.debugDumpEyesAt + 30) return;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe; dir = dir.substr(0, dir.find_last_of(L'\\'));
    wchar_t name[64];
    ID3D11Texture2D* t = HudTexture();
    swprintf(name, 64, t ? L"\\rofvr_hudseq_%d.bmp" : L"\\rofvr_eyeseq_%d.bmp", n);
    if (!t) t = g_eyeTex[0];
    if (t) DumpEyeBmp(CtxImmediate(), t, (dir + name).c_str());
    if (t && n == 0 && HudTexture()) DumpEyeBmp(CtxImmediate(), t, (dir + L"\\rofvr_hudseq_alpha.bmp").c_str(), true);
    ++n;
}

unsigned long long NativeGameplayFrames() { return s_gameplayFrames.load(); }

ID3D11Texture2D* NativeTakeMonoFrame() {
    if (!g_monoCaptured) return nullptr;
    g_monoCaptured = false;
    return g_eyeTex[0];
}

void NativeReport(char* buf, size_t size) {
    double ms = g_replayMsN ? g_replayMsSum / double(g_replayMsN) : 0.0;
    int k0 = snprintf(buf, size, "stereo frames %llu (no head record %llu, eye capture failed %llu; history swap %llu, skipped %llu, alternation ok %llu bad %llu, rt binds %llu final %llu) | ",
                      (unsigned long long)s_stereoFrames.exchange(0), (unsigned long long)s_stereoNoFrame.exchange(0),
                      (unsigned long long)s_eyeCaptureFail.exchange(0), (unsigned long long)s_latApplied.exchange(0),
                      (unsigned long long)s_latSkipped.exchange(0), (unsigned long long)s_latAltOk.exchange(0),
                      (unsigned long long)s_latAltBad.exchange(0), (unsigned long long)s_rtBinds.exchange(0), (unsigned long long)s_finalBinds.exchange(0));
    if (k0 < 0 || size_t(k0) >= size) return;
    buf += k0; size -= size_t(k0);
    snprintf(buf, size, "render jobs: %u last frame, %llu replays (cpu %.2f ms each), %llu camera matrices replaced, view setup %llu (+%llu during replay)%s",
             s_lastJobsPerFrame.load(), (unsigned long long)s_replays.exchange(0), ms, (unsigned long long)s_patchHits.exchange(0),
             (unsigned long long)s_vsCalls.exchange(0), (unsigned long long)s_vsReplayCalls.exchange(0),
             g_replayBroken ? " [replay disabled after exception]" : "");
    g_replayMsSum = 0; g_replayMsN = 0;
}

} // namespace so4vr
