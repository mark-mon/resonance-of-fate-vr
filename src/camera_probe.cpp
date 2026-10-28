#include <windows.h>
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstring>
#include "camera_probe.h"
#include "render_res.h"
#include "image_quality.h"
#include "game_offsets.h"
#include "safe_mem.h"
#include "config.h"
#include "log.h"
#include "head_tracking.h"
#include "native_stereo.h"

namespace so4vr {
namespace {

uintptr_t g_base = 0;

struct CamSample {
    bool     valid = false;
    uintptr_t cam = 0;
    uintptr_t vtableRva = 0;
    float world[16]{}, view[16]{}, proj[16]{}, projNoJitter[16]{};
    float pos[4]{};
    float lensShift[2]{}, jitter[2]{};
    float nearZ = 0, farZ = 0, depthNear = 0, depthFar = 0;
    float fovRad = 0, focal = 0, aspect6F4 = 0, filmAspect = 0, filmW = 0, filmH = 0;
    uint8_t flags7C2 = 0;
    uint32_t flags110 = 0;
    // ViewInfo (render-side snapshot, read slot)
    bool  viValid = false;
    int   viRead = -1, viWrite = -1;
    float viPos[4]{}, viFov = 0, viAspect = 0, viNear = 0, viFar = 0;
};

std::mutex g_mutex;
CamSample g_latest;                 // captured after Prepare (fresh matrices)
std::atomic<uint64_t> g_prepareCalls{0};
std::atomic<DWORD> g_prepareThread{0};
std::atomic<uint64_t> g_prepareFailures{0};
ULONGLONG g_nextLog = 0;
bool g_firstDetailed = false;
float g_savedShift[2]{};
bool g_shiftApplied = false;

uintptr_t ReadMainCamera() {
    uintptr_t root = 0, cam = 0;
    if (!SafeLoad(g_base + off::kSceneRoot, root) || !root) return 0;
    if (!SafeLoad(root + off::kSceneRoot_MainCam, cam)) return 0;
    return cam;
}

bool Capture(uintptr_t cam, CamSample& s) {
    s = {};
    s.cam = cam;
    uintptr_t vt = 0;
    if (!SafeLoad(cam, vt)) return false;
    s.vtableRva = vt - g_base;
    bool ok = SafeRead(s.world, (void*)(cam + off::kCamWorld), 64) &&
              SafeRead(s.view, (void*)(cam + off::kCamView), 64) &&
              SafeRead(s.proj, (void*)(cam + off::kCamProj), 64) &&
              SafeRead(s.projNoJitter, (void*)(cam + off::kCamProjNoJitter), 64) &&
              SafeRead(s.pos, (void*)(cam + off::kCamPos), 16) &&
              SafeRead(s.lensShift, (void*)(cam + off::kCamLensShift), 8) &&
              SafeRead(s.jitter, (void*)(cam + off::kCamJitter), 8) &&
              SafeLoad(cam + off::kCamNear, s.nearZ) && SafeLoad(cam + off::kCamFar, s.farZ) &&
              SafeLoad(cam + off::kCamDepthNear, s.depthNear) && SafeLoad(cam + off::kCamDepthFar, s.depthFar) &&
              SafeLoad(cam + off::kCamFocal70C, s.focal) && SafeLoad(cam + off::kCamAspect6F4, s.aspect6F4) &&
              SafeLoad(cam + off::kCamFilmAspect, s.filmAspect) && SafeLoad(cam + off::kCamFilmW, s.filmW) &&
              SafeLoad(cam + off::kCamFilmH, s.filmH) && SafeLoad(cam + off::kCamFlags7C2, s.flags7C2) &&
              SafeLoad(cam + off::kCamFlags110, s.flags110);
    uintptr_t lens = 0;
    if (ok && SafeLoad(cam + off::kCamLens, lens) && lens) SafeLoad(lens + off::kLensFov, s.fovRad);

    uintptr_t ring = 0;
    if (SafeLoad(g_base + off::kViewInfoRing, ring) && ring &&
        SafeLoad(g_base + off::kViewInfoReadIdx, s.viRead) && SafeLoad(g_base + off::kViewInfoWriteIdx, s.viWrite) &&
        s.viRead >= 0 && s.viRead < 2) {
        uintptr_t vi = ring + 0x10 + uintptr_t(s.viRead) * off::kViewInfoStride;
        s.viValid = SafeRead(s.viPos, (void*)(vi + off::kViCamPos), 16) &&
                    SafeLoad(vi + off::kViFov, s.viFov) && SafeLoad(vi + off::kViAspect, s.viAspect) &&
                    SafeLoad(vi + off::kViNear, s.viNear) && SafeLoad(vi + off::kViFar, s.viFar);
    }
    s.valid = ok;
    return ok;
}

// ---- engine callbacks (called on the Prepare task thread) ----
void OnPrePrepare(void*) {
    g_prepareThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    RenderResTick();
    ImageQualityPrePrepare();
    if (HeadTrackPrePrepare(ReadMainCamera())) return; // head tracking owns the camera this frame
    // Optional diagnostic: push a lens shift so the user can report which way the image moves.
    if (g_config.debugLensShiftX != 0.0f || g_config.debugLensShiftY != 0.0f) {
        uintptr_t cam = ReadMainCamera();
        if (cam && SafeRead(g_savedShift, (void*)(cam + off::kCamLensShift), 8)) {
            float filmW = 0, filmH = 0;
            SafeLoad(cam + off::kCamFilmW, filmW);
            SafeLoad(cam + off::kCamFilmH, filmH);
            // Config values are fractions of half the film back (1.0 = shift by half the image).
            float shift[2] = {g_config.debugLensShiftX * filmW * 0.5f, g_config.debugLensShiftY * filmH * 0.5f};
            uint8_t f = 0;
            if (SafeWrite((void*)(cam + off::kCamLensShift), shift, 8) && SafeLoad(cam + off::kCamFlags7C2, f)) {
                f |= 0x02; // projection dirty
                SafeWrite((void*)(cam + off::kCamFlags7C2), &f, 1);
                g_shiftApplied = true;
            }
        }
    }
}

void OnPostPrepare(void*) {
    g_prepareCalls.fetch_add(1, std::memory_order_relaxed);
    uintptr_t cam = ReadMainCamera();
    CamSample s;
    if (cam && Capture(cam, s)) {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_latest = s;
    } else {
        g_prepareFailures.fetch_add(1, std::memory_order_relaxed);
    }
    HeadTrackPostPrepare(cam);
    if (g_shiftApplied && cam) {
        SafeWrite((void*)(cam + off::kCamLensShift), g_savedShift, 8);
        g_shiftApplied = false;
        // Leave the dirty bit: next frame rebuilds with the shift re-applied in OnPrePrepare.
    }
}

void LogMatrix(const char* name, const float* m) {
    LOGI("    %s: [%9.4f %9.4f %9.4f %9.4f] [%9.4f %9.4f %9.4f %9.4f] [%9.4f %9.4f %9.4f %9.4f] [%9.4f %9.4f %9.4f %9.4f]",
         name, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}

bool InstallCallback(uintptr_t rva, void* fn, const char* name) {
    void* cur = nullptr;
    if (!SafeLoad(g_base + rva, cur)) { LOGE("camera probe: cannot read %s slot", name); return false; }
    if (cur) { LOGW("camera probe: %s slot already in use (%p); not installing", name, cur); return false; }
    if (!SafeWrite((void*)(g_base + rva), &fn, sizeof(fn))) { LOGE("camera probe: cannot write %s slot", name); return false; }
    LOGI("camera probe: installed %s callback at RVA 0x%llx", name, (unsigned long long)rva);
    return true;
}

} // namespace

bool CameraProbeInstall() {
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    bool a = InstallCallback(off::kCbPrePrepare, (void*)&OnPrePrepare, "pre-Prepare");
    bool b = InstallCallback(off::kCbPostPrepare, (void*)&OnPostPrepare, "post-Prepare");
    if (!HeadTrackInstall()) LOGE("head tracking unavailable: view hook failed");
    NativeInstall();
    if (g_config.debugLensShiftX != 0.0f || g_config.debugLensShiftY != 0.0f)
        LOGW("camera probe: DebugLensShift active (x=%.2f y=%.2f): the picture should be visibly offset",
             g_config.debugLensShiftX, g_config.debugLensShiftY);
    return a && b;
}

void CameraProbeOnPresent() {
    if (g_config.cameraProbeIntervalSec <= 0) return;
    ULONGLONG now = GetTickCount64();
    if (now < g_nextLog) return;
    g_nextLog = now + ULONGLONG(g_config.cameraProbeIntervalSec) * 1000;

    CamSample s;
    { std::lock_guard<std::mutex> lk(g_mutex); s = g_latest; }
    uint64_t calls = g_prepareCalls.exchange(0);
    DWORD pt = g_prepareThread.load();
    uint64_t fails = g_prepareFailures.exchange(0);

    if (!s.valid) {
        LOGI("camera: no valid sample yet (prepare calls %llu, failures %llu, present thread %lu, prepare thread %lu)",
             (unsigned long long)calls, (unsigned long long)fails, GetCurrentThreadId(), pt);
        return;
    }
    const bool vtOk = s.vtableRva == off::kVtAskaCamera;
    // Basis vectors: columns of the world matrix (column-vector convention, translation in column 3).
    const float* w = s.world;
    LOGI("camera: cam=%p vtable RVA 0x%llx (%s) | prepare calls %llu (thread %lu, present thread %lu)%s",
         (void*)s.cam, (unsigned long long)s.vtableRva, vtOk ? "matches" : "MISMATCH", (unsigned long long)calls, pt,
         GetCurrentThreadId(), fails ? " | some reads failed" : "");
    LOGI("  pos(world col3)=(%.2f, %.2f, %.2f) pos(+0x560)=(%.2f, %.2f, %.2f)  right=(%.3f %.3f %.3f) up=(%.3f %.3f %.3f) fwd=(%.3f %.3f %.3f)",
         w[3], w[7], w[11], s.pos[0], s.pos[1], s.pos[2], w[0], w[4], w[8], w[1], w[5], w[9], w[2], w[6], w[10]);
    LOGI("  fov=%.2f deg near=%.2f far=%.1f depth(near,far)=(%.2f,%.2f) m00=%.4f m11=%.4f m02=%.5f m12=%.5f m22=%.5f m23=%.4f m32=%.2f",
         s.fovRad * 57.29578f, s.nearZ, s.farZ, s.depthNear, s.depthFar, s.proj[0], s.proj[5], s.proj[2], s.proj[6],
         s.proj[10], s.proj[11], s.proj[14]);
    LOGI("  lensShift=(%.4f, %.4f) jitter=(%.4f, %.4f) film W/H=%.3f/%.3f filmAspect=%.4f aspect6F4=%.4f focal70C=%.4f flags7C2=0x%02x flags110=0x%x",
         s.lensShift[0], s.lensShift[1], s.jitter[0], s.jitter[1], s.filmW, s.filmH, s.filmAspect, s.aspect6F4, s.focal,
         s.flags7C2, s.flags110);
    if (s.viValid)
        LOGI("  ViewInfo[read %d / write %d]: pos=(%.2f, %.2f, %.2f) fov=%.4f aspect=%.4f near=%.2f far=%.1f",
             s.viRead, s.viWrite, s.viPos[0], s.viPos[1], s.viPos[2], s.viFov, s.viAspect, s.viNear, s.viFar);
    if (!g_firstDetailed) {
        g_firstDetailed = true;
        LOGI("  full matrices (first sample):");
        LogMatrix("world  ", s.world);
        LogMatrix("view   ", s.view);
        LogMatrix("proj   ", s.proj);
        LogMatrix("projNJ ", s.projNoJitter);
    }
}

} // namespace so4vr
