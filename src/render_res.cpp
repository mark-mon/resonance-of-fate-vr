// Render resolution override (SO4): makes the game render at the headset's per-eye resolution (times
// RenderScale) instead of its 16:9 graphics resolution.
//
// StarOceanTheLastHope.exe (PE timestamp 1517901502):
//   [0xF76C98] screen config object (vtable 0x90DF20): 16-byte descriptors at +0x28 (final image, render target
//              0xC1: u16 w, u16 h, float aspect, 2 dwords) and +0x38 (scene targets, target 0: u16 w, u16 h,
//              2 format dwords, MSAA count); +0x08/+0x18 mirror them, +0x5C aspect, +0x60 scale.
//   0xD1F0     SetScreenConfig(cfg, const desc* final, const desc* scene): the game's own setter (graphics
//              options). Copies both, recomputes the aspect fields and calls 0x5DEA0(render queue), which sets
//              queue byte +0x500D1 under the render thread's lock; the render thread then rebuilds the screen
//              targets (0x5DBAA -> 0x5D1D0 -> 0xCDC0) and game code waits while it is pending (0x1DF70).
// Writing the fields and the queue byte from the render thread works at the title screen but raced the
// loading thread at startup (039a: crash at +0x9AF1E reading target 0xC1 mid-rebuild), so the setter is
// called from the game thread (UpdateCamera hook), not before 20 s after start.
#include "render_res.h"
#include "config.h"
#include "log.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>

namespace so4vr {
namespace {

constexpr uintptr_t kScreenCfgPtr = 0x1716C08;   // ROF (SO4 0xF76C98)
constexpr uintptr_t kQScreenDirty = 0x500E1;     // ROF (SO4 0x500D1)
constexpr uintptr_t kFinalSize = 0x28, kSceneSize = 0x38;
constexpr uintptr_t kFnSetScreenConfig = 0x28DDD0; // ROF (SO4 0xD1F0)
using FnSetScreenConfig = char(__fastcall*)(void* cfg, const void* fin, const void* scn);
std::atomic<uintptr_t> g_queue{0};
ULONGLONG g_start = GetTickCount64();

std::atomic<uint32_t> g_hmdW{0}, g_hmdH{0};   // recommended per-eye size from the OpenXR runtime
uint16_t g_origW = 0, g_origH = 0;            // the game's own size (restored for Flat / Game)
uint32_t g_appliedW = 0, g_appliedH = 0;      // what we last wrote (0 = game's own)
std::atomic<uint32_t> g_curW{0}, g_curH{0};
int g_applies = 0;

uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

// Target render size for the current config, or false to leave the game's size alone.
bool Desired(uint32_t& w, uint32_t& h) {
    const Config& c = g_config;
    if (c.renderResMode == Config::ResGame) return false;
    if (c.activeMode != Config::Native) return false;   // Mono/Flat show the 16:9 back buffer: keep the game's size
    double bw, bh;
    if (c.renderResMode == Config::ResExplicit) { bw = c.renderResW; bh = c.renderResH; }
    else {
        bw = g_hmdW.load(); bh = g_hmdH.load();
        if (bw <= 0 || bh <= 0) return false;
    }
    double s = c.renderScale > 0.1 ? c.renderScale : 1.0;
    if (c.renderResMode != Config::ResExplicit) {
        // The game's UI only lays out correctly at 16:9 (039: at other shapes the minimap is not drawn and the
        // HUD is squeezed), so Auto keeps 16:9 at the headset's recommended height; the projection is set to
        // the headset's field of view, so the pixels are simply wider than tall.
        bh *= 1.0 + (c.fovOverscan > 0 ? c.fovOverscan : 0.0);   // same pixel density with the wider field of view
        bw = bh * 16.0 / 9.0;
    }
    w = uint32_t(std::lround(bw * s / 8.0) * 8);
    h = uint32_t(std::lround(bh * s / 8.0) * 8);
    if (w < 640) w = 640;
    if (h < 360) h = 360;
    if (w > 8192 || h > 8192) {   // keep the shape
        double k = 8192.0 / double(w > h ? w : h);
        w = uint32_t(std::lround(w * k / 8.0) * 8); h = uint32_t(std::lround(h * k / 8.0) * 8);
    }
    return true;
}

} // namespace

bool RenderResInstall() { return true; }   // nothing to hook: applied from the render thread (RenderResRenderTick)

void RenderResSetHmdRecommended(uint32_t w, uint32_t h) {
    g_hmdW.store(w); g_hmdH.store(h);
    LOGI("headset recommended per-eye size %ux%u (RenderScale %.2f)", w, h, g_config.renderScale);
}

void RenderResTick() {}

// Render thread: remember the queue (its +0x500D1 byte tells whether a rebuild is pending) and the size.
void RenderResRenderTick(uintptr_t queue) {
    if (!queue) return;
    g_queue = queue;
    uintptr_t cfg = *reinterpret_cast<uintptr_t*>(Base() + kScreenCfgPtr);
    if (cfg) { g_curW = reinterpret_cast<uint16_t*>(cfg + kFinalSize)[0]; g_curH = reinterpret_cast<uint16_t*>(cfg + kFinalSize)[1]; }
}

// Game thread (UpdateCamera hook): apply the wanted size through the game's own setter.
void RenderResGameTick() {
    uintptr_t q = g_queue.load();
    if (!q || GetTickCount64() - g_start < 20000) return;
    uintptr_t cfg = *reinterpret_cast<uintptr_t*>(Base() + kScreenCfgPtr);
    if (!cfg) return;
    if (*reinterpret_cast<volatile uint8_t*>(q + kQScreenDirty)) return;   // a rebuild is pending already
    const uint16_t* fin = reinterpret_cast<const uint16_t*>(cfg + kFinalSize);
    const uint16_t* scn = reinterpret_cast<const uint16_t*>(cfg + kSceneSize);
    // The game's own size: whatever is there while we have not changed it (its graphics options can change it).
    if (!g_appliedW || fin[0] != g_appliedW || fin[1] != g_appliedH) { g_origW = fin[0]; g_origH = fin[1]; g_appliedW = g_appliedH = 0; }
    uint32_t w = 0, h = 0;
    bool want = Desired(w, h);
    if (!want) { if (!g_appliedW) return; w = g_origW; h = g_origH; }
    if (fin[0] == w && fin[1] == h && scn[0] == w && scn[1] == h) return;
    if (g_applies >= 64) return;   // safety: never loop
    ++g_applies;
    uint8_t f[16], s2[16];
    memcpy(f, reinterpret_cast<void*>(cfg + kFinalSize), 16);
    memcpy(s2, reinterpret_cast<void*>(cfg + kSceneSize), 16);
    reinterpret_cast<uint16_t*>(f)[0] = reinterpret_cast<uint16_t*>(s2)[0] = uint16_t(w);
    reinterpret_cast<uint16_t*>(f)[1] = reinterpret_cast<uint16_t*>(s2)[1] = uint16_t(h);
    g_appliedW = want ? w : 0; g_appliedH = want ? h : 0;
    LOGI("render resolution: %ux%u%s (game's own %ux%u)", w, h, want ? " [VR]" : " [restored]", g_origW, g_origH);
    reinterpret_cast<FnSetScreenConfig>(Base() + kFnSetScreenConfig)(reinterpret_cast<void*>(cfg), f, s2);
}

void RenderResCurrent(uint32_t& w, uint32_t& h) { w = g_curW.load(); h = g_curH.load(); }

} // namespace so4vr
