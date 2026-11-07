// Enhanced Remaster: runtime exploration (diagnostics only, DebugEeProbe = 1).
//  - hooks the EE's UpdateCamera (RVA 0x94B50) and keeps a table of updated cameras
//  - hooks the render command processor (RVA 0x106940) to count frames / jobs and learn the render thread
//  - on request (file rof_scan_now.txt in the game folder) scans all readable committed memory for copies of
//    the main camera's view / view-projection matrices (exact bit patterns, plain and transposed) and logs them
//  - optionally (DebugEeWatch = N) puts a hardware write breakpoint on the N-th hit of the scan (all threads)
//    and logs the code addresses that write it (with return addresses found on the stack)
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>
#include "MinHook.h"
#include "hook_util.h"
#include "log.h"
#include "config.h"
#include "ee_render.h"

namespace so4vr {
extern uintptr_t g_bigMap; extern uint64_t g_bigMapSize;
namespace {
constexpr uintptr_t kEeUpdateCamera = 0x94B50, kEeProcess = 0x106940, kEeCbUpload = 0xCC060;
constexpr uintptr_t kCamView = 0x120, kCamProj = 0x420, kCamVP = 0x4E0, kCamFlags = 0x7BA;
uintptr_t g_base = 0, g_end = 0;
std::wstring g_dir;

using FnCam = void(__fastcall*)(void*);
using FnProc = uint64_t(__fastcall*)(void*, int*, uint8_t*);
FnCam o_Cam = nullptr; FnProc o_Proc = nullptr;

struct CamRec { uintptr_t cam; uint64_t n; uint64_t nRecent; ULONGLONG last; float view[16]; float vp[16]; float proj[16]; };
std::mutex g_cm; CamRec g_cams[32]; int g_ncams = 0;
std::atomic<uint64_t> s_camCalls{0}, s_procCalls{0};
std::atomic<DWORD> g_renderTid{0}, g_camTid{0};
std::atomic<uint64_t> s_ups8b0{0};

void __fastcall hk_Cam(void* cam) {
    o_Cam(cam);
    s_camCalls++; g_camTid = GetCurrentThreadId();
    uintptr_t c = reinterpret_cast<uintptr_t>(cam);
    std::lock_guard<std::mutex> lk(g_cm);
    int i = 0; for (; i < g_ncams; ++i) if (g_cams[i].cam == c) break;
    if (i == g_ncams) { if (g_ncams == 32) return; g_cams[g_ncams++] = {c, 0, 0, 0, {}, {}, {}}; }
    g_cams[i].n++; g_cams[i].nRecent++; g_cams[i].last = GetTickCount64();
    memcpy(g_cams[i].view, (void*)(c + kCamView), 64);
    memcpy(g_cams[i].vp, (void*)(c + kCamVP), 64);
    memcpy(g_cams[i].proj, (void*)(c + kCamProj), 64);
}
uint64_t __fastcall hk_Proc(void* q, int* counter, uint8_t* flag) {
    s_procCalls++; g_renderTid = GetCurrentThreadId();
    return o_Proc(q, counter, flag);
}

// ---- constant buffer upload (RVA 0xCC060: rcx renderer, rdx buffer object, r8 desc {src, ?, offset +0xC, size +0x10}) ----
struct CbDesc { const uint8_t* src; uint32_t a, offset, size; };
using FnCb = void(__fastcall*)(void*, void*, CbDesc*, uint8_t);
FnCb o_Cb = nullptr;
struct CbStat { uint32_t size, offset; uint64_t n; int viewAt; uint64_t nView; };
std::mutex g_cbm; CbStat g_cbs[64]; int g_ncbs = 0; std::atomic<uint64_t> s_cbCalls{0};
float g_mainView[4] = {};   // row 0 of the main camera's view (set by the worker)
void __fastcall hk_Cb(void* r, void* buf, CbDesc* d, uint8_t f) {
    s_cbCalls++;
    if (d && d->size == 0x8B0 && GetCurrentThreadId() == g_renderTid.load(std::memory_order_relaxed)) s_ups8b0++;
    if (d && d->src) {
        int at = -1;
        if (g_mainView[0] != 0.f) {
            __try {
                for (uint32_t o = 0; o + 16 <= d->size && o < 0x2000; o += 16)
                    if (memcmp(d->src + d->offset + o, g_mainView, 12) == 0) { at = int(o); break; }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        std::lock_guard<std::mutex> lk(g_cbm);
        int i = 0; for (; i < g_ncbs; ++i) if (g_cbs[i].size == d->size && g_cbs[i].offset == d->offset) break;
        if (i == g_ncbs && g_ncbs < 64) g_cbs[g_ncbs++] = {d->size, d->offset, 0, -1, 0};
        if (i < g_ncbs) { g_cbs[i].n++; if (at >= 0) { g_cbs[i].viewAt = at; g_cbs[i].nView++; } }
    }
    o_Cb(r, buf, d, f);
}
void CbReport() {
    std::lock_guard<std::mutex> lk(g_cbm);
    LOGI("probe cb: %llu uploads, %d kinds (size/offset):", (unsigned long long)s_cbCalls.exchange(0), g_ncbs);
    for (int i = 0; i < g_ncbs; ++i) {
        LOGI("probe cb:  size 0x%X offset 0x%X count %llu%s", g_cbs[i].size, g_cbs[i].offset, (unsigned long long)g_cbs[i].n,
             g_cbs[i].nView ? "  <- carries the camera view" : "");
        if (g_cbs[i].nView) LOGI("probe cb:      view row 0 at +0x%X in %llu uploads", g_cbs[i].viewAt, (unsigned long long)g_cbs[i].nView);
        g_cbs[i].n = 0; g_cbs[i].nView = 0;
    }
}

// ---- job trace: the type-0 job call site in the processor (call [rax+0x240] at 0x106A8E) goes through JobThunk;
// on request (rof_trace_now.txt) one frame's jobs are logged with their uploads, draws and render targets ----
constexpr uintptr_t kJobSite = 0x106A8E, kSubmit = 0xD09E0;   // frame submit: closes the open list, ExecuteCommandLists (from 0xA1970, command types 10 / 19)
constexpr uint8_t kJobBytes[6] = {0xFF, 0x90, 0x40, 0x02, 0x00, 0x00};
using FnJob = void(__fastcall*)(void*, void*, uint32_t);
using FnSubmit = void(__fastcall*)(void*);
FnSubmit o_Submit = nullptr;
struct JobRec { uintptr_t vt, fn; uint32_t flags, ups, draws, sets; ID3D12Resource* rt; DXGI_FORMAT fmt; };
constexpr int kMaxTrace = 4096;
JobRec g_trace[kMaxTrace]; int g_traceN = 0;
std::atomic<int> g_traceState{0};   // 0 idle, 1 armed (start at the next frame), 2 recording
std::atomic<uint64_t> s_jobCalls{0}, s_submits{0};
uint64_t g_frameJobs = 0, g_maxFrameJobs = 0;
void __fastcall JobThunk(void* obj, void* arg, uint32_t flags) {
    s_jobCalls++; g_frameJobs++;
    if (!g_renderTid.load(std::memory_order_relaxed)) g_renderTid = GetCurrentThreadId();
    static bool hooked = false;
    if (!hooked) { hooked = true; ID3D12GraphicsCommandList* l = EeEngineList(); LOGI("probe: engine command list %p (vtable %p)", (void*)l, l ? *(void**)l : nullptr); if (l) EeHookListMethods(l, GetCurrentThreadId()); }
    void* fn = (*reinterpret_cast<void***>(obj))[0x240 / 8];
    const bool rec = g_traceState.load(std::memory_order_relaxed) == 2 && g_traceN < kMaxTrace;
    EeRtState& rt = EeRt();
    const uint64_t u0 = s_ups8b0.load(std::memory_order_relaxed), d0 = rt.draws, st0 = rt.sets;
    reinterpret_cast<FnJob>(fn)(obj, arg, flags);
    if (rec) {
        JobRec& j = g_trace[g_traceN++];
        j.vt = *reinterpret_cast<uintptr_t*>(obj) - g_base; j.fn = reinterpret_cast<uintptr_t>(fn) - g_base; j.flags = flags;
        j.ups = uint32_t(s_ups8b0.load() - u0); j.draws = uint32_t(rt.draws - d0); j.sets = uint32_t(rt.sets - st0);
        j.rt = rt.rt[0]; j.fmt = rt.fmt[0];
    }
}
void __fastcall hk_Submit(void* q) {
    s_submits++;
    if (g_frameJobs > g_maxFrameJobs) g_maxFrameJobs = g_frameJobs;
    g_frameJobs = 0;
    int st = g_traceState.load();
    if (st == 2) {
        LOGI("probe trace: %d jobs in the frame", g_traceN);
        ID3D12Resource* last = nullptr;
        for (int i = 0; i < g_traceN; ++i) {
            const JobRec& j = g_trace[i];
            char rtd[96] = "-";
            if (j.rt) { D3D12_RESOURCE_DESC d = j.rt->GetDesc(); snprintf(rtd, sizeof rtd, "%p %llux%u fmt %d", (void*)j.rt, (unsigned long long)d.Width, d.Height, int(j.fmt)); }
            LOGI("probe trace: job %4d vt+%llX fn+%llX flags %u ups %u draws %u rtsets %u rt %s%s", i, (unsigned long long)j.vt, (unsigned long long)j.fn, j.flags,
                 j.ups, j.draws, j.sets, rtd, j.rt != last ? " *" : "");
            last = j.rt;
        }
        g_traceN = 0; g_traceState = 0;
    } else if (st == 1) { g_traceN = 0; g_traceState = 2; }
    o_Submit(q);
}
void* AllocNear(uintptr_t target, size_t size) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    for (uintptr_t d = gran; d < 0x70000000ull; d += gran)
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            uintptr_t a = (sgn < 0 ? target - d : target + d) & ~(gran - 1);
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return p;
        }
    return nullptr;
}
bool PatchJobSite() {
    uint8_t* site = reinterpret_cast<uint8_t*>(g_base + kJobSite);
    if (memcmp(site, kJobBytes, 6) != 0) { LOGE("probe: job call site bytes differ"); return false; }
    void** slot = static_cast<void**>(AllocNear(g_base + kJobSite, 4096));
    if (!slot) return false;
    *slot = reinterpret_cast<void*>(&JobThunk);
    int64_t disp = reinterpret_cast<intptr_t>(slot) - reinterpret_cast<intptr_t>(site + 6);
    if (disp > INT32_MAX || disp < INT32_MIN) return false;
    uint8_t patch[6] = {0xFF, 0x15, 0, 0, 0, 0}; int32_t d32 = int32_t(disp); memcpy(patch + 2, &d32, 4);
    DWORD old; if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(site, patch, 6); VirtualProtect(site, 6, old, &old); FlushInstructionCache(GetCurrentProcess(), site, 6);
    return true;
}

bool Readable(DWORD p) {
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

struct Hit { uintptr_t addr; const char* what; MEMORY_BASIC_INFORMATION m; };
std::vector<Hit> g_hits;
uintptr_t g_srcAddr = 0;

void Scan() {
    CamRec main{}; uint64_t best = 0;
    {
        std::lock_guard<std::mutex> lk(g_cm);
        for (int i = 0; i < g_ncams; ++i) {
            const CamRec& r = g_cams[i];
            bool persp = r.proj[11] != 0.f || r.proj[14] != 0.f;
            LOGI("probe: camera %p updates %llu view row0 (%.3f %.3f %.3f %.1f) t (%.1f %.1f %.1f) proj m00 %.3f m11 %.3f %s", (void*)r.cam,
                 (unsigned long long)r.n, r.view[0], r.view[1], r.view[2], r.view[3], r.view[12], r.view[13], r.view[14], r.proj[0], r.proj[5], persp ? "persp" : "ortho");
            // the field camera: ~72 deg lens (m00 0.8 .. 3), placed away from the origin; most updated of those
            bool field = persp && r.proj[0] > 0.8f && r.proj[0] < 3.0f && (std::fabs(r.view[3]) + std::fabs(r.view[7]) + std::fabs(r.view[11]) > 1.0f);
            const bool recent = GetTickCount64() - r.last < 500;
            if (field && recent && r.nRecent > best) { best = r.nRecent; main = r; }
        }
    }
    if (!main.cam) { LOGI("probe: no perspective camera yet"); return; }
    // patterns: view (16 floats), view transposed (first 3 columns as rows = 12 floats), VP, VP transposed
    float vt[16], vpt[16];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { vt[r * 4 + c] = main.view[c * 4 + r]; vpt[r * 4 + c] = main.vp[c * 4 + r]; }
    // patterns: whole rows (48 bytes) of view / VP (plain and transposed), and rotation-only rows (3 floats at
    // a 16-byte stride: camera-relative rendering keeps the rotation but not the translation)
    struct Pat { const float* p; int chunks; int stride; int floats; const char* name; } pats[] = {
        {main.view, 1, 0, 12, "view"}, {vt, 1, 0, 12, "view^T"}, {main.vp, 1, 0, 12, "VP"}, {vpt, 1, 0, 12, "VP^T"},
        {main.view, 3, 4, 3, "viewrot"}, {vt, 3, 4, 3, "viewrot^T"}};
    g_hits.clear();
    MEMORY_BASIC_INFORMATION m;
    size_t scanned = 0, faults = 0, wc = 0, mapped = 0;
    for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull && VirtualQuery((void*)a, &m, sizeof m); a = (uintptr_t)m.BaseAddress + m.RegionSize) {
        if (m.State != MEM_COMMIT || !Readable(m.Protect) || m.RegionSize < 64) continue;
        if (m.Type == MEM_IMAGE) continue;
        const uint8_t* rb = (const uint8_t*)m.BaseAddress; size_t n = m.RegionSize - 48;
        scanned += m.RegionSize;
        if (m.Protect & PAGE_WRITECOMBINE) wc += m.RegionSize;
        if (m.Type == MEM_MAPPED) mapped += m.RegionSize;
        __try {
            for (size_t o = 0; o < n; o += 4) {
                uint32_t w = *(const uint32_t*)(rb + o);
                if (w == 0) continue;
                for (auto& p : pats) {
                    if (w != *(const uint32_t*)p.p) continue;
                    bool ok = true;
                    for (int c = 0; c < p.chunks && ok; ++c) {
                        size_t at = o + size_t(c) * 16;
                        if (at + p.floats * 4 > m.RegionSize) { ok = false; break; }
                        ok = memcmp(rb + at, p.p + c * p.stride, size_t(p.floats) * 4) == 0;
                    }
                    if (!ok) continue;
                    uintptr_t at = (uintptr_t)(rb + o);
                    if (at >= main.cam && at < main.cam + 0x900) continue;   // the camera itself
                    if (g_hits.size() < 400) g_hits.push_back({at, p.name, m});
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { faults++; }
    }
    LOGI("probe: scanned %llu MB (write-combined %llu MB, mapped %llu MB), %llu regions faulted",
         (unsigned long long)(scanned >> 20), (unsigned long long)(wc >> 20), (unsigned long long)(mapped >> 20), (unsigned long long)faults);
    {   // raw bytes around the first 16 heap copies (offline analysis): record = u64 address, u64 hit, 0x800 bytes from hit-0x400
        FILE* f = _wfopen((g_dir + L"\\rof_scanctx.bin").c_str(), L"ab");
        int k = 0;
        for (auto& h : g_hits) {
            if (h.m.RegionSize <= 0x1000000 || k >= 16) continue;
            uintptr_t from = h.addr - 0x400;
            if (from < (uintptr_t)h.m.BaseAddress || h.addr + 0x400 > (uintptr_t)h.m.BaseAddress + h.m.RegionSize) continue;
            uint64_t hdr[2] = {from, h.addr};
            if (f) { fwrite(hdr, 8, 2, f); __try { fwrite((void*)from, 1, 0x800, f); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
            ++k;
        }
        if (f) fclose(f);
        // and the camera itself
        f = _wfopen((g_dir + L"\\rof_scancam.bin").c_str(), L"ab");
        if (f) { uint64_t hdr[2] = {main.cam, 0}; fwrite(hdr, 8, 2, f); __try { fwrite((void*)main.cam, 1, 0x900, f); } __except (EXCEPTION_EXECUTE_HANDLER) {} fclose(f); }
    }
    if (g_bigMap) {   // the engine's upload ring: rows of the camera view / VP / position (4 floats), any layout
        const float pos[3] = {main.view[0] * 0.f, 0.f, 0.f}; (void)pos;
        struct R { const float* p; int n; const char* name; } rows[] = {
            {main.view, 4, "view r0"}, {main.view + 4, 4, "view r1"}, {main.vp, 4, "VP r0"}, {main.vp + 4, 4, "VP r1"},
            {vt, 4, "view c0"}, {vpt, 4, "VP c0"}, {vt + 12, 3, "view c3"}};
        int found = 0; size_t firstOff = ~size_t(0);
        const uint8_t* rb = (const uint8_t*)g_bigMap;
        __try {
            for (size_t o = 0; o + 64 < g_bigMapSize && found < 60; o += 4) {
                uint32_t w = *(const uint32_t*)(rb + o);
                if (!w) continue;
                for (auto& r : rows) if (w == *(const uint32_t*)r.p && memcmp(rb + o, r.p, r.n * 4) == 0) {
                    if (found < 60) LOGI("probe ring: %s at +%llx", r.name, (unsigned long long)o);
                    if (o < firstOff) firstOff = o; ++found;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { LOGI("probe ring: fault while scanning"); }
        LOGI("probe ring: %d row hits in the %llu MB upload buffer", found, (unsigned long long)(g_bigMapSize >> 20));
        // the CPU-side source search was unreliable (matched KUSER_SHARED_DATA everywhere and flooded the log);
        // instead DebugEeWatch = -2 puts a write watch on the ring slot itself, catching the uploader when it reuses it
        if (firstOff != ~size_t(0)) g_srcAddr = g_bigMap + firstOff + 0x10;
        if (firstOff != ~size_t(0)) {
            FILE* f = _wfopen((g_dir + L"\\rof_ring.bin").c_str(), L"wb");
            size_t from = firstOff > 0x8000 ? firstOff - 0x8000 : 0;
            if (f) { uint64_t hdr[2] = {from, firstOff}; fwrite(hdr, 8, 2, f); __try { fwrite(rb + from, 1, 0x10000, f); } __except (EXCEPTION_EXECUTE_HANDLER) {} fclose(f); }
        }
    }
    LOGI("probe: main camera %p (updates %llu); scanned %llu MB, %zu copies:", (void*)main.cam, (unsigned long long)best, (unsigned long long)(scanned >> 20), g_hits.size());
    for (size_t i = 0; i < g_hits.size() && i < 120; ++i) {
        const Hit& h = g_hits[i];
        LOGI("probe:  #%zu %-6s at %p (region %p size %llx type %s protect %lx, offset %llx)", i, h.what, (void*)h.addr, h.m.BaseAddress,
             (unsigned long long)h.m.RegionSize, h.m.Type == MEM_PRIVATE ? "private" : h.m.Type == MEM_MAPPED ? "mapped" : "?", h.m.Protect,
             (unsigned long long)(h.addr - (uintptr_t)h.m.BaseAddress));
    }
}

// ---- hardware write watch ----
std::atomic<uintptr_t> g_watch{0}; std::atomic<int> g_watchHits{0};
LONG CALLBACK Veh(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_watch.load()) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = e->ContextRecord;
    if (!(c->Dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    int n = ++g_watchHits;
    if (n <= 60) {
        char st[512]; int len = 0;
        const uintptr_t* sp = (const uintptr_t*)c->Rsp;
        for (int k = 0, f = 0; k < 64 && f < 6; ++k) {
            uintptr_t v = 0; __try { v = sp[k]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            if (v >= g_base + 0x1000 && v < g_end) { len += snprintf(st + len, sizeof st - len, " +%llX", (unsigned long long)(v - g_base)); ++f; }
        }
        uintptr_t rip = c->Rip;
        LOGI("probe watch hit %d: thread %lu%s rip %s+%llX | stack:%s", n, GetCurrentThreadId(), GetCurrentThreadId() == g_renderTid ? " (render)" : GetCurrentThreadId() == g_camTid ? " (camera)" : "",
             rip >= g_base && rip < g_end ? "exe" : "other", (unsigned long long)(rip >= g_base && rip < g_end ? rip - g_base : rip), st);
    }
    if (n >= 200) c->Dr7 &= ~0xFFull;   // enough: disable on this thread
    return EXCEPTION_CONTINUE_EXECUTION;
}
void SetWatch(uintptr_t addr, bool readToo = false) {
    static PVOID veh = AddVectoredExceptionHandler(1, Veh);
    (void)veh;
    g_watch = addr; g_watchHits = 0;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof te}; int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                c.Dr0 = addr; c.Dr7 = (c.Dr7 & ~0xF000Full) | 1 | ((readToo ? 3ull : 1ull) << 16) | (3ull << 18);   // L0, write (or read/write), 4 bytes
                if (SetThreadContext(t, &c)) ++n;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    LOGI("probe: write watch on %p set on %d threads", (void*)addr, n);
}

DWORD WINAPI Worker(LPVOID) {
    std::wstring req = g_dir + L"\\rof_scan_now.txt";
    ULONGLONG last = GetTickCount64();
    for (;;) {
        Sleep(250);
        if (GetTickCount64() - last > 10000) {
            last = GetTickCount64();
            {
                std::lock_guard<std::mutex> lk(g_cm);
                uint64_t best = 0;
                for (int i = 0; i < g_ncams; ++i) {
                    const CamRec& c = g_cams[i];
                    bool field = (c.proj[11] != 0.f || c.proj[14] != 0.f) && c.proj[0] > 0.8f && c.proj[0] < 3.0f && GetTickCount64() - c.last < 500;
                    if (field && c.nRecent > best) { best = c.nRecent; memcpy(g_mainView, (void*)(c.cam + kCamView), 16); }
                    g_cams[i].nRecent = 0;
                }
            }
            CbReport();
            LOGI("probe: jobs %llu, submits %llu, most jobs in a frame %llu", (unsigned long long)s_jobCalls.exchange(0), (unsigned long long)s_submits.exchange(0), (unsigned long long)g_maxFrameJobs);
            g_maxFrameJobs = 0;
            LOGI("probe: camera updates %llu (thread %lu), render processor calls %llu (thread %lu), cameras %d",
                 (unsigned long long)s_camCalls.exchange(0), g_camTid.load(), (unsigned long long)s_procCalls.exchange(0), g_renderTid.load(), g_ncams);
        }
        {
            std::wstring tr = g_dir + L"\\rof_trace_now.txt";
            if (GetFileAttributesW(tr.c_str()) != INVALID_FILE_ATTRIBUTES) { DeleteFileW(tr.c_str()); g_traceState = 1; LOGI("probe: frame trace armed"); }
        }
        if (GetFileAttributesW(req.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(req.c_str());
            Scan();
            int w = g_config.debugEeWatch;
            if (w == -2 && g_srcAddr) SetWatch(g_srcAddr);   // who writes the per-view constant slot in the upload ring
            if (w > 0) {
                // the w-th hit that is in private memory (not the upload heaps)
                // the w-th hit in a large heap region (not a thread stack)
                int k = 0;
                for (auto& h : g_hits) if (h.m.Type == MEM_PRIVATE && h.m.RegionSize > 0x1000000 && ++k == w) { SetWatch(h.addr); break; }
            }
            FILE* f = _wfopen((g_dir + L"\\rof_scan_done.txt").c_str(), L"wb"); if (f) { fputs("1", f); fclose(f); }
        }
    }
}
} // namespace

bool EeProbeInstall(const std::wstring& dir) {
    g_dir = dir;
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + reinterpret_cast<IMAGE_DOS_HEADER*>(g_base)->e_lfanew);
    g_end = g_base + nt->OptionalHeader.SizeOfImage;
    bool ok = Hook(reinterpret_cast<void*>(g_base + kEeUpdateCamera), (void*)hk_Cam, (void**)&o_Cam, "EE UpdateCamera (probe)");
    ok = Hook(reinterpret_cast<void*>(g_base + kEeProcess), (void*)hk_Proc, (void**)&o_Proc, "EE render command processor (probe)") && ok;
    ok = Hook(reinterpret_cast<void*>(g_base + kSubmit), (void*)hk_Submit, (void**)&o_Submit, "EE frame submit (probe)") && ok;
    ok = PatchJobSite() && ok;
    ok = Hook(reinterpret_cast<void*>(g_base + kEeCbUpload), (void*)hk_Cb, (void**)&o_Cb, "EE constant upload (probe)") && ok;
    CloseHandle(CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr));
    return ok;
}
} // namespace so4vr
