#include <windows.h>
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstring>
#include <cstdio>
#include "head_tracking.h"
#include "native_stereo.h"
#include "xr_runtime.h"
#include "game_offsets.h"
#include "safe_mem.h"
#include "vrmath.h"
#include "config.h"
#include "log.h"
#include "hook_util.h"
#include "stack_trace.h"

namespace so4vr {
namespace {

// ComputeView (RVA 0x183B88): view = inverse(world @ +0x30), sets +0x110 bit2 (view valid).
// UpdateCamera rebuilds the world matrix from the camera's local transform/parent before calling
// it, which overwrote any world matrix set earlier; so the head pose is applied inside this hook.
using FnComputeView = void (__fastcall*)(void* cam);
constexpr uintptr_t kFnComputeView = 0x183B88;
FnComputeView o_ComputeView = nullptr;

// Render-side frame advance: after it runs, the ViewInfo read slot is the one the renderer is
// about to draw with. Capturing its view here identifies the rendered frame exactly.
using FnAdvance = void (__fastcall*)();
FnAdvance o_Advance = nullptr;
std::mutex g_advMutex;
float g_advView[16];
bool  g_advHave = false;
std::atomic<uint64_t> s_advances{0};
std::atomic<DWORD> s_advThread{0};
constexpr uintptr_t kLensFocal = 0x18, kLensZoom = 0x28, kLensFilmMM = 0x44;

// State saved in pre-Prepare and restored in post-Prepare (main thread only).
struct Saved {
    bool     active = false;
    uintptr_t cam = 0, lens = 0;
    float    headM[16];       // head transform in camera-local space (game units/axes)
    bool     applied = false; // head pose written into the world matrix this frame
    float    base[16];        // the game's own world matrix that the head pose was applied to
    float    eye[16];         // the world matrix we wrote
    float    lensFocal = 0, aspect6F4 = 0;
    float    shift[2]{};
    HeadFrame frame{};
} g_saved;

// Recent frame records (main thread -> render thread). Kept as a history ring rather than a
// queue so a frame can still be matched if the threads drift relative to each other.
constexpr int kHistory = 8;
std::mutex g_qMutex;
HeadFrame g_hist[kHistory];
bool g_histValid[kHistory] = {};
int g_histNext = 0;
uint64_t g_seq = 0;

// Stats
std::atomic<uint64_t> s_applied{0}, s_skipped{0}, s_matched{0}, s_fifo{0}, s_missing{0};
std::atomic<uint64_t> s_viewHooked{0}, s_viewNotReached{0};
std::atomic<uint64_t> s_matchVI{0}, s_matchCam{0}, s_viDiffersFromCam{0};
// Self-check: does the snapshot the renderer reads contain exactly the head-posed camera we built?
std::atomic<uint64_t> s_selfCheckOk{0}, s_selfCheckBad{0};
std::atomic<int> s_selfCheckWorstMilliDeg{0}, s_selfCheckWorstMm{0};
// Timing: render-side advance vs. Present (tells whether the advance marks a frame's start or end).
std::atomic<int64_t> g_lastAdvanceQpc{0};
// Feedback check: does the game's own camera (base) turn when only the head turns?
std::mutex g_fbMutex;
struct Feedback { double sumAbsBase = 0, sumAbsHead = 0, sumBH = 0, sumHH = 0; int n = 0; } g_fb;
float g_prevBaseYaw = 0, g_prevHeadYaw = 0;
bool  g_havePrevYaw = false;
float WrapPi(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }
std::atomic<int64_t> s_advToPresentUsSum{0};
std::atomic<uint64_t> s_advToPresentN{0};

// The head-posed world matrix last written, and the game matrix it was built from. If the camera
// is not rebuilt by the game before the next frame (non-attached camera), the next frame must
// start again from the game matrix, not from our previous result.
bool  g_haveLast = false;
uintptr_t g_lastCam = 0;
float g_lastEye[16], g_lastBase[16];
std::atomic<int> s_lastSkipReason{0};
const char* kSkipReasons[] = {"none", "no XR pose", "camera vtable mismatch", "camera read failed", "lens read failed",
                              "ortho camera", "title/menu default camera"};

// Recentre: yaw + position reference captured on request (F9), applied to every head pose.
std::atomic<bool> g_recenterRequested{false};
bool g_haveRef = false;
float g_refYaw = 0;          // radians, XR space
XrVector3f g_refPos{0, 0, 0};

float YawOf(const XrQuaternionf& q) {
    // Heading of the -Z (forward) axis in XR space, around +Y.
    float fx = -2.0f * (q.x * q.z + q.w * q.y);
    float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    return std::atan2(-fx, -fz);
}

// Applies the recentre reference: rotate by -refYaw around Y and subtract the reference position.
XrPosef ApplyRecenter(const XrPosef& p) {
    if (!g_haveRef) return p;
    float c = std::cos(-g_refYaw * 0.5f), s = std::sin(-g_refYaw * 0.5f); // quaternion for -yaw about Y: (0,s,0,c)
    XrPosef o;
    const XrQuaternionf& q = p.orientation;
    o.orientation = {c * q.x + s * q.z, c * q.y + s * q.w, c * q.z - s * q.x, c * q.w - s * q.y};
    float dx = p.position.x - g_refPos.x, dy = p.position.y - g_refPos.y, dz = p.position.z - g_refPos.z;
    float cy = std::cos(-g_refYaw), sy = std::sin(-g_refYaw);
    o.position = {cy * dx + sy * dz, dy, -sy * dx + cy * dz};
    return o;
}

float Tan(float a) { return std::tan(a); }

void PushFrame(const HeadFrame& f) {
    std::lock_guard<std::mutex> lk(g_qMutex);
    g_hist[g_histNext] = f;
    g_histValid[g_histNext] = true;
    g_histNext = (g_histNext + 1) % kHistory;
}

bool ViewsEqual(const float* a, const float* b) {
    for (int i = 0; i < 16; ++i) if (std::fabs(a[i] - b[i]) > 1e-3f * (1.0f + std::fabs(a[i]))) return false;
    return true;
}

void Skip(int reason) { s_skipped++; s_lastSkipReason.store(reason); }

} // namespace

namespace {
// If a previous frame left a head-posed matrix in a camera and this frame is not head-tracked,
// put the game's matrix back (and force the view to be rebuilt from it).
void RestoreStaleCamera() {
    if (!g_haveLast) return;
    g_haveLast = false;
    float cur[16];
    if (!SafeRead(cur, (void*)(g_lastCam + off::kCamWorld), 64) || memcmp(cur, g_lastEye, 64) != 0) return;
    SafeWrite((void*)(g_lastCam + off::kCamWorld), g_lastBase, 64);
    uint8_t f110 = 0, f7c2 = 0;
    if (SafeLoad(g_lastCam + off::kCamFlags110, f110)) { f110 &= ~0x04; SafeWrite((void*)(g_lastCam + off::kCamFlags110), &f110, 1); }
    if (SafeLoad(g_lastCam + off::kCamFlags7C2, f7c2)) { f7c2 |= 0x20; SafeWrite((void*)(g_lastCam + off::kCamFlags7C2), &f7c2, 1); }
}
bool PrePrepareImpl(uintptr_t cam);
} // namespace

bool HeadTrackPrePrepare(uintptr_t cam) {
    if (PrePrepareImpl(cam)) return true;
    RestoreStaleCamera();
    return false;
}

namespace {
bool PrePrepareImpl(uintptr_t cam) {
    g_saved.active = false;
    if (!HeadTrackedMode() || !cam) return false;

    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t vt = 0;
    if (!SafeLoad(cam, vt) || vt - base != off::kVtAskaCamera) { Skip(2); return false; }
    uint8_t f7c2 = 0;
    if (!SafeLoad(cam + off::kCamFlags7C2, f7c2)) { Skip(3); return false; }
    if (f7c2 & 0x01) { Skip(5); return false; } // orthographic: leave alone
    {
        // The title/menu camera is a default camera parked at the origin with an identity matrix.
        float w[16];
        if (!SafeRead(w, (void*)(cam + off::kCamWorld), 64)) { Skip(3); return false; }
        static const float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        if (memcmp(w, I, sizeof(I)) == 0) { Skip(6); return false; }
    }

    // Head pose + per-eye FOV predicted for when this frame reaches the display.
    XrView views[2];
    if (!XrLocateEyesForGameFrame(views)) { Skip(1); return false; }

    if (!o_ComputeView) { Skip(3); return false; }

    Saved s;
    s.cam = cam;
    if (!SafeLoad(cam + off::kCamLens, s.lens) || !s.lens ||
        !SafeLoad(cam + off::kCamAspect6F4, s.aspect6F4) ||
        !SafeRead(s.shift, (void*)(cam + off::kCamLensShift), 8)) { Skip(3); return false; }
    float zoom = 0, filmMM = 0, filmAspect = 0, filmW = 0, filmH = 0;
    if (!SafeLoad(s.lens + kLensFocal, s.lensFocal) || !SafeLoad(s.lens + kLensZoom, zoom) ||
        !SafeLoad(s.lens + kLensFilmMM, filmMM) || !SafeLoad(cam + off::kCamFilmAspect, filmAspect) ||
        !SafeLoad(cam + off::kCamFilmW, filmW) || !SafeLoad(cam + off::kCamFilmH, filmH) ||
        zoom == 0 || filmMM == 0 || filmAspect == 0 || filmW == 0 || filmH == 0) { Skip(4); return false; }

    // ---- head pose: centre between the eyes, orientation of the left eye (no canting assumed) ----
    XrPosef head;
    head.orientation = views[0].pose.orientation;
    head.position = {(views[0].pose.position.x + views[1].pose.position.x) * 0.5f,
                     (views[0].pose.position.y + views[1].pose.position.y) * 0.5f,
                     (views[0].pose.position.z + views[1].pose.position.z) * 0.5f};
    if (g_recenterRequested.exchange(false)) {
        g_refYaw = YawOf(head.orientation);
        g_refPos = head.position;
        g_haveRef = true;
        LOGI("view recentred (yaw %.1f deg, pos %.2f %.2f %.2f m)", g_refYaw * 57.29578f, g_refPos.x, g_refPos.y, g_refPos.z);
    }
    // The game camera is driven by the recentred pose; the layer keeps the raw pose so the
    // compositor reprojects correctly.
    const XrPosef layerPose = head;
    head = ApplyRecenter(head);
    // Union of both eyes' FOV, so one image covers both eyes.
    XrFovf fov;
    fov.angleLeft  = std::fmin(views[0].fov.angleLeft, views[1].fov.angleLeft);
    fov.angleRight = std::fmax(views[0].fov.angleRight, views[1].fov.angleRight);
    fov.angleUp    = std::fmax(views[0].fov.angleUp, views[1].fov.angleUp);
    fov.angleDown  = std::fmin(views[0].fov.angleDown, views[1].fov.angleDown);

    // ---- camera world = (levelled) game camera x head transform ----
    const float cmPerMetre = 100.0f * g_config.worldScale;
    vm::Quat q = vm::XrToGameRot({head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w});
    vm::Vec3 t = vm::XrToGamePos({head.position.x, head.position.y, head.position.z}, cmPerMetre);
    float r[9];
    vm::QuatToMat3(q, r);
    vm::MakeTransform(r, t, s.headM); // combined with the game's world matrix in hk_ComputeView

    // ---- projection via the lens parameters (keeps culling and depth reconstruction consistent) ----
    const float tl = Tan(fov.angleLeft), tr = Tan(fov.angleRight), tu = Tan(fov.angleUp), td = Tan(fov.angleDown);
    const float hw = (tr - tl) * 0.5f, cx = (tr + tl) * 0.5f;   // horizontal half-width and centre (tangent space)
    const float hh = (tu - td) * 0.5f, cy = (tu + td) * 0.5f;   // vertical
    const float F = 1.0f / hw;                                   // desired m00
    // UpdateCamera: m00 = zoom * focal / (filmMM * 0.5)  ->  focal = F * filmMM * 0.5 / zoom
    const float newFocal = F * filmMM * 0.5f / zoom;
    // m11 = m00 * aspect6F4 * filmAspect, and we want m11 = 1/hh
    const float newAspect = (hw / hh) / filmAspect;
    // m02 = -2*shiftX/filmW must equal -cx/hw (positive shift = frustum moves right; verified)
    float newShift[2] = {(cx / hw) * filmW * 0.5f, (cy / hh) * filmH * 0.5f * g_config.lensShiftYSign};

    // Apply the lens; force the view to be recomputed (that is where the head pose goes in).
    SafeWrite((void*)(s.lens + kLensFocal), &newFocal, 4);
    SafeWrite((void*)(cam + off::kCamAspect6F4), &newAspect, 4);
    SafeWrite((void*)(cam + off::kCamLensShift), newShift, 8);
    f7c2 |= 0x22; // projection dirty + view dirty
    SafeWrite((void*)(cam + off::kCamFlags7C2), &f7c2, 1);
    uint8_t f110 = 0;
    if (SafeLoad(cam + off::kCamFlags110, f110)) { f110 &= ~0x04; SafeWrite((void*)(cam + off::kCamFlags110), &f110, 1); }

    s.frame.pose = layerPose;
    s.frame.fov = fov;
    {
        // Per-eye data relative to the (raw) head pose, converted to the game's view space.
        vm::Quat qh{layerPose.orientation.x, layerPose.orientation.y, layerPose.orientation.z, layerPose.orientation.w};
        vm::Quat qhInv = vm::QConj(qh);
        for (int e = 0; e < 2; ++e) {
            s.frame.eyePose[e] = views[e].pose;
            s.frame.eyeFov[e] = views[e].fov;
            vm::Vec3 d{views[e].pose.position.x - layerPose.position.x, views[e].pose.position.y - layerPose.position.y,
                       views[e].pose.position.z - layerPose.position.z};
            vm::Vec3 local = vm::QRotate(qhInv, d);                        // XR head frame (metres)
            vm::Vec3 g = vm::XrToGamePos(local, cmPerMetre);              // game view space (cm)
            s.frame.eyeOff[e][0] = g.x; s.frame.eyeOff[e][1] = g.y; s.frame.eyeOff[e][2] = g.z;
            vm::Quat qe{views[e].pose.orientation.x, views[e].pose.orientation.y, views[e].pose.orientation.z, views[e].pose.orientation.w};
            vm::Quat rel = vm::XrToGameRot(vm::QMul(qhInv, qe));
            s.frame.eyeRot[e * 4 + 0] = rel.x; s.frame.eyeRot[e * 4 + 1] = rel.y; s.frame.eyeRot[e * 4 + 2] = rel.z; s.frame.eyeRot[e * 4 + 3] = rel.w;
        }
    }
    s.active = true;
    g_saved = s;
    s_applied++;
    return true;
}

} // namespace

void HeadTrackPostPrepare(uintptr_t cam) {
    if (!g_saved.active) return;
    g_saved.active = false;
    if (cam != g_saved.cam) cam = g_saved.cam; // restore the object we modified

    // The view the renderer will use for this frame (for render-thread matching).
    SafeRead(g_saved.frame.view, (void*)(cam + off::kCamView), 64);
    {
        // The snapshot Prepare wrote for this frame: the renderer will read exactly these bytes.
        uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        uintptr_t ring = 0; int64_t widx = -1;
        g_saved.frame.haveVI = SafeLoad(base + off::kViewInfoRing, ring) && ring &&
            SafeLoad(base + off::kViewInfoWriteIdx, widx) && widx >= 0 && widx <= 1 &&
            SafeRead(g_saved.frame.viewVI, (void*)(ring + 0x10 + uintptr_t(widx) * off::kViewInfoStride + off::kViView), 64);
        if (g_saved.frame.haveVI && memcmp(g_saved.frame.viewVI, g_saved.frame.view, 64) != 0) s_viDiffersFromCam++;
    }
    if (g_saved.frame.haveVI && g_saved.applied) {
        // Expected view = inverse(eye world) (rigid transform: R^T, -R^T t).
        const float* w = g_saved.eye;
        float inv[16] = {w[0], w[4], w[8], 0, w[1], w[5], w[9], 0, w[2], w[6], w[10], 0, 0, 0, 0, 1};
        inv[3]  = -(w[0] * w[3] + w[4] * w[7] + w[8] * w[11]);
        inv[7]  = -(w[1] * w[3] + w[5] * w[7] + w[9] * w[11]);
        inv[11] = -(w[2] * w[3] + w[6] * w[7] + w[10] * w[11]);
        const float* v = g_saved.frame.viewVI;
        // Rotation error: angle of R_expected^T * R_actual; translation error in camera units (cm).
        float tr = 0;
        for (int i = 0; i < 3; ++i) for (int k = 0; k < 3; ++k) tr += inv[i * 4 + k] * v[i * 4 + k];
        float c = (tr - 1.0f) * 0.5f; c = c > 1 ? 1 : (c < -1 ? -1 : c);
        int mdeg = int(std::acos(c) * 57295.78f);
        float dx = inv[3] - v[3], dy = inv[7] - v[7], dz = inv[11] - v[11];
        int mm = int(std::sqrt(dx * dx + dy * dy + dz * dz) * 10.0f);
        if (mdeg > 100 || mm > 10) s_selfCheckBad++; else s_selfCheckOk++;
        if (mdeg > s_selfCheckWorstMilliDeg.load()) s_selfCheckWorstMilliDeg = mdeg;
        if (mm > s_selfCheckWorstMm.load()) s_selfCheckWorstMm = mm;
    }
    g_saved.frame.seq = ++g_seq;
    PushFrame(g_saved.frame);

    // Restore so game logic only ever sees its own camera.
    SafeWrite((void*)(g_saved.lens + kLensFocal), &g_saved.lensFocal, 4);
    SafeWrite((void*)(cam + off::kCamAspect6F4), &g_saved.aspect6F4, 4);
    SafeWrite((void*)(cam + off::kCamLensShift), g_saved.shift, 8);
    if (g_saved.applied && !g_config.restoreCameraAfterPrepare) {
        // Leave the head-posed camera in place until the next frame: the render thread draws this
        // frame later and some passes read the live camera rather than the snapshot.
        s_viewHooked++;
        memcpy(g_lastEye, g_saved.eye, 64);
        memcpy(g_lastBase, g_saved.base, 64);
        g_lastCam = cam;
        g_haveLast = true;
    } else if (g_saved.applied) {
        s_viewHooked++;
        g_haveLast = false;
        SafeWrite((void*)(cam + off::kCamWorld), g_saved.base, 64);
        // Invalidate the (head-posed) view so it is rebuilt from the game's matrix next time.
        uint8_t f110 = 0, f7c2 = 0;
        if (SafeLoad(cam + off::kCamFlags110, f110)) { f110 &= ~0x04; SafeWrite((void*)(cam + off::kCamFlags110), &f110, 1); }
        if (SafeLoad(cam + off::kCamFlags7C2, f7c2)) { f7c2 |= 0x20; SafeWrite((void*)(cam + off::kCamFlags7C2), &f7c2, 1); }
    } else {
        s_viewNotReached++;
    }
}

namespace {
void __fastcall hk_ComputeView(void* camPtr) {
    uintptr_t cam = reinterpret_cast<uintptr_t>(camPtr);
    if (g_saved.active && cam == g_saved.cam) LogStackOnce("ComputeView(main camera, head-tracked)", 1);
    if (g_saved.active && cam == g_saved.cam) {
        float cur[16];
        if (SafeRead(cur, camPtr == nullptr ? nullptr : (void*)(cam + off::kCamWorld), 64) &&
            !(g_saved.applied && memcmp(cur, g_saved.eye, 64) == 0)) {
            // `cur` is the game's freshly built world matrix: put the head on top of it. If it is
            // still last frame's head-posed matrix (camera not rebuilt), use last frame's base.
            if (g_haveLast && cam == g_lastCam && memcmp(cur, g_lastEye, 64) == 0) memcpy(cur, g_lastBase, 64);
            float baseM[16];
            if (g_config.horizonLock) vm::LevelCamera(cur, baseM); else memcpy(baseM, cur, 64);
            memcpy(g_saved.base, cur, 64);
            vm::Mul4(baseM, g_saved.headM, g_saved.eye);
            SafeWrite((void*)(cam + off::kCamWorld), g_saved.eye, 64);
            g_saved.applied = true;
            {
                // Yaw of the game camera (forward column) and of the head transform, per frame.
                float baseYaw = std::atan2(cur[2], cur[10]);
                float headYaw = std::atan2(g_saved.headM[2], g_saved.headM[10]);
                if (g_havePrevYaw) {
                    float dB = WrapPi(baseYaw - g_prevBaseYaw), dH = WrapPi(headYaw - g_prevHeadYaw);
                    std::lock_guard<std::mutex> lk(g_fbMutex);
                    g_fb.sumAbsBase += std::fabs(dB); g_fb.sumAbsHead += std::fabs(dH);
                    g_fb.sumBH += double(dB) * dH; g_fb.sumHH += double(dH) * dH; g_fb.n++;
                }
                g_prevBaseYaw = baseYaw; g_prevHeadYaw = headYaw; g_havePrevYaw = true;
            }
        }
    }
    o_ComputeView(camPtr);
}

// Diagnostic: rotate the render-side view (ViewInfo read slot) by DebugRenderViewYawDeg, to see which
// parts of the picture follow ViewInfo (needed for native stereo, which swaps ViewInfo per eye).
void ApplyRenderViewYaw(uintptr_t vi, float deg) {
    float* V = reinterpret_cast<float*>(vi + 0x000);
    float* IV = reinterpret_cast<float*>(vi + 0x040);
    float* P = reinterpret_cast<float*>(vi + 0x080);
    float* IP = reinterpret_cast<float*>(vi + 0x0C0);
    float* VP = reinterpret_cast<float*>(vi + 0x100);
    float* IVP = reinterpret_cast<float*>(vi + 0x140);
    float a = deg * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
    // Yaw about view-space Y (left-handed, +Z forward): x' = c x + s z, z' = -s x + c z.
    const float R[16] = { c, 0, s, 0,  0, 1, 0, 0,  -s, 0, c, 0,  0, 0, 0, 1 };
    const float Rt[16] = { c, 0, -s, 0,  0, 1, 0, 0,  s, 0, c, 0,  0, 0, 0, 1 };
    float nV[16], nIV[16], nVP[16], nIVP[16];
    vm::Mul4(R, V, nV);
    vm::Mul4(IV, Rt, nIV);
    vm::Mul4(P, nV, nVP);
    vm::Mul4(nIV, IP, nIVP);
    memcpy(V, nV, 64); memcpy(IV, nIV, 64); memcpy(VP, nVP, 64); memcpy(IVP, nIVP, 64);
}

void DumpViewInfoOnce(uintptr_t vi) {
    static int dumped = 0;
    if (dumped >= 1 || s_advances.load() < 900) return;
    static float f[0x530 / 4];
    if (!SafeRead(f, reinterpret_cast<void*>(vi), 0x530)) return;
    if (f[0] == 1.0f && f[5] == 1.0f && f[10] == 1.0f && f[3] == 0.0f && f[7] == 0.0f) return; // title/menu identity camera
    ++dumped;
    LOGI("ViewInfo dump (read slot, 0x530 bytes as floats, 16 per line):");
    for (int i = 0; i < 0x530 / 4; i += 16) {
        char line[512]; int n = snprintf(line, sizeof(line), "  +%03X:", i * 4);
        for (int j = 0; j < 16 && i + j < 0x530 / 4; ++j) n += snprintf(line + n, sizeof(line) - n, " %.4g", f[i + j]);
        LOGI("%s", line);
    }
}

uintptr_t ReadSlotViewInfo() {
    uintptr_t b0 = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t r0 = 0; int64_t i0 = -1;
    if (SafeLoad(b0 + off::kViewInfoRing, r0) && r0 && SafeLoad(b0 + off::kViewInfoReadIdx, i0) && i0 >= 0 && i0 <= 1)
        return r0 + 0x10 + uintptr_t(i0) * off::kViewInfoStride;
    return 0;
}

void __fastcall hk_Advance() {
    // End of the frame being drawn: its ViewInfo is still the read slot.
    NativeEndOfFrame(ReadSlotViewInfo());
    o_Advance();
    s_advances++;
    if (s_advances.load() > 600) LogStackOnce("AdvanceRenderView", 2);
    { LARGE_INTEGER q; QueryPerformanceCounter(&q); g_lastAdvanceQpc.store(q.QuadPart); }
    s_advThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t ring = 0; int64_t idx = -1;
    float v[16];
    if (SafeLoad(base + off::kViewInfoRing, ring) && ring && SafeLoad(base + off::kViewInfoReadIdx, idx) &&
        idx >= 0 && idx <= 1 && SafeRead(v, (void*)(ring + 0x10 + uintptr_t(idx) * off::kViewInfoStride + off::kViView), 64)) {
        std::lock_guard<std::mutex> lk(g_advMutex);
        memcpy(g_advView, v, 64);
        g_advHave = true;
    }
    {
        uintptr_t b0 = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        uintptr_t r0 = 0; int64_t i0 = -1;
        if (SafeLoad(b0 + off::kViewInfoRing, r0) && r0 && SafeLoad(b0 + off::kViewInfoReadIdx, i0) && i0 >= 0 && i0 <= 1) {
            uintptr_t vi = r0 + 0x10 + uintptr_t(i0) * off::kViewInfoStride;
            if (g_config.debugStacks && HeadTrackedMode()) DumpViewInfoOnce(vi);
            if (g_config.debugRenderViewYawDeg != 0.0f && HeadTrackedMode()) ApplyRenderViewYaw(vi, g_config.debugRenderViewYawDeg);
            NativeAfterAdvance(vi);
        }
    }
}
} // namespace

bool HeadTrackInstall() {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    bool a = Hook(reinterpret_cast<void*>(base + kFnComputeView), (void*)&hk_ComputeView, (void**)&o_ComputeView,
                  "ComputeView (camera view matrix)");
    bool b = Hook(reinterpret_cast<void*>(base + off::kFnAdvanceRenderView), (void*)&hk_Advance, (void**)&o_Advance,
                  "AdvanceRenderView (render frame start)");
    return a && b;
}

bool HeadTrackTakeRenderView(float out[16]) {
    std::lock_guard<std::mutex> lk(g_advMutex);
    if (!g_advHave) return false;
    memcpy(out, g_advView, 64);
    g_advHave = false;
    return true;
}

bool HeadTrackRecenterRef(float& yaw, XrVector3f& pos) {
    if (!g_haveRef) return false;
    yaw = g_refYaw; pos = g_refPos;
    return true;
}

bool HeadTrackFindFrame(const float* renderedView, HeadFrame& out) {
    if (!renderedView) return false;
    std::lock_guard<std::mutex> lk(g_qMutex);
    for (int k = 1; k <= kHistory; ++k) {
        int i = (g_histNext - k + kHistory) % kHistory;
        if (g_histValid[i] && g_hist[i].haveVI && ViewsEqual(g_hist[i].viewVI, renderedView)) { out = g_hist[i]; return true; }
    }
    for (int k = 1; k <= kHistory; ++k) {
        int i = (g_histNext - k + kHistory) % kHistory;
        if (g_histValid[i] && ViewsEqual(g_hist[i].view, renderedView)) { out = g_hist[i]; return true; }
    }
    return false;
}

bool HeadTrackTakeFrame(const float* renderedView, HeadFrame& out) {
    std::lock_guard<std::mutex> lk(g_qMutex);
    static HeadFrame last;
    static bool haveLast = false;
    static uint64_t lastSeq = 0;
    int best = -1;
    bool viaVI = false;
    if (renderedView) {
        // Exact match on the snapshot bytes (preferred), then on the camera view; newest first.
        for (int k = 1; k <= kHistory && best < 0; ++k) {
            int i = (g_histNext - k + kHistory) % kHistory;
            if (g_histValid[i] && g_hist[i].haveVI && ViewsEqual(g_hist[i].viewVI, renderedView)) { best = i; viaVI = true; }
        }
        for (int k = 1; k <= kHistory && best < 0; ++k) {
            int i = (g_histNext - k + kHistory) % kHistory;
            if (g_histValid[i] && ViewsEqual(g_hist[i].view, renderedView)) best = i;
        }
    }
    if (best >= 0) {
        s_matched++;
        if (viaVI) s_matchVI++; else s_matchCam++;
    } else {
        // Fallback: the record following the last one used, else the newest.
        for (int k = kHistory; k >= 1 && best < 0; --k) {
            int i = (g_histNext - k + kHistory) % kHistory;
            if (g_histValid[i] && g_hist[i].seq > lastSeq) best = i;
        }
        if (best < 0) {
            s_missing++;
            if (!haveLast) return false;
            out = last;
            return true;
        }
        s_fifo++;
    }
    if (g_config.frameMatchOffset != 0) {
        // Diagnostic/correction: pair the image with a neighbouring frame's pose.
        uint64_t want = g_hist[best].seq + uint64_t(int64_t(g_config.frameMatchOffset));
        for (int i = 0; i < kHistory; ++i)
            if (g_histValid[i] && g_hist[i].seq == want) { best = i; break; }
    }
    out = g_hist[best];
    lastSeq = out.seq;
    last = out;
    haveLast = true;
    return true;
}

void HeadTrackRequestRecenter() { g_recenterRequested.store(true); }

void HeadTrackNotePresent() {
    int64_t a = g_lastAdvanceQpc.load();
    if (!a) return;
    LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
    s_advToPresentUsSum += (q.QuadPart - a) * 1000000 / f.QuadPart;
    s_advToPresentN++;
}

void HeadTrackReport(char* buf, size_t size) {
    int reason = s_lastSkipReason.load();
    uint64_t n = s_advToPresentN.exchange(0);
    int64_t us = s_advToPresentUsSum.exchange(0);
    char extra[512];
    Feedback fb;
    { std::lock_guard<std::mutex> lk(g_fbMutex); fb = g_fb; g_fb = {}; }
    char fbs[200] = "";
    if (fb.n > 0)
        snprintf(fbs, sizeof(fbs), " | yaw: head turned %.1f deg, game camera turned %.1f deg, game-camera-per-head-degree %.2f",
                 fb.sumAbsHead * 57.29578, fb.sumAbsBase * 57.29578, fb.sumHH > 1e-9 ? fb.sumBH / fb.sumHH : 0.0);
    snprintf(extra, sizeof(extra), "%s | self-check ok %llu bad %llu (worst %.3f deg, %.1f mm) | advance->present avg %.2f ms",
             fbs, (unsigned long long)s_selfCheckOk.exchange(0), (unsigned long long)s_selfCheckBad.exchange(0),
             s_selfCheckWorstMilliDeg.exchange(0) / 1000.0, s_selfCheckWorstMm.exchange(0) / 1.0,
             n ? double(us) / n / 1000.0 : -1.0);
    (void)0;
    snprintf(buf, size, "render advances %llu (thread %lu) | head tracking: applied %llu (view hook hit %llu, not reached %llu), skipped %llu (last: %s) | frames matched %llu (snapshot %llu, camera %llu), fallback %llu, missing %llu | snapshot!=camera %llu",
             (unsigned long long)s_advances.exchange(0), s_advThread.load(),
             (unsigned long long)s_applied.exchange(0), (unsigned long long)s_viewHooked.exchange(0),
             (unsigned long long)s_viewNotReached.exchange(0), (unsigned long long)s_skipped.exchange(0),
             kSkipReasons[reason < 7 ? reason : 0], (unsigned long long)s_matched.exchange(0), (unsigned long long)s_matchVI.exchange(0), (unsigned long long)s_matchCam.exchange(0),
             (unsigned long long)s_fifo.exchange(0), (unsigned long long)s_missing.exchange(0),
             (unsigned long long)s_viDiffersFromCam.exchange(0));
    size_t len = strlen(buf);
    if (len + 1 < size) snprintf(buf + len, size - len, "%s", extra);
}

} // namespace so4vr
