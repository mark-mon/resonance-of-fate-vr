// SO4 camera discovery (stage 3 groundwork).
// UpdateCamera (RVA 0x2746F0, void(AskaCamera*)) is the SO4 twin of Divine Force's 0x18130C: it rebuilds
// view (+0x120 = inverse(world +0x30)), position (+0x560), projection (+0x420, unjittered +0x5D0),
// view-projection (+0x4E0) and the unjittered VP (+0x4A0). Layout from camera scans (test 007).
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "MinHook.h"
#include "hook_util.h"
#include "log.h"
#include "so4_stereo.h"
#include "safe_mem.h"
#include "so4_camera.h"
#include "render_res.h"
#include "so4_headtrack.h"
#include "config.h"

namespace so4vr {
namespace {

constexpr uintptr_t kFnUpdateCamera = 0x2746F0;
using UpdateCameraFn = void(__fastcall*)(void* cam);
UpdateCameraFn o_UpdateCamera = nullptr;

struct CamStat { uintptr_t cam; uint32_t calls; uint32_t callsTotal; uintptr_t vt; DWORD thread; };
constexpr int kMax = 32;
CamStat g_cams[kMax];
int g_nCams = 0;
std::mutex g_mx;
std::atomic<uint64_t> g_calls{0};
std::atomic<uintptr_t> g_main{0};
uintptr_t g_base = 0;

// Main camera matrix history (game thread writes, render thread reads under the mutex).
struct CamRec { uint64_t seq; LARGE_INTEGER t; uint64_t present; float m[7][16]; };
std::atomic<uint64_t> g_presents{0};   // world, view, proj, projNoJit, VP, VPnoJit, (pos,..)
constexpr int kRec = 16;
CamRec g_rec[kRec]; uint64_t g_recSeq = 0; std::mutex g_recMx;

// View inverse (rigid / scaled), called by UpdateCamera as inv(world @ +0x30) -> view @ +0x120.
constexpr uintptr_t kFnInvRigid = 0x1B9AF0, kFnInvScaled = 0xD50F0;
using InvFn = void(__fastcall*)(const float* src, float* dst);
InvFn o_InvRigid = nullptr, o_InvScaled = nullptr;
void __fastcall hk_InvRigid(const float* src, float* dst) { float tmp[16]; o_InvRigid(So4HtWorldForView(src, tmp), dst); }
void __fastcall hk_InvScaled(const float* src, float* dst) { float tmp[16]; o_InvScaled(So4HtWorldForView(src, tmp), dst); }

// Main camera detection (game thread): the perspective camera with a real (non-identity) world matrix that
// is updated most often, re-chosen every 0.5 s.
struct Cand { uintptr_t cam; uint32_t n; };
Cand g_cand[16]; int g_nCand = 0; LARGE_INTEGER g_candT0{}, g_qpf{};
void NoteCandidate(uintptr_t c) {
    if (*reinterpret_cast<uint8_t*>(c + 0x7AA) & 1) return;                       // orthographic
    const float* w = reinterpret_cast<const float*>(c + 0x30);
    if (w[0] == 1.f && w[5] == 1.f && w[10] == 1.f && w[3] == 0.f && w[7] == 0.f) return;   // default camera at origin
    int i = 0;
    for (; i < g_nCand; ++i) if (g_cand[i].cam == c) { g_cand[i].n++; break; }
    if (i == g_nCand && g_nCand < 16) g_cand[g_nCand++] = {c, 1};
}
void PickMain() {
    // The camera seen in the render thread's per-view upload wins immediately (it is what is drawn).
    uint64_t rt = 0; uintptr_t rc = So4HtRenderedCamera(&rt);
    if (rc && rc != g_main.load() && GetTickCount64() - rt < 500) {
        for (int i = 0; i < g_nCand; ++i) if (g_cand[i].cam == rc) { LOGI("main camera -> %p (seen in the render upload)", (void*)rc); g_main = rc; return; }
    }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!g_qpf.QuadPart) { QueryPerformanceFrequency(&g_qpf); g_candT0 = now; }
    if (now.QuadPart - g_candT0.QuadPart < g_qpf.QuadPart / 2) return;
    uintptr_t best = 0; uint32_t bn = 0;
    // prefer the rendered camera, otherwise the most updated one
    for (int i = 0; i < g_nCand; ++i) if (g_cand[i].n > bn || (g_cand[i].cam == rc && g_cand[i].n == bn)) { bn = g_cand[i].n; best = g_cand[i].cam; }
    if (rc && GetTickCount64() - rt < 1000) for (int i = 0; i < g_nCand; ++i) if (g_cand[i].cam == rc) { best = rc; bn = g_cand[i].n; }
    if (best != g_main.load()) {
        uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        LOGI("main camera -> %p (%u updates in 0.5 s) vt+%llX pos (%.0f %.0f %.0f) cands %d", (void*)best, bn,
             best ? (unsigned long long)(*reinterpret_cast<uintptr_t*>(best) - b) : 0ull,
             best ? *reinterpret_cast<float*>(best + 0x560) : 0.f, best ? *reinterpret_cast<float*>(best + 0x564) : 0.f,
             best ? *reinterpret_cast<float*>(best + 0x568) : 0.f, g_nCand);
        if (best) { const float* w = reinterpret_cast<const float*>(best + 0x30);
            LOGI("  world rows (%.4f %.4f %.4f %.1f) (%.4f %.4f %.4f %.1f) (%.4f %.4f %.4f %.1f)", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11]); }
    }
    g_main = best; g_nCand = 0; g_candT0 = now;
}

void __fastcall hk_UpdateCamera(void* cam) {
    So4StereoGateWait();   // not while the eye replays read the live camera
    uintptr_t c = reinterpret_cast<uintptr_t>(cam);
    RenderResGameTick();
    NoteCandidate(c);
    PickMain();
    if (c == g_main.load() && So4HtPre(c)) { o_UpdateCamera(cam); So4HtPost(c); }
    else {
        So4HtIdle(c);
        o_UpdateCamera(cam);
        if (!(*reinterpret_cast<uint8_t*>(c + 0x7AA) & 1)) So4HtNoteCameraView(c);   // perspective cameras
    }
    g_calls++;
    if (c == g_main.load()) {
        CamRec r; r.seq = 0; QueryPerformanceCounter(&r.t); r.present = g_presents.load();
        const uintptr_t offs[7] = {0x30, 0x120, 0x420, 0x5D0, 0x4E0, 0x4A0, 0x560};
        // The game just passed us this camera, so it is valid: read it directly. (SafeRead's VirtualQuery
        // costs ~1 ms in this game - the protection layer hooks it - far too slow for per-frame use.)
        bool ok = true;
        for (int i = 0; i < 7; ++i) memcpy(r.m[i], reinterpret_cast<void*>(c + offs[i]), 64);
        if (ok) { std::lock_guard<std::mutex> lk(g_recMx); r.seq = ++g_recSeq; g_rec[r.seq % kRec] = r; }
    }
    std::lock_guard<std::mutex> lk(g_mx);
    for (int i = 0; i < g_nCams; ++i) if (g_cams[i].cam == c) { g_cams[i].calls++; g_cams[i].callsTotal++; return; }
    if (g_nCams < kMax) {
        uintptr_t vt = *reinterpret_cast<uintptr_t*>(c);   // valid: the game just passed it
        g_cams[g_nCams++] = { c, 1, 1, vt, GetCurrentThreadId() };
    }
}

// ---- one-frame constant-buffer upload capture ----
struct Up { uint64_t buf; uint32_t bytes, bufBytes, how, off; uint32_t rva[4]; };
std::vector<Up> g_ups; std::vector<uint8_t> g_upData;
int g_capState = 0;   // 0 idle, 1 armed (starts at next Present), 2 capturing
std::wstring g_dir;

} // namespace

void So4UploadTap(void* buffer, const void* data, unsigned bytes, unsigned bufferBytes, int how) {
    if (g_capState != 2 || !data || !bytes || bytes > 65536 || g_upData.size() > (48u << 20)) return;
    Up u{reinterpret_cast<uint64_t>(buffer), bytes, bufferBytes, (uint32_t)how, (uint32_t)g_upData.size(), {0, 0, 0, 0}};
    void* fr[24]; USHORT n = RtlCaptureStackBackTrace(1, 24, fr, nullptr);
    int k = 0;
    for (USHORT i = 0; i < n && k < 4; ++i) {
        uintptr_t a = reinterpret_cast<uintptr_t>(fr[i]);
        if (a >= g_base && a < g_base + 0x6000000) u.rva[k++] = (uint32_t)(a - g_base);
    }
    g_ups.push_back(u);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    g_upData.insert(g_upData.end(), p, p + bytes);
}

void So4CameraOnPresent() {
    g_presents++;
    if (g_dir.empty()) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        g_dir = exe; g_dir = g_dir.substr(0, g_dir.find_last_of(L'\\'));
    }
    if (g_capState == 2) {
        g_capState = 0;
        std::wstring path = g_dir + L"\\rof_upl.bin";
        if (FILE* f = _wfopen(path.c_str(), L"wb")) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            uint32_t magic = 0x4C505553, nu = (uint32_t)g_ups.size(), nr = 0;
            std::vector<CamRec> recs;
            { std::lock_guard<std::mutex> lk(g_recMx); for (int i = 0; i < kRec; ++i) if (g_rec[i].seq) recs.push_back(g_rec[i]); }
            nr = (uint32_t)recs.size();
            uint64_t pc = g_presents.load();
            fwrite(&magic, 4, 1, f); fwrite(&nu, 4, 1, f); fwrite(&nr, 4, 1, f); fwrite(&now, 8, 1, f); fwrite(&pc, 8, 1, f);
            for (auto& r : recs) fwrite(&r, sizeof r, 1, f);
            fwrite(g_ups.data(), sizeof(Up), g_ups.size(), f);
            uint32_t nd = (uint32_t)g_upData.size(); fwrite(&nd, 4, 1, f); fwrite(g_upData.data(), 1, nd, f);
            uint64_t cams[48]; float views[48 * 12];
            uint32_t ncv = (uint32_t)So4HtCopyCamViews(cams, views, 48);
            fwrite(&ncv, 4, 1, f); fwrite(cams, 8, ncv, f); fwrite(views, 48, ncv, f);
            fclose(f);
        }
        LOGI("upload capture: %zu constant-buffer uploads, %zu bytes, written to rof_upl.bin", g_ups.size(), g_upData.size());
        g_ups.clear(); g_upData.clear();
        std::wstring done = g_dir + L"\\rof_upl_done.txt";
        if (FILE* f = _wfopen(done.c_str(), L"wb")) fclose(f);
    } else if (g_capState == 1) {
        g_capState = 2;
    } else {
        static int poll = 0;
        if (++poll % 30 == 0) {
            std::wstring req = g_dir + L"\\rof_upl_now.txt";
            if (GetFileAttributesW(req.c_str()) != INVALID_FILE_ATTRIBUTES) { DeleteFileW(req.c_str()); g_capState = 1; LOGI("upload capture armed"); }
        }
    }
}

bool So4CameraInstall() {
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) return false;
    bool ok = Hook(reinterpret_cast<void*>(g_base + kFnUpdateCamera), (void*)hk_UpdateCamera, (void**)&o_UpdateCamera, "SO4 UpdateCamera");
    ok = Hook(reinterpret_cast<void*>(g_base + kFnInvRigid), (void*)hk_InvRigid, (void**)&o_InvRigid, "SO4 view inverse (rigid)") && ok;
    ok = Hook(reinterpret_cast<void*>(g_base + kFnInvScaled), (void*)hk_InvScaled, (void**)&o_InvScaled, "SO4 view inverse (scaled)") && ok;
    LOGI("SO4 camera hook %s (code hook test: if the game exits right after this, the exe's anti-tamper rejects code hooks)", ok ? "installed" : "FAILED");
    return ok;
}

uintptr_t So4MainCamera() { return g_main.load(); }

void So4CameraReport(char* buf, unsigned size) {
    int len = snprintf(buf, size, "SO4 cameras: %llu UpdateCamera calls;", (unsigned long long)g_calls.exchange(0));
    std::lock_guard<std::mutex> lk(g_mx);
    uintptr_t best = 0; uint32_t bestCalls = 0;
    for (int i = 0; i < g_nCams && len < (int)size - 160; ++i) {
        CamStat& c = g_cams[i];
        float w[16] = {}, p[16] = {};
        SafeRead(w, reinterpret_cast<void*>(c.cam + 0x30), 64);
        SafeRead(p, reinterpret_cast<void*>(c.cam + 0x420), 64);
        bool identityRot = fabsf(w[0] - 1) < 1e-4f && fabsf(w[5] - 1) < 1e-4f && fabsf(w[10] - 1) < 1e-4f;
        if (c.calls > bestCalls && !identityRot && p[0] > 0.3f && p[0] < 10.f) { best = c.cam; bestCalls = c.calls; }
        if (c.calls)
            len += snprintf(buf + len, size - len, " [%p vt+%llX x%u thr %lu pos (%.1f %.1f %.1f) m00 %.3f m11 %.3f m22 %.4f m23 %.3f%s]",
                            (void*)c.cam, (unsigned long long)(c.vt - g_base), c.calls, c.thread, w[3], w[7], w[11], p[0], p[5], p[10], p[11],
                            identityRot ? " idRot" : "");
        c.calls = 0;
    }
    if (len < (int)size - 40) snprintf(buf + len, size - len, " main=%p", (void*)g_main.load());
}

} // namespace so4vr
