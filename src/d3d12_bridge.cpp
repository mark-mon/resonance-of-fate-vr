// Enhanced Edition (Direct3D 12) - stage 1: get the game's frames into the existing XR path.
//
// The EE renders with D3D12. The XR side of this mod (xr_runtime.cpp, the settings menu, HUD layer code) works
// with D3D11 textures on "the game device". For the EE that device is a private D3D11 "bridge" device on the
// same GPU. Each Present, the back buffer is copied (on the game's own command queue) into a shared D3D12
// texture that the bridge device opens; a shared fence orders the two:
//   D3D12 queue: wait(2k)  -> copy -> signal(2k+1)
//   bridge D3D11: wait(2k+1) -> XR copy -> signal(2k+2)
// Hooks: CreateDXGIFactory1/2 -> IDXGIFactory2::CreateSwapChainForHwnd / IDXGIFactory::CreateSwapChain (to get
// the command queue) -> IDXGISwapChain::Present / Present1 / ResizeBuffers.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <string>
#include <vector>
#include "MinHook.h"
#include "hook_util.h"
#include "log.h"
#include "config.h"
#include "xr_runtime.h"
#include "ee_render.h"
#include "head_tracking.h"
#include "so4_stereo.h"
#include "so4_headtrack.h"
#include "so4_camera.h"
#include "so4_hud.h"

namespace so4vr {
namespace {

constexpr int kFactory_CreateSwapChain = 10, kFactory2_CreateSwapChainForHwnd = 15;
constexpr int kSwapChain_Present = 8, kSwapChain_ResizeBuffers = 13, kSwapChain1_Present1 = 22;

using PFN_CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using PFN_CreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PFN_CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PFN_ResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PFN_CreateFactory1 o_CreateFactory1 = nullptr;
PFN_CreateFactory2 o_CreateFactory2 = nullptr;
PFN_CreateSwapChain o_CreateSwapChain = nullptr;
PFN_CreateSwapChainForHwnd o_CreateSwapChainForHwnd = nullptr;
PFN_Present o_Present = nullptr;
PFN_Present1 o_Present1 = nullptr;
PFN_ResizeBuffers o_ResizeBuffers = nullptr;

std::mutex g_m;
std::atomic<bool> g_factoryHooked{false}, g_swapHooked{false};
ID3D12CommandQueue* g_queue = nullptr;   // the queue the game's swapchain presents on (AddRef'd)
IDXGISwapChain* g_sc = nullptr;          // the game's swapchain (not AddRef'd; identity only)
thread_local int t_inPresent = 0;

// ---- bridge state (Present thread only) ----
struct Bridge {
    ID3D12Device* dev12 = nullptr;
    ID3D11Device5* dev11 = nullptr;
    ID3D11DeviceContext4* ctx11 = nullptr;
    ID3D12Fence* fence12 = nullptr;
    ID3D11Fence* fence11 = nullptr;
    UINT64 fenceVal = 0;                 // last value signalled by the D3D11 side (even)
    static constexpr int kAlloc = 3;
    ID3D12CommandAllocator* alloc[kAlloc] = {};
    UINT64 allocDone[kAlloc] = {};       // fence value that marks the allocator's last use as finished
    int allocIdx = 0;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* shared12 = nullptr;
    ID3D11Texture2D* shared11 = nullptr;
    UINT w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    HANDLE evt = nullptr;
    bool failed = false;
} g_b;
std::atomic<uint64_t> s_frames{0}, s_skipped{0};
void D3D12BridgeReportImpl(char* buf, size_t size);

void ReleaseShared() {
    if (g_b.shared11) { g_b.shared11->Release(); g_b.shared11 = nullptr; }
    if (g_b.shared12) { g_b.shared12->Release(); g_b.shared12 = nullptr; }
    g_b.w = g_b.h = 0;
}

bool InitBridge() {
    if (g_b.dev11) return true;
    if (g_b.failed || !g_queue) return false;
    g_b.failed = true;   // until everything below worked
    if (FAILED(g_queue->GetDevice(IID_PPV_ARGS(&g_b.dev12)))) { LOGE("bridge: no D3D12 device from the queue"); return false; }
    D3D12_COMMAND_QUEUE_DESC qd = g_queue->GetDesc();
    LUID luid = g_b.dev12->GetAdapterLuid();
    IDXGIFactory4* f = nullptr;
    HRESULT hr = o_CreateFactory2 ? o_CreateFactory2(0, IID_PPV_ARGS(&f)) : CreateDXGIFactory1(IID_PPV_ARGS(&f));
    IDXGIAdapter1* ad = nullptr;
    if (SUCCEEDED(hr) && f) f->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad));
    if (f) f->Release();
    D3D_FEATURE_LEVEL lv[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D11Device* d = nullptr; ID3D11DeviceContext* c = nullptr;
    hr = D3D11CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           lv, 2, D3D11_SDK_VERSION, &d, nullptr, &c);
    if (ad) ad->Release();
    if (FAILED(hr) || !d) { LOGE("bridge: D3D11CreateDevice failed (0x%08lx)", hr); return false; }
    d->QueryInterface(IID_PPV_ARGS(&g_b.dev11)); c->QueryInterface(IID_PPV_ARGS(&g_b.ctx11));
    d->Release(); c->Release();
    if (!g_b.dev11 || !g_b.ctx11) { LOGE("bridge: D3D11.4 interfaces not available"); return false; }
    if (FAILED(g_b.dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_b.fence12)))) { LOGE("bridge: CreateFence failed"); return false; }
    HANDLE hf = nullptr;
    if (FAILED(g_b.dev12->CreateSharedHandle(g_b.fence12, nullptr, GENERIC_ALL, nullptr, &hf)) ||
        FAILED(g_b.dev11->OpenSharedFence(hf, IID_PPV_ARGS(&g_b.fence11)))) { if (hf) CloseHandle(hf); LOGE("bridge: shared fence failed"); return false; }
    CloseHandle(hf);
    for (auto& a : g_b.alloc)
        if (FAILED(g_b.dev12->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&a)))) { LOGE("bridge: CreateCommandAllocator failed"); return false; }
    if (FAILED(g_b.dev12->CreateCommandList(0, qd.Type, g_b.alloc[0], nullptr, IID_PPV_ARGS(&g_b.list)))) { LOGE("bridge: CreateCommandList failed"); return false; }
    g_b.list->Close();
    g_b.evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_b.failed = false;
    LOGI("bridge: D3D11 device on the game's GPU (luid %08lx:%08lx), queue type %d", luid.HighPart, luid.LowPart, int(qd.Type));
    return true;
}

bool EnsureShared(const D3D12_RESOURCE_DESC& bd) {
    if (g_b.shared11 && g_b.w == bd.Width && g_b.h == bd.Height && g_b.fmt == bd.Format) return true;
    ReleaseShared();
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = bd.Width; d.Height = bd.Height; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = bd.Format; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    HRESULT hr = g_b.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_b.shared12));
    if (FAILED(hr)) { LOGE("bridge: shared texture %llux%u format %d failed (0x%08lx)", (unsigned long long)bd.Width, bd.Height, int(bd.Format), hr); return false; }
    HANDLE h = nullptr;
    hr = g_b.dev12->CreateSharedHandle(g_b.shared12, nullptr, GENERIC_ALL, nullptr, &h);
    if (SUCCEEDED(hr)) hr = g_b.dev11->OpenSharedResource1(h, IID_PPV_ARGS(&g_b.shared11));
    if (h) CloseHandle(h);
    if (FAILED(hr) || !g_b.shared11) { LOGE("bridge: opening the shared texture in D3D11 failed (0x%08lx)", hr); ReleaseShared(); return false; }
    g_b.w = UINT(bd.Width); g_b.h = bd.Height; g_b.fmt = bd.Format;
    LOGI("bridge: frame texture %ux%u format %d", g_b.w, g_b.h, int(g_b.fmt));
    return true;
}

// Diagnostic (DebugEeProbe): which resources the game maps for CPU access (where do the per-draw records go?)
using PFN_Map = HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**);
PFN_Map o_Map = nullptr;
} // namespace
uintptr_t g_bigMap = 0; uint64_t g_bigMapSize = 0;   // largest mapped upload buffer (EE probe)
namespace {
std::atomic<int> s_mapLogged{0};
HRESULT STDMETHODCALLTYPE hk_Map(ID3D12Resource* r, UINT sub, const D3D12_RANGE* rr, void** out) {
    HRESULT hr = o_Map(r, sub, rr, out);
    if (SUCCEEDED(hr) && out && *out && s_mapLogged < 300) {
        D3D12_RESOURCE_DESC d = r->GetDesc();
        D3D12_HEAP_PROPERTIES hp{}; D3D12_HEAP_FLAGS hf{};
        r->GetHeapProperties(&hp, &hf);
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && d.Width >= 0x10000) {
            s_mapLogged++;
            if (d.Width >= (256ull << 20)) { g_bigMap = reinterpret_cast<uintptr_t>(*out); g_bigMapSize = d.Width; }
            LOGI("probe map: buffer %p %llu KB heap type %d cpu page %d pool %d -> cpu %p (thread %lu)", (void*)r, (unsigned long long)(d.Width >> 10),
                 int(hp.Type), int(hp.CPUPageProperty), int(hp.MemoryPoolPreference), *out, GetCurrentThreadId());
        }
    }
    return hr;
}
void HookMap(ID3D12Device* dev) {
    static bool done = false; if (done || !dev) return; done = true;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = 256; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* b = nullptr;
    if (SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&b))) && b) {
        Hook(VtableEntry(b, 8), (void*)hk_Map, (void**)&o_Map, "ID3D12Resource::Map (probe)");
        b->Release();
    }
}
// device creation: hook Map before the game creates and maps its buffers; log heaps (probe)
using PFN_D3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
PFN_D3D12CreateDevice o_D3D12CreateDevice = nullptr;
using PFN_CreateHeap = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_DESC*, REFIID, void**);
PFN_CreateHeap o_CreateHeap = nullptr;
HRESULT STDMETHODCALLTYPE hk_CreateHeap(ID3D12Device* d, const D3D12_HEAP_DESC* hd, REFIID riid, void** out) {
    HRESULT hr = o_CreateHeap(d, hd, riid, out);
    if (hd && hd->SizeInBytes >= (16u << 20))
        LOGI("probe heap: %llu MB type %d cpu page %d pool %d flags 0x%x -> %08lx", (unsigned long long)(hd->SizeInBytes >> 20), int(hd->Properties.Type),
             int(hd->Properties.CPUPageProperty), int(hd->Properties.MemoryPoolPreference), unsigned(hd->Flags), hr);
    return hr;
}
HRESULT WINAPI hk_D3D12CreateDevice(IUnknown* ad, D3D_FEATURE_LEVEL fl, REFIID riid, void** out) {
    HRESULT hr = o_D3D12CreateDevice(ad, fl, riid, out);
    if (SUCCEEDED(hr) && out && *out && !g_config.debugEeProbe) {
        ID3D12Device* d = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&d))) && d) { EeRenderOnDevice(d); d->Release(); }
    }
    if (SUCCEEDED(hr) && out && *out && g_config.debugEeProbe) {
        ID3D12Device* d = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&d))) && d) {
            EeRenderOnDevice(d);
            HookMap(d);
            static bool h = false;
            if (!h) { h = true; Hook(VtableEntry(d, 28), (void*)hk_CreateHeap, (void**)&o_CreateHeap, "ID3D12Device::CreateHeap (probe)"); }
            d->Release();
        }
    }
    return hr;
}
void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    g_b.list->ResourceBarrier(1, &b);
}

// ---- native stereo (EE): eye images copied inside the game's own command list, published at Present ----
struct EyeTex { ID3D12Resource* t12 = nullptr; ID3D11Texture2D* t11 = nullptr; UINT64 w = 0; UINT h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN; };
EyeTex g_eye[4];   // 0/1 eyes, 2/3 final target with the UI over black / white (HUD matte)
std::atomic<bool> g_stereoReady{false}; bool g_stereoHud = false;
HeadFrame g_stereoHf{};
std::atomic<uint64_t> s_eyeCopies{0}, s_stereoPub{0};
bool EnsureEye(int e, const D3D12_RESOURCE_DESC& sd) {
    EyeTex& t = g_eye[e];
    if (t.t11 && t.w == sd.Width && t.h == sd.Height && t.fmt == sd.Format) return true;
    if (t.t11) { t.t11->Release(); t.t11 = nullptr; }
    if (t.t12) { t.t12->Release(); t.t12 = nullptr; }
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = sd.Width; d.Height = sd.Height; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = sd.Format; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    HRESULT hr = g_b.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t.t12));
    if (FAILED(hr)) { LOGE("bridge: eye texture %llux%u format %d failed (0x%08lx)", (unsigned long long)sd.Width, sd.Height, int(sd.Format), hr); return false; }
    HANDLE h = nullptr;
    hr = g_b.dev12->CreateSharedHandle(t.t12, nullptr, GENERIC_ALL, nullptr, &h);
    if (SUCCEEDED(hr)) hr = g_b.dev11->OpenSharedResource1(h, IID_PPV_ARGS(&t.t11));
    if (h) CloseHandle(h);
    if (FAILED(hr) || !t.t11) { LOGE("bridge: opening the eye texture in D3D11 failed (0x%08lx)", hr); t.t12->Release(); t.t12 = nullptr; return false; }
    t.w = sd.Width; t.h = sd.Height; t.fmt = sd.Format;
    LOGI("bridge: eye %d texture %llux%u format %d", e, (unsigned long long)t.w, t.h, int(t.fmt));
    return true;
}
// Diagnostic: rof_dump_eyes.txt in the game folder -> both eye images written as rofvr_ee_eye_<n>_left/right.bmp
std::wstring g_gameDirW;
void DumpEyeBmp(ID3D11Texture2D* t, const wchar_t* name) {
    D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_b.dev11->CreateTexture2D(&sd, nullptr, &st)) || !st) return;
    g_b.ctx11->CopyResource(st, t);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(g_b.ctx11->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        const UINT w = d.Width, h = d.Height, rowOut = (w * 3 + 3) & ~3u;
        std::vector<uint8_t> px(size_t(rowOut) * h);
        for (UINT y = 0; y < h; ++y) {
            const uint32_t* src = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch);
            uint8_t* dst = px.data() + size_t(h - 1 - y) * rowOut;
            for (UINT x = 0; x < w; ++x) {
                uint32_t v = src[x]; uint8_t r, g, b;
                if (d.Format == DXGI_FORMAT_R10G10B10A2_UNORM || d.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS) { r = uint8_t((v & 0x3FF) >> 2); g = uint8_t(((v >> 10) & 0x3FF) >> 2); b = uint8_t(((v >> 20) & 0x3FF) >> 2); }
                else if (d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) { b = uint8_t(v); g = uint8_t(v >> 8); r = uint8_t(v >> 16); }
                else { r = uint8_t(v); g = uint8_t(v >> 8); b = uint8_t(v >> 16); }
                dst[x * 3] = b; dst[x * 3 + 1] = g; dst[x * 3 + 2] = r;
            }
        }
        g_b.ctx11->Unmap(st, 0);
        std::wstring path = g_gameDirW + L"\\" + name;
        if (FILE* f = _wfopen(path.c_str(), L"wb")) {
            BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
            fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + DWORD(px.size());
            ih.biSize = sizeof ih; ih.biWidth = LONG(w); ih.biHeight = LONG(h); ih.biPlanes = 1; ih.biBitCount = 24; ih.biSizeImage = DWORD(px.size());
            fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(px.data(), 1, px.size(), f); fclose(f);
        }
    }
    st->Release();
}
// Diagnostic: rof_dump_seq.txt -> the next 40 stereo frames' left eye at 1/8 size (rofvr_seq_NN.bmp) plus
// rofvr_seq.csv with each frame's head record (pose, fov, projection) - to compare image motion with the poses.
int g_seqLeft = 0, g_seqN = 0;
void DumpSeqFrame() {
    ID3D11Texture2D* t = g_eye[0].t11; if (!t) return;
    D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_b.dev11->CreateTexture2D(&sd, nullptr, &st)) || !st) return;
    g_b.ctx11->CopyResource(st, t);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(g_b.ctx11->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        const UINT w = d.Width / 8, h = d.Height / 8, rowOut = (w * 3 + 3) & ~3u;
        std::vector<uint8_t> px(size_t(rowOut) * h);
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x) {
                const uint32_t v = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m.pData) + size_t(y * 8 + 4) * m.RowPitch)[x * 8 + 4];
                uint8_t* o = px.data() + size_t(h - 1 - y) * rowOut + x * 3;
                o[2] = uint8_t((v & 0x3FF) >> 2); o[1] = uint8_t(((v >> 10) & 0x3FF) >> 2); o[0] = uint8_t(((v >> 20) & 0x3FF) >> 2);
            }
        g_b.ctx11->Unmap(st, 0);
        wchar_t nm[64]; swprintf(nm, 64, L"\\rofvr_seq_%02d.bmp", g_seqN);
        if (FILE* f = _wfopen((g_gameDirW + nm).c_str(), L"wb")) {
            BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
            fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + DWORD(px.size());
            ih.biSize = sizeof ih; ih.biWidth = LONG(w); ih.biHeight = LONG(h); ih.biPlanes = 1; ih.biBitCount = 24; ih.biSizeImage = DWORD(px.size());
            fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(px.data(), 1, px.size(), f); fclose(f);
        }
    }
    st->Release();
    const HeadFrame& h = g_stereoHf;
    if (FILE* f = _wfopen((g_gameDirW + L"\\rofvr_seq.csv").c_str(), g_seqN ? L"a" : L"w")) {
        if (!g_seqN) fprintf(f, "n,tick_us,seq,poseTime,baseTime,hqx,hqy,hqz,hqw,eqx,eqy,eqz,eqw,fovL,fovR,fovU,fovD,p00,p11,p02,p12,v0,v1,v2,v4,v5,v6,v8,v9,v10,vnh0,vnh2,vnh8,vnh10\n");
        LARGE_INTEGER c, fq; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&fq);
        fprintf(f, "%d,%lld,%llu,%lld,%lld,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f\n", g_seqN,
                (long long)(c.QuadPart * 1000000 / fq.QuadPart), (unsigned long long)h.seq, (long long)h.poseTime, (long long)h.baseTime,
                h.pose.orientation.x, h.pose.orientation.y, h.pose.orientation.z, h.pose.orientation.w,
                h.eyePose[0].orientation.x, h.eyePose[0].orientation.y, h.eyePose[0].orientation.z, h.eyePose[0].orientation.w,
                h.eyeFov[0].angleLeft, h.eyeFov[0].angleRight, h.eyeFov[0].angleUp, h.eyeFov[0].angleDown,
                h.proj[0], h.proj[5], h.proj[8], h.proj[9],
                h.view[0], h.view[1], h.view[2], h.view[4], h.view[5], h.view[6], h.view[8], h.view[9], h.view[10],
                h.viewNoHead[0], h.viewNoHead[2], h.viewNoHead[8], h.viewNoHead[10]);
        fclose(f);
    }
    ++g_seqN;
}
void MaybeDumpEyes() {
    static ULONGLONG last = 0; ULONGLONG now = GetTickCount64();
    if (now - last < 500 && g_seqLeft == 0) return; last = now;
    if (g_gameDirW.empty()) { wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH); g_gameDirW = p; g_gameDirW = g_gameDirW.substr(0, g_gameDirW.find_last_of(L'\\')); }
    {
        std::wstring sq = g_gameDirW + L"\\rof_dump_seq.txt";
        if (GetFileAttributesW(sq.c_str()) != INVALID_FILE_ATTRIBUTES) { DeleteFileW(sq.c_str()); g_seqLeft = 40; g_seqN = 0; LOGI("bridge: sequence dump started"); }
    }
    std::wstring req = g_gameDirW + L"\\rof_dump_eyes.txt";
    if (GetFileAttributesW(req.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileW(req.c_str());
    static int n = 0; ++n; wchar_t nm[64];
    swprintf(nm, 64, L"rofvr_ee_eye_%02d_left.bmp", n); DumpEyeBmp(g_eye[0].t11, nm);
    swprintf(nm, 64, L"rofvr_ee_eye_%02d_right.bmp", n); DumpEyeBmp(g_eye[1].t11, nm);
    swprintf(nm, 64, L"rofvr_ee_eye_%02d_desktop.bmp", n); DumpEyeBmp(g_b.shared11, nm);
    if (g_eye[2].t11) { swprintf(nm, 64, L"rofvr_ee_eye_%02d_hud_black.bmp", n); DumpEyeBmp(g_eye[2].t11, nm); }
    if (g_eye[3].t11) { swprintf(nm, 64, L"rofvr_ee_eye_%02d_hud_white.bmp", n); DumpEyeBmp(g_eye[3].t11, nm); }
    LOGI("bridge: eye images dumped (#%d)", n);
}
} // namespace

// Render thread, inside the game's frame: copy `src` (in `srcState`) into eye e's texture through the game's open list.
bool BridgeCaptureEye(ID3D12GraphicsCommandList* l, ID3D12Resource* src, D3D12_RESOURCE_STATES srcState, int e) {
    if (!l || !src || e < 0 || e > 3 || !InitBridge()) return false;
    D3D12_RESOURCE_DESC sd = src->GetDesc();
    if (sd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sd.SampleDesc.Count != 1 || !EnsureEye(e, sd)) return false;
    D3D12_RESOURCE_BARRIER b[2]{};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[0].Transition.pResource = src; b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = srcState; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[1].Transition.pResource = g_eye[e].t12; b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    l->ResourceBarrier(srcState == D3D12_RESOURCE_STATE_COPY_SOURCE ? 1 : 2, srcState == D3D12_RESOURCE_STATE_COPY_SOURCE ? &b[1] : b);
    if (sd.MipLevels == 1 && sd.DepthOrArraySize == 1) l->CopyResource(g_eye[e].t12, src);
    else {   // first subresource only
        D3D12_TEXTURE_COPY_LOCATION dst{}, from{};
        dst.pResource = g_eye[e].t12; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
        from.pResource = src; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = 0;
        l->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
    }
    std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
    l->ResourceBarrier(srcState == D3D12_RESOURCE_STATE_COPY_SOURCE ? 1 : 2, srcState == D3D12_RESOURCE_STATE_COPY_SOURCE ? &b[1] : b);
    s_eyeCopies++;
    return true;
}
// Render thread: both eyes of this frame are recorded; Present hands them to the XR runtime.
void BridgeStereoReady(const HeadFrame& hf, bool hud) { g_stereoHf = hf; g_stereoHud = hud; g_stereoReady = true; }
// Render thread, before the game submits a frame's lists: they must not overwrite the eye textures while the
// D3D11 side still reads the previous frame's.
void BridgeBeforeGameSubmit() { if (g_queue && g_b.fence12 && g_b.fenceVal) g_queue->Wait(g_b.fence12, g_b.fenceVal); }

namespace {
void OnFrame(IDXGISwapChain* sc) {
    if (!g_config.vrOutput || !InitBridge()) return;
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return;
    ID3D12Resource* bb = nullptr;
    HRESULT hr = sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
    sc3->Release();
    if (FAILED(hr) || !bb) return;
    D3D12_RESOURCE_DESC bd = bb->GetDesc();
    if (bd.SampleDesc.Count != 1 || !EnsureShared(bd)) { bb->Release(); return; }
    // allocator reuse: wait until its last list finished on the GPU
    int ai = g_b.allocIdx; g_b.allocIdx = (ai + 1) % Bridge::kAlloc;
    if (g_b.fence12->GetCompletedValue() < g_b.allocDone[ai]) {
        g_b.fence12->SetEventOnCompletion(g_b.allocDone[ai], g_b.evt);
        if (WaitForSingleObject(g_b.evt, 100) != WAIT_OBJECT_0) { s_skipped++; bb->Release(); return; }
    }
    g_b.alloc[ai]->Reset();
    g_b.list->Reset(g_b.alloc[ai], nullptr);
    Barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(g_b.shared12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    g_b.list->CopyResource(g_b.shared12, bb);
    Barrier(g_b.shared12, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    g_b.list->Close();
    // the D3D11 side must be done with the previous frame before we overwrite the shared texture
    if (g_b.fenceVal) g_queue->Wait(g_b.fence12, g_b.fenceVal);
    ID3D12CommandList* lists[] = {g_b.list};
    g_queue->ExecuteCommandLists(1, lists);
    const UINT64 copied = g_b.fenceVal + 1;
    g_queue->Signal(g_b.fence12, copied);
    g_b.allocDone[ai] = copied;
    bb->Release();
    // D3D11: wait for the copy, hand the frame to the XR path (it copies it into its own ring), then release
    g_b.ctx11->Wait(g_b.fence11, copied);
    if (g_stereoReady.exchange(false) && g_eye[0].t11 && g_eye[1].t11) {   // (the eye copies ran in the game's lists, before ours)
        ID3D11Texture2D* hud = nullptr;
        if (g_config.hudLayer && g_stereoHud && g_eye[2].t11 && g_eye[3].t11 && So4HudPrepare(g_b.dev11, g_eye[2].t11, g_eye[2].fmt)) {
            So4HudStore(g_b.ctx11, g_eye[2].t11, 0); So4HudStore(g_b.ctx11, g_eye[3].t11, 1);
            if (So4HudBuildMatte(g_b.ctx11)) hud = So4HudTexture(true);
        } else if (g_config.hudLayer) hud = So4HudTexture(false);   // briefly keep the last one
        XrPublishStereo(g_eye[0].t11, g_eye[1].t11, g_stereoHf, hud);
        s_stereoPub++;
        if (g_seqLeft > 0) { --g_seqLeft; DumpSeqFrame(); }
        MaybeDumpEyes();
    }
    XrOnFrameD3D11(g_b.dev11, g_b.shared11);
    g_b.fenceVal = copied + 1;
    g_b.ctx11->Signal(g_b.fence11, g_b.fenceVal);
    g_b.ctx11->Flush();
    s_frames++;
    static ULONGLONG lastStats = GetTickCount64();
    ULONGLONG now = GetTickCount64();
    if (g_config.statsIntervalSec > 0 && now - lastStats >= ULONGLONG(g_config.statsIntervalSec) * 1000) {
        double secs = double(now - lastStats) / 1000.0; lastStats = now;
        char b1[256], b2[1024];
        unsigned long long n = s_frames.load();
        D3D12BridgeReportImpl(b1, sizeof b1); XrReport(b2, sizeof b2);
        LOGI("stats: %.1f fps | %s", double(n) / secs, b1);
        LOGI("  %s", b2);
        if (!g_config.debugEeProbe) {
            char b3[2048]; So4StereoReport(b3, sizeof b3); LOGI("  %s", b3);
            EeRenderReport(b3, sizeof b3); LOGI("  %s", b3);
            So4HtReport(b3, sizeof b3); LOGI("  %s", b3);
            So4CameraReport(b3, sizeof b3); LOGI("  %s", b3);
            So4HudReport(b3, sizeof b3); LOGI("  %s", b3);
            EeVrsReport(b3, sizeof b3); LOGI("  %s", b3);
        }
    }
}

HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (t_inPresent++ == 0 && sc == g_sc && !(flags & DXGI_PRESENT_TEST)) OnFrame(sc);
    HRESULT hr = o_Present(sc, sync, flags);
    --t_inPresent;
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (t_inPresent++ == 0 && sc == g_sc && !(flags & DXGI_PRESENT_TEST)) OnFrame(sc);
    HRESULT hr = o_Present1(sc, sync, flags, p);
    --t_inPresent;
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    LOGI("ResizeBuffers %ux%u format %d buffers %u", w, h, int(f), n);
    return o_ResizeBuffers(sc, n, w, h, f, fl);
}

void NoteSwapChain(IUnknown* device, IDXGISwapChain* sc, const char* how, UINT w, UINT h, DXGI_FORMAT fmt, UINT buffers) {
    ID3D12CommandQueue* q = nullptr;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&q))) || !q) { LOGI("%s: not a D3D12 queue swapchain, ignored", how); return; }
    {
        std::lock_guard<std::mutex> lk(g_m);
        if (g_queue) g_queue->Release();
        g_queue = q; g_sc = sc;
    }
    LOGI("%s: D3D12 swapchain %ux%u format %d, %u buffers (queue %p)", how, w, h, int(fmt), buffers, (void*)q);
    if (g_config.debugEeProbe) { ID3D12Device* d = nullptr; if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&d))) && d) { HookMap(d); d->Release(); } }
    if (!g_swapHooked.exchange(true)) {
        Hook(VtableEntry(sc, kSwapChain_Present), (void*)hk_Present, (void**)&o_Present, "IDXGISwapChain::Present (EE)");
        Hook(VtableEntry(sc, kSwapChain1_Present1), (void*)hk_Present1, (void**)&o_Present1, "IDXGISwapChain1::Present1 (EE)");
        Hook(VtableEntry(sc, kSwapChain_ResizeBuffers), (void*)hk_ResizeBuffers, (void**)&o_ResizeBuffers, "IDXGISwapChain::ResizeBuffers (EE)");
    }
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* out, IDXGISwapChain1** sc) {
    HRESULT hr = o_CreateSwapChainForHwnd(f, dev, hwnd, d, fs, out, sc);
    if (SUCCEEDED(hr) && sc && *sc && d) NoteSwapChain(dev, *sc, "CreateSwapChainForHwnd", d->Width, d->Height, d->Format, d->BufferCount);
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** sc) {
    HRESULT hr = o_CreateSwapChain(f, dev, d, sc);
    if (SUCCEEDED(hr) && sc && *sc && d) NoteSwapChain(dev, *sc, "CreateSwapChain", d->BufferDesc.Width, d->BufferDesc.Height, d->BufferDesc.Format, d->BufferCount);
    return hr;
}

void HookFactory(void* f) {
    if (!f || g_factoryHooked.exchange(true)) return;
    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(f)->QueryInterface(IID_PPV_ARGS(&f2))) && f2) {
        Hook(VtableEntry(f2, kFactory2_CreateSwapChainForHwnd), (void*)hk_CreateSwapChainForHwnd, (void**)&o_CreateSwapChainForHwnd, "IDXGIFactory2::CreateSwapChainForHwnd (EE)");
        Hook(VtableEntry(f2, kFactory_CreateSwapChain), (void*)hk_CreateSwapChain, (void**)&o_CreateSwapChain, "IDXGIFactory::CreateSwapChain (EE)");
        f2->Release();
    }
}
thread_local int t_inFactory = 0;
HRESULT WINAPI hk_CreateFactory1(REFIID riid, void** out) {
    HRESULT hr = o_CreateFactory1(riid, out);
    if (t_inFactory++ == 0 && SUCCEEDED(hr) && out) HookFactory(*out);
    --t_inFactory;
    return hr;
}
HRESULT WINAPI hk_CreateFactory2(UINT flags, REFIID riid, void** out) {
    HRESULT hr = o_CreateFactory2(flags, riid, out);
    if (t_inFactory++ == 0 && SUCCEEDED(hr) && out) HookFactory(*out);
    --t_inFactory;
    return hr;
}

} // namespace

bool InstallD3D12Bridge() {
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { LOGE("MH_Initialize failed (%s)", MH_StatusToString(s)); return false; }
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) { LOGE("EE: dxgi.dll not available"); return false; }
    bool ok = false;
    {   // render target view tracking (always), Map / heap logging (probe)
        HMODULE d12 = LoadLibraryW(L"d3d12.dll");
        if (void* p = d12 ? (void*)GetProcAddress(d12, "D3D12CreateDevice") : nullptr)
            Hook(p, (void*)hk_D3D12CreateDevice, (void**)&o_D3D12CreateDevice, "D3D12CreateDevice (EE)");
    }
    if (void* p = (void*)GetProcAddress(dxgi, "CreateDXGIFactory2")) ok = Hook(p, (void*)hk_CreateFactory2, (void**)&o_CreateFactory2, "CreateDXGIFactory2 (EE)") || ok;
    if (void* p = (void*)GetProcAddress(dxgi, "CreateDXGIFactory1")) ok = Hook(p, (void*)hk_CreateFactory1, (void**)&o_CreateFactory1, "CreateDXGIFactory1 (EE)") || ok;
    return ok;
}

namespace {
void D3D12BridgeReportImpl(char* buf, size_t size) {
    snprintf(buf, size, "EE bridge: %llu frames to VR, %llu skipped, frame %ux%u format %d, stereo %llu (eye copies %llu)",
             (unsigned long long)s_frames.exchange(0), (unsigned long long)s_skipped.exchange(0), g_b.w, g_b.h, int(g_b.fmt),
             (unsigned long long)s_stereoPub.exchange(0), (unsigned long long)s_eyeCopies.exchange(0));
}
} // namespace

} // namespace so4vr
