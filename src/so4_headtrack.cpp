// SO4 head tracking (stage 3, RenderMode = Mono).
//
// Per frame, on the game thread, inside UpdateCamera (RVA 0x2746F0) for the main camera:
//   * the headset pose predicted for when this frame is displayed is turned into a camera-local transform;
//   * the projection is set through the camera's own lens parameters (focal length, aspect, lens shift), so
//     the game itself builds the headset-FOV projection, the view-projection and its culling data;
//   * the view inverse (RVA 0x10150 / 0x108C0, called by UpdateCamera with world -> view) gets
//     (levelled game world) x head instead of the game's world matrix;
//   * afterwards the lens parameters are put back (the lens block can be shared) and the frame is recorded
//     with the view matrix it produced.
// On the render thread the per-view constant upload (off::kViewBlockBytes bytes, view rows at +0xC0) identifies which record
// the presented frame was rendered with; that pose/FOV is what the OpenXR projection layer reports.
//
// SO4 camera offsets (DF equivalents in brackets): +0x30 world, +0x110 flags (bit2 view valid), +0x120 view,
// +0x650/+0x654 lens shift, +0x6E4 aspect [0x6F4], +0x6FC m00, +0x710 lens* [0x720], +0x7AA flags
// (bit0 ortho, bit1 projection dirty, bit5 view dirty) [0x7C2], +0x7C8 film aspect [0x7E0],
// +0x7CC / +0x7D0 film width / height [0x7E4 / 0x7E8]. Lens: +0x18 focal, +0x28 zoom, +0x44 film mm.
// Projection: m00 = zoom*focal/(filmMM*0.5); m11 = m00*aspect*filmAspect; m02 = -2*shiftX/filmW, m12 likewise.
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include "so4_headtrack.h"
#include "xr_runtime.h"
#include "vrmath.h"
#include "config.h"
#include "log.h"
#include "game_offsets.h"

namespace so4vr {
namespace {

constexpr uintptr_t kWorld = 0x30, kFlags110 = 0x110, kView = 0x120, kShift = 0x650, kAspect = 0x6E4,
                    kLens = 0x710, kFlags7AB = 0x7AA, kFilmAspect = 0x7C8, kFilmW = 0x7CC, kFilmH = 0x7D0;
constexpr uintptr_t kLensFocal = 0x18, kLensZoom = 0x28, kLensFilmMM = 0x44;

template <typename T> T& At(uintptr_t a) { return *reinterpret_cast<T*>(a); }

// ---- game-thread state for the update in progress ----
struct Pending {
    uintptr_t cam = 0, lens = 0;
    float focal = 0, aspect = 0, shift[2]{};
    float headM[16];
    HeadFrame frame{};
} g_p;
thread_local const float* t_src = nullptr;   // world matrix to replace during the tracked UpdateCamera
thread_local float t_headM[16];
bool g_wasTracking = false;
uintptr_t g_lastCam = 0;

// ---- recentre ----
std::atomic<bool> g_recenterReq{false};
bool g_haveRef = false; float g_refYaw = 0; XrVector3f g_refPos{0, 0, 0};
float YawOf(const XrQuaternionf& q) {
    float fx = -2.0f * (q.x * q.z + q.w * q.y);
    float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    return std::atan2(-fx, -fz);
}
XrPosef ApplyRecenter(const XrPosef& p) {
    if (!g_haveRef) return p;
    float c = std::cos(-g_refYaw * 0.5f), s = std::sin(-g_refYaw * 0.5f);
    const XrQuaternionf& q = p.orientation;
    XrPosef o;
    o.orientation = {c * q.x + s * q.z, c * q.y + s * q.w, c * q.z - s * q.x, c * q.w - s * q.y};
    float dx = p.position.x - g_refPos.x, dy = p.position.y - g_refPos.y, dz = p.position.z - g_refPos.z;
    float cy = std::cos(-g_refYaw), sy = std::sin(-g_refYaw);
    o.position = {cy * dx + sy * dz, dy, -sy * dx + cy * dz};
    return o;
}

// ---- frame records (game thread -> render thread) ----
constexpr int kHist = 128;   // ~2 s: during stick turns the camera updates several times per frame and the
                             // render thread can draw a frame that is more than a dozen updates old
std::mutex g_hm;
HeadFrame g_hist[kHist]; bool g_histOk[kHist] = {}; int g_histNext = 0; uint64_t g_seq = 0;
// render thread: the record matched by this frame's view upload
HeadFrame g_matched{}; bool g_haveMatched = false;

std::atomic<uint64_t> s_skipMenu{0};
std::atomic<uint64_t> s_applied{0}, s_skipNoPose{0}, s_skipOther{0}, s_viewHook{0}, s_matched{0}, s_unmatched{0}, s_noUpload{0};
std::atomic<int> s_lastLag{-1};
std::atomic<uint64_t> s_ambig{0}, s_notNewest{0}, s_inexact{0};
std::atomic<uint64_t> s_viewNoHead{0}, s_viewHead{0};
std::mutex g_fbM; double g_fbCH = 0, g_fbHH = 0, g_fbCC = 0; uint64_t g_fbN = 0;

bool ViewEq(const float* a, const float* b) {
    for (int i = 0; i < 12; ++i) {
        float d = std::fabs(a[i] - b[i]), m = std::fmax(1.0f, std::fabs(a[i]));
        if (d > 1e-3f * m) return false;
    }
    return true;
}

} // namespace

bool So4HtPre(uintptr_t cam) {
    if (!HeadTrackedMode()) return false;
    uint8_t f7ab = At<uint8_t>(cam + kFlags7AB);
    if (f7ab & 0x01) { s_skipOther++; return false; }                 // orthographic
    static const float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (memcmp(reinterpret_cast<void*>(cam + kWorld), I, sizeof I) == 0) { s_skipOther++; return false; } // title/menu default camera
    {   // 2D-menu cameras (title, save slots, camp menu...): unrotated, on the z axis (e.g. at 0,0,-41). Head
        // tracking them swings the menu's 3D panels around; leave them alone (the menu then shows on the flat screen).
        const float* w = reinterpret_cast<const float*>(cam + kWorld);
        auto n = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
        if (n(w[0], 1) && n(w[1], 0) && n(w[2], 0) && n(w[4], 0) && n(w[5], 1) && n(w[6], 0) &&
            n(w[8], 0) && n(w[9], 0) && n(w[10], 1) && std::fabs(w[3]) < 0.01f && std::fabs(w[7]) < 0.01f) { s_skipMenu++; return false; }
    }
    uintptr_t lens = At<uintptr_t>(cam + kLens);
    if (!lens) { s_skipOther++; return false; }
    float zoom = At<float>(lens + kLensZoom), filmMM = At<float>(lens + kLensFilmMM);
    float filmAspect = At<float>(cam + kFilmAspect), filmW = At<float>(cam + kFilmW), filmH = At<float>(cam + kFilmH);
    if (zoom == 0 || filmMM == 0 || filmAspect == 0 || filmW == 0 || filmH == 0) { s_skipOther++; return false; }

    XrView views[2]; int64_t baseT = 0, poseT = 0;
    if (!XrLocateEyesForGameFrame(views, &baseT, &poseT)) { s_skipNoPose++; return false; }
    // Overscan: render (and report) a slightly larger field of view than the lenses show, so that when the
    // compositor turns an older frame to the current head pose there is image instead of black at the edges.
    if (g_config.fovOverscan > 0.0f) {
        const float k = 1.0f + g_config.fovOverscan;
        for (auto& v : views) {
            v.fov.angleLeft = std::atan(std::tan(v.fov.angleLeft) * k); v.fov.angleRight = std::atan(std::tan(v.fov.angleRight) * k);
            v.fov.angleUp = std::atan(std::tan(v.fov.angleUp) * k);     v.fov.angleDown = std::atan(std::tan(v.fov.angleDown) * k);
        }
    }

    XrPosef head;
    head.orientation = views[0].pose.orientation;
    head.position = {(views[0].pose.position.x + views[1].pose.position.x) * 0.5f,
                     (views[0].pose.position.y + views[1].pose.position.y) * 0.5f,
                     (views[0].pose.position.z + views[1].pose.position.z) * 0.5f};
    if (g_recenterReq.exchange(false)) {
        g_refYaw = YawOf(head.orientation); g_refPos = head.position; g_haveRef = true;
        LOGI("view recentred (yaw %.1f deg, pos %.2f %.2f %.2f m)", g_refYaw * 57.29578f, g_refPos.x, g_refPos.y, g_refPos.z);
    }
    const XrPosef layerPose = head;           // the layer reports the raw pose (compositor reprojection)
    head = ApplyRecenter(head);               // the camera follows the recentred pose
    XrFovf fov;
    fov.angleLeft  = std::fmin(views[0].fov.angleLeft, views[1].fov.angleLeft);
    fov.angleRight = std::fmax(views[0].fov.angleRight, views[1].fov.angleRight);
    fov.angleUp    = std::fmax(views[0].fov.angleUp, views[1].fov.angleUp);
    fov.angleDown  = std::fmin(views[0].fov.angleDown, views[1].fov.angleDown);

    // head transform in camera-local space (game axes, cm)
    const float cmPerMetre = 100.0f * g_config.worldScale;
    vm::Quat q = vm::XrToGameRot({head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w});
    vm::Vec3 t = vm::XrToGamePos({head.position.x, head.position.y, head.position.z}, cmPerMetre);
    float r[9]; vm::QuatToMat3(q, r); vm::MakeTransform(r, t, g_p.headM);

    // projection through the lens parameters
    const float tl = std::tan(fov.angleLeft), tr = std::tan(fov.angleRight), tu = std::tan(fov.angleUp), td = std::tan(fov.angleDown);
    const float hw = (tr - tl) * 0.5f, cx = (tr + tl) * 0.5f, hh = (tu - td) * 0.5f, cy = (tu + td) * 0.5f;
    const float F = 1.0f / hw;
    g_p.cam = cam; g_p.lens = lens;
    g_p.focal = At<float>(lens + kLensFocal); g_p.aspect = At<float>(cam + kAspect);
    g_p.shift[0] = At<float>(cam + kShift); g_p.shift[1] = At<float>(cam + kShift + 4);
    At<float>(lens + kLensFocal) = F * filmMM * 0.5f / zoom;
    At<float>(cam + kAspect) = (hw / hh) / filmAspect;
    At<float>(cam + kShift) = (cx / hw) * filmW * 0.5f;
    At<float>(cam + kShift + 4) = (cy / hh) * filmH * 0.5f * g_config.lensShiftYSign;
    At<uint8_t>(cam + kFlags7AB) = f7ab | 0x22;                      // projection + view dirty
    At<uint8_t>(cam + kFlags110) &= ~0x04;                           // view not valid -> recomputed (head goes in)

    memcpy(t_headM, g_p.headM, sizeof t_headM);
    {   // diagnostic: does the game camera follow the head? (regression of game-camera yaw steps on head yaw steps)
        const float* w = reinterpret_cast<const float*>(cam + kWorld);
        float cy = std::atan2(w[2], w[10]), hy = YawOf(head.orientation);
        static float pc = 0, ph = 0; static uintptr_t pcam = 0;
        if (pcam == cam) {
            auto wrap = [](float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; };
            double dc = wrap(cy - pc), dh = wrap(hy - ph);
            if (std::fabs(dc) < 0.2 && std::fabs(dh) < 0.2) {
                std::lock_guard<std::mutex> lk(g_fbM); g_fbCH += dc * dh; g_fbHH += dh * dh; g_fbCC += dc * dc; g_fbN++;
            }
        }
        pc = cy; ph = hy; pcam = cam;
    }
    if (g_config.stereoDiag) {   // diagnostic: F12 = log the game camera's own transform for 180 updates (head feedback?)
        static int left = 0; static bool down = false;
        bool now = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (now && !down) left = 180;
        down = now;
        if (left > 0) {
            --left;
            const float* w = reinterpret_cast<const float*>(cam + kWorld);
            LOGI("camdiag yaw %.3f | game cam pos (%.2f %.2f %.2f) fwd (%.5f %.5f %.5f) | +0x560 (%.2f %.2f %.2f)", YawOf(head.orientation) * 57.29578f,
                 w[3], w[7], w[11], w[2], w[6], w[10], At<float>(cam + 0x560), At<float>(cam + 0x564), At<float>(cam + 0x568));
        }
    }
    t_src = reinterpret_cast<const float*>(cam + kWorld);

    g_p.frame = HeadFrame{};
    g_p.frame.pose = layerPose; g_p.frame.fov = fov;
    g_p.frame.baseTime = baseT; g_p.frame.poseTime = poseT;
    {
        // per-eye data relative to the (raw) head pose, in the game's view space (native stereo)
        vm::Quat qh{layerPose.orientation.x, layerPose.orientation.y, layerPose.orientation.z, layerPose.orientation.w};
        vm::Quat qhInv = vm::QConj(qh);
        for (int e = 0; e < 2; ++e) {
            g_p.frame.eyePose[e] = views[e].pose; g_p.frame.eyeFov[e] = views[e].fov;
            vm::Vec3 d{views[e].pose.position.x - layerPose.position.x, views[e].pose.position.y - layerPose.position.y,
                       views[e].pose.position.z - layerPose.position.z};
            vm::Vec3 g = vm::XrToGamePos(vm::QRotate(qhInv, d), cmPerMetre);
            g_p.frame.eyeOff[e][0] = g.x; g_p.frame.eyeOff[e][1] = g.y; g_p.frame.eyeOff[e][2] = g.z;
            vm::Quat qe{views[e].pose.orientation.x, views[e].pose.orientation.y, views[e].pose.orientation.z, views[e].pose.orientation.w};
            vm::Quat rel = vm::XrToGameRot(vm::QMul(qhInv, qe));
            g_p.frame.eyeRot[e * 4 + 0] = rel.x; g_p.frame.eyeRot[e * 4 + 1] = rel.y; g_p.frame.eyeRot[e * 4 + 2] = rel.z; g_p.frame.eyeRot[e * 4 + 3] = rel.w;
        }
    }
    return true;
}

const float* So4HtWorldForView(const float* src, float* tmp) {
    if (!t_src || src != t_src) return src;
    float base[16];
    if (g_config.horizonLock) vm::LevelCamera(src, base); else memcpy(base, src, sizeof base);
    vm::Mul4(base, t_headM, tmp);
    s_viewHook++;
    return tmp;
}

void So4HtPost(uintptr_t cam) {
    t_src = nullptr;
    // lens back to the game's values (the next tracked frame sets ours again)
    At<float>(g_p.lens + kLensFocal) = g_p.focal;
    At<float>(cam + kAspect) = g_p.aspect;
    At<float>(cam + kShift) = g_p.shift[0]; At<float>(cam + kShift + 4) = g_p.shift[1];
    memcpy(g_p.frame.view, reinterpret_cast<void*>(cam + kView), 64);
    memcpy(g_p.frame.proj, reinterpret_cast<void*>(cam + 0x460), 64);   // viewport-adjusted P: VP (+0x4E0) = this * V
    if (!vm::Inverse4(reinterpret_cast<const float*>(cam + kWorld), g_p.frame.viewNoHead)) memset(g_p.frame.viewNoHead, 0, 64);
    {   // did the head get into the view? (some camera modes may build the view without our world hook)
        const float* hm = g_p.headM;
        float rotDev = std::fabs(hm[0] - 1) + std::fabs(hm[5] - 1) + std::fabs(hm[10] - 1);
        if (rotDev > 1e-3f) {
            bool same = true;
            for (int j = 0; j < 12 && same; ++j) same = std::fabs(g_p.frame.view[j] - g_p.frame.viewNoHead[j]) <= 1e-4f * std::fmax(1.0f, std::fabs(g_p.frame.view[j]));
            if (same) s_viewNoHead++; else s_viewHead++;
        }
    }
    g_wasTracking = true; g_lastCam = cam;
    {
        std::lock_guard<std::mutex> lk(g_hm);
        g_p.frame.seq = ++g_seq;
        g_hist[g_histNext] = g_p.frame; g_histOk[g_histNext] = true;
        g_histNext = (g_histNext + 1) % kHist;
    }
    s_applied++;
}

void So4HtIdle(uintptr_t cam) {
    // A camera updated without head tracking that was tracked last time (tracking off, or the main camera
    // moved to another object): make the game rebuild its own projection and view from its own parameters.
    if (!g_wasTracking || cam != g_lastCam) return;
    g_wasTracking = false;
    At<uint8_t>(cam + kFlags7AB) |= 0x22; At<uint8_t>(cam + kFlags110) &= ~0x04;
}

// all perspective cameras' latest views (game thread writes, render thread reads under g_hm)
struct CamView { uintptr_t cam; float view[12]; uint64_t tick; };
constexpr int kCamViews = 16;
CamView g_cv[kCamViews]; int g_cvN = 0;
std::atomic<uintptr_t> g_renderedCam{0}; std::atomic<uint64_t> g_renderedTick{0};
std::atomic<uint64_t> s_held{0}, s_camSwitch{0};
int g_unmatchedRun = 0; HeadFrame g_lastGood{}; bool g_haveLastGood = false;

// diagnostics: this frame's view uploads that matched nothing
int g_upCount = 0, g_upUnmatched = 0; float g_upFirstBad[12]; bool g_haveBad = false;
unsigned g_badOff = 0, g_badBytes = 0;
std::atomic<int> g_upZero{0}, g_upRepBad{0}, g_upRepOk{0}, g_upAll{0}, g_upBoxed{0};

// One candidate view position in a per-view upload. The off::kViewBlockBytes-byte per-view block carries the camera view at
// +0xC0 (written for most passes, 0x50768) and again at +0x340 (0x50823). Shadow passes of some lights put the
// light's view at +0xC0 (e.g. Calnus hallway door: rows (0.1 0.1 0.1 ...)), and in those frames the main view
// only appears at +0x340 - so both positions are checked.
static thread_local bool t_upMatchOk = false; static thread_local HeadFrame t_upMatch;
std::atomic<uint64_t> s_staleRepeat{0};
static void OnViewAt(const float* v, int slot, unsigned offset, unsigned bytes) {
    if (v[0] == 0.f && v[1] == 0.f && v[2] == 0.f && v[4] == 0.f && v[5] == 0.f) { g_upZero++; return; }   // no view here
    // the per-view buffer is re-uploaded up to ~100 times per frame with the same view: only new views count
    // (a repeat of the previous view reuses that view's match, so a still camera keeps matching every frame)
    thread_local float last[2][12]; thread_local bool haveLast[2] = {}, lastOk[2] = {}; thread_local HeadFrame lastHf[2];
    if (haveLast[slot] && memcmp(last[slot], v, sizeof last[slot]) == 0) {
        if (!lastOk[slot]) {
            g_upRepBad++;
            // A still view that matched no record: it can be another camera that the game is drawing. Check it
            // against the other cameras now and then, so the main-camera pick can follow it.
            thread_local unsigned rep = 0;
            if ((rep++ & 31) == 0) {
                std::lock_guard<std::mutex> lk(g_hm);
                if (!g_haveBad) { memcpy(g_upFirstBad, v, sizeof g_upFirstBad); g_haveBad = true; g_badOff = offset; g_badBytes = bytes; }
                for (int i = 0; i < g_cvN; ++i)
                    if (ViewEq(v, g_cv[i].view)) {
                        if (g_renderedCam.load() != g_cv[i].cam) s_camSwitch++;
                        g_renderedCam = g_cv[i].cam; g_renderedTick = GetTickCount64();
                        break;
                    }
            }
        } else {
            g_upRepOk++;
            std::lock_guard<std::mutex> lk(g_hm);
            // a view that has not changed for a long time (a camera the game stopped updating) is not this frame's
            if (g_seq - lastHf[slot].seq > 16) { s_staleRepeat++; return; }
            t_upMatch = lastHf[slot]; t_upMatchOk = true;
            if (!g_haveMatched || lastHf[slot].seq >= g_matched.seq) { g_matched = lastHf[slot]; g_haveMatched = true; }
            g_renderedTick = GetTickCount64();
        }
        return;
    }
    memcpy(last[slot], v, sizeof last[slot]); haveLast[slot] = true; lastOk[slot] = false;
    std::lock_guard<std::mutex> lk(g_hm);
    ++g_upCount;
    // The record whose view is closest (not just the newest within tolerance): when the camera barely moves,
    // several records pass ViewEq and the newest one is not necessarily the one this frame was rendered with -
    // its head pose would then be submitted for an image rendered with another (judder / sway).
    int best = -1, nPass = 0, newest = -1; float bestD = 1e30f;
    for (int k = 1; k <= kHist; ++k) {
        int i = (g_histNext - k + kHist) % kHist;
        if (!g_histOk[i] || !ViewEq(v, g_hist[i].view)) continue;
        float d = 0;
        for (int j = 0; j < 12; ++j) d = std::fmax(d, std::fabs(v[j] - g_hist[i].view[j]) / std::fmax(1.0f, std::fabs(v[j])));
        ++nPass; if (newest < 0) newest = i;
        if (d < bestD) { bestD = d; best = i; }
        if (d == 0.0f) break;   // exact (newest exact wins)
    }
    if (best >= 0) {
        if (!g_config.htNewestMatch) {} else best = newest;
        if (nPass > 1) s_ambig++;
        if (best != newest) s_notNewest++;
        if (bestD > 1e-5f) s_inexact++;
        int i = best;
        if (!g_haveMatched || g_hist[i].seq >= g_matched.seq || !g_config.htNewestMatch) { g_matched = g_hist[i]; g_haveMatched = true; s_lastLag = int(g_seq - g_hist[i].seq); }
        g_renderedTick = GetTickCount64();
        lastHf[slot] = g_hist[i]; lastOk[slot] = true;
        if (g_seq - g_hist[i].seq <= 16) { t_upMatch = g_hist[i]; t_upMatchOk = true; } else s_staleRepeat++;
        return;
    }
    ++g_upUnmatched;
    if (!g_haveBad) { memcpy(g_upFirstBad, v, sizeof g_upFirstBad); g_haveBad = true; g_badOff = offset; g_badBytes = bytes; }
    for (int i = 0; i < g_cvN; ++i)   // which camera is this? (the head-tracked camera is not the one being rendered)
        if (ViewEq(v, g_cv[i].view)) {
            if (g_renderedCam.load() != g_cv[i].cam) s_camSwitch++;
            g_renderedCam = g_cv[i].cam; g_renderedTick = GetTickCount64();
            break;
        }
}

bool So4HtTakeUploadMatch(HeadFrame& out) { if (!t_upMatchOk) return false; out = t_upMatch; t_upMatchOk = false; return true; }

void So4HtOnViewUpload(const void* data, unsigned bytes, unsigned offset) {
    t_upMatchOk = false;
    if (!HeadTrackedMode()) return;
    g_upAll++;
    if (offset) g_upBoxed++;
    static const unsigned kPos[2] = {off::kViewPosA, off::kViewPosB};
    for (int slot = 0; slot < 2; ++slot) {
        unsigned at = kPos[slot];
        if (offset > at || offset + bytes < at + 0x30) continue;   // a partial update (box) that does not cover it
        OnViewAt(reinterpret_cast<const float*>(static_cast<const uint8_t*>(data) + (at - offset)), slot, offset, bytes);
    }
}

void So4HtNoteCameraView(uintptr_t cam) {
    std::lock_guard<std::mutex> lk(g_hm);
    int i = 0;
    for (; i < g_cvN; ++i) if (g_cv[i].cam == cam) break;
    if (i == g_cvN) {
        if (g_cvN < kCamViews) ++g_cvN;
        else { int o = 0; for (int k = 1; k < kCamViews; ++k) if (g_cv[k].tick < g_cv[o].tick) o = k; i = o; }
    }
    g_cv[i].cam = cam; g_cv[i].tick = GetTickCount64();
    memcpy(g_cv[i].view, reinterpret_cast<void*>(cam + kView), sizeof g_cv[i].view);
}

int So4HtCopyCamViews(uint64_t* cams, float* views, int max) {
    std::lock_guard<std::mutex> lk(g_hm);
    int n = 0;
    for (int i = 0; i < g_cvN && n < max; ++i, ++n) { cams[n] = g_cv[i].cam; memcpy(views + n * 12, g_cv[i].view, 48); }
    for (int k = 1; k <= kHist && n < max; ++k) {   // and the head-tracked records (cam = 1)
        int i = (g_histNext - k + kHist) % kHist; if (!g_histOk[i]) continue;
        cams[n] = 1; memcpy(views + n * 12, g_hist[i].view, 48); ++n;
    }
    return n;
}

uintptr_t So4HtRenderedCamera(uint64_t* tick) { if (tick) *tick = g_renderedTick.load(); return g_renderedCam.load(); }

bool So4HtTakeFrame(HeadFrame& out) {
    std::lock_guard<std::mutex> lk(g_hm);
    int ups = g_upCount, bad = g_upUnmatched; g_upCount = g_upUnmatched = 0;
    struct ResetUp { ~ResetUp() { g_upBoxed = 0; g_upZero = 0; g_upRepBad = 0; g_upRepOk = 0; g_upAll = 0; } } resetUp;
    bool haveBad = g_haveBad; g_haveBad = false;
    if (!g_haveMatched && g_haveLastGood && ++g_unmatchedRun <= 10) {
        // short gap (e.g. the game switching cameras): keep the headset in head-tracked mode with the last pose
        out = g_lastGood; s_held++;
        return true;
    }
    if (!g_haveMatched) {
        if (g_seq) s_unmatched++;
        static ULONGLONG lastLog = 0; ULONGLONG now = GetTickCount64();
        if (g_seq && now - lastLog > 1000) {
            lastLog = now;
            LOGI("head tracking: unmatched frame: per-view uploads %d (partial %d, no view %d, new %d of which unmatched %d, repeats unmatched %d / matched %d)",
                 g_upAll.load(), g_upBoxed.load(), g_upZero.load(), ups, bad, g_upRepBad.load(), g_upRepOk.load());
            if (!haveBad) { /* no per-view upload at all (title, loading, menus): normal, flat screen */ }
            else {
                // nearest record by largest element difference
                int bestK = -1; float bestD = 1e30f;
                for (int k = 1; k <= kHist; ++k) {
                    int i = (g_histNext - k + kHist) % kHist; if (!g_histOk[i]) continue;
                    float d = 0; for (int j = 0; j < 12; ++j) d = std::fmax(d, std::fabs(g_upFirstBad[j] - g_hist[i].view[j]));
                    if (d < bestD) { bestD = d; bestK = k; }
                }
                const float* n = bestK > 0 ? g_hist[(g_histNext - bestK + kHist) % kHist].view : g_upFirstBad;
                LOGI("head tracking: frame without a match (upload offset %u bytes %u): %d view uploads (%d unmatched); nearest record %d updates back, max diff %.4f; "
                     "upload row0 (%.4f %.4f %.4f %.2f) rec row0 (%.4f %.4f %.4f %.2f) upload t (%.2f %.2f) rec t (%.2f %.2f)",
                     g_badOff, g_badBytes, ups, bad, bestK, bestD, g_upFirstBad[0], g_upFirstBad[1], g_upFirstBad[2], g_upFirstBad[3], n[0], n[1], n[2], n[3],
                     g_upFirstBad[7], g_upFirstBad[11], n[7], n[11]);
            }
        }
        return false;
    }
    out = g_matched; g_haveMatched = false; s_matched++;
    g_lastGood = g_matched; g_haveLastGood = true; g_unmatchedRun = 0;
    return true;
}

bool So4HtPeekFrame(HeadFrame& out) {
    std::lock_guard<std::mutex> lk(g_hm);
    if (!g_haveMatched) return false;
    out = g_matched; return true;
}

bool So4HtRecordAfter(uint64_t seq, HeadFrame& out) {
    std::lock_guard<std::mutex> lk(g_hm);
    bool found = false; uint64_t best = ~0ull;
    for (int i = 0; i < kHist; ++i)
        if (g_histOk[i] && g_hist[i].seq > seq && g_hist[i].seq < best) { best = g_hist[i].seq; out = g_hist[i]; found = true; }
    return found;
}

bool So4HtRecordBySeq(uint64_t seq, HeadFrame& out) {
    std::lock_guard<std::mutex> lk(g_hm);
    for (int i = 0; i < kHist; ++i) if (g_histOk[i] && g_hist[i].seq == seq) { out = g_hist[i]; return true; }
    return false;
}

// Diagnostic: the record whose view is nearest to v (max element difference) over the whole history.
bool So4HtNearestRecord(const float* v, uint64_t& seq, float& diff, uint64_t& newest, uint64_t& oldest) {
    std::lock_guard<std::mutex> lk(g_hm);
    bool found = false; diff = 1e30f; newest = 0; oldest = ~0ull;
    for (int i = 0; i < kHist; ++i) {
        if (!g_histOk[i]) continue;
        if (g_hist[i].seq > newest) newest = g_hist[i].seq;
        if (g_hist[i].seq < oldest) oldest = g_hist[i].seq;
        float m = 0; for (int j = 0; j < 12; ++j) m = std::fmax(m, std::fabs(v[j] - g_hist[i].view[j]));
        if (m < diff) { diff = m; seq = g_hist[i].seq; found = true; }
    }
    return found;
}

void So4HtRequestRecenter() { g_recenterReq = true; }
bool So4HtRecenterRef(float& yaw, XrVector3f& pos) { if (!g_haveRef) return false; yaw = g_refYaw; pos = g_refPos; return true; }

void So4HtReport(char* buf, unsigned size) {
    double slope = 0, camDeg = 0, headDeg = 0; uint64_t n = 0;
    { std::lock_guard<std::mutex> lk(g_fbM); n = g_fbN;
      if (n) { slope = g_fbHH > 1e-9 ? g_fbCH / g_fbHH : 0; camDeg = std::sqrt(g_fbCC / n) * 57.29578; headDeg = std::sqrt(g_fbHH / n) * 57.29578; }
      g_fbCH = g_fbHH = g_fbCC = 0; g_fbN = 0; }
    snprintf(buf, size, "SO4 head tracking: applied %llu (view hook %llu), skipped %llu no pose / %llu menu camera / %llu other | frames matched %llu, held %llu, unmatched %llu, last lag %d updates, rendered-camera switches %llu | new views: ambiguous %llu, closest not newest %llu, inexact %llu | game camera vs head yaw: slope %.2f (rms step cam %.3f / head %.3f deg, %llu updates) | views with head %llu, WITHOUT head %llu | stale view matches ignored %llu",
             (unsigned long long)s_applied.exchange(0), (unsigned long long)s_viewHook.exchange(0), (unsigned long long)s_skipNoPose.exchange(0),
             (unsigned long long)s_skipMenu.exchange(0), (unsigned long long)s_skipOther.exchange(0), (unsigned long long)s_matched.exchange(0), (unsigned long long)s_held.exchange(0),
             (unsigned long long)s_unmatched.exchange(0), s_lastLag.load(), (unsigned long long)s_camSwitch.exchange(0),
             (unsigned long long)s_ambig.exchange(0), (unsigned long long)s_notNewest.exchange(0), (unsigned long long)s_inexact.exchange(0), slope, camDeg, headDeg, (unsigned long long)n, (unsigned long long)s_viewHead.exchange(0), (unsigned long long)s_viewNoHead.exchange(0), (unsigned long long)s_staleRepeat.exchange(0));
}

} // namespace so4vr
