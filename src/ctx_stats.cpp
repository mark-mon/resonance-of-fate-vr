// Command-submission instrumentation.
//
// The D3D11 runtime can swap a context's vtable at runtime (for example when multithread
// protection is switched on), so hooks placed on the vtable seen at device creation may
// stop seeing calls. This file watches the vtable pointer of the immediate context and of
// every deferred context, and hooks each distinct vtable it sees ("variant"), counting calls
// per variant so the log shows exactly which path the game uses.
#include <windows.h>
#include <d3d11_4.h>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <vector>
#include <unordered_map>
#include "ctx_stats.h"
#include "native_stereo.h"
#include "so4_camera.h"
#include "so4_stereo.h"
#include "hud_layer.h"
#include "config.h"
#include "hook_util.h"
#include "config.h"
#include "log.h"
#include "game_offsets.h"
#include "stack_trace.h"
#include "vrs.h"
#include "temporal.h"

namespace so4vr {
namespace {

// ID3D11DeviceContext vtable indices (verified against d3d11.h)
constexpr int kDrawIndexed = 12, kDraw = 13, kMap = 14, kDrawIndexedInstanced = 20, kDrawInstanced = 21,
              kOMSetRenderTargets = 33, kDrawIndexedInstancedIndirect = 39, kDrawInstancedIndirect = 40,
              kDispatch = 41, kCSSetUnorderedAccessViews = 68, kUpdateSubresource = 48, kClearRenderTargetView = 50, kExecuteCommandList = 58, kFinishCommandList = 114;
// ID3D11Device / Device1 / Device2 / Device3 CreateDeferredContext* indices
constexpr int kCreateDeferredContext = 27, kCreateDeferredContext1 = 44, kCreateDeferredContext2 = 51,
              kCreateDeferredContext3 = 62;

constexpr int kMaxVariants = 4; // per context kind
enum Kind { Imm = 0, Def = 1, NumKinds };
enum Counter { Draw, Dispatch, RT, Clear, Map, ExecCL, FinishCL, NumCounters };

std::atomic<uint64_t> g_frame[NumKinds][kMaxVariants][NumCounters];
uint64_t g_interval[NumKinds][kMaxVariants][NumCounters];

std::mutex g_mutex;
void* g_seenVtables[NumKinds][kMaxVariants] = {};
int   g_numVariants[NumKinds] = {};
bool  g_variantFull[NumKinds] = {};
std::atomic<uint64_t> g_deferredCreated{0};
ID3D11Device* g_device = nullptr;

// Frame-start notification: armed at Present, fired at the first render-target bind afterwards.
std::atomic<bool> g_frameStartArmed{false};
std::atomic<uint64_t> g_gdCalls[16] = {}, g_gdFalse[16] = {};
void (*g_frameStartCb)() = nullptr;

thread_local uint64_t t_immDraws = 0;
// The mod's XR thread has its own D3D11 device, but its immediate context shares the hooked vtable: its calls
// (reprojection Map/Unmap/Draw) must not touch the stereo patcher's state (they consumed the render thread's
// per-block state and patched the wrong data -> one-frame glitches). Only the game device's context is ours.
inline bool GameCtx(ID3D11DeviceContext* c) { ID3D11DeviceContext* g = So4StereoGameContext(); return !g || c == g; }
template <int K, int V> inline void Bump(Counter c) { g_frame[K][V][c].fetch_add(1, std::memory_order_relaxed); if (c == Draw) ++t_immDraws; }
inline void VrsBind(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d) {
    ID3D11Resource* res = nullptr;
    if (n >= 1 && r && r[0]) { r[0]->GetResource(&res); if (res) res->Release(); }   // identity only
    VrsOnSetRT(c, res, d);
}
// Per-eye history tracking (pass 1 of stereo frames only).
inline void TemporalRT(UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d) {
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL && r) for (UINT i = 0; i < n && i < 8; ++i) if (r[i]) TemporalOnWriteView(r[i]);
    if (d) TemporalOnWriteView(d);
}

using PFN_DrawIndexed = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_Draw = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using PFN_Map = HRESULT (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using PFN_DrawIndexedInstanced = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN_DrawInstanced = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PFN_OMSetRenderTargets = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using PFN_GetData = HRESULT (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*, void*, UINT, UINT);
using PFN_OMSetBlend = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
using PFN_OMSetRTUAV = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using PFN_DrawIndirect = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using PFN_Dispatch = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using PFN_ClearRTV = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
using PFN_ExecuteCommandList = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using PFN_FinishCommandList = HRESULT (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
using PFN_CreateDeferred = HRESULT (STDMETHODCALLTYPE*)(void*, UINT, void**);

using PFN_Unmap = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
thread_local ID3D11Resource* g_mapRes = nullptr; thread_local void* g_mapPtr = nullptr;
// Constant buffers the stereo patcher has modified IN the mapped memory. A later Map with WRITE / NO_OVERWRITE
// (not DISCARD) hands the game the previous contents, i.e. our per-eye patch; a partial update then left
// eye-patched rows in the "raw" data (patched twice / leaked into the other eye). Keep the raw contents and put
// them back into the mapped memory before the game sees it (ReplayMapShadow).
std::unordered_map<ID3D11Resource*, std::vector<uint8_t>> g_cbShadow;
std::atomic<uint64_t> s_mapTypes[6], s_mapRestored{0};
uint64_t CtxMapStats(char* buf, size_t n) {
    snprintf(buf, n, "constant buffer maps: discard %llu, no-overwrite %llu, write %llu, other %llu; raw contents restored %llu",
             (unsigned long long)s_mapTypes[D3D11_MAP_WRITE_DISCARD].exchange(0), (unsigned long long)s_mapTypes[D3D11_MAP_WRITE_NO_OVERWRITE].exchange(0),
             (unsigned long long)s_mapTypes[D3D11_MAP_WRITE].exchange(0), (unsigned long long)(s_mapTypes[1].exchange(0) + s_mapTypes[3].exchange(0)),
             (unsigned long long)s_mapRestored.exchange(0));
    return 0;
}
using PFN_UpdateSubresource = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);

// Diagnostic: who uploads the per-frame scene/view constant buffers, and what do they contain?
void InspectUpdate(ID3D11Resource* r, const void* data) {
    if (!g_config.debugStacks || !data || NativeGameplayFrames() < 300) return;
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return;
    D3D11_BUFFER_DESC d; static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
    if (d.ByteWidth != 2224 && d.ByteWidth != 2208 && d.ByteWidth != 2000 && d.ByteWidth != 816 && d.ByteWidth != 1040) return;
    char tag[64]; snprintf(tag, sizeof(tag), "UpdateSubresource %u-byte buffer", d.ByteWidth);
    LogStackOnce(tag, 2);
    static int dumped[5] = {0}, seen[5] = {0};
    int slot = d.ByteWidth == 2224 ? 0 : d.ByteWidth == 2208 ? 1 : d.ByteWidth == 2000 ? 2 : d.ByteWidth == 816 ? 3 : 4;
    if (NativeGameplayFrames() < 300) return;
    ++seen[slot];
    bool want = slot == 0 ? (seen[0] % 97) == 0 && dumped[0] < 8 : (slot == 3 || slot == 4) ? (seen[slot] % 41) == 0 && dumped[slot] < 4 : dumped[slot] < 1;
    if (want) {
        ++dumped[slot];
        const float* f = static_cast<const float*>(data);
        int nf = int(d.ByteWidth / 4) & ~15;
        LOGI("%s contents (first %d floats):", tag, nf);
        for (int i = 0; i < nf; i += 16) {
            char line[512]; int n = snprintf(line, sizeof(line), "  +%04X:", i * 4);
            for (int j = 0; j < 16; ++j) n += snprintf(line + n, sizeof(line) - n, " %.5g", f[i + j]);
            LOGI("%s", line);
        }
    }
}

// ---- UAV snapshots for the second-eye pass ----
using PFN_CSSetUAVs = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using PFN_SetSRVs = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using PFN_CopyResource = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using PFN_CopySubresourceRegion = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
using PFN_ClearUAVu = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11UnorderedAccessView*, const UINT[4]);
using PFN_ClearUAVf = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11UnorderedAccessView*, const FLOAT[4]);
using PFN_ClearDSV = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
struct UavSnap { ID3D11Resource* res; ID3D11Resource* backup; bool takenThisFrame; };
std::vector<UavSnap> g_uavSnaps;
std::atomic<ID3D11DeviceContext*> g_imm{nullptr};

ID3D11Resource* CloneResource(ID3D11Device* dev, ID3D11Resource* r) {
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d; static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
        if (d.ByteWidth > g_config.replayUavMaxBytes) return nullptr;
        d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0;
        ID3D11Buffer* b = nullptr; if (FAILED(dev->CreateBuffer(&d, nullptr, &b))) return nullptr; return b;
    }
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d; static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
        if (uint64_t(d.Width) * d.Height * d.ArraySize > g_config.replayUavMaxTexels) return nullptr;
        d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0;
        ID3D11Texture2D* t = nullptr; if (FAILED(dev->CreateTexture2D(&d, nullptr, &t))) return nullptr; return t;
    }
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        D3D11_TEXTURE3D_DESC d; static_cast<ID3D11Texture3D*>(r)->GetDesc(&d);
        if (uint64_t(d.Width) * d.Height * d.Depth > g_config.replayUavMaxTexels) return nullptr;
        d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0;
        ID3D11Texture3D* t = nullptr; if (FAILED(dev->CreateTexture3D(&d, nullptr, &t))) return nullptr; return t;
    }
    return nullptr;
}

void SnapshotUavs(ID3D11DeviceContext* c, UINT n, ID3D11UnorderedAccessView* const* uavs) {
    if (!uavs || !g_config.replayRestoreState || !NativeReplayEnabled() || NativeInReplay()) return;
    for (UINT i = 0; i < n; ++i) {
        if (!uavs[i]) continue;
        ID3D11Resource* r = nullptr; uavs[i]->GetResource(&r);
        if (!r) continue;
        UavSnap* s = nullptr;
        for (auto& e : g_uavSnaps) if (e.res == r) { s = &e; break; }
        if (!s) {
            if (g_uavSnaps.size() >= 256) { r->Release(); continue; }
            ID3D11Device* dev = nullptr; c->GetDevice(&dev);
            ID3D11Resource* b = dev ? CloneResource(dev, r) : nullptr;
            if (dev) dev->Release();
            r->AddRef();   // keep the game's resource alive while we track it
            g_uavSnaps.push_back({r, b, false});
            s = &g_uavSnaps.back();
        }
        if (s->backup && !s->takenThisFrame) { c->CopyResource(s->backup, s->res); s->takenThisFrame = true; }
        r->Release();
    }
}

// ---- non-constant buffer uploads of the frame, re-applied before the second-eye pass ----
struct BufUpload { ID3D11Resource* res; UINT sub; bool hasBox; D3D11_BOX box; std::vector<uint8_t> data; UINT rp, dp; };
std::vector<BufUpload> g_bufUploads;
void RecordBufferUpload(ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp) {
    if (!g_config.replayReuploadBuffers || !NativeReplayEnabled() || NativeInReplay() || !data) return;
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return;
    D3D11_BUFFER_DESC d; static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
    if (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) return;
    if (!(d.BindFlags & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS))) return;
    UINT bytes = box ? (box->right - box->left) : d.ByteWidth;
    if (bytes == 0 || bytes > (4u << 20) || g_bufUploads.size() >= 64) return;
    BufUpload u; u.res = r; r->AddRef(); u.sub = sub; u.hasBox = box != nullptr; if (box) u.box = *box; u.rp = rp; u.dp = dp;
    u.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
    g_bufUploads.push_back(std::move(u));
}

// One set of detours + trampolines per (context kind, vtable variant).
template <int K, int V>
struct CtxHooks {
    static inline PFN_DrawIndexed DrawIndexed_o;
    static inline PFN_Draw Draw_o;
    static inline PFN_Map Map_o;
    static inline PFN_DrawIndexedInstanced DrawIndexedInstanced_o;
    static inline PFN_DrawInstanced DrawInstanced_o;
    static inline PFN_OMSetRenderTargets OMSetRenderTargets_o;
    static inline PFN_OMSetRTUAV OMSetRTUAV_o;
    static inline PFN_OMSetBlend OMSetBlend_o;
    static inline PFN_GetData GetData_o;
    static HRESULT STDMETHODCALLTYPE GetDataF(ID3D11DeviceContext* c, ID3D11Asynchronous* a, void* d, UINT sz, UINT fl) {
        HRESULT hr = GetData_o(c, a, d, sz, fl);
        if (K == Imm && GameCtx(c)) {
            int kind = -1;
            ID3D11Query* q = nullptr;
            if (a && SUCCEEDED(a->QueryInterface(__uuidof(ID3D11Query), (void**)&q)) && q) { D3D11_QUERY_DESC qd; q->GetDesc(&qd); kind = qd.Query; q->Release(); }
            if (kind >= 0 && kind < 16) { g_gdCalls[kind]++; if (hr == S_FALSE) g_gdFalse[kind]++; }
            if (hr == S_FALSE && g_config.queryWait && kind >= 0) {
                // Wait for the result instead of letting the game fall back (diagnostic / fix for UI flicker).
                for (int i = 0; i < 2000 && hr == S_FALSE; ++i) { hr = GetData_o(c, a, d, sz, 0); if (hr == S_FALSE) YieldProcessor(); }
            }
        }
        return hr;
    }
    static void STDMETHODCALLTYPE OMSetBlend(ID3D11DeviceContext* c, ID3D11BlendState* bs, const FLOAT f[4], UINT m) {
        if (K == Imm && GameCtx(c)) bs = HudOnSetBlend(c, bs, f, m);
        OMSetBlend_o(c, bs, f, m);
    }
    static void STDMETHODCALLTYPE OMSetRTUAV(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d, UINT us, UINT un, ID3D11UnorderedAccessView* const* u, const UINT* ic) {
        Bump<K, V>(RT);
        if (K == Imm && GameCtx(c) && g_latTrack && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
            ID3D11Resource* res = nullptr;
            if (n >= 1 && r && r[0]) { r[0]->GetResource(&res); if (res) res->Release(); }
            NativeOnSetRT(res);
            NativeOnPassTarget(res, d);
        }
        if (K == Imm && GameCtx(c) && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL && g_config.foveation) VrsBind(c, n, r, d);
        if (K == Imm && GameCtx(c) && TemporalActive()) {
            TemporalRT(n, r, d);
            if (un != D3D11_KEEP_UNORDERED_ACCESS_VIEWS && u) for (UINT i = 0; i < un; ++i) if (u[i]) TemporalOnUavView(u[i]);
        }
        ID3D11RenderTargetView* scratch[8];
        if (K == Imm && GameCtx(c) && g_hudFrame && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) HudOnSetRT(c, n, r, scratch);
        OMSetRTUAV_o(c, n, r, d, us, un, u, ic);
    }
    static inline PFN_DrawIndirect DrawIndexedInstancedIndirect_o;
    static inline PFN_DrawIndirect DrawInstancedIndirect_o;
    static inline PFN_Dispatch Dispatch_o;
    static inline PFN_ClearRTV ClearRTV_o;
    static inline PFN_ExecuteCommandList ExecuteCommandList_o;
    static inline PFN_FinishCommandList FinishCommandList_o;
    static inline PFN_UpdateSubresource UpdateSubresource_o;
    static inline PFN_CSSetUAVs CSSetUAVs_o;
    static void STDMETHODCALLTYPE CSSetUAVsF(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11UnorderedAccessView* const* u, const UINT* counts) {
        if (K == Imm && GameCtx(c)) { g_imm.store(c); SnapshotUavs(c, n, u); }
        if (K == Imm && GameCtx(c) && TemporalActive() && u) for (UINT i = 0; i < n; ++i) if (u[i]) TemporalOnUavView(u[i]);
        CSSetUAVs_o(c, start, n, u, counts);
    }
    static inline PFN_SetSRVs PSSetSRVs_o, VSSetSRVs_o, CSSetSRVs_o;
    static void SrvReads(UINT n, ID3D11ShaderResourceView* const* v) { if (v) for (UINT i = 0; i < n; ++i) if (v[i]) TemporalOnReadView(v[i]); }
    static void STDMETHODCALLTYPE PSSetSRVsF(ID3D11DeviceContext* c, UINT s0, UINT n, ID3D11ShaderResourceView* const* v) { if (K == Imm && GameCtx(c) && TemporalActive()) SrvReads(n, v); PSSetSRVs_o(c, s0, n, v); }
    static void STDMETHODCALLTYPE VSSetSRVsF(ID3D11DeviceContext* c, UINT s0, UINT n, ID3D11ShaderResourceView* const* v) { if (K == Imm && GameCtx(c) && TemporalActive()) SrvReads(n, v); VSSetSRVs_o(c, s0, n, v); }
    static void STDMETHODCALLTYPE CSSetSRVsF(ID3D11DeviceContext* c, UINT s0, UINT n, ID3D11ShaderResourceView* const* v) { if (K == Imm && GameCtx(c) && TemporalActive()) SrvReads(n, v); CSSetSRVs_o(c, s0, n, v); }
    static inline PFN_CopyResource CopyResource_o;
    static inline PFN_CopySubresourceRegion CopySubresourceRegion_o;
    static void STDMETHODCALLTYPE CopyResourceF(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
        if (K == Imm && GameCtx(c) && TemporalActive()) { TemporalOnRead(src); TemporalOnWrite(dst); }
        CopyResource_o(c, dst, src);
    }
    static void STDMETHODCALLTYPE CopySubresourceRegionF(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT ss, const D3D11_BOX* b) {
        if (K == Imm && GameCtx(c) && TemporalActive()) { TemporalOnRead(src); TemporalOnWrite(dst); }
        CopySubresourceRegion_o(c, dst, ds, x, y, z, src, ss, b);
    }
    static inline PFN_ClearUAVu ClearUAVu_o;
    static inline PFN_ClearUAVf ClearUAVf_o;
    static inline PFN_ClearDSV ClearDSV_o;
    static void STDMETHODCALLTYPE ClearUAVuF(ID3D11DeviceContext* c, ID3D11UnorderedAccessView* u, const UINT v[4]) { if (K == Imm && GameCtx(c) && TemporalActive()) TemporalOnWriteView(u); ClearUAVu_o(c, u, v); }
    static void STDMETHODCALLTYPE ClearUAVfF(ID3D11DeviceContext* c, ID3D11UnorderedAccessView* u, const FLOAT v[4]) { if (K == Imm && GameCtx(c) && TemporalActive()) TemporalOnWriteView(u); ClearUAVf_o(c, u, v); }
    static void STDMETHODCALLTYPE ClearDSVF(ID3D11DeviceContext* c, ID3D11DepthStencilView* d, UINT f, FLOAT z, UINT8 st) { if (K == Imm && GameCtx(c) && TemporalActive()) TemporalOnWriteView(d); ClearDSV_o(c, d, f, z, st);
    }
    static void STDMETHODCALLTYPE UpdateSubresourceF(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp) {
        if (K == Imm && GameCtx(c) && data && GameCtx(c)) {
            D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
                D3D11_BUFFER_DESC d; static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
                if (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) {
                    UINT bytes = box ? (box->right - box->left) : d.ByteWidth;
                    So4StereoBeforeUpload(data, bytes, d.ByteWidth, box ? box->left : 0);   // SO4: frame identification + left-eye start
                    So4StereoScanUpload(data, bytes);
                    if (So4StereoPatching()) {
                        thread_local std::vector<uint8_t> tmp;
                        tmp.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
                        So4StereoPatch(tmp.data(), bytes);
                        So4UploadTap(r, data, bytes, d.ByteWidth, 0);
                        UpdateSubresource_o(c, r, sub, box, tmp.data(), rp, dp);
                        return;
                    }
                }
            }
        }
        if (K == Imm && GameCtx(c)) RecordBufferUpload(r, sub, box, data, rp, dp);
        if (K == Imm && GameCtx(c)) {
            InspectUpdate(r, data);
            if (data) {
                D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
                if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
                    D3D11_BUFFER_DESC d; static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
                    UINT bytes = box ? (box->right - box->left) : d.ByteWidth;
                    NativeScanUpload(data, bytes, d.ByteWidth, "UpdateSubresource");
                    if (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) So4UploadTap(r, data, bytes, d.ByteWidth, 0);
                }
            }
        }
        UpdateSubresource_o(c, r, sub, box, data, rp, dp);
    }

    static void STDMETHODCALLTYPE DrawIndexed(ID3D11DeviceContext* c, UINT a, UINT b, INT d) { Bump<K, V>(Draw); if (K == Imm && GameCtx(c) && g_latWantSrv) NativeOnDraw(c); if (K == Imm && GameCtx(c) && g_hudActive && HudBeforeDraw()) return; DrawIndexed_o(c, a, b, d); if (K == Imm && GameCtx(c) && g_hudActive) HudAfterDraw(c); }
    static void STDMETHODCALLTYPE DrawF(ID3D11DeviceContext* c, UINT a, UINT b) { Bump<K, V>(Draw); if (K == Imm && GameCtx(c) && g_latWantSrv) NativeOnDraw(c); if (K == Imm && GameCtx(c) && g_hudActive && HudBeforeDraw()) return; Draw_o(c, a, b); if (K == Imm && GameCtx(c) && g_hudActive) HudAfterDraw(c); }
    static HRESULT STDMETHODCALLTYPE MapF(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s, D3D11_MAP t, UINT f, D3D11_MAPPED_SUBRESOURCE* m) {
        Bump<K, V>(Map);
        HRESULT hr = Map_o(c, r, s, t, f, m);
        if (K == Imm && GameCtx(c) && SUCCEEDED(hr) && m && (t == D3D11_MAP_WRITE_DISCARD || t == D3D11_MAP_WRITE_NO_OVERWRITE || t == D3D11_MAP_WRITE)) {
            D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
                g_mapRes = r; g_mapPtr = m->pData;
                D3D11_BUFFER_DESC d; static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
                if (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) {
                    if (unsigned(t) < 6) s_mapTypes[t]++;
                    if (t != D3D11_MAP_WRITE_DISCARD && g_config.replayMapShadow) {
                        auto it = g_cbShadow.find(r);
                        if (it != g_cbShadow.end() && it->second.size() == d.ByteWidth) { memcpy(m->pData, it->second.data(), d.ByteWidth); s_mapRestored++; }
                    }
                }
            }
        }
        return hr;
    }
    static inline PFN_Unmap Unmap_o;
    static void STDMETHODCALLTYPE UnmapF(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s) {
        if (K == Imm && GameCtx(c) && r == g_mapRes && g_mapPtr && GameCtx(c)) {
            D3D11_BUFFER_DESC d; static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
            if (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) {
                if (d.ByteWidth == off::kViewBlockBytes) So4StereoBeforeUpload(g_mapPtr, d.ByteWidth, d.ByteWidth);   // per-view block via Map
                if (So4StereoPatching()) {
                    if (g_config.replayMapShadow) g_cbShadow[r].assign(static_cast<uint8_t*>(g_mapPtr), static_cast<uint8_t*>(g_mapPtr) + d.ByteWidth);
                    So4StereoPatch(g_mapPtr, d.ByteWidth);
                } else if (g_config.replayMapShadow) { auto it = g_cbShadow.find(r); if (it != g_cbShadow.end()) it->second.assign(static_cast<uint8_t*>(g_mapPtr), static_cast<uint8_t*>(g_mapPtr) + d.ByteWidth); }
                else NativeScanUpload(g_mapPtr, d.ByteWidth, d.ByteWidth, "Map");
                So4UploadTap(r, g_mapPtr, d.ByteWidth, d.ByteWidth, 1);
            }
            g_mapRes = nullptr; g_mapPtr = nullptr;
        }
        Unmap_o(c, r, s);
    }
    static void STDMETHODCALLTYPE DrawIndexedInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f) { Bump<K, V>(Draw); if (K == Imm && GameCtx(c) && g_latWantSrv) NativeOnDraw(c); if (K == Imm && GameCtx(c) && g_hudActive && HudBeforeDraw()) return; DrawIndexedInstanced_o(c, a, b, d, e, f); if (K == Imm && GameCtx(c) && g_hudActive) HudAfterDraw(c); }
    static void STDMETHODCALLTYPE DrawInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e) { Bump<K, V>(Draw); if (K == Imm && GameCtx(c) && g_latWantSrv) NativeOnDraw(c); if (K == Imm && GameCtx(c) && g_hudActive && HudBeforeDraw()) return; DrawInstanced_o(c, a, b, d, e); if (K == Imm && GameCtx(c) && g_hudActive) HudAfterDraw(c); }
    static void STDMETHODCALLTYPE OMSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d) {
        Bump<K, V>(RT);
        if (K == Imm && GameCtx(c) && g_frameStartArmed.load(std::memory_order_relaxed) && g_frameStartArmed.exchange(false)) {
            if (g_frameStartCb) g_frameStartCb();
            static int frames = 0;
            if (++frames > 600) LogStackOnce("first OMSetRenderTargets of frame", 3);
        }
        if (K == Imm && GameCtx(c) && g_config.debugStacks && n >= 1 && r && r[0] && NativeGameplayFrames() > 600) {
            // Diagnostic: who renders into full-res single-channel R16_FLOAT targets (the 2137 pass)?
            ID3D11Resource* res = nullptr; r[0]->GetResource(&res);
            if (res) {
                D3D11_RESOURCE_DIMENSION dim; res->GetType(&dim);
                if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                    D3D11_TEXTURE2D_DESC td; static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
                    if (td.Format == DXGI_FORMAT_R16_FLOAT && td.Width >= 1280) LogStackOnce(NativePatchActive() || NativeInReplay() ? "R16F full-res RT (replay)" : "R16F full-res RT", 3);
                }
                res->Release();
            }
        }
        if (K == Imm && GameCtx(c)) {
            static std::atomic<uint32_t> rtCount{0};
            uint32_t c = ++rtCount;
            if (c > 60000 && (c % 997) == 0) LogStackOnce("OMSetRenderTargets (sampled)", 12);
        }
        if (K == Imm && GameCtx(c) && g_latTrack) {
            ID3D11Resource* res = nullptr;
            if (n >= 1 && r && r[0]) { r[0]->GetResource(&res); if (res) res->Release(); }  // identity only
            NativeOnSetRT(res);
            NativeOnPassTarget(res, d);
        }
        if (K == Imm && GameCtx(c) && g_config.foveation) VrsBind(c, n, r, d);
        if (K == Imm && GameCtx(c) && TemporalActive()) TemporalRT(n, r, d);
        ID3D11RenderTargetView* scratch[8];
        if (K == Imm && GameCtx(c) && g_hudFrame) HudOnSetRT(c, n, r, scratch);
        OMSetRenderTargets_o(c, n, r, d);
    }
    static void STDMETHODCALLTYPE DrawIndexedInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) { Bump<K, V>(Draw); if (K == Imm && GameCtx(c) && g_hudActive && HudBeforeDraw()) return; DrawIndexedInstancedIndirect_o(c, b, o); if (K == Imm && GameCtx(c) && g_hudActive) HudAfterDraw(c); }
    static void STDMETHODCALLTYPE DrawInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) { Bump<K, V>(Draw); if (K == Imm && GameCtx(c) && g_hudActive && HudBeforeDraw()) return; DrawInstancedIndirect_o(c, b, o); if (K == Imm && GameCtx(c) && g_hudActive) HudAfterDraw(c); }
    static void STDMETHODCALLTYPE DispatchF(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) { Bump<K, V>(Dispatch); Dispatch_o(c, x, y, z); }
    static void STDMETHODCALLTYPE ClearRTV(ID3D11DeviceContext* c, ID3D11RenderTargetView* r, const FLOAT col[4]) { Bump<K, V>(Clear); if (K == Imm && GameCtx(c) && TemporalActive()) TemporalOnWriteView(r); ClearRTV_o(c, r, col); }
    static void STDMETHODCALLTYPE ExecuteCommandList(ID3D11DeviceContext* c, ID3D11CommandList* cl, BOOL restore) { Bump<K, V>(ExecCL); ExecuteCommandList_o(c, cl, restore); }
    static HRESULT STDMETHODCALLTYPE FinishCommandList(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** cl) { Bump<K, V>(FinishCL); return FinishCommandList_o(c, restore, cl); }

    static void Install(ID3D11DeviceContext* c) {
        char n[64];
        const char* kn = K == Imm ? "Imm" : "Def";
        int shared = 0;
        auto h = [&](int idx, void* det, void** orig, const char* name) {
            snprintf(n, sizeof(n), "%s%d::%s", kn, V, name);
            if (HookEx(VtableEntry(c, idx), det, orig, n) == HookResult::AlreadyHooked) ++shared;
        };
        h(kDrawIndexed, (void*)DrawIndexed, (void**)&DrawIndexed_o, "DrawIndexed");
        h(kDraw, (void*)DrawF, (void**)&Draw_o, "Draw");
        h(kMap, (void*)MapF, (void**)&Map_o, "Map");
        h(15, (void*)UnmapF, (void**)&Unmap_o, "Unmap");
        h(kDrawIndexedInstanced, (void*)DrawIndexedInstanced, (void**)&DrawIndexedInstanced_o, "DrawIndexedInstanced");
        h(kDrawInstanced, (void*)DrawInstanced, (void**)&DrawInstanced_o, "DrawInstanced");
        h(kOMSetRenderTargets, (void*)OMSetRenderTargets, (void**)&OMSetRenderTargets_o, "OMSetRenderTargets");
        h(34, (void*)OMSetRTUAV, (void**)&OMSetRTUAV_o, "OMSetRenderTargetsAndUnorderedAccessViews");
        h(35, (void*)OMSetBlend, (void**)&OMSetBlend_o, "OMSetBlendState");
        h(29, (void*)GetDataF, (void**)&GetData_o, "GetData");
        h(kDrawIndexedInstancedIndirect, (void*)DrawIndexedInstancedIndirect, (void**)&DrawIndexedInstancedIndirect_o, "DrawIndexedInstancedIndirect");
        h(kDrawInstancedIndirect, (void*)DrawInstancedIndirect, (void**)&DrawInstancedIndirect_o, "DrawInstancedIndirect");
        h(kDispatch, (void*)DispatchF, (void**)&Dispatch_o, "Dispatch");
        h(kClearRenderTargetView, (void*)ClearRTV, (void**)&ClearRTV_o, "ClearRenderTargetView");
        h(kUpdateSubresource, (void*)UpdateSubresourceF, (void**)&UpdateSubresource_o, "UpdateSubresource");
        h(kCSSetUnorderedAccessViews, (void*)CSSetUAVsF, (void**)&CSSetUAVs_o, "CSSetUnorderedAccessViews");
        if (K == Imm) {
            h(8, (void*)PSSetSRVsF, (void**)&PSSetSRVs_o, "PSSetShaderResources");
            h(25, (void*)VSSetSRVsF, (void**)&VSSetSRVs_o, "VSSetShaderResources");
            h(67, (void*)CSSetSRVsF, (void**)&CSSetSRVs_o, "CSSetShaderResources");
            h(46, (void*)CopySubresourceRegionF, (void**)&CopySubresourceRegion_o, "CopySubresourceRegion");
            h(47, (void*)CopyResourceF, (void**)&CopyResource_o, "CopyResource");
            h(51, (void*)ClearUAVuF, (void**)&ClearUAVu_o, "ClearUnorderedAccessViewUint");
            h(52, (void*)ClearUAVfF, (void**)&ClearUAVf_o, "ClearUnorderedAccessViewFloat");
            h(53, (void*)ClearDSVF, (void**)&ClearDSV_o, "ClearDepthStencilView");
        }
        if (K == Imm) h(kExecuteCommandList, (void*)ExecuteCommandList, (void**)&ExecuteCommandList_o, "ExecuteCommandList");
        else          h(kFinishCommandList, (void*)FinishCommandList, (void**)&FinishCommandList_o, "FinishCommandList");
        if (shared) LOGI("  %s%d: %d entries share code with an earlier variant (counted there)", kn, V, shared);
    }
};

template <int K>
void InstallVariant(int v, ID3D11DeviceContext* c) {
    switch (v) {
    case 0: CtxHooks<K, 0>::Install(c); break;
    case 1: CtxHooks<K, 1>::Install(c); break;
    case 2: CtxHooks<K, 2>::Install(c); break;
    case 3: CtxHooks<K, 3>::Install(c); break;
    }
}

// Hooks `c`'s vtable if it is one we have not seen for this kind of context.
void CheckContext(ID3D11DeviceContext* c, Kind k, const char* why) {
    if (!c) return;
    void* vt = *reinterpret_cast<void**>(c);
    // Fast path without the lock: already known?
    for (int i = 0; i < g_numVariants[k]; ++i) if (g_seenVtables[k][i] == vt) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    for (int i = 0; i < g_numVariants[k]; ++i) if (g_seenVtables[k][i] == vt) return;
    if (g_numVariants[k] >= kMaxVariants) {
        if (!g_variantFull[k]) { LOGW("more than %d %s vtables seen; ignoring new ones", kMaxVariants, k == Imm ? "immediate" : "deferred"); g_variantFull[k] = true; }
        return;
    }
    int v = g_numVariants[k];
    void** e = *reinterpret_cast<void***>(c);
    LOGI("%s context vtable variant %d = %p (%s): DrawIndexed=%p Map=%p OMSetRenderTargets=%p ExecuteCommandList=%p",
         k == Imm ? "immediate" : "deferred", v, vt, why, e[kDrawIndexed], e[kMap], e[kOMSetRenderTargets], e[kExecuteCommandList]);
    if (k == Imm) InstallVariant<Imm>(v, c); else InstallVariant<Def>(v, c);
    g_seenVtables[k][v] = vt;
    g_numVariants[k] = v + 1;
}

// ---- deferred context creation (all four API versions) ----
PFN_CreateDeferred o_CreateDeferred[4] = {};

template <int N>
HRESULT STDMETHODCALLTYPE hk_CreateDeferred(void* dev, UINT flags, void** out) {
    HRESULT hr = o_CreateDeferred[N](dev, flags, out);
    uint64_t n = ++g_deferredCreated;
    if (n <= 32) LOGI("CreateDeferredContext%s #%llu on thread %lu -> 0x%08lx ctx=%p", N == 0 ? "" : (N == 1 ? "1" : (N == 2 ? "2" : "3")),
                      (unsigned long long)n, GetCurrentThreadId(), hr, out ? *out : nullptr);
    if (SUCCEEDED(hr) && out && *out) CheckContext(static_cast<ID3D11DeviceContext*>(*out), Def, "created by game");
    return hr;
}

// Diagnostic: who creates the game's full-size render targets (to find where the render resolution comes from).
using PFN_CreateTex2D = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
PFN_CreateTex2D o_CreateTex2D = nullptr;
HRESULT STDMETHODCALLTYPE hk_CreateTex2D(ID3D11Device* d, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    if (desc && (desc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) && desc->Width >= 960) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 200)
            LOGI("CreateTexture2D RT %ux%u fmt %d mips %u array %u bind 0x%x thread %lu", desc->Width, desc->Height, desc->Format, desc->MipLevels, desc->ArraySize, desc->BindFlags, GetCurrentThreadId());
        char tag[64]; snprintf(tag, sizeof(tag), "CreateTexture2D RT %ux%u", desc->Width, desc->Height);
        LogStackOnce(tag, 2);
    }
    return o_CreateTex2D(d, desc, init, out);
}

void HookDeferredCreation(ID3D11Device* dev) {
    if (g_config.debugStacks) Hook(VtableEntry(dev, 5), (void*)hk_CreateTex2D, (void**)&o_CreateTex2D, "Device::CreateTexture2D");
    Hook(VtableEntry(dev, kCreateDeferredContext), (void*)hk_CreateDeferred<0>, (void**)&o_CreateDeferred[0], "Device::CreateDeferredContext");
    struct { REFIID iid; int idx; void* det; int n; const char* name; } v[] = {
        {__uuidof(ID3D11Device1), kCreateDeferredContext1, (void*)hk_CreateDeferred<1>, 1, "Device1::CreateDeferredContext1"},
        {__uuidof(ID3D11Device2), kCreateDeferredContext2, (void*)hk_CreateDeferred<2>, 2, "Device2::CreateDeferredContext2"},
        {__uuidof(ID3D11Device3), kCreateDeferredContext3, (void*)hk_CreateDeferred<3>, 3, "Device3::CreateDeferredContext3"},
    };
    for (auto& x : v) {
        IUnknown* d = nullptr;
        if (SUCCEEDED(dev->QueryInterface(x.iid, (void**)&d)) && d) {
            Hook(VtableEntry(d, x.idx), x.det, (void**)&o_CreateDeferred[x.n], x.name);
            d->Release();
        }
    }
}

bool g_installed = false;

} // namespace
void CtxMapReport(char* b, size_t n) { CtxMapStats(b, n); }

void CtxStatsInstall(ID3D11Device* dev) {
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_installed || !dev) return;
        g_installed = true;
        g_device = dev;
        g_device->AddRef();
    }
    ID3D11DeviceContext* imm = nullptr;
    dev->GetImmediateContext(&imm);
    CheckContext(imm, Imm, "at device creation");
    if (imm) imm->Release();

    // Probe deferred context so the deferred vtable is hooked before the game makes its own.
    ID3D11DeviceContext* def = nullptr;
    HRESULT hr = dev->CreateDeferredContext(0, &def);
    if (SUCCEEDED(hr) && def) { CheckContext(def, Def, "probe"); def->Release(); }
    else LOGW("could not create a probe deferred context (0x%08lx)", hr);

    HookDeferredCreation(dev);
}

void CtxStatsWatchDevice(ID3D11Device* dev) {
    if (!dev) return;
    ID3D11DeviceContext* imm = nullptr;
    dev->GetImmediateContext(&imm);
    CheckContext(imm, Imm, "additional device");
    if (imm) imm->Release();
}

void CtxStatsSetFrameStartCallback(void (*cb)()) { g_frameStartCb = cb; }
void CtxStatsArmFrameStart() { g_frameStartArmed.store(true); }

void CtxRestoreUavSnapshots(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    for (auto& e : g_uavSnaps) if (e.backup && e.takenThisFrame) ctx->CopyResource(e.res, e.backup);
}
void CtxNewFrameUavSnapshots() {
    for (auto& e : g_uavSnaps) e.takenThisFrame = false;
    for (auto& u : g_bufUploads) u.res->Release();
    g_bufUploads.clear();
}
void CtxReapplyBufferUploads(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    for (auto& u : g_bufUploads) ctx->UpdateSubresource(u.res, u.sub, u.hasBox ? &u.box : nullptr, u.data.data(), u.rp, u.dp);
}
ID3D11DeviceContext* CtxImmediate() { return g_imm.load(); }

void CtxStatsEndFrame() {
    // Re-check the immediate context's vtable every frame; the runtime may have swapped it.
    if (g_device) {
        ID3D11DeviceContext* imm = nullptr;
        g_device->GetImmediateContext(&imm);
        CheckContext(imm, Imm, "changed at runtime");
        if (imm) imm->Release();
    }
    for (int k = 0; k < NumKinds; ++k)
        for (int v = 0; v < kMaxVariants; ++v)
            for (int c = 0; c < NumCounters; ++c)
                g_interval[k][v][c] += g_frame[k][v][c].exchange(0, std::memory_order_relaxed);
}

void CtxStatsReport(char* buf, size_t size, double frames) {
    size_t used = 0;
    buf[0] = 0;
    for (int k = 0; k < NumKinds; ++k) {
        for (int v = 0; v < g_numVariants[k]; ++v) {
            auto avg = [&](Counter c) { return frames > 0 ? double(g_interval[k][v][c]) / frames : 0.0; };
            int w = snprintf(buf + used, size - used,
                             "%s%s%d: %.0f draws, %.0f dispatches, %.0f RT, %.0f clears, %.0f maps, %.1f exec, %.1f finish",
                             used ? " | " : "", k == Imm ? "imm" : "def", v,
                             avg(Draw), avg(Dispatch), avg(RT), avg(Clear), avg(Map), avg(ExecCL), avg(FinishCL));
            if (w < 0 || (size_t)w >= size - used) { used = size - 1; break; }
            used += (size_t)w;
        }
    }
    if (used + 48 < size) {
        int w = snprintf(buf + used, size - used, " | %llu deferred contexts created | GetData (type: calls/not ready)", (unsigned long long)g_deferredCreated.load());
        if (w > 0) used += (size_t)w;
        for (int q = 0; q < 16 && used + 32 < size; ++q) {
            uint64_t c = g_gdCalls[q].exchange(0), f = g_gdFalse[q].exchange(0);
            if (!c) continue;
            w = snprintf(buf + used, size - used, " %d:%llu/%llu", q, (unsigned long long)c, (unsigned long long)f);
            if (w > 0) used += (size_t)w;
        }
    }
    for (auto& a : g_interval) for (auto& b : a) for (auto& c : b) c = 0;
}

uint64_t CtxImmDraws() { return t_immDraws; }   // this thread's immediate-context draws
} // namespace so4vr
