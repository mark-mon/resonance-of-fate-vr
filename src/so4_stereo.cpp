// SO4 native stereo.
//
// The render thread is an Aska command processor (RVA 0x5D910, process(thread, int* counter, bool* flag)):
// a ring of 0x2001 commands of 0x28 bytes at +0xA8 (type byte; args at +8/+0x10/+0x18/+0x20), read index +0xA4
// (last consumed), write index +0xA0. Type 0 runs a render job: job->vt[0x250/8](job, arg, flags) at 0x5DA3A.
// Type 10 ends the frame and calls 0x0D2E0 (which presents).
//
// Typical gameplay frame: ~114 type-0 jobs in 15 classes (none flagged for completion, so the job objects stay
// valid until the frame ends) plus a few other commands (types 1, 2, 3, 6, 17).
//
// Stereo (RenderMode = Native), per frame:
//   pass 1 (the game's own execution) = LEFT eye: when the per-view constant upload (off::kViewBlockBytes bytes) identifies the
//     frame's head record, every following constant upload is patched from the head camera to the left eye
//     (exact V/IV/P/IP/VP/IVP matches and any VP*World matrix through delta = VP_eye * inverse(VP_head));
//   frame end (0x0D2E0, before Present): the back buffer is copied as the left eye; the per-view constant blocks
//     are marked dirty (renderer+0x270B7C bits 0/3) so they are uploaded again, and the frame's recorded jobs run a
//     second time with the right eye's patch; the back buffer is copied as the right eye and both go to OpenXR.
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "MinHook.h"
#include "hook_util.h"
#include "log.h"
#include "so4_camera.h"
#include "ctx_stats.h"
#include "game_offsets.h"
#include "config.h"
#include "so4_stereo.h"
#include "temporal.h"
#include "so4_headtrack.h"
#include "so4_hud.h"
#include "render_res.h"
#include "xr_runtime.h"
#include "vrmath.h"
#include <d3d11.h>
#include <dxgi.h>
#include <string>

namespace so4vr {
void PrepareScanAround(uint64_t base);
void TallyScan(uint64_t frameSeq);
uint64_t CtxImmDraws();
namespace {

constexpr uintptr_t kFnProcess = 0x2A2FA0, kJobCallSite = 0x2A30C3, kFnFrameEnd = 0x28DE80;   // ROF (SO4 0x5D910 / 0x5DA3A / 0x0D2E0)
constexpr uint8_t   kJobCallBytes[6] = {0xFF, 0x90, 0x48, 0x02, 0x00, 0x00};   // ROF: call [rax+0x248]
constexpr int       kJobVt = 0x248 / 8;
constexpr uintptr_t kQRead = 0xB4, kQWrite = 0xB0, kQRing = 0xB8, kQEntry = 0x28;   // ROF (SO4 0xA4 / 0xA0 / 0xA8)
constexpr int       kQEntries = 0x2001;

uintptr_t g_base = 0;
using FnJob = void(__fastcall*)(void* obj, void* arg, uint32_t flags);
using FnProcess = uint64_t(__fastcall*)(void* q, int* counter, uint8_t* flag);
using FnFrameEnd = void(__fastcall*)(void* self);
FnProcess o_Process = nullptr; FnFrameEnd o_FrameEnd = nullptr;
std::atomic<uintptr_t> g_queue{0};

struct Job { void* obj; void* arg; uint32_t flags; };
constexpr int kMaxJobs = 8192;
Job g_jobs[kMaxJobs]; int g_jobN = 0; bool g_overflow = false;
bool g_inReplay = false, g_patching = false;
int g_prevEnd = -1;
std::atomic<uint64_t> s_jobs{0}, s_frames{0};

// The final 3D composite job (Aska::PostProcessCombiner); UI jobs (2D, minimap, ...) come after it.
constexpr uintptr_t kVtCombiner = 0xA26790;   // ROF PostProcessCombiner (SO4 0x90F038)
constexpr uintptr_t kVtMotionBlur = 0xA48170;   // ROF PostProcessMotionBlur (job fn 0x7F58C0; SO4 0x9296C8)   // camera motion blur job: smears every head movement in VR
std::atomic<uint64_t> s_mbSkipped{0};
int g_combinerIdx = -1;            // index of this frame's combiner job (pass 1)
void OnCombinerDone();             // pass 1, stereo frame: left eye captured here (no UI)

// diagnostic (StereoDiag = 1): for one gameplay frame, log every job with the render target it draws into
int g_traceFrame = 0; bool g_tracing = false; bool g_dumpReq = false;
void TraceJob(int idx, void* obj, const char* when) {
    if (!g_tracing) return;
    ID3D11DeviceContext* ctx = nullptr;
    uintptr_t r = *reinterpret_cast<uintptr_t*>(g_base + 0x1714AC0);
    ID3D11Device* dev = r ? *reinterpret_cast<ID3D11Device**>(r + 0x24A9B8) : nullptr;
    if (dev) dev->GetImmediateContext(&ctx);
    ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr;
    unsigned w = 0, h = 0; void* res = nullptr;
    if (ctx) {
        ctx->OMGetRenderTargets(1, &rtv, &dsv);
        if (rtv) { ID3D11Resource* rr = nullptr; rtv->GetResource(&rr); if (rr) { res = rr; D3D11_TEXTURE2D_DESC d{}; static_cast<ID3D11Texture2D*>(rr)->GetDesc(&d); w = d.Width; h = d.Height; rr->Release(); } rtv->Release(); }
        if (dsv) dsv->Release();
        ctx->Release();
    }
    LOGI("trace %s job %d vt+%llX rt0 %p %ux%u depth %d", when, idx, (unsigned long long)(*reinterpret_cast<uintptr_t*>(obj) - g_base), res, w, h, dsv != nullptr);
}

// Diagnostic: draws per job in pass 1 and in each eye's replay (a job drawing differently in one eye = glitch).
constexpr int kJobDiag = 512;
uint32_t g_jobDraws[3][kJobDiag]; uintptr_t g_jobVt[kJobDiag]; int g_jobDiagN[3] = {}; int g_jobPass = 0;
std::atomic<uint64_t> s_jobMismatchFrames{0};
// Draw packets (ReplayPacketSnapshot). Most jobs (generic fn +1EF9A0) draw `count` packets of 0x260 bytes that
// the game thread wrote for this frame, with each object's matrices built from that frame's camera. The game
// thread is already writing later frames while the eyes replay; when it reused this frame's packet memory, part
// of one eye was drawn with another frame's packets (tests 031-035: one extra per-view upload with the next
// camera in the glitched eye -> one-frame white surfaces). Pass 1 copies the packets; the replays draw the copies.
constexpr uintptr_t kFnJobPackets = 0x1EF9A0; constexpr uint32_t kPacketBytes = 0x260;
struct PktSnap { uint32_t off, size; };
PktSnap g_pkt[kMaxJobs]; std::vector<uint8_t> g_pktArena; size_t g_pktUsed = 0; int g_pktCur = 0;
std::atomic<uint64_t> s_pktJobs{0}, s_pktChanged{0}, s_pktBytesChanged{0}, s_pktOverflow{0};
// Pass-1 per-view blocks (ReplayRawFromPass1). The jobs build each draw's object matrix (+0, VP * World) from
// game data that other threads already update for the next frame while the eyes replay: now and then an eye's
// replay picked up the next frame's matrix for some draws (an extra per-view upload in that eye only, tests
// 031-046) -> parts of one eye drawn with another camera -> one-frame white surfaces. Each replay upload is
// matched to pass 1's uploads of the same job (all rows equal except the object matrix and the view rows) and
// the object matrix of the closest one is used.
constexpr int kP1Blocks = 8192;
struct P1Block { uint16_t job; uint32_t h; uint8_t b[off::kViewBlockBytes]; };
uint32_t g_umRows[140]; std::atomic<uint64_t> s_umSamples{0};
inline uint32_t P1Hash(const uint8_t* b) { uint32_t h = 2166136261u; for (unsigned k = 0x70; k < 0x340; k += 4) h = (h ^ *reinterpret_cast<const uint32_t*>(b + k)) * 16777619u; for (unsigned k = 0x370; k < off::kViewBlockBytes; k += 4) h = (h ^ *reinterpret_cast<const uint32_t*>(b + k)) * 16777619u; return h; }
P1Block* g_p1b = nullptr; int g_p1bN = 0; int g_jobSeq = 0;
int g_p1JobFirst[kMaxJobs]; int g_p1JobCount[kMaxJobs];
std::atomic<uint64_t> s_p1Nearest{0}, s_p1Partial{0}, s_p1Fixed{0}, s_p1Exact{0}, s_p1NoMatch{0}, s_p1Overflow{0};
// Diagnostic: does the main camera object's VP (+0x4E0) change while an eye replays?
uint32_t g_camHash = 0; int g_camChangedAt = -1; float g_camRow0[4];
std::atomic<uint64_t> s_camChanged[2]; std::atomic<uint64_t> s_camChecked{0};
inline uint32_t CamHash(uintptr_t cam) { uint32_t h = 2166136261u; const uint8_t* p = reinterpret_cast<const uint8_t*>(cam + 0x4E0); for (int k = 0; k < 64; ++k) h = (h ^ p[k]) * 16777619u; return h; }
void __fastcall JobThunk(void* obj, void* arg, uint32_t flags) {
    if (g_jobPass > 0 || !g_inReplay) ++g_jobSeq;
    if (g_inReplay && g_jobPass > 0 && g_config.debugGlitchDetect) {
        if (uintptr_t cam = So4MainCamera()) {
            const uint32_t h = CamHash(cam); s_camChecked++;
            if (h != g_camHash && g_camChangedAt < 0) {
                g_camChangedAt = g_jobDiagN[g_jobPass]; s_camChanged[g_jobPass - 1]++;
                static int lg = 0; const float* r = reinterpret_cast<const float*>(cam + 0x4E0);
                if (lg++ < 20) LOGI("camera VP changed during the %s-eye replay at job %d: row0 (%.4f %.4f %.4f %.2f) was (%.4f %.4f %.4f %.2f)", g_jobPass == 1 ? "left" : "right",
                                    g_camChangedAt, r[0], r[1], r[2], r[3], g_camRow0[0], g_camRow0[1], g_camRow0[2], g_camRow0[3]);
            }
        }
    }
    if (!g_inReplay && g_tracing) TraceJob(g_jobN, obj, "before");
    int idx = -1;
    if (!g_inReplay) { if (g_jobN < kMaxJobs) { idx = g_jobN; g_jobs[g_jobN++] = {obj, arg, flags}; } else g_overflow = true; }
    const bool pktJob = arg && flags > 0 && flags <= 8192 &&
                        reinterpret_cast<uintptr_t>((*reinterpret_cast<void***>(obj))[kJobVt]) == g_base + kFnJobPackets;
    if (idx >= 0) {
        g_pkt[idx] = {0, 0};
        if (pktJob && g_config.replayPacketSnapshot && g_config.activeMode == Config::Native) {
            const size_t sz = size_t(flags) * kPacketBytes;
            if (g_pktArena.size() < (16u << 20)) g_pktArena.resize(16u << 20);
            if (g_pktUsed + sz <= g_pktArena.size()) { memcpy(g_pktArena.data() + g_pktUsed, arg, sz); g_pkt[idx] = {uint32_t(g_pktUsed), uint32_t(sz)}; g_pktUsed += sz; }
            else s_pktOverflow++;
        }
    } else if (g_inReplay && g_jobPass > 0 && pktJob) {
        int c = g_pktCur, lim = g_pktCur + 64;
        while (c < g_jobN && c < lim && !(g_jobs[c].obj == obj && g_jobs[c].arg == arg)) ++c;
        if (c < g_jobN && c < lim) {
            g_pktCur = c + 1;
            const PktSnap& ps = g_pkt[c];
            if (ps.size == size_t(flags) * kPacketBytes) {
                s_pktJobs++;
                uint8_t* snap = g_pktArena.data() + ps.off;
                if (memcmp(arg, snap, ps.size) != 0) {
                    s_pktChanged++;
                    for (uint32_t k = 0; k < ps.size; k += 16) if (memcmp(static_cast<uint8_t*>(arg) + k, snap + k, 16)) s_pktBytesChanged += 16;
                }
                if (g_config.replayPacketSnapshot) arg = snap;
            }
        }
    }
    s_jobs++;
    if (!g_config.motionBlur && *reinterpret_cast<uintptr_t*>(obj) - g_base == kVtMotionBlur && g_config.activeMode != Config::Flat) { s_mbSkipped++; return; }
    const int jp = g_jobPass, ji = g_jobDiagN[jp];
    const uint64_t d0 = CtxImmDraws();
    reinterpret_cast<FnJob>((*reinterpret_cast<void***>(obj))[kJobVt])(obj, arg, flags);
    if (ji < kJobDiag && (jp != 0 || g_combinerIdx < 0)) {
        g_jobDraws[jp][ji] = uint32_t(CtxImmDraws() - d0); if (jp == 0) g_jobVt[ji] = *reinterpret_cast<uintptr_t*>(obj) - g_base; g_jobDiagN[jp] = ji + 1;
    }
    if (idx >= 0 && *reinterpret_cast<uintptr_t*>(obj) - g_base == kVtCombiner && g_combinerIdx < 0) { g_combinerIdx = idx; OnCombinerDone(); }
}

// Diagnostic: the renderer's constant setters (memcmp + copy into the CPU copy + dirty bit). Called from any
// thread other than the render thread = the game thread writing render constants concurrently with the replays.
std::atomic<uint64_t> s_offThreadSet{0}; DWORD g_renderTid = 0;
// Replay gate: the jobs compute per-object matrices from the LIVE camera; when the game thread updated the camera
// in the middle of an eye's replay, that eye drew part of the frame with the next camera (one extra per-view
// upload in that eye) -> one-frame white surfaces. Camera updates wait while the replays run (ReplayCameraGate).
std::atomic<bool> g_gate{false};
std::atomic<DWORD> g_gameTid{0}; std::atomic<uint64_t> s_suspends{0};
std::atomic<uint64_t> s_crossEye{0}, s_ownEye{0}, s_deltaLooseOnly{0}, s_deltaNearMiss{0};
std::atomic<uint64_t> s_gateWaits{0}, s_gateTimeouts{0}; std::atomic<int64_t> s_gateUs{0};
using FnSetConst = void(__fastcall*)(void*, int, int, const void*, int);
FnSetConst o_SetConstA = nullptr, o_SetConstB = nullptr;
static void OffThread(const char* which, int bit, int idx, int cnt, void* ret) {
    uint64_t n = ++s_offThreadSet;
    if (n <= 12 || (n & 4095) == 0) LOGI("constant setter %s on thread %lu (render %lu, replay %d): bit %d index %d count %d from +%llX (%llu so far)", which, GetCurrentThreadId(),
                                     g_renderTid, g_inReplay ? 1 : 0, bit, idx, cnt, (unsigned long long)(reinterpret_cast<uintptr_t>(ret) - g_base), (unsigned long long)n);
}
void __fastcall hk_SetConstA(void* r, int bit, int idx, const void* src, int cnt) {
    if (g_renderTid && GetCurrentThreadId() != g_renderTid) OffThread("A(+26DF78)", bit, idx, cnt, __builtin_return_address(0));
    o_SetConstA(r, bit, idx, src, cnt);
}
void __fastcall hk_SetConstB(void* r, int bit, int idx, const void* src, int cnt) {
    if (g_renderTid && GetCurrentThreadId() != g_renderTid) OffThread("B(+26DF7C)", bit, idx, cnt, __builtin_return_address(0));
    o_SetConstB(r, bit, idx, src, cnt);
}
uint64_t __fastcall hk_Process(void* q, int* counter, uint8_t* flag) {
    if (!g_inReplay) { g_renderTid = GetCurrentThreadId(); g_queue = reinterpret_cast<uintptr_t>(q); RenderResRenderTick(reinterpret_cast<uintptr_t>(q)); }
    return o_Process(q, counter, flag);
}

// ---- stage 1 diagnostics ----
void LogFrame(int n, int from, int to) {
    static ULONGLONG last = 0; static int logs = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 5000 || logs > 40) return;
    last = now; ++logs;
    struct C { uintptr_t vt; int n, flagged; void* fn; } cls[64]; int nc = 0;
    for (int i = 0; i < n; ++i) {
        uintptr_t vt = *reinterpret_cast<uintptr_t*>(g_jobs[i].obj);
        bool fl = (*reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(g_jobs[i].obj) + 0x190) & 0x80000) != 0;
        int k = 0; for (; k < nc; ++k) if (cls[k].vt == vt) break;
        if (k == nc && nc < 64) cls[nc++] = {vt, 0, 0, (*reinterpret_cast<void***>(g_jobs[i].obj))[kJobVt]};
        if (k < nc) { cls[k].n++; if (fl) cls[k].flagged++; }
    }
    char line[2048]; int len = snprintf(line, sizeof line, "stereo diag: frame %llu: %d jobs, %d classes:", (unsigned long long)s_frames.load(), n, nc);
    for (int k = 0; k < nc && len < 1900; ++k)
        len += snprintf(line + len, sizeof line - len, " [vt+%llX fn+%llX x%d f%d]", (unsigned long long)(cls[k].vt - g_base),
                        (unsigned long long)(reinterpret_cast<uintptr_t>(cls[k].fn) - g_base), cls[k].n, cls[k].flagged);
    LOGI("%s", line);
    uintptr_t q = g_queue.load();
    if (q && from >= 0) {
        int hist[32] = {}; int total = 0;
        for (int i = from; ; ) {
            i = i + 1 < kQEntries ? i + 1 : 0;
            if (i == to) break;
            uint8_t t = *reinterpret_cast<uint8_t*>(q + kQRing + uintptr_t(i) * kQEntry);
            if (t < 32) hist[t]++; total++;
            if (total > 9000) break;
        }
        len = snprintf(line, sizeof line, "stereo diag: commands %d (%d..%d):", total, from, to);
        for (int t = 0; t < 32; ++t) if (hist[t]) len += snprintf(line + len, sizeof line - len, " type%d=%d", t, hist[t]);
        LOGI("%s", line);
    }
}

// ---------------------------------------------------------------- stereo state
constexpr uintptr_t kRendererGlobal = 0x1714AC0, kRendSwapChain = 0x24A9C8, kRendDirty = 0x26DF7C;   // ROF (SO4 0xF76C70 / 0x270B7C)
struct Eye { float m[6][16]; };   // V, IV, P, IP, VP, IVP
HeadFrame g_hf{};
float g_center[6][16], g_eye[2][6][16], g_delta[16];
int   g_patchEye = -1;            // eye being patched (0 left in pass 1, 1 right in the replay), -1 none
bool  g_pass1Done = false;
ID3D11Device* g_dev = nullptr; ID3D11DeviceContext* g_ctx = nullptr;
ID3D11Texture2D* g_eyeTex[2] = {};
ID3D11Texture2D* g_desktop = nullptr;   // the full pass-1 frame (with UI), put back for the monitor after the replay
std::atomic<uint64_t> s_stereo{0}, s_noRecord{0}, s_patchMats{0}, s_patchDelta{0}, s_replayUploads{0};
double g_replayMs = 0; uint64_t g_replayN = 0;

float g_prevCenter[6][16]; bool g_prevOk = false; std::atomic<uint64_t> s_patchPrev{0};
bool ViewSame(const float* a, const float* b) { return memcmp(a, b, 48) == 0; }
bool Near(float a, float b) { return std::fabs(a - b) <= 1e-4f * std::fmax(1.0f, std::fabs(a)); }

void BuildCenter(const HeadFrame& hf, float out[6][16]) {
    memcpy(out[0], hf.view, 64);
    vm::Inverse4(out[0], out[1]);
    memcpy(out[2], hf.proj, 64);
    vm::Inverse4(out[2], out[3]);
    vm::Mul4(out[2], out[0], out[4]);
    vm::Mul4(out[1], out[3], out[5]);
}

// Eye matrices from the head (centre) camera: eye offset/rotation in view space, eye frustum from the union one.
void BuildEye(const HeadFrame& hf, const float C[6][16], int e, float out[6][16]) {
    vm::Quat q{hf.eyeRot[e * 4 + 0], hf.eyeRot[e * 4 + 1], hf.eyeRot[e * 4 + 2], hf.eyeRot[e * 4 + 3]};
    float r[9]; vm::QuatToMat3(q, r);
    const float* o = hf.eyeOff[e];
    float M[16]; vm::MakeTransform(r, {o[0], o[1], o[2]}, M);          // eye -> head (view space)
    float E[16] = { r[0], r[3], r[6], 0,  r[1], r[4], r[7], 0,  r[2], r[5], r[8], 0,  0, 0, 0, 1 };
    E[3]  = -(r[0] * o[0] + r[3] * o[1] + r[6] * o[2]);
    E[7]  = -(r[1] * o[0] + r[4] * o[1] + r[7] * o[2]);
    E[11] = -(r[2] * o[0] + r[5] * o[1] + r[8] * o[2]);                  // head -> eye
    vm::Mul4(E, C[0], out[0]);
    vm::Mul4(C[1], M, out[1]);
    auto tn = [](float a) { return std::tan(a); };
    const XrFovf& u = hf.fov; const XrFovf& f = hf.eyeFov[e];
    float uhw = (tn(u.angleRight) - tn(u.angleLeft)) * 0.5f, ucx = (tn(u.angleRight) + tn(u.angleLeft)) * 0.5f;
    float uhh = (tn(u.angleUp) - tn(u.angleDown)) * 0.5f,    ucy = (tn(u.angleUp) + tn(u.angleDown)) * 0.5f;
    float ehw = (tn(f.angleRight) - tn(f.angleLeft)) * 0.5f, ecx = (tn(f.angleRight) + tn(f.angleLeft)) * 0.5f;
    float ehh = (tn(f.angleUp) - tn(f.angleDown)) * 0.5f,    ecy = (tn(f.angleUp) + tn(f.angleDown)) * 0.5f;
    const float* Pc = C[2]; float* P = out[2];
    memcpy(P, Pc, 64);
    P[0] = Pc[0] * (uhw / ehw);
    P[5] = Pc[5] * (uhh / ehh);
    P[2] = Pc[2] + (ucx / uhw - ecx / ehw);
    P[6] = Pc[6] + (ucy / uhh - ecy / ehh) * g_config.lensShiftYSign;
    if (!vm::Inverse4(P, out[3])) memcpy(out[3], C[3], 64);
    vm::Mul4(out[2], out[0], out[4]);
    vm::Mul4(out[1], out[3], out[5]);
}

// Neighbouring head records' matrices (centre and both eyes), cached per frame. In the replay the renderer
// rebuilds the per-view blocks from the live camera, which the game thread has often already advanced to the next
// update (record +1, test 063) - while the objects' own matrices still carry this frame's record. Blocks built from
// another record are mapped to THIS frame's eye matrices, or anything drawn with the per-view camera (the player
// character) is off by one head update -> judder / sway when turning the head.
struct RecMats { uint64_t seq = 0; bool ok = false; float c[6][16]; float e[2][6][16]; float nh[6][16]; };
RecMats g_recCache[6]; uint64_t g_recFrame = ~0ull;
int g_blockRel = 0; bool g_blockNoHead = false; ULONGLONG g_markUntil = 0; int g_fr0 = 0, g_frShift = 0, g_frOther = 0;   // record offset of the per-view block being uploaded in the replay (0 = this frame's)
std::atomic<uint64_t> s_shiftPatched{0}, s_shiftBlocks{0}, s_deltaOrtho{0}, s_otherNew{0}, s_dupFixed{0};
// Per-frame diagnostics ring (dumped when the XR thread's frame scan finds a one-frame glitch): draws per job in
// pass 1 / left / right and per-eye patch counts.
uint32_t g_frStat[2][5];
inline void FrStat(int k) { if (g_patchEye >= 0 && g_patchEye < 2) g_frStat[g_patchEye][k]++; }
struct UpLog { uint16_t job; int8_t rel; uint8_t deltas; uint32_t hash; uint32_t full; uint16_t doff[4]; float r0[4]; float t[2]; float head[48]; };
UpLog* g_curUl = nullptr;
constexpr int kUpMax = 400;
UpLog g_upCur[2][kUpMax]; int g_upCurN[2] = {};
struct FrameDiag { uint64_t seq = 0; int n = 0; uint16_t draws[3][192]; uint32_t vt[192]; uint32_t st[2][5]; UpLog up[2][kUpMax]; int nUp[2]; int rawDiff = 0, rawFirst = -1, rawN[2] = {}; uint16_t rowHist[140]; struct RawSample { int idx, job, row; float l[4], r[4]; } smp[8]; int nSmp = 0; struct MaskDiff { int i, j, job, off; uint64_t ml, mr; float l[16], r[16]; } md[4]; int nMd = 0, maskDiff = 0; struct Extra { int eye, idx, job, rows; uint32_t vt; float xt[3][4]; float pm[16]; int16_t row[8]; float v[8][4], pv[8][4]; } ex[4]; int nEx = 0, unaligned = 0; };
FrameDiag g_diagRing[16]; int g_diagHead = 0; std::mutex g_diagM;
// Raw (unpatched) per-view block uploads of the last frames, both eyes (DebugGlitchDetect): the two eyes replay
// the same commands, so their raw uploads should be identical; a difference is live data that changed between
// the replays (game thread) - printed for the glitched frame.
constexpr int kRawMax = 2048, kRawFrames = 4;
struct RawFrame { uint64_t seq; int n[2]; uint16_t job[2][kRawMax]; uint64_t dmask[2][kRawMax]; int diff, first; };
uint64_t* g_curDmask = nullptr;   // delta-patched 16-byte rows (bit = offset / 64) of the per-view upload being patched
RawFrame* g_raw = nullptr; uint8_t* g_rawBlk = nullptr; int g_rawCur = 0;
inline uint8_t* RawBlk(int f, int e, int i) { return g_rawBlk + ((size_t(f) * 2 + e) * kRawMax + i) * off::kViewBlockBytes; }
constexpr int kP1Max = 4096; float g_p1Views[kP1Max][12]; int g_p1N = 0;
const RecMats* GetRec(uint64_t seq) {
    if (g_recFrame != g_hf.seq) { for (auto& r : g_recCache) r.ok = false, r.seq = 0; g_recFrame = g_hf.seq; }
    for (auto& r : g_recCache) if (r.ok && r.seq == seq) return &r;
    RecMats* slot = nullptr;
    for (auto& r : g_recCache) if (!r.ok && r.seq == 0) { slot = &r; break; }
    if (!slot) return nullptr;
    HeadFrame h;
    if (seq == g_hf.seq) h = g_hf;
    else if (!So4HtRecordBySeq(seq, h)) { slot->seq = seq; return nullptr; }   // remember the miss
    BuildCenter(h, slot->c);
    for (int e = 0; e < 2; ++e) BuildEye(h, slot->c, e, slot->e[e]);
    { HeadFrame g = h; memcpy(g.view, h.viewNoHead, 64); BuildCenter(g, slot->nh); }
    slot->seq = seq; slot->ok = true;
    return slot;
}

void SetPatchEye(int e) {
    g_patchEye = e;
    g_patching = e >= 0;
    if (e >= 0) vm::Mul4(g_eye[e][4], g_center[5], g_delta);
}

bool IsTitleView(const float* V) { return V[0] == 1.0f && V[5] == 1.0f && V[10] == 1.0f && V[3] == 0.0f && V[7] == 0.0f; }

bool EnsureDevice() {
    if (g_ctx) return true;
    uintptr_t r = *reinterpret_cast<uintptr_t*>(g_base + kRendererGlobal);
    if (!r) return false;
    IDXGISwapChain* sc = *reinterpret_cast<IDXGISwapChain**>(r + kRendSwapChain);
    if (!sc || FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_dev)) || !g_dev) return false;
    g_dev->GetImmediateContext(&g_ctx);
    return g_ctx != nullptr;
}

// The combiner renders the final 3D image into an offscreen target (copied to the back buffer only at frame
// end): capture whatever is bound as render target 0 right after the combiner job.
bool CaptureBackBuffer(int e) {
    ID3D11Texture2D* bb = nullptr;
    {
        ID3D11RenderTargetView* rtv = nullptr;
        g_ctx->OMGetRenderTargets(1, &rtv, nullptr);
        if (!rtv) return false;
        ID3D11Resource* res = nullptr; rtv->GetResource(&res); rtv->Release();
        if (!res) return false;
        D3D11_RESOURCE_DIMENSION dim; res->GetType(&dim);
        if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) { res->Release(); return false; }
        bb = static_cast<ID3D11Texture2D*>(res);
        static bool logged = false;
        if (!logged) { logged = true; D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d); LOGI("stereo: combiner output %ux%u format %d samples %u", d.Width, d.Height, d.Format, d.SampleDesc.Count); }
    }
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    D3D11_TEXTURE2D_DESC have{};
    if (g_eyeTex[e]) g_eyeTex[e]->GetDesc(&have);
    if (!g_eyeTex[e] || have.Width != d.Width || have.Height != d.Height || have.Format != d.Format) {
        if (g_eyeTex[e]) { g_eyeTex[e]->Release(); g_eyeTex[e] = nullptr; }
        D3D11_TEXTURE2D_DESC n = d; n.MipLevels = 1; n.Usage = D3D11_USAGE_DEFAULT;
        n.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; n.CPUAccessFlags = 0; n.MiscFlags = 0;
        g_dev->CreateTexture2D(&n, nullptr, &g_eyeTex[e]); if (g_eyeTex[e]) TemporalMarkModTexture(g_eyeTex[e]);
        if (g_eyeTex[e]) LOGI("stereo: eye texture %ux%u format %d", d.Width, d.Height, d.Format);
    }
    if (g_eyeTex[e]) g_ctx->CopyResource(g_eyeTex[e], bb);
    bb->Release();
    return g_eyeTex[e] != nullptr;
}

ID3D11Texture2D* BackBuffer() {
    uintptr_t r = *reinterpret_cast<uintptr_t*>(g_base + kRendererGlobal);
    IDXGISwapChain* sc = r ? *reinterpret_cast<IDXGISwapChain**>(r + kRendSwapChain) : nullptr;
    ID3D11Texture2D* bb = nullptr;
    if (!sc || FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return nullptr;
    return bb;
}
bool CaptureDesktop() {
    ID3D11Texture2D* bb = BackBuffer(); if (!bb) return false;
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    D3D11_TEXTURE2D_DESC have{}; if (g_desktop) g_desktop->GetDesc(&have);
    if (!g_desktop || have.Width != d.Width || have.Height != d.Height || have.Format != d.Format) {
        if (g_desktop) { g_desktop->Release(); g_desktop = nullptr; }
        D3D11_TEXTURE2D_DESC n = d; n.MipLevels = 1; n.Usage = D3D11_USAGE_DEFAULT; n.BindFlags = D3D11_BIND_SHADER_RESOURCE; n.CPUAccessFlags = 0; n.MiscFlags = 0;
        g_dev->CreateTexture2D(&n, nullptr, &g_desktop); if (g_desktop) TemporalMarkModTexture(g_desktop);
    }
    if (g_desktop) g_ctx->CopyResource(g_desktop, bb);
    bb->Release();
    return g_desktop != nullptr;
}
void RestoreDesktop() {
    ID3D11Texture2D* bb = BackBuffer(); if (!bb) return;
    if (g_desktop) g_ctx->CopyResource(bb, g_desktop);
    bb->Release();
}

void DumpBmp(ID3D11Texture2D* tex, const wchar_t* name, bool alpha = false, UINT step = 1) {
    D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr; g_dev->CreateTexture2D(&sd, nullptr, &st);
    if (!st) return;
    g_ctx->CopyResource(st, tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        UINT w = d.Width / step, h = d.Height / step, rowBytes = (w * 3 + 3) & ~3u;
        std::vector<uint8_t> px(size_t(rowBytes) * h);
        bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        for (UINT y = 0; y < h; ++y) {
            const uint8_t* src = static_cast<const uint8_t*>(m.pData) + size_t(y) * step * m.RowPitch;
            uint8_t* dst = px.data() + size_t(h - 1 - y) * rowBytes;
            for (UINT x = 0; x < w; ++x) { const uint8_t* sp = src + size_t(x) * step * 4; if (alpha) { dst[x*3] = dst[x*3+1] = dst[x*3+2] = sp[3]; continue; } dst[x*3] = bgra ? sp[0] : sp[2]; dst[x*3+1] = sp[1]; dst[x*3+2] = bgra ? sp[2] : sp[0]; }
        }
        g_ctx->Unmap(st, 0);
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe; path = path.substr(0, path.find_last_of(L'\\') + 1) + name;
        BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
        fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = DWORD(fh.bfOffBits + px.size());
        ih.biSize = sizeof ih; ih.biWidth = LONG(w); ih.biHeight = LONG(h); ih.biPlanes = 1; ih.biBitCount = 24;
        if (FILE* f = _wfopen(path.c_str(), L"wb")) { fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(px.data(), 1, px.size(), f); fclose(f); }
        LOGI("stereo: wrote %ls", name);
    }
    st->Release();
}

bool g_leftCaptured = false;
bool g_guessed = false, g_stereoThisFrame = false, g_frameReady = false;
bool g_combinerSeen = false;
struct Vote { HeadFrame hf; int n; };
constexpr int kMaxVotes = 16; Vote g_votes[kMaxVotes]; int g_voteN = 0;
std::atomic<uint64_t> s_votePickNotFirst{0}, s_voteMulti{0};
void ApplyFrameRecord(const HeadFrame& hf);
// The frame's record: most votes among the records at most 8 updates older than the newest voted one.
bool PickFrameRecord() {
    if (!g_voteN) return false;
    uint64_t newest = 0; for (int k = 0; k < g_voteN; ++k) if (g_votes[k].hf.seq > newest) newest = g_votes[k].hf.seq;
    int best = -1;
    for (int k = 0; k < g_voteN; ++k) {
        if (g_votes[k].hf.seq + 8 < newest) continue;
        if (best < 0 || g_votes[k].n > g_votes[best].n || (g_votes[k].n == g_votes[best].n && g_votes[k].hf.seq > g_votes[best].hf.seq)) best = k;
    }
    if (best < 0) return false;
    if (g_voteN > 1) s_voteMulti++;
    if (best != 0) s_votePickNotFirst++;
    ApplyFrameRecord(g_votes[best].hf);
    return true;
}
std::atomic<uint64_t> s_guessOk{0}, s_guessBad{0}, s_guessUnconfirmed{0};
bool g_hudBuilt = false;
DXGI_FORMAT g_hudViewFmt = DXGI_FORMAT_UNKNOWN;
std::atomic<uint64_t> s_hudNoEnd{0}, s_hudNoRt{0};
bool BuildFakeQueue(int from, int upto);
extern uint8_t* g_fakeQ;

// HUD layer. Gameplay frame (trace): ... Bloom, PostProcessCombiner (leaves the 1280x720 final target bound
// with depth), then ~10 UI jobs (minimap, Base2D, ...) drawing into that target, then commands 3 (resolve),
// 6 (release the game thread), 10 (frame end). At the combiner the render thread is ~2 frames behind, so the
// frame's UI commands are already in the ring: they are run here once over black and once over white (target
// cleared, left eye already copied out) and the pair gives the HUD with real coverage. The right-eye replay
// that follows rewrites the target, and the game's own UI pass then draws over the right eye (monitor).
bool CaptureHud() {
    uintptr_t q = g_queue.load();
    if (!q || !g_fakeQ) return false;
    const int cur = *reinterpret_cast<volatile int*>(q + kQRead);
    const int wr = *reinterpret_cast<volatile int*>(q + kQWrite);
    int last = -1, jobs = 0;
    for (int i = cur, n = 0; n < 4096; ++n) {
        i = i + 1 < kQEntries ? i + 1 : 0;
        if (i == wr) break;
        uint8_t t = *reinterpret_cast<uint8_t*>(q + kQRing + uintptr_t(i) * kQEntry);
        if (t == 3 || t == 6 || t == 10) { last = i == 0 ? kQEntries - 1 : i - 1; break; }
        if (t == 0) jobs++;
    }
    if (last < 0 || jobs == 0) { s_hudNoEnd++; return false; }
    ID3D11RenderTargetView* rtv[8] = {}; ID3D11DepthStencilView* dsv = nullptr;
    g_ctx->OMGetRenderTargets(8, rtv, &dsv);
    D3D11_VIEWPORT vps[16]; UINT nvp = 16; g_ctx->RSGetViewports(&nvp, vps);
    ID3D11Texture2D* fin = nullptr;
    if (rtv[0]) { ID3D11Resource* r = nullptr; rtv[0]->GetResource(&r); D3D11_RESOURCE_DIMENSION dim{}; if (r) r->GetType(&dim);
                  if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) fin = static_cast<ID3D11Texture2D*>(r); else if (r) r->Release(); }
    DXGI_FORMAT vf = DXGI_FORMAT_UNKNOWN;
    if (rtv[0]) { D3D11_RENDER_TARGET_VIEW_DESC rd{}; rtv[0]->GetDesc(&rd); vf = rd.Format; }
    g_hudViewFmt = vf;
    bool ok = fin && So4HudPrepare(g_dev, fin, vf);
    if (!fin) s_hudNoRt++;
    for (int bg = 0; ok && bg < 2; ++bg) {
        const float c[4] = {float(bg), float(bg), float(bg), 1.0f};
        g_ctx->ClearRenderTargetView(rtv[0], c);
        g_ctx->OMSetRenderTargets(8, rtv, dsv);
        if (nvp) g_ctx->RSSetViewports(nvp, vps);
        if (!BuildFakeQueue(cur, last)) { ok = false; break; }
        g_inReplay = true;
        int counter = 0; uint8_t flag = 1;
        o_Process(g_fakeQ, &counter, &flag);
        g_inReplay = false;
        So4HudStore(g_ctx, fin, bg);
    }
    if (ok) ok = So4HudBuildMatte(g_ctx);
    g_ctx->OMSetRenderTargets(8, rtv, dsv);   // as the combiner left it
    if (nvp) g_ctx->RSSetViewports(nvp, vps);
    if (fin) fin->Release();
    for (auto* v : rtv) if (v) v->Release();
    if (dsv) dsv->Release();
    return ok;
}
bool CaptureBackBuffer(int e);
void ReplayRightEye(int last);
bool ReplayEye(int eye);
void OnCombinerDone() {
    if (g_config.stereoBothReplay) {
        // Pass 1 rendered the centre camera unpatched. Both eyes are rendered by replaying this frame's commands
        // (types 0/2/3 up to the combiner) with each eye's patch: the replay re-uploads every per-view block, so
        // nothing camera-dependent is left over from before the frame's view upload (pass-1 patching could not
        // reach uploads made before it - e.g. a light's shadow pass at the Calnus door -> broken left-eye shadows).
        TemporalEndPass1();   // history textures: found during pass 1
        g_combinerSeen = true;
        if (!g_frameReady) return;
        g_frameReady = false;
        if (!PickFrameRecord()) return;
        if (!EnsureDevice()) return;
        g_gate.store(g_config.replayCameraGate);
        HANDLE susp = nullptr;
        if (g_config.debugSuspendGame && g_gameTid.load()) {   // diagnostic: the game thread does nothing while the eyes replay
            susp = OpenThread(THREAD_SUSPEND_RESUME, FALSE, g_gameTid.load());
            if (susp && SuspendThread(susp) == (DWORD)-1) { CloseHandle(susp); susp = nullptr; }
            if (susp) s_suspends++;
        }
        { static int lg = 0; if (lg < 3 && g_config.debugSuspendGame) { ++lg; LOGI("suspend game thread: tid %lu -> %s (err %lu)", g_gameTid.load(), susp ? "ok" : "FAILED", GetLastError()); } }
        g_hudBuilt = CaptureHud();
        if (ReplayEye(0)) ReplayEye(1);
        if (susp) { ResumeThread(susp); CloseHandle(susp); }
        g_gate.store(false);
        return;
    }
    if (g_patchEye != 0) return;
    if (g_guessed) { g_guessed = false; SetPatchEye(-1); s_guessUnconfirmed++; return; }   // no view upload confirmed the guess: no stereo
    // Still inside the frame (before the render thread releases the game thread with command type 6, after
    // which the game starts rewriting this frame's draw data): left eye = what pass 1 produced; the right
    // eye is rendered right now by running jobs 0..combiner again. The UI jobs that follow then draw over
    // the right eye on the monitor only.
    bool left = EnsureDevice() && CaptureBackBuffer(0);
    SetPatchEye(-1);            // UI after this point is not camera-dependent
    if (!left) return;
    g_hudBuilt = CaptureHud();   // before the right eye: its replay ends in the state the game's UI expects
    ReplayRightEye(g_combinerIdx);
}

bool RunJobs(int n) {
    static int traced = 0;
    const bool trace = traced < 2; ++traced;
    for (int i = 0; i < n; ++i) {
        const Job& j = g_jobs[i];
        if (trace) LOGI("stereo replay: job %d/%d obj %p vt+%llX fn+%llX arg %p flags %x", i, n, j.obj,
                        (unsigned long long)(*reinterpret_cast<uintptr_t*>(j.obj) - g_base),
                        (unsigned long long)(reinterpret_cast<uintptr_t>((*reinterpret_cast<void***>(j.obj))[kJobVt]) - g_base), j.arg, j.flags);
        reinterpret_cast<FnJob>((*reinterpret_cast<void***>(j.obj))[kJobVt])(j.obj, j.arg, j.flags);
    }
    return true;
}

// Private queue for the replay: same layout as the render thread object, ring filled with this frame's
// commands of types 0 (jobs), 2 (set render target + clear) and 3 (resolve) up to the combiner.
constexpr size_t kQSize = 0x50200;   // ROF queue object: ring ends at 0x500E0, fields up to ~0x50110
uint8_t* g_fakeQ = nullptr;   // (declared above for CaptureHud)
std::atomic<uint64_t> s_replayCmds{0};
bool BuildFakeQueue(int from, int upto) {
    uintptr_t q = g_queue.load();
    if (!q || !g_fakeQ || from < 0 || upto < 0) return false;
    memcpy(g_fakeQ, reinterpret_cast<void*>(q), kQRing);                  // header fields
    *reinterpret_cast<uint64_t*>(g_fakeQ + 0x88) = 0;   // ROF (SO4 +0x78)                       // no pending callback
    int k = 0;
    for (int i = from; ; ) {
        i = i + 1 < kQEntries ? i + 1 : 0;
        const uint8_t* e = reinterpret_cast<const uint8_t*>(q + kQRing + uintptr_t(i) * kQEntry);
        uint8_t t = e[0];
        if (t == 0 || t == 2 || t == 3) { ++k; memcpy(g_fakeQ + kQRing + uintptr_t(k) * kQEntry, e, kQEntry); }
        if (i == upto || k >= kQEntries - 2) break;
    }
    *reinterpret_cast<int*>(g_fakeQ + kQRead) = 0;      // last consumed
    *reinterpret_cast<int*>(g_fakeQ + kQWrite) = k + 1; // one past the last entry
    s_replayCmds += k;
    return k > 0;
}

bool ReplayEye(int eye);
void ReplayRightEye(int last) { (void)last; ReplayEye(1); }
bool ReplayEye(int eye) {
    LARGE_INTEGER t0, t1, qf; QueryPerformanceCounter(&t0);
    uintptr_t r = *reinterpret_cast<uintptr_t*>(g_base + kRendererGlobal);
    if (r) *reinterpret_cast<uint32_t*>(r + kRendDirty) |= 0x09;   // re-upload the per-view constant blocks
    if (g_config.debugReplay == 7) {   // diagnostic: magenta back buffer before the replay (what the replay does not draw stays magenta)
        if (ID3D11Texture2D* bb = BackBuffer()) {
            ID3D11RenderTargetView* rtv = nullptr;
            D3D11_RENDER_TARGET_VIEW_DESC rd{}; D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
            rd.Format = d.Format; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            if (SUCCEEDED(g_dev->CreateRenderTargetView(bb, &rd, &rtv)) && rtv) { const float mg[4] = {1, 0, 1, 1}; g_ctx->ClearRenderTargetView(rtv, mg); rtv->Release(); }
            bb->Release();
        }
    }
    uintptr_t q = g_queue.load();
    int cur = q ? *reinterpret_cast<volatile int*>(q + kQRead) : -1;   // the combiner command being executed
    if (!BuildFakeQueue(g_prevEnd, cur)) return false;
    if (g_config.stereoBothReplay) TemporalBeforeEye(g_ctx, eye);   // each eye blends with its own previous frame (TAA etc.)
    g_jobPass = eye + 1; g_jobDiagN[g_jobPass] = 0; g_pktCur = 0; g_jobSeq = 0;
    g_camChangedAt = -1; if (uintptr_t cam = So4MainCamera()) { g_camHash = CamHash(cam); memcpy(g_camRow0, reinterpret_cast<const void*>(cam + 0x4E0), 16); }
    g_inReplay = true;
    SetPatchEye(eye);
    int counter = 0; uint8_t flag = 1;
    o_Process(g_fakeQ, &counter, &flag);
    SetPatchEye(-1);
    g_inReplay = false;
    g_jobPass = 0;
    if (g_config.stereoBothReplay) TemporalAfterEye(g_ctx, eye, true);
    if (eye == 1) {   // compare the jobs' draw counts: pass 1 / left / right
        int n = g_jobDiagN[0]; if (g_jobDiagN[1] < n) n = g_jobDiagN[1]; if (g_jobDiagN[2] < n) n = g_jobDiagN[2];
        int bad = 0;
        for (int j = 0; j < n; ++j) {
            uint32_t a = g_jobDraws[0][j], l = g_jobDraws[1][j], r = g_jobDraws[2][j];
            if (a == l && a == r) continue;
            if (bad++ < 6) {
                static ULONGLONG lastLog = 0; static int burst = 0; ULONGLONG now = GetTickCount64();
                if (now - lastLog > 2000) { burst = 0; lastLog = now; }
                if (burst++ < 60) LOGI("job draw mismatch: record %llu job %d vt+%llX draws pass1 %u left %u right %u",
                                       (unsigned long long)g_hf.seq, j, (unsigned long long)g_jobVt[j], a, l, r);
            }
        }
        if (bad) s_jobMismatchFrames++;
        {
            std::lock_guard<std::mutex> lk(g_diagM);
            FrameDiag& f = g_diagRing[g_diagHead]; g_diagHead = (g_diagHead + 1) % 16;
            f.seq = g_hf.seq; f.n = n < 192 ? n : 192;
            for (int j = 0; j < f.n; ++j) { for (int p = 0; p < 3; ++p) f.draws[p][j] = uint16_t(g_jobDraws[p][j]); f.vt[j] = uint32_t(g_jobVt[j]); }
            memcpy(f.st, g_frStat, sizeof f.st);
            for (int e = 0; e < 2; ++e) { f.nUp[e] = g_upCurN[e]; memcpy(f.up[e], g_upCur[e], sizeof(UpLog) * g_upCurN[e]); }
            if (g_raw) {
                RawFrame& rf = g_raw[g_rawCur]; rf.seq = g_hf.seq; rf.diff = 0; rf.first = -1;
                const int m = rf.n[0] < rf.n[1] ? rf.n[0] : rf.n[1];
                constexpr int kRows = int(off::kViewBlockBytes / 16);
                memset(f.rowHist, 0, sizeof f.rowHist); f.nSmp = 0;
                for (int i = 0; i < m; ++i) {
                    const uint8_t* a = RawBlk(g_rawCur, 0, i); const uint8_t* b = RawBlk(g_rawCur, 1, i);
                    if (memcmp(a, b, off::kViewBlockBytes) == 0) continue;
                    if (rf.first < 0) rf.first = i; rf.diff++;
                    for (int r = 0; r < kRows; ++r) if (memcmp(a + r * 16, b + r * 16, 16)) f.rowHist[r]++;
                }
                // samples: differences in rows that rarely differ (<= 4 uploads this frame) - candidates for torn data
                for (int i = 0; i < m && f.nSmp < 8; ++i) {
                    const uint8_t* a = RawBlk(g_rawCur, 0, i); const uint8_t* b = RawBlk(g_rawCur, 1, i);
                    for (int r = 0; r < kRows && f.nSmp < 8; ++r)
                        if (f.rowHist[r] <= 4 && memcmp(a + r * 16, b + r * 16, 16)) {
                            auto& sm = f.smp[f.nSmp++]; sm.idx = i; sm.job = rf.job[0][i]; sm.row = r;
                            memcpy(sm.l, a + r * 16, 16); memcpy(sm.r, b + r * 16, 16);
                        }
                }
                // alignment: uploads present in one eye only (rows +0x40..+0x6F and +0x340..+0x36F differ normally)
                {
                    auto same = [&](int i, int j) {
                        const uint8_t* a = RawBlk(g_rawCur, 0, i); const uint8_t* b = RawBlk(g_rawCur, 1, j);
                        return memcmp(a, b, 0x40) == 0 && memcmp(a + 0x70, b + 0x70, 0x340 - 0x70) == 0 && memcmp(a + 0x370, b + 0x370, off::kViewBlockBytes - 0x370) == 0;
                    };
                    f.nEx = 0; f.unaligned = 0; f.maskDiff = 0; f.nMd = 0;
                    int i = 0, j = 0;
                    while (i < rf.n[0] && j < rf.n[1]) {
                        if (same(i, j)) {
                            const uint64_t ma = rf.dmask[0][i], mb = rf.dmask[1][j];
                            if (ma != mb) {
                                f.maskDiff++;
                                if (f.nMd < 4) {
                                    auto& md = f.md[f.nMd++]; md.i = i; md.j = j; md.job = rf.job[0][i]; md.ml = ma; md.mr = mb;
                                    const uint64_t x = ma ^ mb; int b = 0; while (b < 63 && !((x >> b) & 1)) ++b; md.off = b * 64;
                                    memcpy(md.l, RawBlk(g_rawCur, 0, i) + md.off, 64); memcpy(md.r, RawBlk(g_rawCur, 1, j) + md.off, 64);
                                }
                            }
                            ++i; ++j; continue;
                        }
                        int eye = -1;
                        for (int k = 1; k <= 3 && eye < 0; ++k) {
                            if (j + k < rf.n[1] && same(i, j + k)) eye = 1;        // right has k extra
                            else if (i + k < rf.n[0] && same(i + k, j)) eye = 0;   // left has k extra
                        }
                        if (eye < 0) { f.unaligned++; ++i; ++j; continue; }
                        const int x = eye ? j : i;
                        if (f.nEx < 4) {
                            auto& ex = f.ex[f.nEx++]; ex.eye = eye; ex.idx = x; ex.job = rf.job[eye][x]; ex.rows = 0; ex.vt = ex.job < kJobDiag ? uint32_t(g_jobVt[ex.job]) : 0;
                            {   // VP*W test of its +0 matrix against the centre / left / right IVP of this frame
                                const float* M = reinterpret_cast<const float*>(RawBlk(g_rawCur, eye, x));
                                const float* refs[3] = {g_center[5], g_eye[0][5], g_eye[1][5]};
                                for (int t = 0; t < 3; ++t) { const float* r3 = refs[t] + 12; for (int c = 0; c < 4; ++c) ex.xt[t][c] = r3[0] * M[c] + r3[1] * M[4 + c] + r3[2] * M[8 + c] + r3[3] * M[12 + c]; }
                                memcpy(ex.pm, g_center[4], 64);
                            }
                            const uint8_t* c = RawBlk(g_rawCur, eye, x); const uint8_t* pb = x > 0 ? RawBlk(g_rawCur, eye, x - 1) : c;
                            for (int r = 0; r < int(off::kViewBlockBytes / 16); ++r) if (memcmp(c + r * 16, pb + r * 16, 16)) {
                                if (ex.rows < 8) { ex.row[ex.rows] = int16_t(r * 16); memcpy(ex.v[ex.rows], c + r * 16, 16); memcpy(ex.pv[ex.rows], pb + r * 16, 16); }
                                ex.rows++;
                            }
                        }
                        if (eye) ++j; else ++i;
                    }
                }
                f.rawDiff = rf.diff; f.rawFirst = rf.first; f.rawN[0] = rf.n[0]; f.rawN[1] = rf.n[1];
            }
        }
        if (g_jobDiagN[0] != g_jobDiagN[1] || g_jobDiagN[1] != g_jobDiagN[2]) {
            static int logged = 0; if (logged++ < 20) LOGI("job count mismatch: record %llu pass1 %d left %d right %d", (unsigned long long)g_hf.seq, g_jobDiagN[0], g_jobDiagN[1], g_jobDiagN[2]);
        }
    }
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&qf);
    g_replayMs += double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(qf.QuadPart); g_replayN++;
    if (!CaptureBackBuffer(eye)) return false;
    if (eye == 1) {
        if (!g_config.hudLayer && g_hudBuilt) {   // HudLayer = 0: the HUD goes into both eye images (flat, at screen depth)
            for (int e = 0; e < 2; ++e) {
                D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = g_hudViewFmt; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                ID3D11RenderTargetView* v = nullptr;
                if (SUCCEEDED(g_dev->CreateRenderTargetView(g_eyeTex[e], &rd, &v)) && v) { So4HudComposite(g_ctx, v); v->Release(); }
            }
        }
        XrPublishStereo(g_eyeTex[0], g_eyeTex[1], g_hf, g_config.hudLayer ? So4HudTexture(g_hudBuilt) : nullptr);
        g_hudBuilt = false;
        s_stereo++;
        g_stereoThisFrame = true;
        if (g_config.debugDumpSeq > 0) {   // diagnostic: left-eye sequences in each replay-view state (does the image follow the head?)
            static uint64_t n = 0; static int done[2] = {}; static int phase[2] = {}; ++n;
            int st = g_frShift > g_fr0 ? 1 : 0;
            if (n > 900 && done[st] < g_config.debugDumpSeq && (phase[st]++ % 6) == 0) {
                const XrQuaternionf& q = g_hf.pose.orientation;
                float yaw = std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.y * q.y + q.x * q.x)) * 57.29578f;
                wchar_t nm[96]; swprintf(nm, 96, L"rofvr_seq_s%d_%02d.bmp", st, done[st]);
                DumpBmp(g_eyeTex[0], nm, false, 4);
                LOGI("seq dump s%d #%d: record %llu head yaw %.3f, replay views this %d shifted %d", st, done[st], (unsigned long long)g_hf.seq, yaw, g_fr0, g_frShift);
                done[st]++;
            }
        }
        if (g_traceFrame < 900) g_traceFrame++;
        if (g_dumpReq) {
            g_dumpReq = false;
            static int n = 0; ++n; wchar_t nm[64];
            swprintf(nm, 64, L"rofvr_eye_f11_%02d_left.bmp", n); DumpBmp(g_eyeTex[0], nm);
            swprintf(nm, 64, L"rofvr_eye_f11_%02d_right.bmp", n); DumpBmp(g_eyeTex[1], nm);
            if (ID3D11Texture2D* h = So4HudTexture(true)) { swprintf(nm, 64, L"rofvr_eye_f11_%02d_hud.bmp", n); DumpBmp(h, nm); }
        }
        if (g_config.stereoDiag >= 4) {   // diagnostic: left eye at head-yaw zero crossings (moving right / left)
            const XrQuaternionf& q = g_hf.pose.orientation;
            float yaw = std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.y * q.y + q.x * q.x)) * 57.29578f;
            static float prevYaw = 0; static int zc = 0; static uint64_t frames = 0;
            if (++frames > 600 && zc < 4 && ((prevYaw < 0) != (yaw < 0)) && std::fabs(yaw - prevYaw) < 10) {
                wchar_t nm[64]; swprintf(nm, 64, L"rofvr_eye_zc%d_%s_left.bmp", zc, yaw > prevYaw ? L"up" : L"down");
                DumpBmp(g_eyeTex[0], nm); LOGI("zero-crossing dump %d: yaw %.3f -> %.3f", zc, prevYaw, yaw); ++zc;
            }
            prevYaw = yaw;
        }
        static int dumped = 0; static uint64_t total = 0; ++total;   // s_stereo is reset by each stats report
        if (g_config.debugDumpEyesAt > 0 && total >= uint64_t(g_config.debugDumpEyesAt) && !dumped) {
            dumped = 1; DumpBmp(g_eyeTex[0], L"rofvr_eye_left.bmp"); DumpBmp(g_eyeTex[1], L"rofvr_eye_right.bmp");
            if (ID3D11Texture2D* h = So4HudTexture(true)) { DumpBmp(h, L"rofvr_eye_hud.bmp"); DumpBmp(h, L"rofvr_eye_hud_alpha.bmp", true); }
            if (ID3D11Texture2D* h = So4HudDebugTex(0)) DumpBmp(h, L"rofvr_eye_hud_black.bmp");
            if (ID3D11Texture2D* h = So4HudDebugTex(1)) DumpBmp(h, L"rofvr_eye_hud_white.bmp");
        }
    }
    return true;
}

// diagnostic (StereoDiag = 2): where the game keeps its render size (looking for 1280/720 pairs)
void ScanRes(uint32_t w, uint32_t h) {
    auto scan = [&](uintptr_t lo, uintptr_t hi, const char* what) {
        int n = 0;
        for (uintptr_t a = lo; a + 8 <= hi && n < 40; a += 4) {
            uint32_t x = *reinterpret_cast<uint32_t*>(a), y = *reinterpret_cast<uint32_t*>(a + 4);
            float fx = *reinterpret_cast<float*>(a), fy = *reinterpret_cast<float*>(a + 4);
            uint16_t sx = *reinterpret_cast<uint16_t*>(a), sy = *reinterpret_cast<uint16_t*>(a + 2);
            if ((x == w && y == h) || (fx == float(w) && fy == float(h))) { LOGI("res scan %s +0x%llX: %s pair", what, (unsigned long long)(a - lo), x == w ? "int" : "float"); ++n; }
            else if (sx == w && sy == h) { LOGI("res scan %s +0x%llX: u16 pair", what, (unsigned long long)(a - lo)); ++n; }
        }
    };
    uintptr_t r = *reinterpret_cast<uintptr_t*>(g_base + kRendererGlobal);
    if (r) scan(r, r + 0x281000, "renderer");
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t a = g_base; a < g_base + 0x5800000; a = uintptr_t(mbi.BaseAddress) + mbi.RegionSize) {
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD)) {
            char tag[64]; snprintf(tag, sizeof tag, "image@0x%llX", (unsigned long long)(uintptr_t(mbi.BaseAddress) - g_base));
            scan(uintptr_t(mbi.BaseAddress), uintptr_t(mbi.BaseAddress) + mbi.RegionSize, tag);
        }
    }
    LOGI("res scan done");
}

void __fastcall hk_FrameEnd(void* self) {
    if (g_inReplay) { o_FrameEnd(self); return; }
    int n = g_jobN; g_jobN = 0; s_frames++; g_pktUsed = 0; g_jobSeq = 0; g_p1bN = 0;
    for (int k = 0; k < kMaxJobs; ++k) g_p1JobCount[k] = 0;
    uintptr_t q = g_queue.load();
    int cur = q ? *reinterpret_cast<volatile int*>(q + kQRead) : -1;
    int prev = g_prevEnd; g_prevEnd = cur;
    bool overflow = g_overflow; g_overflow = false;
    if (g_config.stereoDiag && !overflow) LogFrame(n, prev, cur);
    if (g_tracing) {   // command order of the traced frame
        g_tracing = false;
        g_tracing = true; TraceJob(-1, self, "frame-end"); g_tracing = false;
        if (g_config.stereoDiag >= 2 && g_eyeTex[0]) { D3D11_TEXTURE2D_DESC d; g_eyeTex[0]->GetDesc(&d); ScanRes(d.Width, d.Height); }
        uintptr_t qq = g_queue.load();
        if (qq && prev >= 0) {
            char line[4096]; int len = snprintf(line, sizeof line, "trace commands:");
            int jobIdx = 0;
            for (int i = prev; len < 4000; ) {
                i = i + 1 < kQEntries ? i + 1 : 0;
                uint8_t t = *reinterpret_cast<uint8_t*>(qq + kQRing + uintptr_t(i) * kQEntry);
                if (t == 0) jobIdx++; else len += snprintf(line + len, sizeof line - len, " [t%d after job %d]", t, jobIdx - 1);
                if (i == cur) break;
            }
            LOGI("%s", line);
        }
    }
    if (g_config.stereoDiag && g_traceFrame == 900) { g_tracing = true; g_traceFrame++; }
    // diagnostic (StereoDiag): F11 = trace the next frame's jobs and dump its eye / HUD images
    if (g_config.stereoDiag) {
        static bool down = false;
        bool now = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (now && !down) { g_tracing = true; g_dumpReq = true; LOGI("F11: trace + dump requested"); }
        down = now;
    }
    {   // F12 (always): mark the log ("the problem is happening now") and log more detail for 3 s
        static bool down = false;
        bool now = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (now && !down) { g_markUntil = GetTickCount64() + 3000; LOGI("F12: user mark - detailed log for 3 s"); }
        down = now;
    }
    if (GetTickCount64() < g_markUntil) {
        const XrQuaternionf& q = g_hf.pose.orientation;
        float yaw = std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.y * q.y + q.x * q.x)) * 57.29578f;
        LOGI("mark frame: record %llu head yaw %.2f stereo %d | replay views: this record %d, shifted %d, other %d",
             (unsigned long long)g_hf.seq, yaw, g_stereoThisFrame ? 1 : 0, g_fr0, g_frShift, g_frOther);
    }
    g_fr0 = g_frShift = g_frOther = 0;
    if (g_config.stereoDiag >= 5) { if (g_stereoThisFrame) TallyScan(g_hf.seq); PrepareScanAround(g_hf.seq); }
    g_pass1Done = false; g_leftCaptured = false; g_combinerIdx = -1;
    g_combinerSeen = false; g_voteN = 0; g_jobDiagN[0] = 0; g_p1N = 0; memset(g_frStat, 0, sizeof g_frStat); g_upCurN[0] = g_upCurN[1] = 0;
    if (!g_raw && g_config.debugGlitchDetect) {
        g_rawBlk = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_t(kRawFrames) * 2 * kRawMax * off::kViewBlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        g_raw = g_rawBlk ? new RawFrame[kRawFrames]() : nullptr;
        LOGI("glitch diagnostics: raw per-view upload ring %s", g_raw ? "allocated" : "FAILED");
    }
    if (g_raw) { g_rawCur = (g_rawCur + 1) % kRawFrames; g_raw[g_rawCur].seq = 0; g_raw[g_rawCur].n[0] = g_raw[g_rawCur].n[1] = 0; g_raw[g_rawCur].diff = 0; g_raw[g_rawCur].first = -1; }
    SetPatchEye(-1);
    TemporalEndPass1();   // (no combiner this frame: stop collecting)
    TemporalBeginFrame(g_config.stereoBothReplay && g_config.activeMode == Config::Native && g_stereoThisFrame);
    g_guessed = false;
    // Start the next frame's left-eye patch right away with a guess (the head record after this frame's), so that
    // camera-dependent uploads made before the frame's own view upload are patched too (Calnus door: a light's
    // shadow pass comes first and the shadow receivers were left in the centre camera -> broken left-eye shadows).
    // The frame's view upload confirms or replaces the guess; an unconfirmed guess never makes a stereo frame.
    g_frameReady = false;
    if (g_stereoThisFrame && g_config.activeMode == Config::Native && !g_config.stereoBothReplay) {
        HeadFrame next;
        if (So4HtRecordAfter(g_hf.seq, next) && !IsTitleView(next.view)) {
            g_hf = next;
            BuildCenter(next, g_center);
            for (int e = 0; e < 2; ++e) BuildEye(next, g_center, e, g_eye[e]);
            SetPatchEye(0);
            g_guessed = true;
        }
    }
    g_stereoThisFrame = false;
    o_FrameEnd(self);
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

} // namespace

bool So4StereoInstall() {
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uint8_t* site = reinterpret_cast<uint8_t*>(g_base + kJobCallSite);
    if (memcmp(site, kJobCallBytes, 6) != 0) { LOGE("stereo: job call site bytes differ; not installed"); return false; }
    void** slot = static_cast<void**>(AllocNear(g_base + kJobCallSite, 4096));
    if (!slot) { LOGE("stereo: no memory near the job call site"); return false; }
    *slot = reinterpret_cast<void*>(&JobThunk);
    int64_t disp = reinterpret_cast<intptr_t>(slot) - reinterpret_cast<intptr_t>(site + 6);
    if (disp > INT32_MAX || disp < INT32_MIN) { LOGE("stereo: slot out of range"); return false; }
    uint8_t patch[6] = {0xFF, 0x15, 0, 0, 0, 0};
    int32_t d32 = int32_t(disp); memcpy(patch + 2, &d32, 4);
    DWORD old;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) { LOGE("stereo: VirtualProtect failed"); return false; }
    memcpy(site, patch, 6);
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    g_fakeQ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kQSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    bool ok = Hook(reinterpret_cast<void*>(g_base + kFnProcess), (void*)hk_Process, (void**)&o_Process, "SO4 render command processor");
    ok = Hook(reinterpret_cast<void*>(g_base + kFnFrameEnd), (void*)hk_FrameEnd, (void**)&o_FrameEnd, "SO4 frame end") && ok;
    if (g_config.debugGlitchDetect) {
        Hook(reinterpret_cast<void*>(g_base + 0x294FF0), (void*)hk_SetConstA, (void**)&o_SetConstA, "constant setter A (diagnostic)");
        Hook(reinterpret_cast<void*>(g_base + 0x295A90), (void*)hk_SetConstB, (void**)&o_SetConstB, "constant setter B (diagnostic)");
    }
    LOGI("stereo: job call site patched (slot %p) %s", (void*)slot, ok ? "" : "- hooks FAILED");
    return ok;
}

void So4StereoGateWait() {
    if (!g_gameTid.load(std::memory_order_relaxed)) g_gameTid = GetCurrentThreadId();
    if (!g_gate.load(std::memory_order_acquire) || GetCurrentThreadId() == g_renderTid) return;
    LARGE_INTEGER a, b, f; QueryPerformanceCounter(&a); QueryPerformanceFrequency(&f);
    s_gateWaits++;
    for (int i = 0; g_gate.load(std::memory_order_acquire); ++i) {
        QueryPerformanceCounter(&b);
        if ((b.QuadPart - a.QuadPart) * 1000 > f.QuadPart * 40) { s_gateTimeouts++; break; }   // never hold the game > 40 ms
        if (i < 64) YieldProcessor(); else Sleep(0);
    }
    QueryPerformanceCounter(&b); s_gateUs += (b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart;
}
bool So4StereoPatching() { return g_patching; }
ID3D11DeviceContext* So4StereoGameContext() { return g_ctx; }
bool So4StereoInReplay() { return g_inReplay; }
int So4StereoCurrentEye() { return (g_inReplay && g_patchEye >= 0 && g_patchEye < 2) ? g_patchEye : -1; }
bool So4StereoEyeTan(int e, float t[4]) {
    if (e < 0 || e > 1 || g_config.activeMode != Config::Native || g_hf.seq == 0) return false;
    const XrFovf& f = g_hf.eyeFov[e];
    t[0] = std::tan(f.angleLeft); t[1] = std::tan(f.angleRight); t[2] = std::tan(f.angleUp); t[3] = std::tan(f.angleDown);
    return true;
}

// Diagnostic (StereoDiag >= 5): which head record's view-projection do pass-1 uploads carry, relative to the record
// the frame was matched to through its per-view block? (exact VP copies, e.g. objects with an identity world)
// Candidates are prepared at the previous frame end (records prev-2 .. prev+6); hits are tallied at frame end
// relative to the frame's own record.
constexpr int kScanN = 9;
float g_scanVP[kScanN][16]; bool g_scanOk[kScanN] = {}; uint64_t g_scanSeq[kScanN] = {}; uint64_t g_scanCnt[kScanN] = {};
std::atomic<uint64_t> s_scanRel[9];
std::atomic<uint64_t> s_rvRel[7], s_rvOther{0}, s_rvNoHead{0};   // replay per-view views by record offset -3..+3, other camera   // relative offset -4..+4
void PrepareScanAround(uint64_t base) {
    for (int k = 0; k < kScanN; ++k) {
        HeadFrame r; g_scanOk[k] = false; g_scanCnt[k] = 0;
        int64_t sq = int64_t(base) + k - 2; g_scanSeq[k] = uint64_t(sq);
        if (sq > 0 && So4HtRecordBySeq(uint64_t(sq), r)) { vm::Mul4(r.proj, r.view, g_scanVP[k]); g_scanOk[k] = true; }
    }
}
void TallyScan(uint64_t frameSeq) {
    for (int k = 0; k < kScanN; ++k) {
        if (!g_scanCnt[k]) continue;
        int64_t rel = int64_t(g_scanSeq[k]) - int64_t(frameSeq);
        if (rel < -4) rel = -4; if (rel > 4) rel = 4;
        s_scanRel[rel + 4] += g_scanCnt[k];
    }
}
void So4StereoScanUpload(const void* data, unsigned bytes) {
    if (g_config.stereoDiag < 5 || g_inReplay || !data || bytes < 64) return;
    const float* d = static_cast<const float*>(data);
    const int nf = int(bytes / 4);
    for (int i = 0; i + 16 <= nf; i += 4)
        for (int k = 0; k < kScanN; ++k) {
            if (!g_scanOk[k]) continue;
            const float* v = g_scanVP[k];
            bool row = true, tr = true;
            for (int j = 0; j < 16 && (row || tr); ++j) {
                if (row && !Near(d[i + j], v[j])) row = false;
                if (tr && !Near(d[i + j], v[(j & 3) * 4 + (j >> 2)])) tr = false;
            }
            if (row || tr) { g_scanCnt[k]++; break; }
        }
}

// Render thread, before every constant-buffer upload (unpatched data): finds the frame's head record through the
// per-view upload and starts the left-eye patch for the rest of pass 1.
void So4StereoBeforeUpload(const void* data, unsigned bytes, unsigned bufferBytes, unsigned offset) {
    if (bufferBytes != off::kViewBlockBytes || !data) return;
    if (!g_inReplay && g_combinerIdx < 0 && g_config.replayRawFromPass1 && bytes == off::kViewBlockBytes && offset == 0 && g_config.activeMode == Config::Native) {
        if (!g_p1b) g_p1b = static_cast<P1Block*>(VirtualAlloc(nullptr, sizeof(P1Block) * kP1Blocks, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        const int j = g_jobSeq;
        if (g_p1b && g_p1bN < kP1Blocks && j > 0 && j < kMaxJobs) {
            if (g_p1JobCount[j] == 0) g_p1JobFirst[j] = g_p1bN;
            if (g_p1JobFirst[j] + g_p1JobCount[j] == g_p1bN) {   // contiguous per job
                g_p1b[g_p1bN].job = uint16_t(j); memcpy(g_p1b[g_p1bN].b, data, bytes); g_p1b[g_p1bN].h = P1Hash(g_p1b[g_p1bN].b); g_p1bN++; g_p1JobCount[j]++;
            }
        } else if (g_p1b) s_p1Overflow++;
    }
    So4HtOnViewUpload(data, bytes, offset);
    if (g_inReplay) {
        s_replayUploads++;
        // diagnostic: which camera does the replay's per-view block carry? (should be this frame's record)
        static const unsigned kPos[2] = {off::kViewPosA, off::kViewPosB};
        for (unsigned at : kPos) {
            if (offset > at || offset + bytes < at + 0x30) continue;
            const float* v = reinterpret_cast<const float*>(static_cast<const uint8_t*>(data) + (at - offset));
            if (v[0] == 0.f && v[1] == 0.f && v[2] == 0.f) continue;
            auto same = [&](const float* r) { for (int j = 0; j < 12; ++j) if (std::fabs(v[j] - r[j]) > 1e-5f * std::fmax(1.0f, std::fabs(v[j]))) return false; return true; };
            if (same(g_hf.view)) { s_rvRel[3]++; g_fr0++; break; }
            int rel = 99;
            for (int d = -3; d <= 3 && rel == 99; ++d) { HeadFrame r; if (d && So4HtRecordBySeq(g_hf.seq + d, r) && same(r.view)) rel = d; }
            if (rel != 99) { g_frShift++; s_rvRel[rel + 3]++; if (g_config.replayViewFix) { g_blockRel = rel; s_shiftBlocks++; } break; }
            {   // the game camera's own view without the head (some camera modes rebuild it): the scene would not follow the head
                int nh = 99;
                for (int d = -3; d <= 3 && nh == 99; ++d) { HeadFrame r; if (So4HtRecordBySeq(g_hf.seq + d, r) && same(r.viewNoHead)) nh = d; }
                if (nh != 99) { s_rvNoHead++; if (g_config.replayViewFix) { g_blockRel = nh; g_blockNoHead = true; } break; }
            }
            s_rvOther++; g_frOther++;
            {   // a camera view pass 1 never uploaded (e.g. a light whose data changed between pass 1 and this replay)
                bool seen = false;
                for (int q = 0; q < g_p1N && !seen; ++q) seen = memcmp(g_p1Views[q], v, 48) == 0;
                if (!seen && g_patchEye >= 0) {
                    s_otherNew++;
                    float bd = 1e30f; for (int q = 0; q < g_p1N; ++q) { float m = 0; for (int j = 0; j < 12; ++j) m = std::fmax(m, std::fabs(g_p1Views[q][j] - v[j])); if (m < bd) bd = m; }
                    static ULONGLONG lastL = 0; static int burstL = 0; ULONGLONG nowL = GetTickCount64();
                    if (nowL - lastL > 1000) { lastL = nowL; burstL = 0; }
                    if (burstL++ < 8) LOGI("replay view not in pass 1: record %llu eye %d @+0x%X row0 (%.4f %.4f %.4f %.1f) t (%.1f %.1f) nearest pass-1 view diff %.5f (pass-1 views %d)",
                                           (unsigned long long)g_hf.seq, g_patchEye, at, v[0], v[1], v[2], v[3], v[7], v[11], bd, g_p1N);
                }
            }
            {   // diagnostic: describe one unclassified view per second (nearest record by max element difference)
                static ULONGLONG last = 0; ULONGLONG now = GetTickCount64();
                const bool ui = v[0] > 0.9999f && v[5] > 0.9999f && v[10] > 0.9999f;   // unrotated 2D/UI cameras: not interesting
                if (!ui && now < g_markUntil && now - last > 100u) {
                    last = now;
                    float best = 1e30f; int bd = 0; float bnh = 1e30f;
                    for (int d = -12; d <= 12; ++d) { HeadFrame r; if (!So4HtRecordBySeq(g_hf.seq + d, r)) continue;
                        float m = 0, mn = 0; for (int j = 0; j < 12; ++j) { m = std::fmax(m, std::fabs(v[j] - r.view[j])); mn = std::fmax(mn, std::fabs(v[j] - r.viewNoHead[j])); }
                        if (m < best) { best = m; bd = d; } if (mn < bnh) bnh = mn; }
                    uint64_t cams[24]; float views[24 * 12]; int nc = So4HtCopyCamViews(cams, views, 24); int cm = -1;
                    for (int c = 0; c < nc; ++c) { bool eq = true; for (int j = 0; j < 12 && eq; ++j) eq = std::fabs(v[j] - views[c * 12 + j]) <= 1e-3f * std::fmax(1.0f, std::fabs(v[j])); if (eq) { cm = c; break; } }
                    {   // rotation / position difference to this frame's camera, and the nearest record in the whole history
                        const float* h = g_hf.view;
                        float tr = 0; for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) tr += v[a * 4 + b] * h[a * 4 + b];
                        float ang = std::acos(std::fmax(-1.0f, std::fmin(1.0f, (tr - 1) * 0.5f))) * 57.29578f;
                        float pv[3], ph[3];
                        for (int a = 0; a < 3; ++a) { pv[a] = -(v[a] * v[3] + v[4 + a] * v[7] + v[8 + a] * v[11]); ph[a] = -(h[a] * h[3] + h[4 + a] * h[7] + h[8 + a] * h[11]); }
                        float dp = std::sqrt((pv[0]-ph[0])*(pv[0]-ph[0]) + (pv[1]-ph[1])*(pv[1]-ph[1]) + (pv[2]-ph[2])*(pv[2]-ph[2]));
                        uint64_t ns = 0, nw = 0, od = 0; float nd = 0; bool nf = So4HtNearestRecord(v, ns, nd, nw, od);
                        LOGI("replay other view: vs this frame's camera %.2f deg, %.1f cm (pos %.0f %.0f %.0f) | nearest record in history %s%lld (max diff %.4f) | frame record %llu, history %llu..%llu",
                             ang, dp, pv[0], pv[1], pv[2], nf ? "" : "none ", nf ? (long long)(ns - g_hf.seq) : 0LL, nd,
                             (unsigned long long)g_hf.seq, (unsigned long long)od, (unsigned long long)nw);
                    }
                    LOGI("replay other view @+0x%X: row0 (%.4f %.4f %.4f %.1f) row2 (%.4f %.4f %.4f %.1f) | nearest record %+d max diff %.4f, nearest no-head %.4f, live camera %d%s",
                         at, v[0], v[1], v[2], v[3], v[8], v[9], v[10], v[11], bd, best, bnh, cm, cm >= 0 && cams[cm] == 1 ? " (record)" : "");
                }
            }
        }
        return;
    }
    if (g_config.activeMode != Config::Native || (!g_ctx && !EnsureDevice())) return;
    if (g_config.stereoBothReplay) {
        // Which head record is this frame? Every pass-1 per-view upload before the combiner votes; the record is picked
        // at the combiner (most votes among the recent ones). The first upload alone is not enough: some passes draw
        // with a camera the game stopped updating (after turning the camera around the player), which matched an old
        // record -> the frame was patched and published with a stale head pose -> the scene swayed with the head.
        if (g_combinerSeen) return;
        {   // diagnostic: every view this frame's pass 1 uploads (the replays should upload no others)
            static const unsigned kP[2] = {off::kViewPosA, off::kViewPosB};
            for (unsigned at : kP) {
                if (offset > at || offset + bytes < at + 0x30 || g_p1N >= kP1Max) continue;
                memcpy(g_p1Views[g_p1N++], static_cast<const uint8_t*>(data) + (at - offset), 48);
            }
        }
        HeadFrame m;
        if (!So4HtTakeUploadMatch(m)) { s_noRecord++; return; }
        if (IsTitleView(m.view)) return;
        static const unsigned kPos[2] = {off::kViewPosA, off::kViewPosB};
        for (unsigned at : kPos) {   // the exact uploaded view (the record matched within a tolerance)
            if (offset > at || offset + bytes < at + 0x30) continue;
            const float* v = reinterpret_cast<const float*>(static_cast<const uint8_t*>(data) + (at - offset));
            bool isNear = true;
            for (int j = 0; j < 12 && isNear; ++j) isNear = std::fabs(v[j] - m.view[j]) <= 1e-3f * std::fmax(1.0f, std::fabs(v[j]));
            if (isNear) { if (g_config.exactUploadView) memcpy(m.view, v, 48); break; }
        }
        int k = 0;
        for (; k < g_voteN; ++k) if (g_votes[k].hf.seq == m.seq) break;
        if (k == g_voteN) { if (g_voteN >= kMaxVotes) return; g_votes[g_voteN].hf = m; g_votes[g_voteN].n = 0; ++g_voteN; }
        g_votes[k].n++;
        g_frameReady = true;
        return;
    }
    if (g_patchEye >= 0 && !g_guessed) return;
    HeadFrame hf;
    if (!So4HtPeekFrame(hf)) { if (!g_guessed) s_noRecord++; return; }
    if (IsTitleView(hf.view)) return;
    if (g_guessed) {   // the frame's own view upload: was the guess right?
        g_guessed = false;
        if (hf.seq == g_hf.seq) { s_guessOk++; return; }
        s_guessBad++;
    }
    {   // use the exact view this frame uploads (the record matched within a tolerance), so exact copies are found
        static const unsigned kPos[2] = {off::kViewPosA, off::kViewPosB};
        for (unsigned at : kPos) {
            if (offset > at || offset + bytes < at + 0x30) continue;
            const float* v = reinterpret_cast<const float*>(static_cast<const uint8_t*>(data) + (at - offset));
            bool isNear = true;
            for (int j = 0; j < 12 && isNear; ++j) isNear = std::fabs(v[j] - hf.view[j]) <= 1e-3f * std::fmax(1.0f, std::fabs(v[j]));
            if (isNear) { if (g_config.exactUploadView) memcpy(hf.view, v, 48); break; }
        }
    }
    ApplyFrameRecord(hf);
    if (g_config.stereoBothReplay) { g_frameReady = true; return; }   // pass 1 stays the centre camera; both eyes are replays
    SetPatchEye(0);
}

namespace {
void ApplyFrameRecord(const HeadFrame& hf) {
    g_hf = hf;
    BuildCenter(hf, g_center);
    {   // the previous head record's matrices: some objects (e.g. the player character) are drawn with the camera
        // of one update earlier; exact copies of those are replaced with this frame's eye matrices as well, or the
        // character lags one frame behind the head (jitter when turning the head).
        HeadFrame pr; g_prevOk = So4HtRecordBySeq(hf.seq - 1, pr) && !ViewSame(pr.view, hf.view);
        if (g_prevOk) BuildCenter(pr, g_prevCenter);
    }
    for (int e = 0; e < 2; ++e) BuildEye(hf, g_center, e, g_eye[e]);
    static int logged = 0;
    if (logged < 2) {
        ++logged;
        LOGI("stereo: eye offsets L(%.2f %.2f %.2f) R(%.2f %.2f %.2f) cm; centre P m00 %.4f m11 %.4f m02 %.4f | L m00 %.4f m02 %.4f | R m00 %.4f m02 %.4f",
             hf.eyeOff[0][0], hf.eyeOff[0][1], hf.eyeOff[0][2], hf.eyeOff[1][0], hf.eyeOff[1][1], hf.eyeOff[1][2],
             g_center[2][0], g_center[2][5], g_center[2][2], g_eye[0][2][0], g_eye[0][2][2], g_eye[1][2][0], g_eye[1][2][2]);
    }
}
} // namespace

// Replaces the head camera's matrices in a constant upload with the current eye's.
std::atomic<int64_t> s_patchTicks{0};
void So4StereoPatchImpl(void* data, unsigned bytes);
void So4StereoPatch(void* data, unsigned bytes) {
    if (g_patchEye < 0 || !data || bytes < 64) return;
    LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
    So4StereoPatchImpl(data, bytes);
    QueryPerformanceCounter(&b); s_patchTicks += b.QuadPart - a.QuadPart;
}
void So4StereoPatchImpl(void* data, unsigned bytes) {
    FrStat(3); if (bytes == off::kViewBlockBytes) FrStat(4);
    if (g_config.replayRawFromPass1 && g_p1b && bytes == off::kViewBlockBytes && g_inReplay && g_jobPass > 0) {
        const int j = g_jobSeq;
        if (j > 0 && j < kMaxJobs && g_p1JobCount[j] > 0) {
            uint8_t* d8 = static_cast<uint8_t*>(data);
            int best = -1; float bestDist = 1e30f; const uint32_t hh = P1Hash(d8);
            for (int k = g_p1JobFirst[j], e = k + g_p1JobCount[j]; k < e; ++k) {
                const uint8_t* c = g_p1b[k].b;
                if (g_p1b[k].h != hh || memcmp(d8 + 0x70, c + 0x70, 0x340 - 0x70) || memcmp(d8 + 0x370, c + 0x370, off::kViewBlockBytes - 0x370)) continue;
                const float* a = reinterpret_cast<const float*>(d8); const float* b = reinterpret_cast<const float*>(c);
                float dist = 0; for (int q = 0; q < 28; ++q) dist += std::fabs(a[q] - b[q]);
                if (dist < bestDist) { bestDist = dist; best = k; if (dist == 0) break; }
            }
            if (best < 0 && g_config.replayPass1Nearest) {
                // other rows changed too (animated / per-frame values): the pass-1 block of the job whose object
                // matrices are clearly the closest (ratio test against the runner-up) is the same draw
                const float* a = reinterpret_cast<const float*>(d8);
                float n0 = 0; for (int q = 0; q < 28; ++q) n0 += std::fabs(a[q]);
                int b1 = -1; float d1 = 1e30f, d2 = 1e30f;
                for (int k = g_p1JobFirst[j], e = k + g_p1JobCount[j]; k < e; ++k) {
                    const float* b = reinterpret_cast<const float*>(g_p1b[k].b);
                    float dist = 0; for (int q = 0; q < 28 && dist < d2; ++q) dist += std::fabs(a[q] - b[q]);
                    if (dist < d1) { d2 = d1; d1 = dist; b1 = k; } else if (dist < d2) d2 = dist;
                }
                if (b1 >= 0 && d1 * 4.0f < d2 && d1 < 0.02f * n0) {
                    if (d1 > 0) { memcpy(d8, g_p1b[b1].b, 0x70); s_p1Nearest++; } else s_p1Exact++;
                    best = b1; bestDist = -1;
                }
            }
            if (best < 0) {   // no exact match of the other rows: the closest pass-1 block of the job, if it is clearly the same draw
                int bk = -1, bn = 1 << 30, ties = 0;
                for (int k = g_p1JobFirst[j], e = k + g_p1JobCount[j]; k < e; ++k) {
                    int n = 0;
                    for (int r = 0; r < int(off::kViewBlockBytes / 16) && n <= bn; ++r) { if (r >= 0x34 && r < 0x37) continue; n += memcmp(d8 + r * 16, g_p1b[k].b + r * 16, 16) != 0; }
                    if (n < bn) { bn = n; bk = k; ties = 0; } else if (n == bn) ties++;
                }
                if (bk >= 0 && bn <= g_config.replayPass1MaxRows && ties == 0) {
                    memcpy(d8, g_p1b[bk].b, 0x340); memcpy(d8 + 0x370, g_p1b[bk].b + 0x370, off::kViewBlockBytes - 0x370);
                    s_p1Partial++; best = bk; bestDist = -1;
                }
            }
            if (best < 0) {
                s_p1NoMatch++;
                if ((s_umSamples++ & 63) == 0) {   // diagnostic: rows that differ from the closest pass-1 block of the job
                    int bk = -1, bn = 1 << 30;
                    for (int k = g_p1JobFirst[j], e = k + g_p1JobCount[j]; k < e; ++k) {
                        int n = 0; for (int r = 0; r < int(off::kViewBlockBytes / 16); ++r) n += memcmp(d8 + r * 16, g_p1b[k].b + r * 16, 16) != 0;
                        if (n < bn) { bn = n; bk = k; }
                    }
                    if (bk >= 0) for (int r = 0; r < int(off::kViewBlockBytes / 16); ++r) if (memcmp(d8 + r * 16, g_p1b[bk].b + r * 16, 16)) g_umRows[r]++;
                }
            }
            else if (bestDist < 0) {}
            else if (bestDist == 0) s_p1Exact++;
            else { memcpy(d8, g_p1b[best].b, 0x70); s_p1Fixed++; }
        }
    }
    if (bytes == off::kViewBlockBytes && g_inReplay && g_patchEye < 2 && g_config.replayDupFix) {
        // The renderer sometimes uploads the same per-view block a second time within one job of a replay, with
        // half-updated contents (the game thread is already writing the next frame's camera): that copy lit the
        // scene with a torn camera -> one-frame black shadow areas in one eye. Keep the job's first, consistent copy.
        static uint8_t lastRaw[2][off::kViewBlockBytes]; static int lastJob[2] = {-1, -1}; static int lastPassFrame[2] = {}; static int passFrame = 0;
        const int e = g_patchEye; const int job = g_jobDiagN[g_jobPass];
        const float* v = static_cast<const float*>(data) + off::kViewPosMain / 4;
        const float* lv = reinterpret_cast<const float*>(lastRaw[e]) + off::kViewPosMain / 4;
        const bool sameJob = lastJob[e] == job && lastPassFrame[e] == g_upCurN[e];
        if (sameJob && memcmp(v, lv, 48) == 0 && (v[0] != 0.f || v[1] != 0.f || v[2] != 0.f) && memcmp(data, lastRaw[e], off::kViewBlockBytes) != 0) {
            memcpy(data, lastRaw[e], off::kViewBlockBytes); s_dupFixed++;
        } else memcpy(lastRaw[e], data, off::kViewBlockBytes);
        lastJob[e] = job; lastPassFrame[e] = g_upCurN[e] + 1; (void)passFrame;
    }
    if (g_raw && bytes == off::kViewBlockBytes && g_inReplay && g_patchEye >= 0 && g_patchEye < 2) {
        RawFrame& rf = g_raw[g_rawCur]; const int e = g_patchEye;
        if (rf.n[e] < kRawMax) { rf.job[e][rf.n[e]] = uint16_t(g_jobDiagN[g_jobPass]); memcpy(RawBlk(g_rawCur, e, rf.n[e]), data, bytes); rf.dmask[e][rf.n[e]] = 0; g_curDmask = &rf.dmask[e][rf.n[e]]; rf.n[e]++; }
    }
    struct DmaskReset { ~DmaskReset() { g_curDmask = nullptr; } } dmaskReset;
    UpLog* ul = nullptr; uint64_t deltaBefore = s_patchDelta.load();
    if (bytes == off::kViewBlockBytes && g_patchEye < 2 && g_upCurN[g_patchEye] < kUpMax) {
        ul = &g_upCur[g_patchEye][g_upCurN[g_patchEye]++];
        const float* v = static_cast<const float*>(data) + 0xC0 / 4;
        uint32_t h = 2166136261u; for (int j = 0; j < 12 * 4; ++j) h = (h ^ reinterpret_cast<const uint8_t*>(v)[j]) * 16777619u;
        ul->job = uint16_t(g_jobDiagN[g_jobPass]); ul->rel = int8_t(g_blockRel); ul->hash = h; ul->deltas = 0;
        ul->r0[0] = v[0]; ul->r0[1] = v[1]; ul->r0[2] = v[2]; ul->r0[3] = v[3]; ul->t[0] = v[7]; ul->t[1] = v[11];
        uint32_t fh = 2166136261u; for (unsigned j = 0; j < bytes; ++j) fh = (fh ^ static_cast<const uint8_t*>(data)[j]) * 16777619u;
        ul->full = fh; memset(ul->doff, 0xFF, sizeof ul->doff);
        memcpy(ul->head, data, sizeof ul->head);
    }
    g_curUl = ul;
    struct DeltaNote { UpLog* u; uint64_t b; ~DeltaNote() { if (u) u->deltas = uint8_t(s_patchDelta.load() - b); g_curUl = nullptr; } } deltaNote{ul, deltaBefore};
    float* d = static_cast<float*>(data);
    const int nf = int(bytes / 4);
    const float (*o)[16] = g_center; const float (*nw)[16] = g_eye[g_patchEye];
    // per-view block built from another head record (see GetRec): its record (and the one before, the block's
    // "previous" camera) map to this frame's record (and the one before)
    const RecMats* mapSrc[2] = {}; const RecMats* mapDst[2] = {};
    const int rel = g_blockRel; const bool noHead = g_blockNoHead; g_blockRel = 0; g_blockNoHead = false;
    if (rel || noHead) for (int k = 0; k < 2; ++k) { mapSrc[k] = GetRec(g_hf.seq + rel - k); mapDst[k] = GetRec(g_hf.seq - k); if (!mapSrc[k] || !mapDst[k]) mapSrc[k] = nullptr; }
    auto tryMap = [&](float* x, const float (*src)[16], const float (*dst)[16]) -> bool {
        for (int m = 0; m < 6; ++m) {
            const float* q = src[m];
            if (Near(x[0], q[0]) && Near(x[1], q[1]) && Near(x[2], q[2]) && Near(x[3], q[3])) {
                bool all = true; for (int k = 4; k < 16 && all; ++k) all = Near(x[k], q[k]);
                if (all) { memcpy(x, dst[m], 64); return true; }
            }
            if (Near(x[0], q[0]) && Near(x[1], q[4]) && Near(x[2], q[8]) && Near(x[3], q[12])) {
                bool all = true; for (int k = 0; k < 16 && all; ++k) all = Near(x[k], q[(k & 3) * 4 + (k >> 2)]);
                if (all) { for (int k = 0; k < 16; ++k) x[k] = dst[m][(k & 3) * 4 + (k >> 2)]; return true; }
            }
        }
        return false;
    };
    // affine-delta reference: this frame's centre, or for a shifted block the block's own record
    const float* ivpRef = o[5]; const float* deltaRef = g_delta; float shiftDelta[16];
    auto srcMats = [&](int k) -> const float (*)[16] { return noHead ? mapSrc[k]->nh : mapSrc[k]->c; };
    if (mapSrc[0]) { vm::Mul4(mapDst[0]->e[g_patchEye][4], srcMats(0)[5], shiftDelta); ivpRef = srcMats(0)[5]; deltaRef = shiftDelta; }
    // HLSL constant buffers keep float4x4 on 16-byte boundaries, so (PatchAligned = 1) only those positions are scanned.
    const int step = g_config.patchAligned ? 4 : 1, skip = 16 - step;
    for (int i = 0; i + 16 <= nf; i += step) {
        for (int k = 0; k < 2; ++k)
            if (mapSrc[k] && tryMap(d + i, srcMats(k), mapDst[k]->e[g_patchEye])) { s_shiftPatched++; FrStat(0); i += skip; goto next; }
        // exact copies of the camera matrices, row-major or transposed
        for (int m = 0; m < 6; ++m) {
            if (Near(d[i], o[m][0]) && Near(d[i + 1], o[m][1]) && Near(d[i + 2], o[m][2]) && Near(d[i + 3], o[m][3])) {
                bool all = true; for (int k = 4; k < 16 && all; ++k) all = Near(d[i + k], o[m][k]);
                if (all) { memcpy(d + i, nw[m], 64); s_patchMats++; FrStat(1); i += skip; goto next; }
            }
            if (Near(d[i], o[m][0]) && Near(d[i + 1], o[m][4]) && Near(d[i + 2], o[m][8]) && Near(d[i + 3], o[m][12])) {
                bool all = true; for (int k = 0; k < 16 && all; ++k) all = Near(d[i + k], o[m][(k & 3) * 4 + (k >> 2)]);
                if (all) { for (int k = 0; k < 16; ++k) d[i + k] = nw[m][(k & 3) * 4 + (k >> 2)]; s_patchMats++; FrStat(1); i += skip; goto next; }
            }
        }
        if (g_prevOk && g_config.patchPrevCamera)
            for (int m = 0; m < 6; ++m) {
                const float* q = g_prevCenter[m];
                if (Near(d[i], q[0]) && Near(d[i + 1], q[1]) && Near(d[i + 2], q[2]) && Near(d[i + 3], q[3])) {
                    bool all = true; for (int k = 4; k < 16 && all; ++k) all = Near(d[i + k], q[k]);
                    if (all) { memcpy(d + i, nw[m], 64); s_patchPrev++; i += skip; goto next; }
                }
                if (Near(d[i], q[0]) && Near(d[i + 1], q[4]) && Near(d[i + 2], q[8]) && Near(d[i + 3], q[12])) {
                    bool all = true; for (int k = 0; k < 16 && all; ++k) all = Near(d[i + k], q[(k & 3) * 4 + (k >> 2)]);
                    if (all) { for (int k = 0; k < 16; ++k) d[i + k] = nw[m][(k & 3) * 4 + (k >> 2)]; s_patchPrev++; i += skip; goto next; }
                }
            }
        // clip-from-object matrices (VP * World, previous-frame variants): inverse(VP) * M is affine
        if ((i & 3) == 0) {
            const float* r3 = &ivpRef[12];
            const float* M = d + i;
            float x[4];
            for (int c = 0; c < 4; ++c) x[c] = r3[0] * M[c] + r3[1] * M[4 + c] + r3[2] * M[8 + c] + r3[3] * M[12 + c];
            const bool strict = std::fabs(x[0]) < 1e-3f && std::fabs(x[1]) < 1e-3f && std::fabs(x[2]) < 1e-3f && std::fabs(x[3] - 1.0f) < 1e-3f;
            if (strict) {
                bool finite = true; for (int k = 0; k < 16; ++k) if (!std::isfinite(M[k])) { finite = false; break; }
                bool nonzero = std::fabs(M[0]) + std::fabs(M[5]) + std::fabs(M[10]) > 1e-6f;
                // an orthographic clip matrix (w row = 0 0 0 1: a directional light's shadow cascade) is never the camera's
                const bool ortho = std::fabs(M[12]) + std::fabs(M[13]) + std::fabs(M[14]) < 1e-7f && std::fabs(M[15] - 1.0f) < 1e-4f;
                if (finite && nonzero && ortho) {
                    s_deltaOrtho++;
                    static ULONGLONG last = 0; ULONGLONG now = GetTickCount64();
                    if (now - last > 200) { last = now; LOGI("delta patch on an orthographic matrix: record %llu eye %d %s (row0 %.6f %.6f %.6f %.3f)",
                        (unsigned long long)g_hf.seq, g_patchEye, g_config.deltaSkipOrtho ? "skipped" : "PATCHED", M[0], M[1], M[2], M[3]); }
                    if (g_config.deltaSkipOrtho) goto next;
                }
                if (finite && nonzero && !g_config.debugNoDelta) { float out[16]; vm::Mul4(deltaRef, M, out); memcpy(d + i, out, 64); s_patchDelta++; FrStat(2); if (g_curDmask) *g_curDmask |= 1ull << ((i * 4 / 64) & 63); if (g_curUl) { for (auto& o : g_curUl->doff) if (o == 0xFFFF) { o = uint16_t(i * 4); break; } } i += skip; }
            } else if (g_config.replayCrossEyeFix && !mapSrc[0] && (g_patchEye == 0 || g_patchEye == 1)) {
                // Already patched for an eye? (tests 034-038: the game's raw data sometimes carries a matrix we
                // patched earlier - e.g. the other eye's - and the centre test above misses it). Each eye's own
                // IVP decides; another eye's matrix is converted to this eye.
                for (int oe = 0; oe < 2; ++oe) {
                    const float* r3e = &g_eye[oe][5][12];
                    float y[4];
                    for (int c = 0; c < 4; ++c) y[c] = r3e[0] * M[c] + r3e[1] * M[4 + c] + r3e[2] * M[8 + c] + r3e[3] * M[12 + c];
                    if (std::fabs(y[0]) < 1e-3f && std::fabs(y[1]) < 1e-3f && std::fabs(y[2]) < 1e-3f && std::fabs(y[3] - 1.0f) < 1e-3f) {
                        if (oe == g_patchEye) { s_ownEye++; }
                        else {
                            float dlt[16], out[16]; vm::Mul4(g_eye[g_patchEye][4], g_eye[oe][5], dlt); vm::Mul4(dlt, M, out);
                            bool fin = true; for (int k = 0; k < 16; ++k) if (!std::isfinite(out[k])) { fin = false; break; }
                            if (fin) { memcpy(d + i, out, 64); s_crossEye++; i += skip; }
                        }
                        break;
                    }
                }
            }
        }
    next:;
    }
}

void So4StereoReport(char* buf, unsigned size) {
    double ms = g_replayN ? g_replayMs / double(g_replayN) : 0; g_replayMs = 0; g_replayN = 0;
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    const uint64_t nst = s_stereo.load();
    const double patchMs = nst ? double(s_patchTicks.exchange(0)) * 1000.0 / double(qf.QuadPart) / double(nst) : 0.0;
    snprintf(buf, size, "SO4 stereo: %llu stereo frames (replay %.2f ms cpu, patching %.2f ms per stereo frame, %llu commands), %llu frames, %llu jobs | patched %llu matrices + %llu VP*W, no record %llu, replay view uploads %llu",
             (unsigned long long)s_stereo.exchange(0), ms, patchMs, (unsigned long long)s_replayCmds.exchange(0), (unsigned long long)s_frames.exchange(0), (unsigned long long)s_jobs.exchange(0),
             (unsigned long long)s_patchMats.exchange(0), (unsigned long long)s_patchDelta.exchange(0), (unsigned long long)s_noRecord.exchange(0),
             (unsigned long long)s_replayUploads.exchange(0));
    size_t k = strlen(buf);
    if (k + 1 < size) { snprintf(buf + k, size - k, ", previous-camera copies %llu, motion blur skipped %llu, early patch guess ok %llu / wrong %llu / unconfirmed %llu", (unsigned long long)s_patchPrev.exchange(0), (unsigned long long)s_mbSkipped.exchange(0),
        (unsigned long long)s_guessOk.exchange(0), (unsigned long long)s_guessBad.exchange(0), (unsigned long long)s_guessUnconfirmed.exchange(0)); k = strlen(buf); }
    if (k + 1 < size) { snprintf(buf + k, size - k, ", replay views by record -3..+3: %llu %llu %llu [%llu] %llu %llu %llu, other %llu, no-head views %llu; shifted blocks %llu (matrices %llu); frames with several candidate records %llu, picked other than first %llu; torn per-view uploads replaced %llu",
        (unsigned long long)s_rvRel[0].exchange(0), (unsigned long long)s_rvRel[1].exchange(0), (unsigned long long)s_rvRel[2].exchange(0), (unsigned long long)s_rvRel[3].exchange(0),
        (unsigned long long)s_rvRel[4].exchange(0), (unsigned long long)s_rvRel[5].exchange(0), (unsigned long long)s_rvRel[6].exchange(0), (unsigned long long)s_rvOther.exchange(0), (unsigned long long)s_rvNoHead.exchange(0),
        (unsigned long long)s_shiftBlocks.exchange(0), (unsigned long long)s_shiftPatched.exchange(0),
        (unsigned long long)s_voteMulti.exchange(0), (unsigned long long)s_votePickNotFirst.exchange(0), (unsigned long long)s_dupFixed.exchange(0)); k = strlen(buf); s_jobMismatchFrames = 0; s_deltaOrtho = 0; s_otherNew = 0; }
    if (g_config.stereoDiag >= 5 && k + 1 < size) {
        snprintf(buf + k, size - k, ", exact VP copies by record offset -4..+4: %llu %llu %llu %llu [%llu] %llu %llu %llu %llu",
                 (unsigned long long)s_scanRel[0].exchange(0), (unsigned long long)s_scanRel[1].exchange(0), (unsigned long long)s_scanRel[2].exchange(0),
                 (unsigned long long)s_scanRel[3].exchange(0), (unsigned long long)s_scanRel[4].exchange(0), (unsigned long long)s_scanRel[5].exchange(0),
                 (unsigned long long)s_scanRel[6].exchange(0), (unsigned long long)s_scanRel[7].exchange(0), (unsigned long long)s_scanRel[8].exchange(0));
        k = strlen(buf);
    }
    if (k + 2 < size) { buf[k++] = '\n'; buf[k] = 0; So4HudReport(buf + k, unsigned(size - k));
        k = strlen(buf); snprintf(buf + k, size - k, ", no UI end %llu, no target %llu | pass-1 object matrices: %llu replaced, %llu whole blocks, %llu nearest, %llu exact, %llu unmatched, overflow %llu | camera VP changed mid-replay L %llu R %llu (of %llu checks) | game thread suspended %llu | camera gate: %llu waits (%.1f ms total), %llu timeouts | eye-patched raw matrices: other eye %llu, own eye %llu | delta tolerance: %llu only with the relative test, %llu near misses | packets: %llu replayed jobs, %llu changed since pass 1 (%llu KB), overflow %llu", (unsigned long long)s_hudNoEnd.exchange(0), (unsigned long long)s_hudNoRt.exchange(0), (unsigned long long)s_p1Fixed.exchange(0), (unsigned long long)s_p1Partial.exchange(0), (unsigned long long)s_p1Nearest.exchange(0), (unsigned long long)s_p1Exact.exchange(0), (unsigned long long)s_p1NoMatch.exchange(0), (unsigned long long)s_p1Overflow.exchange(0), (unsigned long long)s_camChanged[0].exchange(0), (unsigned long long)s_camChanged[1].exchange(0), (unsigned long long)s_camChecked.exchange(0), (unsigned long long)s_suspends.exchange(0),
                 (unsigned long long)s_gateWaits.exchange(0), double(s_gateUs.exchange(0)) / 1000.0, (unsigned long long)s_gateTimeouts.exchange(0), (unsigned long long)s_crossEye.exchange(0), (unsigned long long)s_ownEye.exchange(0), (unsigned long long)s_deltaLooseOnly.exchange(0), (unsigned long long)s_deltaNearMiss.exchange(0), (unsigned long long)s_pktJobs.exchange(0), (unsigned long long)s_pktChanged.exchange(0), (unsigned long long)(s_pktBytesChanged.exchange(0) >> 10), (unsigned long long)s_pktOverflow.exchange(0)); }
    if (g_config.replayRawFromPass1) { size_t k2 = strlen(buf); int w = snprintf(buf + k2, size - k2, "\n  unmatched replay blocks, rows differing from pass 1:");
        for (int r = 0; r < 140 && w > 0; ++r) if (g_umRows[r]) { k2 = strlen(buf); if (k2 + 16 < size) w = snprintf(buf + k2, size - k2, " +%X:%u", r * 16, g_umRows[r]); g_umRows[r] = 0; } }
    { char mb[256]; CtxMapReport(mb, sizeof mb); size_t k2 = strlen(buf); if (k2 + 4 < size) snprintf(buf + k2, size - k2, "\n  %s", mb); }
}

static void RawDump(uint64_t seq) {
    int fi = -1; for (int k = 0; k < kRawFrames; ++k) if (g_raw[k].seq == seq) fi = k;
    if (fi < 0) { LOGI("    raw: frame not in the ring"); return; }
    const RawFrame& rf = g_raw[fi]; const int m = rf.n[0] < rf.n[1] ? rf.n[0] : rf.n[1];
    int shown = 0;
    for (int i = 0; i < m && shown < 6; ++i) {
        const float* a = reinterpret_cast<const float*>(RawBlk(fi, 0, i)); const float* b = reinterpret_cast<const float*>(RawBlk(fi, 1, i));
        if (memcmp(a, b, off::kViewBlockBytes) == 0) continue;
        ++shown;
        int rows = 0; char ln[2048]; int len = snprintf(ln, sizeof ln, "    raw #%d job L %u R %u differs at", i, rf.job[0][i], rf.job[1][i]);
        for (int r = 0; r < int(off::kViewBlockBytes / 16); ++r) if (memcmp(a + r * 4, b + r * 4, 16)) { if (rows < 40) len += snprintf(ln + len, sizeof ln - len, " +%X", r * 16); rows++; }
        LOGI("%s (%d rows)", ln, rows);
        int pr = 0;
        for (int r = 0; r < int(off::kViewBlockBytes / 16) && pr < 10; ++r) if (memcmp(a + r * 4, b + r * 4, 16)) {
            ++pr; LOGI("      +%03X L (%.5g %.5g %.5g %.5g) R (%.5g %.5g %.5g %.5g)", r * 16, a[r*4], a[r*4+1], a[r*4+2], a[r*4+3], b[r*4], b[r*4+1], b[r*4+2], b[r*4+3]);
        }
    }
    if (rf.n[0] != rf.n[1]) {   // unequal counts: where do the job sequences diverge?
        int i = 0; while (i < m && rf.job[0][i] == rf.job[1][i]) ++i;
        LOGI("    raw upload counts differ (L %d R %d): job sequence diverges at #%d (L job %d, R job %d)", rf.n[0], rf.n[1], i,
             i < rf.n[0] ? rf.job[0][i] : -1, i < rf.n[1] ? rf.job[1][i] : -1);
    }
}
void So4StereoDumpDiag(uint64_t seq, int eye, float score) {
    std::lock_guard<std::mutex> lk(g_diagM);
    LOGI("GLITCH: one-frame %s-eye anomaly at record %llu (score %.1f) - per-frame diagnostics:", eye ? "right" : "left", (unsigned long long)seq, score);
    for (int k = 0; k < 16; ++k) {
        const FrameDiag& f = g_diagRing[k];
        if (!f.seq || f.seq + 7 < seq || f.seq > seq + 4) continue;
        char line[4096]; int len = snprintf(line, sizeof line, "  %s record %llu: L shift %u exact %u delta %u uploads %u views %u | R shift %u exact %u delta %u uploads %u views %u | draws p/L/R:",
            f.seq == seq ? ">>" : "  ", (unsigned long long)f.seq, f.st[0][0], f.st[0][1], f.st[0][2], f.st[0][3], f.st[0][4], f.st[1][0], f.st[1][1], f.st[1][2], f.st[1][3], f.st[1][4]);
        LOGI("  %s record %llu: raw per-view uploads L %d R %d, %d differ between the eyes (first #%d)", f.seq == seq ? ">>" : "  ", (unsigned long long)f.seq, f.rawN[0], f.rawN[1], f.rawDiff, f.rawFirst);
        {
            char hl[2048]; int n = 0;
            for (int r = 0; r < int(off::kViewBlockBytes / 16) && n < 1900; ++r) if (f.rowHist[r]) n += snprintf(hl + n, sizeof hl - n, " +%X:%u", r * 16, f.rowHist[r]);
            LOGI("     rows differing (offset:uploads):%s", n ? hl : " none");
            LOGI("     extra uploads %d, unaligned %d, delta-patch decisions differing between the eyes %d", f.nEx, f.unaligned, f.maskDiff);
            for (int k = 0; k < f.nMd; ++k) { const auto& md = f.md[k];
                LOGI("     DELTA MISMATCH L #%d R #%d job %d +%03X: masks L %llX R %llX", md.i, md.j, md.job, md.off, (unsigned long long)md.ml, (unsigned long long)md.mr);
                for (int r = 0; r < 4; ++r) LOGI("       L (%.5g %.5g %.5g %.5g)  R (%.5g %.5g %.5g %.5g)", md.l[r*4], md.l[r*4+1], md.l[r*4+2], md.l[r*4+3], md.r[r*4], md.r[r*4+1], md.r[r*4+2], md.r[r*4+3]); }
            for (int k = 0; k < f.nEx; ++k) { const auto& ex = f.ex[k];
                LOGI("     EXTRA in %s eye: #%d job %d (vt+%X), %d rows differ from the previous upload", ex.eye ? "right" : "left", ex.idx, ex.job, ex.vt, ex.rows);
                LOGI("       +0 VP*W test x: centre (%.4f %.4f %.4f %.4f) L (%.4f %.4f %.4f %.4f) R (%.4f %.4f %.4f %.4f) | centre VP row0 (%.4f %.4f %.4f %.3f) row3 (%.4f %.4f %.4f %.3f)",
                     ex.xt[0][0], ex.xt[0][1], ex.xt[0][2], ex.xt[0][3], ex.xt[1][0], ex.xt[1][1], ex.xt[1][2], ex.xt[1][3], ex.xt[2][0], ex.xt[2][1], ex.xt[2][2], ex.xt[2][3],
                     ex.pm[0], ex.pm[1], ex.pm[2], ex.pm[3], ex.pm[12], ex.pm[13], ex.pm[14], ex.pm[15]);
                for (int r = 0; r < ex.rows && r < 8; ++r) LOGI("       +%03X now (%.5g %.5g %.5g %.5g) before (%.5g %.5g %.5g %.5g)", ex.row[r], ex.v[r][0], ex.v[r][1], ex.v[r][2], ex.v[r][3], ex.pv[r][0], ex.pv[r][1], ex.pv[r][2], ex.pv[r][3]); }
            for (int k = 0; k < f.nSmp && f.seq != seq; ++k) { const auto& sm = f.smp[k];
                LOGI("     rare #%d job %d +%03X L (%.5g %.5g %.5g %.5g) R (%.5g %.5g %.5g %.5g)", sm.idx, sm.job, sm.row * 16, sm.l[0], sm.l[1], sm.l[2], sm.l[3], sm.r[0], sm.r[1], sm.r[2], sm.r[3]); }
        }
        for (int j = 0; j < f.n && len < 4000; ++j) len += snprintf(line + len, sizeof line - len, " %d:%X=%u/%u/%u", j, f.vt[j] & 0xFFFFF, f.draws[0][j], f.draws[1][j], f.draws[2][j]);
        LOGI("%s", line);
        if (f.seq == seq) {   // where do the two eyes' per-view upload sequences differ?
            int a = 0;
            const int n0 = f.nUp[0], n1 = f.nUp[1];
            while (a < n0 && a < n1 && f.up[0][a].hash == f.up[1][a].hash) ++a;
            LOGI("  per-view uploads: left %d, right %d, first difference at #%d", n0, n1, a);
            for (int e = 0; e < 2; ++e)
                for (int q = a - 2; q <= a + 3; ++q) {
                    if (q < 0 || q >= f.nUp[e]) continue;
                    const UpLog& u = f.up[e][q];
                    LOGI("    %s #%d job %u rel %d deltas %u at +%X +%X +%X view row0 (%.4f %.4f %.4f %.1f) t (%.1f %.1f) hash %08X block %08X", e ? "R" : "L", q, u.job, u.rel, u.deltas,
                         u.doff[0], u.doff[1], u.doff[2], u.r0[0], u.r0[1], u.r0[2], u.r0[3], u.t[0], u.t[1], u.hash, u.full);
                    if (q >= a - 1 && q <= a) {
                        char hb[1024]; int hl = 0;
                        for (int k = 0; k < 48 && hl < 1000; ++k) hl += snprintf(hb + hl, sizeof hb - hl, "%s%.5g", k % 16 == 0 ? " |" : " ", u.head[k]);
                        LOGI("      first 48 floats:%s", hb);
                    }
                }
        }
    }
}
} // namespace so4vr
