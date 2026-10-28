// D3D11/DXGI bootstrap hooks + frame statistics (stage-1 instrumentation, still used): observe how SO6 creates its D3D11 device and swapchain,
// hook Present/ResizeBuffers, and gather per-frame statistics. Nothing is changed
// on screen; this exists to prove we are in the render path before adding VR.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <atomic>
#include <mutex>
#include "MinHook.h"
#include "hook_util.h"
#include "ctx_stats.h"
#include "d3d_hooks.h"
#include "config.h"
#include "log.h"
#include "xr_runtime.h"
#include "hud_layer.h"
#include "temporal.h"
#include "reticle.h"
#include "native_stereo.h"
#include "rdoc.h"
#include "camera_probe.h"
#include "head_tracking.h"
#include "stack_trace.h"
#include "so4_camera.h"
#include "so4_headtrack.h"
#include "so4_stereo.h"

namespace so4vr {
void RofInputReport(char* buf, unsigned size);
namespace {

// ---- vtable indices (from the d3d11.h / dxgi.h interface declaration order) ----
constexpr int kFactory_CreateSwapChain         = 10; // IDXGIFactory
constexpr int kFactory2_CreateSwapChainForHwnd = 15; // IDXGIFactory2
constexpr int kSwapChain_Present               = 8;  // IDXGISwapChain
constexpr int kSwapChain_ResizeBuffers         = 13;
constexpr int kSwapChain1_Present1             = 22; // IDXGISwapChain1


std::mutex g_hookMutex;
thread_local int t_inCreate = 0; // suppress double-logging when D3D calls itself internally

// ---- original function pointers ----
using PFN_LoadLibraryExW = HMODULE (WINAPI*)(LPCWSTR, HANDLE, DWORD);
using PFN_CreateFactory1 = HRESULT (WINAPI*)(REFIID, void**);
using PFN_CreateFactory2 = HRESULT (WINAPI*)(UINT, REFIID, void**);
using PFN_CreateSwapChain = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PFN_CreateSwapChainForHwnd = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
    const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using PFN_Present  = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_Present1 = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PFN_ResizeBuffers = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PFN_LoadLibraryExW         o_LoadLibraryExW = nullptr;
PFN_D3D11_CREATE_DEVICE    o_CreateDevice = nullptr;
PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN o_CreateDeviceAndSwapChain = nullptr;
PFN_CreateFactory1         o_CreateFactory1 = nullptr;
PFN_CreateFactory2         o_CreateFactory2 = nullptr;
PFN_CreateSwapChain        o_CreateSwapChain = nullptr;
PFN_CreateSwapChainForHwnd o_CreateSwapChainForHwnd = nullptr;
PFN_Present                o_Present = nullptr;
PFN_Present1               o_Present1 = nullptr;
PFN_ResizeBuffers          o_ResizeBuffers = nullptr;

bool g_d3d11Hooked = false, g_factoryVtHooked = false, g_factory2VtHooked = false;
bool g_swapVtHooked = false, g_swap1VtHooked = false;

// ---- per-frame statistics ----
uint64_t g_frame = 0;
LARGE_INTEGER g_qpf{}, g_lastPresent{}, g_statsStart{};
double g_frameMsMax = 0, g_frameMsSum = 0;
uint64_t g_statsFrames = 0;

const char* FormatName(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    default: return "other";
    }
}

const char* SwapEffectName(DXGI_SWAP_EFFECT e) {
    switch (e) {
    case DXGI_SWAP_EFFECT_DISCARD: return "DISCARD";
    case DXGI_SWAP_EFFECT_SEQUENTIAL: return "SEQUENTIAL";
    case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: return "FLIP_SEQUENTIAL";
    case DXGI_SWAP_EFFECT_FLIP_DISCARD: return "FLIP_DISCARD";
    default: return "?";
    }
}

void LogWindow(HWND hwnd) {
    if (!hwnd) return;
    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, 255);
    RECT rc{}; GetClientRect(hwnd, &rc);
    char t8[512]; WideCharToMultiByte(CP_UTF8, 0, title, -1, t8, sizeof(t8), nullptr, nullptr);
    LOGI("  window hwnd=%p title=\"%s\" client=%ldx%ld", hwnd, t8, rc.right - rc.left, rc.bottom - rc.top);
}

// ---- swapchain hooks ----
void OnPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    RdocOnPresent(NativeGameplayFrames());
    if (g_frame == 0) {
        g_statsStart = now;
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d)))
            LOGI("first Present: %ux%u %s buffers=%u sync=%u flags=0x%x",
                 d.BufferDesc.Width, d.BufferDesc.Height, FormatName(d.BufferDesc.Format), d.BufferCount, sync, flags);
        ID3D11Device* dev = nullptr;
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) && dev) {
            CtxStatsInstall(dev);
            dev->Release();
        }
    } else {
        double ms = double(now.QuadPart - g_lastPresent.QuadPart) * 1000.0 / double(g_qpf.QuadPart);
        g_frameMsSum += ms;
        if (ms > g_frameMsMax) g_frameMsMax = ms;
        g_statsFrames++;
    }
    g_lastPresent = now;
    g_frame++;

    CtxStatsEndFrame();
    CameraProbeOnPresent();
    if (g_config.cameraHook) So4CameraOnPresent();
    if (g_frame > 600) LogStackOnce("Present", 2);
    CtxStatsArmFrameStart(); // next render-target bind = start of the next frame

    // Hotkeys (only while the game window is in front): F8 = flat screen / head tracking, F9 = recentre.
    {
        static bool f8Prev = false, f9Prev = false, f10Prev = false;
        DXGI_SWAP_CHAIN_DESC d{};
        bool focused = SUCCEEDED(sc->GetDesc(&d)) && GetForegroundWindow() == d.OutputWindow;
        bool f8 = focused && (GetAsyncKeyState(VK_F8) & 0x8000);
        bool f9 = focused && (GetAsyncKeyState(VK_F9) & 0x8000);
        if (f8 && !f8Prev) {
            g_config.activeMode = g_config.activeMode == Config::Flat
                ? (g_config.renderMode == Config::Flat ? Config::Mono : g_config.renderMode) : Config::Flat;
            LOGI("F8: mode -> %s", ModeName(g_config.activeMode));
        }
        if (f9 && !f9Prev) { So4HtRequestRecenter(); LOGI("F9: recentre requested"); }
        bool f10 = focused && (GetAsyncKeyState(VK_F10) & 0x8000);
        if (f10 && !f10Prev) XrToggleHud();
        f8Prev = f8; f9Prev = f9; f10Prev = f10;
    }

    if (g_config.statsIntervalSec > 0 && g_statsFrames > 0) {
        double elapsed = double(now.QuadPart - g_statsStart.QuadPart) / double(g_qpf.QuadPart);
        if (elapsed >= g_config.statsIntervalSec) {
            double n = double(g_statsFrames);
            char ctx[2048];
            CtxStatsReport(ctx, sizeof(ctx), n);
            LOGI("stats: frame %llu | %.1f fps | avg %.2f ms, worst %.2f ms",
                 (unsigned long long)g_frame, n / elapsed, g_frameMsSum / n, g_frameMsMax);
            LOGI("  per frame: %s", ctx);
            char ht[2048], xr[512];
            HeadTrackReport(ht, sizeof(ht));
            XrReport(xr, sizeof(xr));
            LOGI("  %s | %s", ht, xr);
            char ns[640]; NativeReport(ns, sizeof(ns));
            LOGI("  %s", ns);
            if (g_config.cameraHook) { char cr[2048]; So4CameraReport(cr, sizeof(cr)); LOGI("  %s", cr); }
            if (HeadTrackedMode()) { char sh[1024]; So4HtReport(sh, sizeof(sh)); LOGI("  %s", sh); }
            if (g_config.activeMode == Config::Native) { char st[2048]; So4StereoReport(st, sizeof(st)); LOGI("  %s", st); }
            char hr[256]; HudReport(hr, sizeof(hr));
            LOGI("  %s", hr);
            { char pr[256]; RofInputReport(pr, sizeof(pr)); LOGI("  %s", pr); }
            if (g_config.activeMode == Config::Native) { char th[256]; TemporalReport(th, sizeof(th)); LOGI("  %s", th); ReticleReport(th, sizeof(th)); LOGI("  %s", th); }
            g_statsStart = now; g_statsFrames = 0; g_frameMsSum = 0; g_frameMsMax = 0;
        }
    }
}

thread_local int t_inPresent = 0; // DXGI may route Present through Present1 internally; count once

HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (t_inPresent++ == 0 && !(flags & DXGI_PRESENT_TEST)) { OnPresent(sc, sync, flags); XrOnPresent(sc); }
    if (g_config.debugPresentSync >= 0 && t_inPresent == 1) sync = UINT(g_config.debugPresentSync);
    HRESULT hr = o_Present(sc, sync, flags);
    --t_inPresent;
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (t_inPresent++ == 0 && !(flags & DXGI_PRESENT_TEST)) { OnPresent(sc, sync, flags); XrOnPresent(sc); }
    if (g_config.debugPresentSync >= 0 && t_inPresent == 1) sync = UINT(g_config.debugPresentSync);
    HRESULT hr = o_Present1(sc, sync, flags, p);
    --t_inPresent;
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    if (g_config.debugSwapW && g_config.debugSwapH && w && h) { LOGI("ResizeBuffers override %ux%u -> %ux%u", w, h, g_config.debugSwapW, g_config.debugSwapH); w = g_config.debugSwapW; h = g_config.debugSwapH; }
    HRESULT hr = o_ResizeBuffers(sc, count, w, h, fmt, flags);
    XrOnResize();
    LOGI("ResizeBuffers count=%u %ux%u fmt=%s(%d) flags=0x%x -> 0x%08lx", count, w, h, FormatName(fmt), fmt, flags, hr);
    return hr;
}

void HookSwapChain(IDXGISwapChain* sc) {
    if (!sc) return;
    std::lock_guard<std::mutex> lk(g_hookMutex);
    if (!g_swapVtHooked) {
        g_swapVtHooked = true;
        Hook(VtableEntry(sc, kSwapChain_Present), (void*)hk_Present, (void**)&o_Present, "IDXGISwapChain::Present");
        Hook(VtableEntry(sc, kSwapChain_ResizeBuffers), (void*)hk_ResizeBuffers, (void**)&o_ResizeBuffers, "IDXGISwapChain::ResizeBuffers");
    }
    if (!g_swap1VtHooked) {
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
            g_swap1VtHooked = true;
            Hook(VtableEntry(sc1, kSwapChain1_Present1), (void*)hk_Present1, (void**)&o_Present1, "IDXGISwapChain1::Present1");
            sc1->Release();
        }
    }
}

void LogSwapChainDesc(const char* via, const DXGI_SWAP_CHAIN_DESC* d) {
    if (!d) return;
    LOGI("swapchain via %s: %ux%u %s(%d) refresh=%u/%u buffers=%u effect=%s windowed=%d flags=0x%x msaa=%u",
         via, d->BufferDesc.Width, d->BufferDesc.Height, FormatName(d->BufferDesc.Format), d->BufferDesc.Format,
         d->BufferDesc.RefreshRate.Numerator, d->BufferDesc.RefreshRate.Denominator, d->BufferCount,
         SwapEffectName(d->SwapEffect), d->Windowed, d->Flags, d->SampleDesc.Count);
    LogWindow(d->OutputWindow);
}

// ---- factory hooks ----
HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    HRESULT hr = o_CreateSwapChain(f, dev, desc, out);
    LogSwapChainDesc("IDXGIFactory::CreateSwapChain", desc);
    LOGI("  -> 0x%08lx swapchain=%p", hr, out ? (void*)*out : nullptr);
    if (SUCCEEDED(hr) && out) HookSwapChain(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out) {
    HRESULT hr = o_CreateSwapChainForHwnd(f, dev, hwnd, d, fs, o, out);
    if (d) LOGI("swapchain via CreateSwapChainForHwnd: %ux%u %s(%d) buffers=%u effect=%s flags=0x%x windowed=%d",
                d->Width, d->Height, FormatName(d->Format), d->Format, d->BufferCount, SwapEffectName(d->SwapEffect),
                d->Flags, fs ? fs->Windowed : 1);
    LogWindow(hwnd);
    LOGI("  -> 0x%08lx swapchain=%p", hr, out ? (void*)*out : nullptr);
    if (SUCCEEDED(hr) && out) HookSwapChain(*out);
    return hr;
}

void HookFactory(void* factory) {
    if (!factory) return;
    std::lock_guard<std::mutex> lk(g_hookMutex);
    IUnknown* unk = static_cast<IUnknown*>(factory);
    if (!g_factoryVtHooked) {
        g_factoryVtHooked = true;
        Hook(VtableEntry(factory, kFactory_CreateSwapChain), (void*)hk_CreateSwapChain, (void**)&o_CreateSwapChain, "IDXGIFactory::CreateSwapChain");
    }
    if (!g_factory2VtHooked) {
        IDXGIFactory2* f2 = nullptr;
        if (SUCCEEDED(unk->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2)) && f2) {
            g_factory2VtHooked = true;
            Hook(VtableEntry(f2, kFactory2_CreateSwapChainForHwnd), (void*)hk_CreateSwapChainForHwnd, (void**)&o_CreateSwapChainForHwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
            f2->Release();
        }
    }
}

HRESULT WINAPI hk_CreateDXGIFactory1(REFIID riid, void** out) {
    HRESULT hr = o_CreateFactory1(riid, out);
    LOGD("CreateDXGIFactory1 -> 0x%08lx", hr);
    if (SUCCEEDED(hr) && out) HookFactory(*out);
    return hr;
}

HRESULT WINAPI hk_CreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
    HRESULT hr = o_CreateFactory2(flags, riid, out);
    LOGD("CreateDXGIFactory2 flags=0x%x -> 0x%08lx", flags, hr);
    if (SUCCEEDED(hr) && out) HookFactory(*out);
    return hr;
}

// ---- device creation ----
void LogDevice(ID3D11Device* dev, D3D_FEATURE_LEVEL fl, UINT flags) {
    if (!dev) return;
    LOGI("  device=%p feature level %x.%x createFlags=0x%x", dev, (fl >> 12) & 0xF, (fl >> 8) & 0xF, flags);
    IDXGIDevice* dxgiDev = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) && dxgiDev) {
        IDXGIAdapter* ad = nullptr;
        if (SUCCEEDED(dxgiDev->GetAdapter(&ad)) && ad) {
            DXGI_ADAPTER_DESC d{};
            ad->GetDesc(&d);
            char name[256]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
            LOGI("  adapter \"%s\" vendor=0x%04x device=0x%04x vram=%llu MB luid=%08lx:%08lx",
                 name, d.VendorId, d.DeviceId, (unsigned long long)(d.DedicatedVideoMemory >> 20), d.AdapterLuid.HighPart, d.AdapterLuid.LowPart);
            IDXGIFactory* f = nullptr;
            if (SUCCEEDED(ad->GetParent(__uuidof(IDXGIFactory), (void**)&f)) && f) { HookFactory(f); f->Release(); }
            ad->Release();
        }
        dxgiDev->Release();
    }
}

HRESULT WINAPI hk_D3D11CreateDevice(IDXGIAdapter* ad, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* fls,
                                    UINT nfl, UINT sdk, ID3D11Device** dev, D3D_FEATURE_LEVEL* fl, ID3D11DeviceContext** ctx) {
    ++t_inCreate;
    HRESULT hr = o_CreateDevice(ad, dt, sw, flags, fls, nfl, sdk, dev, fl, ctx);
    --t_inCreate;
    if (t_inCreate == 0) {
        LOGI("D3D11CreateDevice driverType=%d flags=0x%x levels=%u -> 0x%08lx", dt, flags, nfl, hr);
        if (SUCCEEDED(hr) && dev && *dev) {
            LogDevice(*dev, fl ? *fl : (*dev)->GetFeatureLevel(), flags);
            // The game's real device is the hardware one; the later UNKNOWN/WARP/REFERENCE ones are probes.
            if (dt == D3D_DRIVER_TYPE_HARDWARE) CtxStatsInstall(*dev);
            else CtxStatsWatchDevice(*dev);
        }
    }
    return hr;
}

HRESULT WINAPI hk_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* ad, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* fls,
                                                UINT nfl, UINT sdk, const DXGI_SWAP_CHAIN_DESC* scd, IDXGISwapChain** sc,
                                                ID3D11Device** dev, D3D_FEATURE_LEVEL* fl, ID3D11DeviceContext** ctx) {
    DXGI_SWAP_CHAIN_DESC forced{};
    if (scd && t_inCreate == 0 && g_config.debugSwapW && g_config.debugSwapH) {   // experiment: back buffer size override
        forced = *scd; LOGI("swapchain size override %ux%u -> %ux%u", scd->BufferDesc.Width, scd->BufferDesc.Height, g_config.debugSwapW, g_config.debugSwapH);
        forced.BufferDesc.Width = g_config.debugSwapW; forced.BufferDesc.Height = g_config.debugSwapH; scd = &forced;
    }
    ++t_inCreate;
    HRESULT hr = o_CreateDeviceAndSwapChain(ad, dt, sw, flags, fls, nfl, sdk, scd, sc, dev, fl, ctx);
    --t_inCreate;
    if (t_inCreate != 0) return hr; // called internally by D3D11CreateDevice; the outer hook logs it
    LOGI("D3D11CreateDeviceAndSwapChain driverType=%d flags=0x%x levels=%u -> 0x%08lx", dt, flags, nfl, hr);
    if (SUCCEEDED(hr)) {
        if (dev && *dev) LogDevice(*dev, fl ? *fl : (*dev)->GetFeatureLevel(), flags);
        LogSwapChainDesc("D3D11CreateDeviceAndSwapChain", scd);
        if (sc && *sc) HookSwapChain(*sc);
        if (g_config.debugStacks && dev && *dev) CtxStatsInstall(*dev);   // diagnostic: see the startup render-target creation
    }
    return hr;
}

void TryHookD3D11() {
    std::lock_guard<std::mutex> lk(g_hookMutex);
    if (g_d3d11Hooked) return;
    HMODULE d3d = GetModuleHandleW(L"d3d11.dll");
    if (!d3d) return;
    g_d3d11Hooked = true;
    wchar_t path[MAX_PATH]; GetModuleFileNameW(d3d, path, MAX_PATH);
    char p8[MAX_PATH * 3]; WideCharToMultiByte(CP_UTF8, 0, path, -1, p8, sizeof(p8), nullptr, nullptr);
    LOGI("d3d11.dll loaded: %s", p8);
    {
        wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
        size_t n = wcslen(sys);
        if (_wcsnicmp(path, sys, n) != 0)
            LOGW("d3d11.dll is NOT the system one: another D3D mod/wrapper (e.g. 3DMigoto/Geo-11 3D, ReShade) is installed in the game folder. "
                 "It can break VR output and change the image; remove it (d3d11.dll and its ini/folder) if VR does not start.");
    }
    Hook((void*)GetProcAddress(d3d, "D3D11CreateDevice"), (void*)hk_D3D11CreateDevice, (void**)&o_CreateDevice, "D3D11CreateDevice");
    Hook((void*)GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain"), (void*)hk_D3D11CreateDeviceAndSwapChain,
         (void**)&o_CreateDeviceAndSwapChain, "D3D11CreateDeviceAndSwapChain");
}

// The game loads d3d11.dll at runtime; every LoadLibrary variant ends up in KernelBase!LoadLibraryExW.
HMODULE WINAPI hk_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    HMODULE m = o_LoadLibraryExW(name, file, flags);
    if (m && !g_d3d11Hooked) TryHookD3D11();
    return m;
}

} // namespace

bool InstallD3DHooks() {
    QueryPerformanceFrequency(&g_qpf);
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { LOGE("MH_Initialize failed (%s)", MH_StatusToString(s)); return false; }

    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    void* lle = kb ? (void*)GetProcAddress(kb, "LoadLibraryExW") : nullptr;
    if (!lle || !Hook(lle, (void*)hk_LoadLibraryExW, (void**)&o_LoadLibraryExW, "KernelBase!LoadLibraryExW"))
        LOGW("LoadLibraryExW hook unavailable; relying on d3d11 already being loaded");

    // dxgi.dll is a static import of SO6.exe, so it is already mapped when we run.
    if (HMODULE dxgi = GetModuleHandleW(L"dxgi.dll")) {
        if (void* p = (void*)GetProcAddress(dxgi, "CreateDXGIFactory1"))
            Hook(p, (void*)hk_CreateDXGIFactory1, (void**)&o_CreateFactory1, "CreateDXGIFactory1");
        if (void* p = (void*)GetProcAddress(dxgi, "CreateDXGIFactory2"))
            Hook(p, (void*)hk_CreateDXGIFactory2, (void**)&o_CreateFactory2, "CreateDXGIFactory2");
    } else {
        LOGW("dxgi.dll not loaded at startup (unexpected)");
    }

    TryHookD3D11(); // in case it is already loaded
    return true;
}

void RemoveD3DHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

} // namespace so4vr
