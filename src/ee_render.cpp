// Enhanced Edition (D3D12) render-side helpers shared by the probe and native stereo.
//  - render target views: CreateRenderTargetView is hooked from device creation, so any RTV handle the game binds
//    can be mapped back to its resource (D3D12 has no OMGetRenderTargets)
//  - the engine's open command list: renderer global 0x2545A90 -> +0x298C30 device object -> +0x1C28D0 list
//    (the frame's lists are closed into a vector and submitted together at frame end, 0x1062E0)
//  - command list hooks (OMSetRenderTargets / draws) for the job trace; installed once from the render thread
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <cstdio>
#include "MinHook.h"
#include "hook_util.h"
#include "log.h"
#include "ee_render.h"
#include <vector>
#include "so4_stereo.h"
#include "game_offsets.h"
#include "head_tracking.h"
#include "render_res.h"
#include "xr_runtime.h"
#include "config.h"
#include <cmath>
#include <deque>

namespace so4vr {
void EeVrsOnSetRT(ID3D12GraphicsCommandList* l, ID3D12Resource* rt0, bool depth);
namespace {
constexpr uintptr_t kEeRendererGlobal = 0x2545A90, kEeRendDevObj = 0x298C30, kEeDevCurList = 0x1C28D0;

struct Rtv { ID3D12Resource* res; DXGI_FORMAT fmt; };
std::shared_mutex g_rm;
std::unordered_map<SIZE_T, Rtv> g_rtv;
UINT g_rtvInc = 0;

using PFN_CreateRTV = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
PFN_CreateRTV o_CreateRTV = nullptr;
void STDMETHODCALLTYPE hk_CreateRTV(ID3D12Device* d, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    o_CreateRTV(d, r, desc, h);
    DXGI_FORMAT f = desc ? desc->Format : (r ? r->GetDesc().Format : DXGI_FORMAT_UNKNOWN);
    std::unique_lock<std::shared_mutex> lk(g_rm);
    g_rtv[h.ptr] = {r, f};
}

using PFN_OMSetRT = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using PFN_DrawI = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using PFN_DrawII = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
PFN_OMSetRT o_OMSetRT = nullptr; PFN_DrawI o_DrawI = nullptr; PFN_DrawII o_DrawII = nullptr;
std::atomic<DWORD> g_watchTid{0};
EeRtState g_rt;
void STDMETHODCALLTYPE hk_OMSetRT(ID3D12GraphicsCommandList* l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    if (GetCurrentThreadId() == g_watchTid.load(std::memory_order_relaxed)) {
        g_rt.sets++;
        g_rt.n = n > 2 ? 2 : n;
        for (UINT i = 0; i < g_rt.n; ++i) {
            SIZE_T p = single ? rts[0].ptr + SIZE_T(i) * g_rtvInc : rts[i].ptr;
            g_rt.rt[i] = EeRtvResource(p, &g_rt.fmt[i]);
            if (i == 0) g_rt.rtv0 = p;
        }
        if (!n) g_rt.rt[0] = g_rt.rt[1] = nullptr;
        g_rt.list = l;
        EeVrsOnSetRT(l, n ? g_rt.rt[0] : nullptr, ds != nullptr);
    }
    o_OMSetRT(l, n, rts, single, ds);
}
void STDMETHODCALLTYPE hk_DrawI(ID3D12GraphicsCommandList* l, UINT a, UINT b, UINT c, UINT d) {
    if (GetCurrentThreadId() == g_watchTid.load(std::memory_order_relaxed)) g_rt.draws++;
    o_DrawI(l, a, b, c, d);
}
void STDMETHODCALLTYPE hk_DrawII(ID3D12GraphicsCommandList* l, UINT a, UINT b, UINT c, INT d, UINT e) {
    if (GetCurrentThreadId() == g_watchTid.load(std::memory_order_relaxed)) g_rt.draws++;
    o_DrawII(l, a, b, c, d, e);
}
} // namespace

void EeRenderOnDevice(ID3D12Device* d) {
    static bool done = false; if (done || !d) return; done = true;
    g_rtvInc = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    Hook(VtableEntry(d, 20), (void*)hk_CreateRTV, (void**)&o_CreateRTV, "ID3D12Device::CreateRenderTargetView (EE)");
}

ID3D12Resource* EeRtvResource(SIZE_T handle, DXGI_FORMAT* fmt) {
    std::shared_lock<std::shared_mutex> lk(g_rm);
    auto it = g_rtv.find(handle);
    if (it == g_rtv.end()) { if (fmt) *fmt = DXGI_FORMAT_UNKNOWN; return nullptr; }
    if (fmt) *fmt = it->second.fmt;
    return it->second.res;
}

ID3D12GraphicsCommandList* EeEngineList() {
    static uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t r = *reinterpret_cast<uintptr_t*>(base + kEeRendererGlobal);
    if (!r) return nullptr;
    uintptr_t dev = *reinterpret_cast<uintptr_t*>(r + kEeRendDevObj);
    if (!dev) return nullptr;
    return *reinterpret_cast<ID3D12GraphicsCommandList**>(dev + kEeDevCurList);
}

bool EeHookListMethods(ID3D12GraphicsCommandList* l, DWORD renderTid) {
    g_watchTid = renderTid;
    static bool done = false; if (done || !l) return done; done = true;
    bool ok = Hook(VtableEntry(l, 46), (void*)hk_OMSetRT, (void**)&o_OMSetRT, "ID3D12GraphicsCommandList::OMSetRenderTargets (EE)");
    ok = Hook(VtableEntry(l, 12), (void*)hk_DrawI, (void**)&o_DrawI, "ID3D12GraphicsCommandList::DrawInstanced (EE)") && ok;
    ok = Hook(VtableEntry(l, 13), (void*)hk_DrawII, (void**)&o_DrawII, "ID3D12GraphicsCommandList::DrawIndexedInstanced (EE)") && ok;
    return ok;
}

EeRtState& EeRt() { return g_rt; }

// ---------------------------------------------------------------- native stereo (EE)
bool BridgeCaptureEye(ID3D12GraphicsCommandList* l, ID3D12Resource* src, D3D12_RESOURCE_STATES srcState, int e);
void BridgeStereoReady(const HeadFrame& hf, bool hud);
void BridgeBeforeGameSubmit();
namespace {
constexpr uintptr_t kEeCbUpload = 0xCC060;   // (rcx renderer, rdx buffer object, r8 desc, r9b flag): memcpy into the upload ring
struct CbDesc { const uint8_t* src; uint32_t a, offset, size, b; uint8_t rest[8]; };   // 0x20 bytes built by 0xE34C0
using FnCb = void(__fastcall*)(void*, void*, CbDesc*, uint8_t);
FnCb o_Cb = nullptr;
std::atomic<DWORD> g_renderTid{0};
const uint8_t* g_lastViewSrc = nullptr;   // CPU per-view block of the last pass-1 upload (live: the game keeps writing it)
std::atomic<uint64_t> s_cbPatched{0}, s_views{0}, s_capFail{0};
void __fastcall hk_Cb(void* r, void* buf, CbDesc* d, uint8_t f) {
    if (d && d->src && d->size && GetCurrentThreadId() == g_renderTid.load(std::memory_order_relaxed)) {
        const uint8_t* p = d->src + d->offset;
        if (d->size == off::kViewBlockBytes) { if (!So4StereoInReplay()) g_lastViewSrc = p; So4StereoBeforeUpload(p, d->size, d->size, 0); s_views++; }
        if (So4StereoPatching()) {
            thread_local std::vector<uint8_t> tmp;
            tmp.assign(p, p + d->size);
            So4StereoPatch(tmp.data(), d->size);
            CbDesc c = *d; c.src = tmp.data() - d->offset;
            s_cbPatched++;
            o_Cb(r, buf, &c, f);
            return;
        }
    }
    o_Cb(r, buf, d, f);
}
} // namespace

const uint8_t* EeLastViewBlock() { return g_lastViewSrc; }
void EeNoteRenderThread(DWORD tid) {
    g_renderTid.store(tid, std::memory_order_relaxed);
    static bool listHooked = false;
    if (!listHooked) if (ID3D12GraphicsCommandList* l = EeEngineList()) { listHooked = true; EeHookListMethods(l, tid); LOGI("EE: engine command list %p hooked (render thread %lu)", (void*)l, tid); }
}

bool EeCaptureEye(int e) {
    ID3D12GraphicsCommandList* l = EeEngineList();
    ID3D12Resource* src = g_rt.rt[0];
    static int logged = 0;
    if (logged < 2) {
        ++logged;
        D3D12_RESOURCE_DESC d{}; if (src) d = src->GetDesc();
        LOGI("EE stereo: eye %d capture from %p (%llux%u format %d mips %u) via list %p (last bound list %p)", e, (void*)src,
             (unsigned long long)d.Width, d.Height, int(d.Format), d.MipLevels, (void*)l, (void*)g_rt.list);
    }
    if (!l || !src || !BridgeCaptureEye(l, src, D3D12_RESOURCE_STATE_RENDER_TARGET, e)) { s_capFail++; return false; }
    return true;
}
void EeStereoReady(const HeadFrame& hf, bool hud) { BridgeStereoReady(hf, hud); }
bool EeHudClear(int bg) {
    ID3D12GraphicsCommandList* l = EeEngineList();
    if (!l || !g_rt.rt[0] || !g_rt.rtv0) return false;
    const float c[4] = {float(bg), float(bg), float(bg), 1.0f};
    D3D12_CPU_DESCRIPTOR_HANDLE h{g_rt.rtv0};
    l->ClearRenderTargetView(h, c, 0, nullptr);
    return true;
}
bool EeHudCapture(int bg) {
    ID3D12GraphicsCommandList* l = EeEngineList();
    if (!l || !g_rt.rt[0]) return false;
    return BridgeCaptureEye(l, g_rt.rt[0], D3D12_RESOURCE_STATE_RENDER_TARGET, 2 + (bg & 1));
}
void EeVrsEndOfFrame();
void EeBeforeSubmit() { EeVrsEndOfFrame(); BridgeBeforeGameSubmit(); }

bool EeRenderInstall() {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return Hook(reinterpret_cast<void*>(base + kEeCbUpload), (void*)hk_Cb, (void**)&o_Cb, "EE constant upload");
}
void EeRenderReport(char* buf, size_t size) {
    snprintf(buf, size, "EE render: per-view uploads %llu, patched uploads %llu, eye capture failures %llu",
             (unsigned long long)s_views.exchange(0), (unsigned long long)s_cbPatched.exchange(0), (unsigned long long)s_capFail.exchange(0));
}

} // namespace so4vr

// ---------------------------------------------------------------- foveated rendering (EE): D3D12 VRS tier 2
// Scene passes (render target 0 at the render size, with depth) get a per-eye shading-rate image: full rate inside
// FoveationInner, reduced between the rings, lowest outside FoveationOuter (Foveation = Fixed / Gaze,
// FoveationStrength Low / Medium / High). Everything else (post, UI, our own copies) runs at full rate.
namespace so4vr {
namespace {
struct VrsEye { ID3D12Resource* img = nullptr; UINT tw = 0, th = 0; float key[8] = {}; };
VrsEye g_vrs[2];
UINT g_vrsTile = 0; bool g_vrsChecked = false, g_vrsFailed = false;
ID3D12Device* g_vrsDev = nullptr;
ID3D12GraphicsCommandList* g_vrsBoundList = nullptr; int g_vrsBoundEye = -1;
std::deque<std::pair<ID3D12Resource*, uint64_t>> g_vrsOld;   // upload buffers kept until the GPU is surely done
uint64_t g_vrsFrame = 0;
std::atomic<uint64_t> s_vrsScene{0}, s_vrsOther{0}, s_vrsUploads{0};

bool VrsSupported(ID3D12GraphicsCommandList* l) {
    if (g_vrsChecked) return !g_vrsFailed;
    g_vrsChecked = true;
    if (FAILED(l->GetDevice(IID_PPV_ARGS(&g_vrsDev))) || !g_vrsDev) { g_vrsFailed = true; return false; }
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
    if (FAILED(g_vrsDev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof o6)) || o6.VariableShadingRateTier < D3D12_VARIABLE_SHADING_RATE_TIER_2 || !o6.ShadingRateImageTileSize) {
        LOGW("foveated rendering: the GPU has no variable rate shading tier 2 - off"); g_vrsFailed = true; return false;
    }
    ID3D12GraphicsCommandList5* l5 = nullptr;
    if (FAILED(l->QueryInterface(IID_PPV_ARGS(&l5))) || !l5) { LOGW("foveated rendering: no ID3D12GraphicsCommandList5 - off"); g_vrsFailed = true; return false; }
    l5->Release();
    g_vrsTile = o6.ShadingRateImageTileSize;
    LOGI("foveated rendering (EE): VRS tier 2, %u-pixel tiles", g_vrsTile);
    return true;
}

void RatesFor(int strength, uint8_t& mid, uint8_t& outer) {
    switch (strength) {
    case 0:  mid = D3D12_SHADING_RATE_2X1; outer = D3D12_SHADING_RATE_2X2; break;   // Low
    case 2:  mid = D3D12_SHADING_RATE_2X2; outer = D3D12_SHADING_RATE_4X4; break;   // High
    default: mid = D3D12_SHADING_RATE_2X2; outer = D3D12_SHADING_RATE_4X2; break;   // Medium
    }
}

// Eye e's image for a w x h target; re-uploaded (copy recorded in `l`) only when the pattern changes.
bool EnsureVrsImage(ID3D12GraphicsCommandList* l, int e, UINT w, UINT h) {
    VrsEye& p = g_vrs[e];
    const UINT tw = (w + g_vrsTile - 1) / g_vrsTile, th = (h + g_vrsTile - 1) / g_vrsTile;
    float t[4], cx = 0.5f, cy = 0.5f, aspect = float(w) / float(h ? h : 1);
    if (So4StereoEyeTan(e, t) && t[1] > t[0] && t[2] > t[3]) {
        float gx = 0.0f, gy = 0.0f;
        if (g_config.foveation == 2 && XrGazeTangents(gx, gy)) {
            gx = std::fmin(std::fmax(gx, t[0] * 0.8f), t[1] * 0.8f);
            gy = std::fmin(std::fmax(gy, t[3] * 0.8f), t[2] * 0.8f);
        }
        cx = (gx - t[0]) / (t[1] - t[0]); cy = (t[2] - gy) / (t[2] - t[3]);
        cx = std::floor(cx * tw + 0.5f) / float(tw); cy = std::floor(cy * th + 0.5f) / float(th);
        aspect = (t[1] - t[0]) / (t[2] - t[3]);
    }
    float key[8] = {float(w), float(h), cx, cy, g_config.foveationInner, g_config.foveationOuter, aspect, float(g_config.foveationStrength)};
    if (p.img && p.tw == tw && p.th == th && memcmp(key, p.key, sizeof key) == 0) return true;
    if (!p.img || p.tw != tw || p.th != th) {
        if (p.img) g_vrsOld.push_back({p.img, g_vrsFrame}), p.img = nullptr;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = tw; d.Height = th; d.DepthOrArraySize = 1;
        d.MipLevels = 1; d.Format = DXGI_FORMAT_R8_UINT; d.SampleDesc.Count = 1;
        if (FAILED(g_vrsDev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE, nullptr, IID_PPV_ARGS(&p.img)))) {
            LOGW("foveated rendering: shading-rate image %ux%u failed - off", tw, th); g_vrsFailed = true; return false;
        }
        p.tw = tw; p.th = th;
    }
    // pattern -> upload buffer -> image (recorded in the engine's list, before the pass that uses it)
    const UINT pitch = (tw + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = UINT64(pitch) * th; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buf = nullptr;
    if (FAILED(g_vrsDev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf)))) return false;
    uint8_t mid, outerRate; RatesFor(g_config.foveationStrength, mid, outerRate);
    float inner = g_config.foveationInner, outer = std::fmax(g_config.foveationOuter, inner);
    uint8_t* m = nullptr; D3D12_RANGE none{0, 0};
    if (FAILED(buf->Map(0, &none, reinterpret_cast<void**>(&m)))) { buf->Release(); return false; }
    for (UINT y = 0; y < th; ++y)
        for (UINT x = 0; x < tw; ++x) {
            float u = (x + 0.5f) / tw, v = (y + 0.5f) / th;
            float dx = (u - cx) * 2.0f, dy = (v - cy) * 2.0f / aspect;
            float r = std::sqrt(dx * dx + dy * dy);
            m[size_t(y) * pitch + x] = r < inner ? uint8_t(D3D12_SHADING_RATE_1X1) : (r < outer ? mid : outerRate);
        }
    buf->Unmap(0, nullptr);
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = p.img;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    l->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = p.img; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
    src.pResource = buf; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8_UINT, tw, th, 1, pitch};
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    l->ResourceBarrier(1, &b);
    g_vrsOld.push_back({buf, g_vrsFrame});
    memcpy(p.key, key, sizeof key);
    static unsigned perEye[2] = {};
    s_vrsUploads++;
    if (++perEye[e] <= 3) LOGI("foveated rendering: eye %d pattern %ux%u tiles, centre (%.3f, %.3f), rings %.2f / %.2f, strength %d",
                               e, tw, th, cx, cy, inner, outer, g_config.foveationStrength);
    return true;
}

void VrsUnbind(ID3D12GraphicsCommandList* l) {
    if (g_vrsBoundList != l || g_vrsBoundEye < 0) return;
    ID3D12GraphicsCommandList5* l5 = nullptr;
    if (SUCCEEDED(l->QueryInterface(IID_PPV_ARGS(&l5))) && l5) { l5->RSSetShadingRateImage(nullptr); l5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr); l5->Release(); }
    g_vrsBoundEye = -1;
}
} // namespace

void EeVrsOnSetRT(ID3D12GraphicsCommandList* l, ID3D12Resource* rt0, bool depth) {
    if (g_vrsBoundList != l) { g_vrsBoundList = l; g_vrsBoundEye = -1; }   // a new list starts with VRS off
    const bool want = g_config.foveation != 0 && !g_vrsFailed && g_config.activeMode != Config::Flat && g_config.activeMode != Config::Mono;
    bool scene = false; UINT w = 0, h = 0;
    if (want && rt0 && depth) {
        D3D12_RESOURCE_DESC d = rt0->GetDesc();
        uint32_t rw = 0, rh = 0; RenderResCurrent(rw, rh);
        scene = rw && d.Width == rw && d.Height == rh && d.SampleDesc.Count == 1;
        w = UINT(d.Width); h = d.Height;
    }
    if (!scene) { if (want) s_vrsOther++; VrsUnbind(l); return; }
    if (!VrsSupported(l)) return;
    s_vrsScene++;
    int e = So4StereoCurrentEye(); if (e < 0) e = 0;
    if (!EnsureVrsImage(l, e, w, h)) { VrsUnbind(l); return; }
    if (g_vrsBoundEye == e) return;
    ID3D12GraphicsCommandList5* l5 = nullptr;
    if (FAILED(l->QueryInterface(IID_PPV_ARGS(&l5))) || !l5) return;
    const D3D12_SHADING_RATE_COMBINER comb[2] = {D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, D3D12_SHADING_RATE_COMBINER_OVERRIDE};
    l5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, comb);
    l5->RSSetShadingRateImage(g_vrs[e].img);
    l5->Release();
    g_vrsBoundEye = e;
}

// Frame end (render thread, before the game submits): VRS off in the open list, old upload buffers released.
void EeVrsEndOfFrame() {
    if (ID3D12GraphicsCommandList* l = EeEngineList()) VrsUnbind(l);
    ++g_vrsFrame;
    while (!g_vrsOld.empty() && g_vrsFrame - g_vrsOld.front().second > 8) { g_vrsOld.front().first->Release(); g_vrsOld.pop_front(); }
}
void EeVrsReport(char* buf, size_t size) {
    snprintf(buf, size, "foveated rendering (EE): %s, scene binds %llu, other binds %llu, pattern uploads %llu",
             g_config.foveation ? (g_vrsFailed ? "FAILED/unsupported" : "on") : "off",
             (unsigned long long)s_vrsScene.exchange(0), (unsigned long long)s_vrsOther.exchange(0), (unsigned long long)s_vrsUploads.exchange(0));
}
} // namespace so4vr
