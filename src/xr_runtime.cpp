// OpenXR output, decoupled from the game's frame rate.
//
// Render thread (game's Present): copies the finished frame into one of three shared textures
// together with the head pose / FOV it was rendered with, then returns to the game.
// XR thread (ours): owns its own D3D11 device on the same GPU and the OpenXR session, and runs
// the OpenXR frame loop at the headset's refresh rate. Every refresh it submits the most recent
// game frame with that frame's pose, so the compositor can re-project it to the current head
// pose. Head motion is therefore smooth at the headset rate regardless of the game's frame rate.
//
// Cross-device synchronisation uses keyed mutexes (key 0, used as a plain GPU lock).
//
// The OpenXR loader is loaded at runtime, so a missing openxr_loader.dll, no SteamVR or a headset
// that is off never stops the game: we log it, retry, and the game keeps running on the monitor.
#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <vector>
#include <string>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <atomic>
#include <mutex>
#include "temporal.h"
#include "xr_runtime.h"
#include "image_quality.h"
#include "vrs.h"
#include "render_res.h"
#include "native_stereo.h"
#include "hud_layer.h"
#include "menu_options.h"
#include "config.h"
#include "log.h"
#include "head_tracking.h"
#include "so4_headtrack.h"
#include "ctx_stats.h"
#include "game_offsets.h"
#include "safe_mem.h"
#include "reproject.h"
#include "so4_stereo.h"

namespace so4vr {
namespace {

// ---- OpenXR function table (resolved through xrGetInstanceProcAddr) ----
#define SO4VR_XR_GLOBAL_FUNCS(X) \
    X(xrEnumerateInstanceExtensionProperties) \
    X(xrCreateInstance)

#define SO4VR_XR_INSTANCE_FUNCS(X) \
    X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrResultToString) \
    X(xrGetSystem) X(xrGetSystemProperties) X(xrPollEvent) \
    X(xrCreateSession) X(xrDestroySession) X(xrBeginSession) X(xrEndSession) \
    X(xrCreateReferenceSpace) X(xrDestroySpace) \
    X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain) \
    X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) \
    X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrLocateViews) X(xrEnumerateViewConfigurationViews) \
    X(xrGetD3D11GraphicsRequirementsKHR) \
    X(xrStringToPath) X(xrCreateActionSet) X(xrDestroyActionSet) X(xrCreateAction) \
    X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) X(xrCreateActionSpace) \
    X(xrSyncActions) X(xrLocateSpace)

PFN_xrGetInstanceProcAddr p_xrGetInstanceProcAddr = nullptr;
#define SO4VR_DECL(fn) PFN_##fn p_##fn = nullptr;
SO4VR_XR_GLOBAL_FUNCS(SO4VR_DECL)
SO4VR_XR_INSTANCE_FUNCS(SO4VR_DECL)
#undef SO4VR_DECL

// ---- XR state (owned by the XR thread; a few atomics are read elsewhere) ----
HMODULE     g_loader = nullptr;
XrInstance  g_instance = XR_NULL_HANDLE;
XrSystemId  g_system = XR_NULL_SYSTEM_ID;
XrSession   g_session = XR_NULL_HANDLE;
XrSpace     g_space = XR_NULL_HANDLE;
std::atomic<bool> g_running{false};          // session running (frame loop active)
std::atomic<XrTime> g_lastDisplayTime{0};    // published for main-thread pose prediction
std::atomic<XrDuration> g_displayPeriod{11111111};
std::mutex  g_sessionMutex;                  // session/space teardown vs. main-thread pose queries

ID3D11Device*        g_xrDevice = nullptr;   // our own device on the game's GPU
ID3D11Device1*       g_xrDevice1 = nullptr;
ID3D11DeviceContext* g_xrContext = nullptr;

XrSwapchain g_swapchain = XR_NULL_HANDLE;
std::vector<XrSwapchainImageD3D11KHR> g_images;
UINT        g_scWidth = 0, g_scHeight = 0;
DXGI_FORMAT g_scFormat = DXGI_FORMAT_UNKNOWN;     // source (game) format the swapchain was made for
DXGI_FORMAT g_scSwapFormat = DXGI_FORMAT_UNKNOWN; // actual swapchain format
bool        g_warnedFormat = false;
Reprojector g_reproj;

// ---- frame hand-off between the render thread and the XR thread ----
constexpr int kSlots = 3;
struct SlotMeta {
    bool      mono = false;   // rendered head-tracked (projection layer) vs flat screen (quad)
    bool      stereo = false; // native stereo: left eye in the left half, right eye in the right half
    bool      hud = false;    // this slot's HUD texture holds the frame's HUD (quad layer)
    HeadFrame hf{};
    uint64_t  seq = 0;
};
struct SharedSet {
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    // game-device side
    ID3D11Texture2D* gameTex[kSlots] = {};
    IDXGIKeyedMutex* gameMutex[kSlots] = {};
    HANDLE           handle[kSlots] = {};
    uint32_t         generation = 0;
    uint64_t         lastRealloc = 0;     // GetTickCount64 of the last size change
};
SharedSet g_shared;                       // created and written by the render thread
std::atomic<uint64_t> g_lastStereoTick{0};
std::atomic<uint64_t> s_flatHeld{0};
std::mutex g_metaMutex;
SlotMeta  g_meta[kSlots];
std::atomic<int> g_latestSlot{-1};
std::atomic<uint64_t> g_latestSeq{0};
std::atomic<int> g_readingSlot{-1};       // slot the XR thread is copying from
std::atomic<uint32_t> g_publishedGeneration{0};
std::mutex g_handleMutex;                 // protects handle/size/format for the XR thread

// XR-thread side of the shared textures
struct XrShared {
    uint32_t generation = 0;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ID3D11Texture2D* tex[kSlots] = {};
    IDXGIKeyedMutex* mutex[kSlots] = {};
} g_xs;

// HUD hand-off: same slot indices as the frames, separate textures (game side / XR side).
SharedSet g_hudShared;
std::atomic<uint32_t> g_hudPublishedGeneration{0};
XrShared g_xh;
XrSwapchain g_hudSwapchain = XR_NULL_HANDLE;
std::vector<XrSwapchainImageD3D11KHR> g_hudImages;
UINT g_hudScW = 0, g_hudScH = 0;
DXGI_FORMAT g_hudScFormat = DXGI_FORMAT_UNKNOWN;
bool g_hudScHasImage = false;
XrSpace g_viewSpace = XR_NULL_HANDLE;
// Eye gaze (XR_EXT_eye_gaze_interaction), for gaze-following foveated rendering.
bool g_gazeExt = false, g_gazeSupported = false;
XrActionSet g_gazeSet = XR_NULL_HANDLE;
XrAction g_gazeAction = XR_NULL_HANDLE;
XrSpace g_gazeSpace = XR_NULL_HANDLE;
std::atomic<float> g_gazeTx{0.0f}, g_gazeTy{0.0f};
std::atomic<uint64_t> g_gazeTick{0};      // GetTickCount64 of the last valid sample
std::atomic<bool> g_hudVisible{true};
std::atomic<uint64_t> s_hudLayers{0}, s_hudCopies{0};
bool SyncXrHud();

// Settings menu overlay: one shared texture (keyed mutex), written by the render thread while the menu is
// open, copied by the XR thread into its own swapchain and shown as a head-locked quad on top.
struct MenuOverlay {
    // render-thread side
    UINT w = 0, h = 0;
    ID3D11Texture2D* gameTex = nullptr; IDXGIKeyedMutex* gameMutex = nullptr; HANDLE handle = nullptr;
    std::atomic<uint32_t> gen{0};
    std::atomic<uint64_t> seq{0};
    std::atomic<bool> visible{false};
    // XR-thread side
    uint32_t xgen = 0;
    ID3D11Texture2D* xtex = nullptr; IDXGIKeyedMutex* xmutex = nullptr;
    XrSwapchain sc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
    UINT scw = 0, sch = 0;
    uint64_t xseq = 0;
    bool hasImage = false;
} g_menu;
std::mutex g_menuMutex;   // protects handle/size between the threads

// Game-side state
ID3D11Device*        g_gameDevice = nullptr;
ID3D11DeviceContext* g_gameContext = nullptr;
LUID                 g_gameLuid{};
std::atomic<bool>    g_threadStarted{false};
bool                 g_initFailed = false;

// Stats
std::atomic<bool> g_stereoPublished{false};
std::atomic<uint64_t> s_stereoFrames{0};
std::atomic<uint64_t> s_xrFrames{0}, s_xrProj{0}, s_xrQuad{0}, s_xrEmpty{0}, s_newFrames{0}, s_published{0}, s_copySkipped{0};

const char* XrStr(XrResult r) {
    static thread_local char buf[XR_MAX_RESULT_STRING_SIZE];
    if (p_xrResultToString && g_instance && XR_SUCCEEDED(p_xrResultToString(g_instance, r, buf))) return buf;
    snprintf(buf, sizeof(buf), "XrResult(%d)", (int)r);
    return buf;
}

#define XR_CHECK(call)                                                       \
    do { XrResult _r = (call);                                               \
         if (XR_FAILED(_r)) { LOGE("%s failed: %s", #call, XrStr(_r)); return false; } } while (0)

template <typename T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// =====================================================================================
// OpenXR setup (XR thread)
// =====================================================================================
bool LoadLoader() {
    if (g_loader) return true;
    wchar_t path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LoadLoader), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"openxr_loader.dll");
    g_loader = LoadLibraryW(path);
    if (!g_loader) { LOGE("openxr_loader.dll not found next to the mod (err %lu); VR output disabled", GetLastError()); return false; }
    p_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)GetProcAddress(g_loader, "xrGetInstanceProcAddr");
    if (!p_xrGetInstanceProcAddr) { LOGE("openxr_loader.dll has no xrGetInstanceProcAddr"); return false; }
#define SO4VR_LOAD_GLOBAL(fn) p_xrGetInstanceProcAddr(XR_NULL_HANDLE, #fn, (PFN_xrVoidFunction*)&p_##fn);
    SO4VR_XR_GLOBAL_FUNCS(SO4VR_LOAD_GLOBAL)
#undef SO4VR_LOAD_GLOBAL
    return p_xrCreateInstance && p_xrEnumerateInstanceExtensionProperties;
}

bool CreateInstance() {
    uint32_t n = 0;
    XR_CHECK(p_xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr));
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(p_xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data()));
    bool hasD3D11 = false;
    for (auto& e : exts) if (strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) hasD3D11 = true;
    if (!hasD3D11) { LOGE("OpenXR runtime lacks %s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME); return false; }

    const char* enabled[2] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, nullptr};
    uint32_t enabledCount = 1;
    for (auto& e : exts) if (strcmp(e.extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0) g_gazeExt = true;
    if (g_gazeExt) enabled[enabledCount++] = XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME;
    LOGI("OpenXR eye gaze extension: %s", g_gazeExt ? "available" : "not offered by the runtime");
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "Resonance of Fate VR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "rofvr");
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = enabledCount;
    ci.enabledExtensionNames = enabled;
    XR_CHECK(p_xrCreateInstance(&ci, &g_instance));

#define SO4VR_LOAD_INST(fn) \
    if (XR_FAILED(p_xrGetInstanceProcAddr(g_instance, #fn, (PFN_xrVoidFunction*)&p_##fn)) || !p_##fn) { LOGE("missing OpenXR function " #fn); return false; }
    SO4VR_XR_INSTANCE_FUNCS(SO4VR_LOAD_INST)
#undef SO4VR_LOAD_INST

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(p_xrGetInstanceProperties(g_instance, &ip)))
        LOGI("OpenXR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
             XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    return true;
}

// 1 = got a system, 0 = headset not available yet, -1 = hard failure
int TryGetSystem() {
    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = p_xrGetSystem(g_instance, &gi, &g_system);
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) return 0;
    if (XR_FAILED(r)) { LOGE("xrGetSystem failed: %s", XrStr(r)); return -1; }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    XrSystemEyeGazeInteractionPropertiesEXT gp{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
    if (g_gazeExt) sp.next = &gp;
    if (XR_SUCCEEDED(p_xrGetSystemProperties(g_instance, g_system, &sp))) {
        g_gazeSupported = g_gazeExt && gp.supportsEyeGazeInteraction;
        LOGI("eye tracking: %s", g_gazeSupported ? "supported" : "not available");
    }
    if (true)
        LOGI("headset: %s (max swapchain %ux%u, max layers %u)", sp.systemName,
             sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight,
             sp.graphicsProperties.maxLayerCount);
    uint32_t nv = 0;
    if (XR_SUCCEEDED(p_xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nv, nullptr)) && nv >= 1) {
        std::vector<XrViewConfigurationView> v(nv, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        if (XR_SUCCEEDED(p_xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nv, &nv, v.data())))
            RenderResSetHmdRecommended(v[0].recommendedImageRectWidth, v[0].recommendedImageRectHeight);
    }
    return 1;
}

// Our own D3D11 device on the adapter the runtime asks for (normally the game's GPU).
bool CreateXrDevice(const LUID& luidIn, D3D_FEATURE_LEVEL minLevel) {
    if (g_xrDevice) return true;
    // Some runtimes (e.g. SteamVR's null headset) report an all-zero LUID: use the game's GPU then.
    LUID luid = luidIn;
    if (luid.LowPart == 0 && luid.HighPart == 0) {
        luid = g_gameLuid;
        LOGI("runtime reported no specific GPU; using the game's GPU (luid %08lx:%08lx)", luid.HighPart, luid.LowPart);
    }
    // Resolved at runtime so the mod DLL does not import d3d11/dxgi (keeps the game's load order).
    using PFN_CreateFactory1 = HRESULT (WINAPI*)(REFIID, void**);
    // Always the system DLLs by full path: a D3D wrapper in the game folder (3DMigoto/Geo-11, ReShade, ...)
    // would otherwise wrap our device too, and SteamVR then refuses to create swapchains on it.
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring sysDir(sys);
    HMODULE sysDxgi = LoadLibraryW((sysDir + L"\\dxgi.dll").c_str());
    HMODULE sysD3d = LoadLibraryW((sysDir + L"\\d3d11.dll").c_str());
    if (sysD3d && sysD3d != GetModuleHandleW(L"d3d11.dll")) LOGW("the game uses a different d3d11.dll than System32 (a D3D wrapper); the headset device uses the system one");
    auto pCreateFactory1 = (PFN_CreateFactory1)GetProcAddress(sysDxgi, "CreateDXGIFactory1");
    auto pCreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(sysD3d, "D3D11CreateDevice");
    if (!pCreateFactory1 || !pCreateDevice) { LOGE("could not resolve DXGI/D3D11 entry points"); return false; }
    IDXGIFactory1* factory = nullptr;
    if (FAILED(pCreateFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) { LOGE("CreateDXGIFactory1 failed"); return false; }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if (memcmp(&d.AdapterLuid, &luid, sizeof(LUID)) == 0) break;
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    if (!adapter) { LOGE("headset GPU (luid %08lx:%08lx) not found", luid.HighPart, luid.LowPart); return false; }
    if (memcmp(&luid, &g_gameLuid, sizeof(LUID)) != 0)
        LOGW("the headset uses a different GPU than the game; frame sharing will fail");

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = pCreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                   &g_xrDevice, &got, &g_xrContext);
    adapter->Release();
    if (FAILED(hr)) { LOGE("could not create the XR D3D11 device (0x%08lx)", hr); return false; }
    if (got < minLevel) LOGW("XR device feature level %x below runtime minimum %x", got, minLevel);
    g_xrDevice->QueryInterface(__uuidof(ID3D11Device1), (void**)&g_xrDevice1);
    LOGI("XR thread D3D11 device created (feature level %x)", got);
    g_reproj.Init(g_xrDevice);
    return g_xrDevice1 != nullptr;
}

void CreateGaze() {
    if (!g_gazeSupported) return;
    XrActionSetCreateInfo asi{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(asi.actionSetName, "so4vr_gaze"); strcpy_s(asi.localizedActionSetName, "Eye gaze");
    if (XR_FAILED(p_xrCreateActionSet(g_instance, &asi, &g_gazeSet))) { LOGW("eye gaze: action set failed"); return; }
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    ai.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy_s(ai.actionName, "gaze_pose"); strcpy_s(ai.localizedActionName, "Gaze pose");
    XrPath profile = 0, gazePath = 0;
    bool ok = XR_SUCCEEDED(p_xrCreateAction(g_gazeSet, &ai, &g_gazeAction)) &&
              XR_SUCCEEDED(p_xrStringToPath(g_instance, "/interaction_profiles/ext/eye_gaze_interaction", &profile)) &&
              XR_SUCCEEDED(p_xrStringToPath(g_instance, "/user/eyes_ext/input/gaze_ext/pose", &gazePath));
    if (ok) {
        XrActionSuggestedBinding b{g_gazeAction, gazePath};
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = profile; sb.countSuggestedBindings = 1; sb.suggestedBindings = &b;
        ok = XR_SUCCEEDED(p_xrSuggestInteractionProfileBindings(g_instance, &sb));
    }
    if (ok) {
        XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        at.countActionSets = 1; at.actionSets = &g_gazeSet;
        ok = XR_SUCCEEDED(p_xrAttachSessionActionSets(g_session, &at));
    }
    if (ok) {
        XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sci.action = g_gazeAction; sci.poseInActionSpace.orientation.w = 1.0f;
        ok = XR_SUCCEEDED(p_xrCreateActionSpace(g_session, &sci, &g_gazeSpace));
    }
    LOGI("eye gaze: %s", ok ? "ready" : "setup failed");
    if (!ok) { if (g_gazeSet) p_xrDestroyActionSet(g_gazeSet); g_gazeSet = XR_NULL_HANDLE; g_gazeAction = XR_NULL_HANDLE; g_gazeSpace = XR_NULL_HANDLE; }
}

// XR thread, once per headset frame: gaze direction relative to the head as tangents (x right, y up).
void UpdateGaze(XrTime t) {
    if (g_config.debugFakeGaze) {   // test without a tracker: a slow circle
        double a = double(GetTickCount64() % 6000) / 6000.0 * 6.2831853;
        g_gazeTx.store(float(0.35 * std::cos(a))); g_gazeTy.store(float(0.25 * std::sin(a))); g_gazeTick.store(GetTickCount64());
        return;
    }
    if (!g_gazeSpace || !g_viewSpace) return;
    XrActiveActionSet aas{g_gazeSet, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1; si.activeActionSets = &aas;
    if (XR_FAILED(p_xrSyncActions(g_session, &si))) return;
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(p_xrLocateSpace(g_gazeSpace, g_viewSpace, t, &loc))) return;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;
    const XrQuaternionf& q = loc.pose.orientation;
    // forward = q * (0, 0, -1)
    float fx = -2.0f * (q.x * q.z + q.w * q.y);
    float fy = -2.0f * (q.y * q.z - q.w * q.x);
    float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    if (fz > -0.2f) return;   // not looking forward (or garbage)
    float tx = fx / -fz, ty = fy / -fz;
    float k = 0.5f;           // light smoothing
    g_gazeTx.store(g_gazeTx.load() + (tx - g_gazeTx.load()) * k);
    g_gazeTy.store(g_gazeTy.load() + (ty - g_gazeTy.load()) * k);
    g_gazeTick.store(GetTickCount64());
    static int logged = 0;
    if (logged < 3 && GetTickCount64() % 1000 < 20) { ++logged; LOGI("eye gaze: tan (%.3f, %.3f)", tx, ty); }
}

bool CreateSession() {
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XR_CHECK(p_xrGetD3D11GraphicsRequirementsKHR(g_instance, g_system, &req));
    if (!CreateXrDevice(req.adapterLuid, req.minFeatureLevel)) return false;

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = g_xrDevice;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = g_system;
    XR_CHECK(p_xrCreateSession(g_instance, &sci, &g_session));

    // LOCAL = seated space centred where the headset was when the session started.
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    XR_CHECK(p_xrCreateReferenceSpace(g_session, &rs, &g_space));
    XrReferenceSpaceCreateInfo vs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    vs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vs.poseInReferenceSpace.orientation.w = 1.0f;
    if (XR_FAILED(p_xrCreateReferenceSpace(g_session, &vs, &g_viewSpace))) g_viewSpace = XR_NULL_HANDLE;
    CreateGaze();
    LOGI("OpenXR session created (decoupled XR thread)");
    return true;
}

void DestroySwapchain() {
    if (g_swapchain) p_xrDestroySwapchain(g_swapchain);
    g_swapchain = XR_NULL_HANDLE;
    g_images.clear();
    g_scWidth = g_scHeight = 0;
}

// The game's back buffer is R8G8B8A8_UNORM holding gamma-encoded colour. Copying those bytes into an
// *_SRGB swapchain tells the compositor they are sRGB-encoded, so colours look as on the monitor.
DXGI_FORMAT PickSwapchainFormat(DXGI_FORMAT src) {
    uint32_t n = 0;
    p_xrEnumerateSwapchainFormats(g_session, 0, &n, nullptr);
    std::vector<int64_t> fmts(n);
    p_xrEnumerateSwapchainFormats(g_session, n, &n, fmts.data());
    auto has = [&](DXGI_FORMAT f) { for (auto x : fmts) if (x == f) return true; return false; };
    DXGI_FORMAT preferred = src;
    if (src == DXGI_FORMAT_R8G8B8A8_UNORM) preferred = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (src == DXGI_FORMAT_B8G8R8A8_UNORM) preferred = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (has(preferred)) return preferred;
    if (has(src)) {
        if (!g_warnedFormat) { LOGW("runtime has no sRGB variant of format %d; colours may look washed out", src); g_warnedFormat = true; }
        return src;
    }
    LOGE("runtime supports neither format %d nor %d", preferred, src);
    return DXGI_FORMAT_UNKNOWN;
}

bool CreateSwapchainImpl(UINT w, UINT h, DXGI_FORMAT srcFormat);
// Wrapper: after a failure, wait (1 s, growing to 10 s) before trying again instead of retrying every frame.
bool CreateSwapchain(UINT w, UINT h, DXGI_FORMAT srcFormat) {
    static ULONGLONG nextTry = 0; static int fails = 0;
    ULONGLONG now = GetTickCount64();
    if (now < nextTry) return false;
    if (CreateSwapchainImpl(w, h, srcFormat)) { fails = 0; return true; }
    ++fails; nextTry = now + (fails < 10 ? 1000ull * fails : 10000ull);
    if (fails == 1 || fails % 20 == 0) LOGE("XR swapchain creation failed %d time(s) (%ux%u format %d); retrying in %llu s", fails, w, h, srcFormat, (nextTry - now) / 1000);
    return false;
}

bool CreateSwapchainImpl(UINT w, UINT h, DXGI_FORMAT srcFormat) {
    DestroySwapchain();
    DXGI_FORMAT fmt = PickSwapchainFormat(srcFormat);
    if (fmt == DXGI_FORMAT_UNKNOWN) return false;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fmt;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XR_CHECK(p_xrCreateSwapchain(g_session, &ci, &g_swapchain));
    uint32_t n = 0;
    XR_CHECK(p_xrEnumerateSwapchainImages(g_swapchain, 0, &n, nullptr));
    g_images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XR_CHECK(p_xrEnumerateSwapchainImages(g_swapchain, n, &n, (XrSwapchainImageBaseHeader*)g_images.data()));
    g_scWidth = w; g_scHeight = h; g_scFormat = srcFormat; g_scSwapFormat = fmt;
    g_reproj.ReleaseViews();
    LOGI("XR swapchain %ux%u format %d (%u images)", w, h, fmt, n);
    return true;
}

void DestroyHudSwapchain() {
    if (g_hudSwapchain) p_xrDestroySwapchain(g_hudSwapchain);
    g_hudSwapchain = XR_NULL_HANDLE;
    g_hudImages.clear();
    g_hudScW = g_hudScH = 0;
    g_hudScHasImage = false;
}

bool CreateHudSwapchain(UINT w, UINT h, DXGI_FORMAT srcFormat) {
    DestroyHudSwapchain();
    DXGI_FORMAT fmt = PickSwapchainFormat(srcFormat);
    if (fmt == DXGI_FORMAT_UNKNOWN) return false;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fmt; ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    XR_CHECK(p_xrCreateSwapchain(g_session, &ci, &g_hudSwapchain));
    uint32_t n = 0;
    XR_CHECK(p_xrEnumerateSwapchainImages(g_hudSwapchain, 0, &n, nullptr));
    g_hudImages.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XR_CHECK(p_xrEnumerateSwapchainImages(g_hudSwapchain, n, &n, (XrSwapchainImageBaseHeader*)g_hudImages.data()));
    g_hudScW = w; g_hudScH = h; g_hudScFormat = srcFormat;
    LOGI("XR HUD swapchain %ux%u format %d (%u images)", w, h, fmt, n);
    return true;
}

// XR thread: copy the slot's HUD into the HUD swapchain (new frames only).
bool UpdateHudImage(int slot) {
    if (!SyncXrHud()) return false;
    if (!g_hudSwapchain || g_hudScW != g_xh.width || g_hudScH != g_xh.height || g_hudScFormat != g_xh.format)
        if (!CreateHudSwapchain(g_xh.width, g_xh.height, g_xh.format)) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100000000;
    if (XR_FAILED(p_xrAcquireSwapchainImage(g_hudSwapchain, &ai, &idx))) return false;
    bool ok = false;
    if (XR_SUCCEEDED(p_xrWaitSwapchainImage(g_hudSwapchain, &wi))) {
        if (g_xh.mutex[slot] && g_xh.mutex[slot]->AcquireSync(0, 20) == S_OK) {
            g_xrContext->CopyResource(g_hudImages[idx].texture, g_xh.tex[slot]);
            g_xh.mutex[slot]->ReleaseSync(0);
            ok = true;
        }
    }
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    p_xrReleaseSwapchainImage(g_hudSwapchain, &ri);
    if (ok) { g_hudScHasImage = true; s_hudCopies++; }
    return ok;
}

bool HudQuadPose(XrSpace& space, XrPosef& pose) {
    pose.orientation = {0, 0, 0, 1};
    pose.position = {0.0f, g_config.hudOffsetY, -g_config.hudDistance};
    if (g_config.hudAnchor == 1 && g_viewSpace) { space = g_viewSpace; return true; }
    space = g_space;
    float yaw = 0; XrVector3f ref{0, 0, 0};
    if (So4HtRecenterRef(yaw, ref)) {
        float c = std::cos(yaw), sn = std::sin(yaw);
        float x = pose.position.x, z = pose.position.z;
        pose.position = {ref.x + c * x + sn * z, ref.y + pose.position.y, ref.z - sn * x + c * z};
        pose.orientation = {0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
    }
    return true;
}

void DestroyMenuSwapchain() {
    if (g_menu.sc) p_xrDestroySwapchain(g_menu.sc);
    g_menu.sc = XR_NULL_HANDLE; g_menu.images.clear(); g_menu.scw = g_menu.sch = 0; g_menu.hasImage = false;
}

// XR thread: bring the menu swapchain up to date. True when there is an image to show.
bool UpdateMenuImage() {
    if (!g_menu.visible.load()) return false;
    uint32_t gen = g_menu.gen.load();
    if (!gen) return false;
    if (gen != g_menu.xgen) {
        std::lock_guard<std::mutex> lk(g_menuMutex);
        SafeRelease(g_menu.xmutex); SafeRelease(g_menu.xtex);
        if (FAILED(g_xrDevice1->OpenSharedResource1(g_menu.handle, __uuidof(ID3D11Texture2D), (void**)&g_menu.xtex))) return false;
        g_menu.xtex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&g_menu.xmutex);
        g_menu.xgen = gen;
        g_menu.xseq = 0;
        if (g_menu.scw != g_menu.w || g_menu.sch != g_menu.h) {
            DestroyMenuSwapchain();
            DXGI_FORMAT fmt = PickSwapchainFormat(DXGI_FORMAT_R8G8B8A8_UNORM);
            XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            ci.format = fmt; ci.sampleCount = 1; ci.width = g_menu.w; ci.height = g_menu.h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
            if (fmt == DXGI_FORMAT_UNKNOWN || XR_FAILED(p_xrCreateSwapchain(g_session, &ci, &g_menu.sc))) { g_menu.sc = XR_NULL_HANDLE; return false; }
            uint32_t n = 0;
            p_xrEnumerateSwapchainImages(g_menu.sc, 0, &n, nullptr);
            g_menu.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            p_xrEnumerateSwapchainImages(g_menu.sc, n, &n, (XrSwapchainImageBaseHeader*)g_menu.images.data());
            g_menu.scw = g_menu.w; g_menu.sch = g_menu.h;
            LOGI("XR menu swapchain %ux%u format %d", g_menu.w, g_menu.h, fmt);
        }
    }
    if (!g_menu.sc || !g_menu.xtex) return false;
    uint64_t sq = g_menu.seq.load();
    if (sq != g_menu.xseq) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 100000000;
        if (XR_SUCCEEDED(p_xrAcquireSwapchainImage(g_menu.sc, &ai, &idx))) {
            if (XR_SUCCEEDED(p_xrWaitSwapchainImage(g_menu.sc, &wi)) && g_menu.xmutex && g_menu.xmutex->AcquireSync(0, 10) == S_OK) {
                g_xrContext->CopyResource(g_menu.images[idx].texture, g_menu.xtex);
                g_menu.xmutex->ReleaseSync(0);
                g_menu.hasImage = true;
                g_menu.xseq = sq;
            }
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            p_xrReleaseSwapchainImage(g_menu.sc, &ri);
        }
    }
    return g_menu.hasImage;
}

void TearDownSession() {
    DestroySwapchain();
    DestroyHudSwapchain();
    DestroyMenuSwapchain();
    g_menu.xgen = 0;
    if (g_viewSpace) { p_xrDestroySpace(g_viewSpace); g_viewSpace = XR_NULL_HANDLE; }
    if (g_gazeSpace) { p_xrDestroySpace(g_gazeSpace); g_gazeSpace = XR_NULL_HANDLE; }
    if (g_gazeSet) { p_xrDestroyActionSet(g_gazeSet); g_gazeSet = XR_NULL_HANDLE; g_gazeAction = XR_NULL_HANDLE; }
    std::lock_guard<std::mutex> lk(g_sessionMutex);
    g_running = false;
    if (g_space) p_xrDestroySpace(g_space);
    if (g_session) p_xrDestroySession(g_session);
    g_space = XR_NULL_HANDLE;
    g_session = XR_NULL_HANDLE;
}

// Returns false if the session was lost and must be recreated.
bool HandleEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (p_xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            LOGI("XR session state -> %d", (int)e->state);
            if (e->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = p_xrBeginSession(g_session, &bi);
                g_running = XR_SUCCEEDED(r);
                LOGI("xrBeginSession: %s", XrStr(r));
            } else if (e->state == XR_SESSION_STATE_STOPPING) {
                g_running = false;
                p_xrEndSession(g_session);
                LOGI("XR session ended (headset/runtime stopped it)");
            } else if (e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING) {
                LOGW("XR session lost; will retry");
                return false;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            LOGE("OpenXR instance loss pending");
            return false;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return true;
}

// =====================================================================================
// Shared textures
// =====================================================================================

// Render thread: (re)create the shared ring to match the back buffer.
bool EnsureSetImpl(SharedSet& g_shared, std::atomic<uint32_t>& g_publishedGeneration, const char* what, UINT w, UINT h, DXGI_FORMAT fmt) {
    if (fmt == DXGI_FORMAT_R8G8B8A8_TYPELESS) fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS) fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (g_shared.width == w && g_shared.height == h && g_shared.format == fmt && g_shared.gameTex[0]) return true;
    // Never reallocate in quick succession: frames alternating between two sizes (stereo / flat) once
    // recreated ~470 MB of shared textures every frame until video memory ran out and the game exited.
    if (g_shared.gameTex[0] && GetTickCount64() - g_shared.lastRealloc < 500) {
        static uint64_t refused = 0;
        if ((++refused & (refused - 1)) == 0) LOGW("shared %s textures: size change to %ux%u refused (too soon after the last one; %llu refusals)", what, w, h, refused);
        return false;
    }
    g_shared.lastRealloc = GetTickCount64();
    // Wait until the XR thread is not reading, then drop the old set.
    for (int i = 0; i < 200 && g_readingSlot.load() >= 0; ++i) Sleep(1);
    std::lock_guard<std::mutex> lk(g_handleMutex);
    if (&g_shared == &::so4vr::g_shared) g_latestSlot = -1;
    for (int i = 0; i < kSlots; ++i) {
        SafeRelease(g_shared.gameMutex[i]);
        SafeRelease(g_shared.gameTex[i]);
        if (g_shared.handle[i]) { CloseHandle(g_shared.handle[i]); g_shared.handle[i] = nullptr; }
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    for (int i = 0; i < kSlots; ++i) {
        HRESULT hr = g_gameDevice->CreateTexture2D(&td, nullptr, &g_shared.gameTex[i]);
        if (SUCCEEDED(hr)) TemporalMarkModTexture(g_shared.gameTex[i]);
        if (FAILED(hr)) { LOGE("shared texture creation failed (0x%08lx)", hr); return false; }
        g_shared.gameTex[i]->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&g_shared.gameMutex[i]);
        IDXGIResource1* res = nullptr;
        if (SUCCEEDED(g_shared.gameTex[i]->QueryInterface(__uuidof(IDXGIResource1), (void**)&res))) {
            hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &g_shared.handle[i]);
            res->Release();
        }
        if (FAILED(hr) || !g_shared.handle[i] || !g_shared.gameMutex[i]) { LOGE("shared handle creation failed (0x%08lx)", hr); return false; }
    }
    g_shared.width = w; g_shared.height = h; g_shared.format = fmt;
    g_shared.generation++;
    g_publishedGeneration = g_shared.generation;
    LOGI("shared %s textures %ux%u format %d (generation %u)", what, w, h, fmt, g_shared.generation);
    return true;
}
bool EnsureSharedSet(UINT w, UINT h, DXGI_FORMAT fmt) { return EnsureSetImpl(g_shared, g_publishedGeneration, "frame", w, h, fmt); }
bool EnsureHudSet(UINT w, UINT h, DXGI_FORMAT fmt) { return EnsureSetImpl(g_hudShared, g_hudPublishedGeneration, "HUD", w, h, fmt); }

// XR thread: open the current shared set on our device if it changed.
bool SyncImpl(XrShared& g_xs, SharedSet& g_shared, std::atomic<uint32_t>& g_publishedGeneration) {
    uint32_t gen = g_publishedGeneration.load();
    if (gen == 0) return false;
    if (gen == g_xs.generation && g_xs.tex[0]) return true;
    std::lock_guard<std::mutex> lk(g_handleMutex);
    for (int i = 0; i < kSlots; ++i) { SafeRelease(g_xs.mutex[i]); SafeRelease(g_xs.tex[i]); }
    for (int i = 0; i < kSlots; ++i) {
        HRESULT hr = g_xrDevice1->OpenSharedResource1(g_shared.handle[i], __uuidof(ID3D11Texture2D), (void**)&g_xs.tex[i]);
        if (FAILED(hr)) { LOGE("OpenSharedResource1 failed (0x%08lx)", hr); g_xs.generation = 0; return false; }
        g_xs.tex[i]->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&g_xs.mutex[i]);
    }
    g_xs.generation = g_shared.generation;
    g_xs.width = g_shared.width; g_xs.height = g_shared.height; g_xs.format = g_shared.format;
    LOGI("XR thread opened shared textures %ux%u (generation %u)", g_xs.width, g_xs.height, g_xs.generation);
    return true;
}
bool SyncXrShared() { return SyncImpl(g_xs, g_shared, g_publishedGeneration); }
bool SyncXrHud() { return SyncImpl(g_xh, g_hudShared, g_hudPublishedGeneration); }

// =====================================================================================
// XR thread
// =====================================================================================
// Diagnostic fake head motion (for testing with SteamVR's stationary null headset): yaw oscillation
// as a function of display time, applied identically wherever head poses are read.
void ApplyFakeHead(XrTime t, XrView* v, int n) {
    if (g_config.debugFakeHeadYawDeg == 0.0f) return;
    double sec = double(t) * 1e-9;
    float yaw = g_config.debugFakeHeadYawDeg * float(std::sin(sec * 6.283185307 * g_config.debugFakeHeadHz));
    float h = yaw * 0.0087266463f, s = std::sin(h), c = std::cos(h);
    float cx = 0, cz = 0;
    for (int i = 0; i < n; ++i) { cx += v[i].pose.position.x / n; cz += v[i].pose.position.z / n; }
    float cy2 = std::cos(yaw * 0.017453293f), sy2 = std::sin(yaw * 0.017453293f);
    for (int i = 0; i < n; ++i) {
        const XrQuaternionf q = v[i].pose.orientation;
        v[i].pose.orientation = {c * q.x + s * q.z, c * q.y + s * q.w, c * q.z - s * q.x, c * q.w - s * q.y};
        float dx = v[i].pose.position.x - cx, dz = v[i].pose.position.z - cz;
        v[i].pose.position.x = cx + cy2 * dx + sy2 * dz;
        v[i].pose.position.z = cz - sy2 * dx + cy2 * dz;
    }
}

// Head pose (centre between the eyes, left-eye orientation, as used for rendering) at time t.
bool LocateHead(XrTime t, XrPosef& out) {
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = t;
    li.space = g_space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView v[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t n = 0;
    if (XR_FAILED(p_xrLocateViews(g_session, &li, &vs, 2, &n, v)) || n != 2) return false;
    if (!(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return false;
    ApplyFakeHead(t, v, 2);
    out.orientation = v[0].pose.orientation;
    out.position = {(v[0].pose.position.x + v[1].pose.position.x) * 0.5f, (v[0].pose.position.y + v[1].pose.position.y) * 0.5f,
                    (v[0].pose.position.z + v[1].pose.position.z) * 0.5f};
    return true;
}

// Rotates a pose's orientation by `deg` around the space's +Y axis (diagnostic).
void YawPose(XrPosef& p, float deg) {
    if (deg == 0.0f) return;
    float h = deg * 0.0087266463f; // half angle in radians
    float s = std::sin(h), c = std::cos(h);
    const XrQuaternionf q = p.orientation;
    p.orientation = {c * q.x + s * q.z, c * q.y + s * q.w, c * q.z - s * q.x, c * q.w - s * q.y};
}

// Pose prediction: how far after the latest display time a frame prepared now is actually shown (measured
// on the XR thread when each new frame is first displayed; exponential average, ns).
std::atomic<int64_t> g_predNeeded{0};
std::atomic<int64_t> s_predSum{0}, s_predErrSum{0}, s_predN{0};

std::atomic<uint64_t> s_reprojected{0};
// Diagnostic (DebugFrameScan = seconds): quarter-ish thumbnails (both eyes side by side) of every new stereo frame,
// downscaled on the XR device and read back through a small staging ring, written to so4vr_scan_NN.raw files
// (header "SO4S" w h n, then per frame: u64 seq, w*h RGBA). For finding one-frame glitches offline.
namespace scan {
constexpr UINT kW = 960, kH = 270; constexpr int kRing = 4, kChunk = 200;
ID3D11Texture2D* rt = nullptr; ID3D11Texture2D* stg[kRing] = {}; uint64_t stgSeq[kRing] = {}; int head = 0, pending = 0;
std::vector<uint8_t> buf; int bufN = 0, fileN = 0; uint64_t total = 0, limit = 0; bool done = false;
void Flush() {
    if (!bufN) return;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = exe; path = path.substr(0, path.find_last_of(L'\\') + 1);
    wchar_t nm[64]; swprintf(nm, 64, L"rofvr_scan_%02d.raw", fileN++); path += nm;
    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
        uint32_t hdr[4] = {0x53344F53u, kW, kH, uint32_t(bufN)};
        fwrite(hdr, sizeof hdr, 1, f); fwrite(buf.data(), 1, buf.size(), f); fclose(f);
        LOGI("frame scan: wrote %ls (%d frames)", nm, bufN);
    }
    buf.clear(); bufN = 0;
}
bool writing = false;
// One-frame glitch detector: per eye, 120x68 luminance; frame i is a glitch when it differs a lot from both
// neighbours while the neighbours are close to each other.
constexpr int kDW = 120, kDH = 67;
float g_lum[3][2][kDW * kDH]; uint64_t g_lumSeq[3] = {}; int g_lumN = 0;
std::vector<uint8_t> g_thumb[3];
void DumpGlitch(int a, int b, int c, int eye, uint64_t seq, double score) {
    static int n = 0; if (n >= 24) return;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = exe; path = path.substr(0, path.find_last_of(L'\\') + 1);
    wchar_t nm[96]; swprintf(nm, 96, L"rofvr_glitch_%02d_%s_%llu.bmp", n++, eye ? L"right" : L"left", (unsigned long long)seq); path += nm;
    const int W = int(kW), H = int(kH) * 3, stride = (W * 3 + 3) & ~3;
    BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
    ih.biSize = sizeof ih; ih.biWidth = W; ih.biHeight = H; ih.biPlanes = 1; ih.biBitCount = 24;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + DWORD(stride) * H;
    std::vector<uint8_t> out(size_t(stride) * H, 0);
    const int order[3] = {a, b, c};   // top: previous, middle: the anomaly, bottom: next
    for (int r = 0; r < 3; ++r) {
        const std::vector<uint8_t>& t = g_thumb[order[r]];
        if (t.size() < size_t(kW) * kH * 4) continue;
        for (int y = 0; y < int(kH); ++y) {
            uint8_t* d = out.data() + size_t(H - 1 - (r * int(kH) + y)) * stride;
            const uint8_t* sp = t.data() + size_t(y) * kW * 4;
            for (int x = 0; x < W; ++x) { d[x * 3 + 0] = sp[x * 4 + 2]; d[x * 3 + 1] = sp[x * 4 + 1]; d[x * 3 + 2] = sp[x * 4 + 0]; }
        }
    }
    if (FILE* f = _wfopen(path.c_str(), L"wb")) { fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(out.data(), 1, out.size(), f); fclose(f); }
    LOGI("glitch dump: %ls (score %.1f)", nm, score);
}
void Detect(const uint8_t* px, uint64_t seq) {
    g_thumb[g_lumN % 3].assign(px, px + size_t(kW) * kH * 4);
    float (*cur)[kDW * kDH] = g_lum[g_lumN % 3];
    for (int e = 0; e < 2; ++e)
        for (int y = 0; y < kDH; ++y)
            for (int x = 0; x < kDW; ++x) {
                const uint8_t* p = px + (size_t(y * 4) * kW + e * (kW / 2) + x * 4) * 4;
                cur[e][y * kDW + x] = 0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2];
            }
    g_lumSeq[g_lumN % 3] = seq; ++g_lumN;
    if (g_lumN < 3) return;
    const int a = (g_lumN - 3) % 3, b = (g_lumN - 2) % 3, c = (g_lumN - 1) % 3;
    // consecutive stereo frames (RoF updates the camera several times per frame: records only have to increase)
    if (g_lumSeq[b] <= g_lumSeq[a] || g_lumSeq[c] <= g_lumSeq[b]) return;
    static uint64_t frames = 0, globalHits = 0, tileHits[2] = {}; static double maxTile = 0; static ULONGLONG lastHitLog = 0;
    ++frames;
    for (int e = 0; e < 2; ++e) {
        double dab = 0, dbc = 0, dac = 0;
        for (int k = 0; k < kDW * kDH; ++k) { dab += std::fabs(g_lum[a][e][k] - g_lum[b][e][k]); dbc += std::fabs(g_lum[b][e][k] - g_lum[c][e][k]); dac += std::fabs(g_lum[a][e][k] - g_lum[c][e][k]); }
        dab /= kDW * kDH; dbc /= kDW * kDH; dac /= kDW * kDH;
        double score = std::fmin(dab, dbc) - dac;
        if (score > 6.0) { globalHits++; So4StereoDumpDiag(g_lumSeq[b], e, float(score)); }
        // Local one-frame anomalies (rofvr): the same test on 6x6-cell tiles (~24 px of the 480x270 eye thumbnail).
        constexpr int T = 6;
        double best = 0; int bx = 0, by = 0;
        for (int ty = 0; ty + T <= kDH; ty += T)
            for (int tx = 0; tx + T <= kDW; tx += T) {
                double tab = 0, tbc = 0, tac = 0;
                for (int y = ty; y < ty + T; ++y) for (int x = tx; x < tx + T; ++x) {
                    const int k = y * kDW + x;
                    tab += std::fabs(g_lum[a][e][k] - g_lum[b][e][k]); tbc += std::fabs(g_lum[b][e][k] - g_lum[c][e][k]); tac += std::fabs(g_lum[a][e][k] - g_lum[c][e][k]);
                }
                const double sc = (std::fmin(tab, tbc) - tac) / (T * T);
                if (sc > best) { best = sc; bx = tx; by = ty; }
            }
        if (best > maxTile) maxTile = best;
        if (best > 20.0) {
            tileHits[e]++;
            DumpGlitch(a, b, c, e, g_lumSeq[b], best);
            So4StereoDumpDiag(g_lumSeq[b], e, float(best));
            ULONGLONG now = GetTickCount64();
            if (now - lastHitLog > 300) { lastHitLog = now; LOGI("GLITCH tile: %s eye, record %llu, tile at (%d,%d) of %dx%d, score %.1f (whole-image %.2f)", e ? "right" : "left",
                (unsigned long long)g_lumSeq[b], bx, by, kDW, kDH, best, score); }
        }
    }
    if (frames % 600 == 0) { LOGI("glitch scan: %llu frames, one-frame tile anomalies left %llu right %llu (max score %.1f), whole-image %llu",
        (unsigned long long)frames, (unsigned long long)tileHits[0], (unsigned long long)tileHits[1], maxTile, (unsigned long long)globalHits); maxTile = 0; }
}
void Drain(bool all) {
    while (pending > (all ? 0 : kRing - 1)) {
        int i = (head - pending + kRing) % kRing;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_xrContext->Map(stg[i], 0, D3D11_MAP_READ, 0, &m))) {
            size_t at = buf.size(); buf.resize(at + 8 + size_t(kW) * kH * 4);
            memcpy(buf.data() + at, &stgSeq[i], 8);
            for (UINT y = 0; y < kH; ++y) memcpy(buf.data() + at + 8 + size_t(y) * kW * 4, static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, kW * 4);
            g_xrContext->Unmap(stg[i], 0);
            Detect(buf.data() + at + 8, stgSeq[i]);
            if (!writing) { buf.resize(at); }
            else if (++bufN >= kChunk) Flush();
        }
        --pending;
    }
}
void Capture(ID3D11Texture2D* src, const XrFovf& fov, uint64_t seq) {
    if (done || (g_config.debugFrameScan <= 0 && !g_config.debugGlitchDetect)) return;
    static uint64_t skipped = 0; if (++skipped < 1500) return;   // start ~25 s into stereo (past loading)
    if (!limit) { limit = g_config.debugFrameScan > 0 ? uint64_t(g_config.debugFrameScan) * 60 : 0; writing = limit > 0; }
    if (!rt) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = kW; d.Height = kH; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_xrDevice->CreateTexture2D(&d, nullptr, &rt))) { done = true; return; }
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& t : stg) if (FAILED(g_xrDevice->CreateTexture2D(&d, nullptr, &t))) { done = true; return; }
        LOGI("frame scan: capturing %llu frames", (unsigned long long)limit);
    }
    const XrQuaternionf id{0, 0, 0, 1};
    if (!g_reproj.Draw(g_xrContext, src, rt, DXGI_FORMAT_R8G8B8A8_UNORM, kW, kH, fov, id, fov, id, false)) return;
    Drain(false);
    g_xrContext->CopyResource(stg[head], rt); stgSeq[head] = seq; head = (head + 1) % kRing; ++pending;
    if (writing && ++total >= limit) { Drain(true); Flush(); writing = false; LOGI("frame scan: files done (%llu frames)", (unsigned long long)total); if (!g_config.debugGlitchDetect) done = true; }
}
} // namespace scan

void SubmitOneFrame() {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(p_xrWaitFrame(g_session, &wi, &fs))) { Sleep(5); return; }
    g_lastDisplayTime.store(fs.predictedDisplayTime);
    UpdateGaze(fs.predictedDisplayTime);
    if (fs.predictedDisplayPeriod > 0) {
        XrDuration prev = g_displayPeriod.exchange(fs.predictedDisplayPeriod);
        static bool logged = false;
        if (!logged || std::llabs(prev - fs.predictedDisplayPeriod) > 200000) {
            LOGI("headset refresh: %.1f Hz (display period %.3f ms)", 1e9 / double(fs.predictedDisplayPeriod),
                 fs.predictedDisplayPeriod / 1e6);
            logged = true;
        }
    }
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(p_xrBeginFrame(g_session, &bi))) return;
    s_xrFrames++;

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    const XrCompositionLayerBaseHeader* layers[3] = {};
    XrCompositionLayerQuad menuQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad hudQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    uint32_t layerCount = 0;

    static uint64_t lastSeq = 0;
    int slot = g_latestSlot.load();
    if (fs.shouldRender && slot >= 0 && SyncXrShared()) {
        g_readingSlot = slot;
        if (g_latestSlot.load() != slot) slot = g_readingSlot = g_latestSlot.load(); // render thread moved on
        SlotMeta meta;
        { std::lock_guard<std::mutex> lk(g_metaMutex); meta = g_meta[slot]; }
        if (slot >= 0 && (!g_swapchain || g_scWidth != g_xs.width || g_scHeight != g_xs.height || g_scFormat != g_xs.format))
            CreateSwapchain(g_xs.width, g_xs.height, g_xs.format);
        if (slot >= 0 && g_swapchain) {
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo swi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            swi.timeout = 100000000;
            if (XR_SUCCEEDED(p_xrAcquireSwapchainImage(g_swapchain, &ai, &idx)) &&
                XR_SUCCEEDED(p_xrWaitSwapchainImage(g_swapchain, &swi))) {
                bool copied = false;
                // Mod reprojection: warp the frame to the head orientation at this refresh's display
                // time and submit it with that pose, so the runtime has nothing left to correct.
                XrPosef curPose{};
                bool useMod = (meta.mono || meta.stereo) && g_config.reprojection == 1 && g_reproj.Ready() &&
                              LocateHead(fs.predictedDisplayTime, curPose);
                // Stereo: each eye keeps its orientation relative to the head; only the head's rotation since the
                // frame was rendered is corrected (the game runs at 60 fps: on a 90 Hz headset every third refresh
                // repeats a frame, and the runtime did not correct those for head rotation -> judder when turning).
                XrQuaternionf dstEye[2] = {};
                if (useMod && meta.stereo) {
                    auto mul = [](const XrQuaternionf& a, const XrQuaternionf& b) {
                        return XrQuaternionf{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                                             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
                    };
                    const XrQuaternionf& h = meta.hf.pose.orientation;
                    const XrQuaternionf hInv{-h.x, -h.y, -h.z, h.w};
                    for (int e = 0; e < 2; ++e) dstEye[e] = mul(curPose.orientation, mul(hInv, meta.hf.eyePose[e].orientation));
                }
                if (g_xs.mutex[slot] && g_xs.mutex[slot]->AcquireSync(0, 20) == S_OK) {
                    bool srgbDst = g_scSwapFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || g_scSwapFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                    if (useMod && meta.stereo) {
                        const UINT ew = g_scWidth / 2;
                        bool ok = true;
                        for (int e = 0; e < 2 && ok; ++e)
                            ok = g_reproj.DrawRect(g_xrContext, g_xs.tex[slot], g_images[idx].texture, g_scSwapFormat, e * ew, ew, g_scHeight,
                                                   e * 0.5f, 0.5f, meta.hf.eyeFov[e], meta.hf.eyePose[e].orientation,
                                                   meta.hf.eyeFov[e], dstEye[e], srgbDst, e == 1);
                        if (ok) s_reprojected++;
                        else { useMod = false; g_xrContext->CopyResource(g_images[idx].texture, g_xs.tex[slot]); }
                    } else if (useMod && g_reproj.Draw(g_xrContext, g_xs.tex[slot], g_images[idx].texture, g_scSwapFormat,
                                                g_scWidth, g_scHeight, meta.hf.fov, meta.hf.pose.orientation,
                                                meta.hf.fov, curPose.orientation, srgbDst)) {
                        s_reprojected++;
                    } else {
                        useMod = false;
                        g_xrContext->CopyResource(g_images[idx].texture, g_xs.tex[slot]);
                    }
                    if (meta.stereo && meta.seq != lastSeq) { XrFovf whole{-0.785f, 0.785f, 0.785f, -0.785f}; scan::Capture(g_xs.tex[slot], whole, meta.hf.seq); }
                    g_xs.mutex[slot]->ReleaseSync(0);
                    copied = true;
                }
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                p_xrReleaseSwapchainImage(g_swapchain, &ri);
                if (copied) {
                    if (meta.seq != lastSeq) {
                        s_newFrames++; lastSeq = meta.seq;
                        if ((meta.stereo || meta.mono) && meta.hf.baseTime > 0) {
                            int64_t need = fs.predictedDisplayTime - meta.hf.baseTime;
                            if (need > 0 && need < 250000000) {
                                int64_t cur = g_predNeeded.load();
                                g_predNeeded.store(cur > 0 ? cur + (need - cur) / 16 : need);
                                s_predSum += meta.hf.poseTime - meta.hf.baseTime;
                                s_predErrSum += fs.predictedDisplayTime - meta.hf.poseTime;
                                s_predN++;
                            }
                        }
                    }
                    const XrRect2Di rect{{0, 0}, {(int32_t)g_scWidth, (int32_t)g_scHeight}};
                    if (meta.stereo) {
                        const int32_t ew = (int32_t)g_scWidth / 2;
                        for (int e = 0; e < 2; ++e) {
                            pv[e].pose = meta.hf.eyePose[e];
                            if (useMod) pv[e].pose.orientation = dstEye[e];
                            pv[e].fov = meta.hf.eyeFov[e];
                            pv[e].subImage.swapchain = g_swapchain;
                            pv[e].subImage.imageRect = {{e * ew, 0}, {ew, (int32_t)g_scHeight}};
                        }
                        proj.space = g_space;
                        proj.viewCount = 2;
                        proj.views = pv;
                        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
                        s_xrProj++;
                        s_stereoFrames++;
                    } else if (meta.mono) {
                        XrPosef layerPose = useMod ? curPose : meta.hf.pose;
                        YawPose(layerPose, g_config.debugLayerYawDeg);
                        for (int e = 0; e < 2; ++e) {
                            pv[e].pose = layerPose;
                            pv[e].fov = meta.hf.fov;
                            pv[e].subImage.swapchain = g_swapchain;
                            pv[e].subImage.imageRect = rect;
                        }
                        proj.space = g_space;
                        proj.viewCount = 2;
                        proj.views = pv;
                        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
                        s_xrProj++;
                    } else {
                        quad.space = g_space;
                        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                        quad.subImage.swapchain = g_swapchain;
                        quad.subImage.imageRect = rect;
                        quad.pose.orientation = {0, 0, 0, 1};
                        quad.pose.position = {0.0f, g_config.screenHeight, -g_config.screenDistance};
                        quad.size = {g_config.screenWidth, g_config.screenWidth * float(g_scHeight) / float(g_scWidth)};
                        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                        s_xrQuad++;
                    }
                    layerCount = 1;
                    // HUD panel over the 3D view (head-tracked frames only).
                    if (meta.hud && (meta.mono || meta.stereo) && g_hudVisible.load()) {
                        static uint64_t lastHudSeq = 0;
                        if (meta.seq != lastHudSeq) { if (UpdateHudImage(slot)) lastHudSeq = meta.seq; }
                        if (g_hudScHasImage && HudQuadPose(hudQuad.space, hudQuad.pose)) {
                            hudQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                            hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                            hudQuad.subImage.swapchain = g_hudSwapchain;
                            hudQuad.subImage.imageRect = {{0, 0}, {(int32_t)g_hudScW, (int32_t)g_hudScH}};
                            hudQuad.size = {g_config.hudWidth, g_config.hudWidth * float(g_hudScH) / float(g_hudScW)};
                            layers[1] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudQuad);
                            layerCount = 2;
                            s_hudLayers++;
                        }
                    }
                }
            }
        }
        g_readingSlot = -1;
    }
    if (layerCount == 0) s_xrEmpty++;
    // Settings menu, on top of everything. World anchor (default): placed in front of where you were
    // looking when it opened and then fixed in space, so you can turn your head to read the edges.
    // Head anchor: follows the headset.
    {
        static vrkit::menu::QuadPlacer placer;
        bool shown = fs.shouldRender && UpdateMenuImage();
        XrPosef head{};
        bool headOk = shown && LocateHead(fs.predictedDisplayTime, head);
        placer.Update(shown, headOk, head);
        if (shown) {
            menuQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            menuQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            menuQuad.subImage.swapchain = g_menu.sc;
            menuQuad.subImage.imageRect = {{0, 0}, {(int32_t)g_menu.scw, (int32_t)g_menu.sch}};
            placer.Apply(g_config.menu, g_space, g_viewSpace, g_menu.scw, g_menu.sch, menuQuad);
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menuQuad);
        }
    }

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layers;
    XrResult r = p_xrEndFrame(g_session, &ei);
    if (XR_FAILED(r)) {
        static int failures = 0;
        if (failures++ < 5) LOGE("xrEndFrame failed: %s", XrStr(r));
    }
}

DWORD WINAPI XrThreadMain(LPVOID) {
    SetThreadDescription(GetCurrentThread(), L"so4vr XR");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    if (!LoadLoader() || !CreateInstance()) { LOGE("VR output disabled for this run"); return 0; }
    bool loggedWaiting = false;
    for (;;) {
        // Find a headset.
        if (g_system == XR_NULL_SYSTEM_ID) {
            int r = TryGetSystem();
            if (r < 0) { LOGE("VR output disabled for this run"); return 0; }
            if (r == 0) {
                if (!loggedWaiting) { LOGI("no headset available yet; retrying every 2 s"); loggedWaiting = true; }
                Sleep(2000);
                continue;
            }
        }
        if (!g_session && !CreateSession()) { LOGE("VR session creation failed; retrying in 5 s"); Sleep(5000); continue; }
        // Frame loop.
        if (!HandleEvents()) { TearDownSession(); g_system = XR_NULL_SYSTEM_ID; Sleep(2000); continue; }
        if (!g_running) { Sleep(10); continue; }
        SubmitOneFrame();
    }
}

// =====================================================================================
// Render thread helpers
// =====================================================================================

// View matrix of the ViewInfo snapshot the renderer is drawing (read slot), or null.
const float* RenderedView() {
    static float view[16];
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t ring = 0; int64_t idx = -1;
    if (!off::kCameraKnown || !off::kViewInfoRing) return nullptr;
    if (!SafeLoad(base + off::kViewInfoRing, ring) || !ring || !SafeLoad(base + off::kViewInfoReadIdx, idx) || idx < 0 || idx > 1)
        return nullptr;
    if (!SafeRead(view, (void*)(ring + 0x10 + uintptr_t(idx) * off::kViewInfoStride + off::kViView), 64)) return nullptr;
    return view;
}

const float* ViewForThisFrame() {
    static float v[16];
    if (HeadTrackTakeRenderView(v)) return v;   // exact: captured when the renderer advanced
    return RenderedView();
}

bool StartXr(IDXGISwapChain* sc) {
    if (!sc || FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_gameDevice)) || !g_gameDevice) {
        LOGE("could not get the game's D3D11 device from the swapchain");
        return false;
    }
    g_gameDevice->GetImmediateContext(&g_gameContext);
    IDXGIDevice* dxgiDev = nullptr;
    if (SUCCEEDED(g_gameDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) {
        IDXGIAdapter* ad = nullptr;
        if (SUCCEEDED(dxgiDev->GetAdapter(&ad))) { DXGI_ADAPTER_DESC d{}; ad->GetDesc(&d); g_gameLuid = d.AdapterLuid; ad->Release(); }
        dxgiDev->Release();
    }
    if (g_config.d3dMultithreadProtect) {
        ID3D11Multithread* mt = nullptr;
        if (SUCCEEDED(g_gameContext->QueryInterface(__uuidof(ID3D11Multithread), (void**)&mt)) && mt) {
            mt->SetMultithreadProtected(TRUE);
            mt->Release();
            LOGI("D3D11 multithread protection enabled on the game device");
        }
    }
    HANDLE t = CreateThread(nullptr, 0, XrThreadMain, nullptr, 0, nullptr);
    if (!t) { LOGE("could not start the XR thread"); return false; }
    CloseHandle(t);
    LOGI("XR thread started");
    return true;
}

} // namespace

void XrOnResize() { /* the shared ring follows the back buffer size automatically */ }

// Render thread: copy this frame's HUD into the slot's HUD texture.
bool PublishHud(int slot, ID3D11Texture2D* hud) {
    if (!hud) return false;
    D3D11_TEXTURE2D_DESC d{}; hud->GetDesc(&d);
    if (!EnsureHudSet(d.Width, d.Height, d.Format)) return false;
    if (g_hudShared.gameMutex[slot]->AcquireSync(0, 20) != S_OK) return false;
    g_gameContext->CopyResource(g_hudShared.gameTex[slot], hud);
    g_hudShared.gameMutex[slot]->ReleaseSync(0);
    return true;
}

void XrOnPresent(IDXGISwapChain* sc) {
    if (!g_config.vrOutput || g_initFailed) return;
    if (!g_threadStarted.exchange(true)) {
        if (!StartXr(sc)) { g_initFailed = true; return; }
    }

    VrsEndOfFrame(g_gameContext);
    MenuOnPresent(g_gameDevice, g_gameContext);
    HeadTrackNotePresent();
    // Identify this frame's head pose (also keeps the frame history moving when not in VR).
    HeadFrame hf;
    bool mono = HeadTrackedMode() && So4HtTakeFrame(hf);   // SO4: matched through the per-view constant upload
    if (g_stereoPublished.exchange(false)) return; // native stereo already handed over this frame's eyes
    // Native stereo: a frame without stereo eyes right after stereo ones (a skipped stereo frame, a loading or
    // transition frame) is not shown; the headset keeps the last stereo frame. Only when stereo has stopped
    // for a moment (title, menus, flat mode) does the flat image take over. Switching every frame would
    // reallocate the shared textures back and forth between the two sizes.
    if (g_config.activeMode == Config::Native && GetTickCount64() - g_lastStereoTick.load() < 750) { s_flatHeld++; return; }
    if (!g_running.load()) return; // nobody to hand frames to

    ID3D11Texture2D* back = nullptr;
    ID3D11Texture2D* monoFrame = NativeTakeMonoFrame();
    if (mono && monoFrame) { back = monoFrame; back->AddRef(); }
    else if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return;
    D3D11_TEXTURE2D_DESC bd{};
    back->GetDesc(&bd);
    if (bd.SampleDesc.Count != 1 || !EnsureSharedSet(bd.Width, bd.Height, bd.Format)) { back->Release(); return; }

    // Pick a slot that is neither the newest (may be in use next) nor being read right now.
    int latest = g_latestSlot.load(), reading = g_readingSlot.load();
    int slot = -1;
    for (int i = 1; i <= kSlots; ++i) {
        int c = ((latest < 0 ? 0 : latest) + i) % kSlots;
        if (c != reading && c != latest) { slot = c; break; }
    }
    if (slot < 0) { s_copySkipped++; back->Release(); return; }

    if (g_shared.gameMutex[slot]->AcquireSync(0, 20) == S_OK) {
        // Sharpen only our own mono copy (never hold references on the swapchain's buffers).
        ID3D11Texture2D* img = (mono && back == monoFrame) ? SharpenForHeadset(g_gameContext, back, 0) : back;
        g_gameContext->CopyResource(g_shared.gameTex[slot], img);
        g_shared.gameMutex[slot]->ReleaseSync(0);
        bool hasHud = mono && PublishHud(slot, HudTakeFrame());
        {
            std::lock_guard<std::mutex> lk(g_metaMutex);
            g_meta[slot].mono = mono;
            g_meta[slot].stereo = false;
            g_meta[slot].hud = hasHud;
            if (mono) g_meta[slot].hf = hf;
            g_meta[slot].seq = g_latestSeq.fetch_add(1) + 1;
        }
        g_latestSlot = slot;
        s_published++;
    } else {
        s_copySkipped++;
    }
    back->Release();
}

void XrPublishStereo(ID3D11Texture2D* left, ID3D11Texture2D* right, const HeadFrame& hf, ID3D11Texture2D* hud) {
    VrsEndOfFrame(g_gameContext);
    if (!g_config.vrOutput || g_initFailed || !g_gameContext || !g_running.load() || !left || !right) return;
    D3D11_TEXTURE2D_DESC d{};
    left->GetDesc(&d);
    if (!EnsureSharedSet(d.Width * 2, d.Height, d.Format)) return;
    int latest = g_latestSlot.load(), reading = g_readingSlot.load();
    int slot = -1;
    for (int i = 1; i <= kSlots; ++i) {
        int c = ((latest < 0 ? 0 : latest) + i) % kSlots;
        if (c != reading && c != latest) { slot = c; break; }
    }
    if (slot < 0) { s_copySkipped++; return; }
    if (g_shared.gameMutex[slot]->AcquireSync(0, 20) != S_OK) { s_copySkipped++; return; }
    left = SharpenForHeadset(g_gameContext, left, 0);
    right = SharpenForHeadset(g_gameContext, right, 1);
    g_gameContext->CopySubresourceRegion(g_shared.gameTex[slot], 0, 0, 0, 0, left, 0, nullptr);
    g_gameContext->CopySubresourceRegion(g_shared.gameTex[slot], 0, d.Width, 0, 0, right, 0, nullptr);
    g_shared.gameMutex[slot]->ReleaseSync(0);
    bool hasHud = PublishHud(slot, hud);
    {
        std::lock_guard<std::mutex> lk(g_metaMutex);
        g_meta[slot].mono = true;
        g_meta[slot].stereo = true;
        g_meta[slot].hud = hasHud;
        g_meta[slot].hf = hf;
        g_meta[slot].seq = g_latestSeq.fetch_add(1) + 1;
    }
    g_latestSlot = slot;
    s_published++;
    g_stereoPublished = true;
    g_lastStereoTick.store(GetTickCount64());
}

void XrPublishMenu(ID3D11Texture2D* tex) {
    if (!tex || !g_gameDevice || !g_gameContext) { g_menu.visible = false; return; }
    D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
    if (!g_menu.gameTex || g_menu.w != d.Width || g_menu.h != d.Height) {
        std::lock_guard<std::mutex> lk(g_menuMutex);
        g_menu.visible = false;
        SafeRelease(g_menu.gameMutex); SafeRelease(g_menu.gameTex);
        if (g_menu.handle) { CloseHandle(g_menu.handle); g_menu.handle = nullptr; }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = d.Width; td.Height = d.Height; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (FAILED(g_gameDevice->CreateTexture2D(&td, nullptr, &g_menu.gameTex))) { LOGE("menu: shared texture creation failed"); return; }
        TemporalMarkModTexture(g_menu.gameTex);
        TemporalMarkModTexture(tex);   // the menu's own (vrkit) texture
        g_menu.gameTex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&g_menu.gameMutex);
        IDXGIResource1* res = nullptr;
        if (SUCCEEDED(g_menu.gameTex->QueryInterface(__uuidof(IDXGIResource1), (void**)&res))) {
            res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &g_menu.handle);
            res->Release();
        }
        if (!g_menu.handle || !g_menu.gameMutex) { LOGE("menu: shared handle creation failed"); SafeRelease(g_menu.gameMutex); SafeRelease(g_menu.gameTex); return; }
        g_menu.w = d.Width; g_menu.h = d.Height;
        g_menu.gen.fetch_add(1);
    }
    if (g_menu.gameMutex->AcquireSync(0, 5) == S_OK) {
        g_gameContext->CopyResource(g_menu.gameTex, tex);
        g_menu.gameMutex->ReleaseSync(0);
        g_menu.seq.fetch_add(1);
    }
    g_menu.visible = true;
}

void XrToggleHud() { bool v = !g_hudVisible.load(); g_hudVisible = v; LOGI("F10: HUD panel %s", v ? "shown" : "hidden"); }

bool XrLocateEyesForGameFrame(XrView views[2], int64_t* baseTime, int64_t* poseTime) {
    if (!g_running.load()) return false;
    std::lock_guard<std::mutex> lk(g_sessionMutex);
    if (!g_running.load() || !g_session || !g_space || !p_xrLocateViews) return false;
    XrTime last = g_lastDisplayTime.load();
    if (last == 0) return false;
    // The game prepares a frame about one game frame before it is drawn and published.
    XrTime t = last + XrTime(double(g_displayPeriod.load()) * g_config.predictionFrames);
    int64_t needNs = g_predNeeded.load();
    if (g_config.predictionAuto && needNs > 0) t = last + needNs;
    if (baseTime) *baseTime = last;
    if (poseTime) *poseTime = t;
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = t;
    li.space = g_space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    views[0] = {XR_TYPE_VIEW}; views[1] = {XR_TYPE_VIEW};
    uint32_t n = 0;
    if (XR_FAILED(p_xrLocateViews(g_session, &li, &vs, 2, &n, views)) || n != 2) return false;
    const XrViewStateFlags need = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    ApplyFakeHead(t, views, 2);
    static bool loggedEyes = false;
    if (!loggedEyes) {
        loggedEyes = true;
        const XrQuaternionf& a = views[0].pose.orientation; const XrQuaternionf& b = views[1].pose.orientation;
        float dot = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
        float angle = 2.0f * std::acos(dot > 1.0f ? 1.0f : dot) * 57.29578f;
        LOGI("eye views: IPD %.1f mm, angle between eyes %.2f deg (canting), FOV L(%.1f %.1f %.1f %.1f) R(%.1f %.1f %.1f %.1f) deg",
             1000.0f * std::sqrt(std::pow(views[0].pose.position.x - views[1].pose.position.x, 2.0f) +
                                 std::pow(views[0].pose.position.y - views[1].pose.position.y, 2.0f) +
                                 std::pow(views[0].pose.position.z - views[1].pose.position.z, 2.0f)),
             angle,
             views[0].fov.angleLeft * 57.29578f, views[0].fov.angleRight * 57.29578f, views[0].fov.angleUp * 57.29578f, views[0].fov.angleDown * 57.29578f,
             views[1].fov.angleLeft * 57.29578f, views[1].fov.angleRight * 57.29578f, views[1].fov.angleUp * 57.29578f, views[1].fov.angleDown * 57.29578f);
    }
    return (vs.viewStateFlags & need) == need;
}

bool XrGazeTangents(float& tx, float& ty) {
    uint64_t t = g_gazeTick.load();
    if (!t || GetTickCount64() - t > 250) return false;
    tx = g_gazeTx.load(); ty = g_gazeTy.load();
    return true;
}

void XrReport(char* buf, size_t size) {
    snprintf(buf, size, "xr thread: %llu headset frames (%llu head-tracked [%llu stereo], %llu flat, %llu empty, %llu mod-reprojected), %llu new game frames shown, HUD panel %llu (updates %llu) | game frames published %llu, skipped %llu, flat held back %llu",
             (unsigned long long)s_xrFrames.exchange(0), (unsigned long long)s_xrProj.exchange(0), (unsigned long long)s_stereoFrames.exchange(0),
             (unsigned long long)s_xrQuad.exchange(0), (unsigned long long)s_xrEmpty.exchange(0),
             (unsigned long long)s_reprojected.exchange(0),
             (unsigned long long)s_newFrames.exchange(0), (unsigned long long)s_hudLayers.exchange(0), (unsigned long long)s_hudCopies.exchange(0), (unsigned long long)s_published.exchange(0),
             (unsigned long long)s_copySkipped.exchange(0), (unsigned long long)s_flatHeld.exchange(0));
    int64_t n = s_predN.exchange(0), ps = s_predSum.exchange(0), pe = s_predErrSum.exchange(0);
    size_t k = strlen(buf);
    if (n > 0 && k + 1 < size)
        snprintf(buf + k, size - k, " | pose predicted %.1f ms ahead, shown %.1f ms after that (%s, needed %.1f ms)",
                 ps / 1e6 / n, pe / 1e6 / n, g_config.predictionAuto ? "auto" : "fixed", g_predNeeded.load() / 1e6);
}

} // namespace so4vr
