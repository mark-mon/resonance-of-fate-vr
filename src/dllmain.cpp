#include <windows.h>
#include <string>
#include "log.h"
#include "config.h"
#include "d3d_hooks.h"
#include "camera_probe.h"
#include "render_res.h"
#include "image_quality.h"
#include "rdoc.h"
#include "game_offsets.h"
#include "camera_scan.h"
#include "so4_camera.h"
#include "so4_stereo.h"

#ifdef ROFVR_EE
#define SO4VR_VERSION "1.1.0"   // Enhanced Edition build (xinput9_1_0.dll)
#else
#define SO4VR_VERSION "1.0.0"   // 4K/HD build (xinput1_3.dll) - the 1.1.0 release ships the unchanged 1.0.0 binary
#endif

namespace so4vr { void CrashLogInstall(HMODULE self); bool RofInputInstall(); bool InstallD3D12Bridge(); bool EeProbeInstall(const std::wstring& dir); bool EeRenderInstall(); }

namespace {

// The build we reverse-engineer against (Steam, "Resonance of Fate.exe" ver.1.0.0.4, linked 2019-01-30).
constexpr DWORD kKnownExeSize = 22071664;
constexpr DWORD kKnownExeTimestamp = 1548819852;
// Enhanced Remaster (2026-10 update, Direct3D 12, "RESONANCE OF FATE/END OF ETERNITY ENHANCED REMASTER ver.1.0.0.4").
constexpr DWORD kEeExeSize = 37080008;
constexpr DWORD kEeExeTimestamp = 1792059323;
bool exeIsEe = false;

std::wstring g_gameDir;

bool exeMatches = false;

void LogExeInfo() {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<BYTE*>(exe) + dos->e_lfanew);
    DWORD ts = nt->FileHeader.TimeDateStamp;

    wchar_t path[MAX_PATH]; GetModuleFileNameW(exe, path, MAX_PATH);
    DWORD size = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) { size = GetFileSize(h, nullptr); CloseHandle(h); }

    char p8[MAX_PATH * 3]; WideCharToMultiByte(CP_UTF8, 0, path, -1, p8, sizeof(p8), nullptr, nullptr);
    LOGI("exe: %s base=%p size=%lu timestamp=%lu", p8, exe, size, ts);
    if (size == kEeExeSize && ts == kEeExeTimestamp) { LOGI("exe is the Enhanced Remaster (Direct3D 12)"); exeIsEe = true; return; }
    if (size != kKnownExeSize || ts != kKnownExeTimestamp)
        LOGW("Resonance of Fate.exe differs from the build this mod was made for (expected size %lu, timestamp %lu). "
             "The game may have been updated; later stages may need new offsets.", kKnownExeSize, kKnownExeTimestamp);
    else {
        LOGI("exe matches known build");
        exeMatches = true;
    }
}

// Known code bytes at hook targets (from the decrypted image). All must match before any game hook is installed.
struct CodeSig { uintptr_t rva; unsigned char b[8]; };
const CodeSig kSigs[] = {
    {0x2746F0, {0x40, 0x53, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC}},   // UpdateCamera
    {0x1B9AF0, {0x48, 0x83, 0xEC, 0x68, 0x0F, 0x29, 0x74, 0x24}},   // view inverse (rigid)
    {0x0D50F0, {0x48, 0x8B, 0xC4, 0x48, 0x81, 0xEC, 0xB8, 0x00}},   // view inverse (scaled)
    {0x2A2FA0, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C}},   // render command processor
    {0x2A30C3, {0xFF, 0x90, 0x48, 0x02, 0x00, 0x00, 0xF7, 0x87}},   // job call site
    {0x28DE80, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x80, 0x79}},   // frame end
    {0x7DFBF0, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83}},   // pad poll
    {0x7DFDE0, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B}},   // pad find-active
};
bool CodeDecrypted() {
    auto base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    for (const CodeSig& c : kSigs) {
        if (memcmp(base + c.rva, c.b, 8) != 0) return false;
        MEMORY_BASIC_INFORMATION m{};   // SteamStub restores PAGE_EXECUTE_READ only after decrypting
        if (!VirtualQuery(base + c.rva, &m, sizeof m) ||
            !(m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    }
    return true;
}
DWORD WINAPI DeferredGameHooks(LPVOID) {
    ULONGLONG t0 = GetTickCount64();
    while (!CodeDecrypted()) {
        if (GetTickCount64() - t0 > 120000) { LOGW("game code never matched the known bytes; camera / stereo hooks NOT installed"); return 0; }
        Sleep(20);
    }
    Sleep(200);   // let the stub finish (it may still be fixing up other sections)
    LOGI("game code decrypted after %llu ms; installing game hooks", (unsigned long long)(GetTickCount64() - t0));
    if (so4vr::off::kRofCameraMapped) so4vr::So4CameraInstall();   // camera hooks: main-camera detection + head tracking
    if (so4vr::off::kRofStereoMapped) so4vr::So4StereoInstall();   // render-job recording (native stereo replay)
    so4vr::RofInputInstall();
    return 0;
}

bool IsTargetProcess() {
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"Resonance of Fate.exe") == 0;
}

void Startup(HMODULE self) {
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_gameDir = path;
    g_gameDir = g_gameDir.substr(0, g_gameDir.find_last_of(L'\\'));

    so4vr::Log::Open((g_gameDir + L"\\rofvr.log").c_str());
    LOGI("Resonance of Fate VR mod (rofvr) %s", SO4VR_VERSION);
    so4vr::CrashLogInstall(self);
    LogExeInfo();
    so4vr::LoadConfig(g_gameDir);

    if (!so4vr::g_config.enabled) { LOGI("Enabled=0 in config.txt; running as plain XInput passthrough"); return; }
#ifdef ROFVR_EE
    // Enhanced Remaster build of the mod (xinput9_1_0.dll): Direct3D 12. Stage 1: the game on the flat virtual
    // screen; the D3D11 hooks and the 4K/HD edition's game hooks are not used.
    if (!exeIsEe) { LOGW("this is the Enhanced Remaster build of the mod (xinput9_1_0.dll) but the exe is not the Enhanced Remaster; XInput passthrough only"); return; }
    if (so4vr::g_config.debugEeProbe) { so4vr::g_config.renderMode = so4vr::Config::Flat; so4vr::g_config.activeMode = so4vr::Config::Flat; }
    if (so4vr::InstallD3D12Bridge()) LOGI("EE: D3D12 bridge hooks installed");
    if (!so4vr::g_config.debugEeProbe) CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        // game hooks once the code is executable (SteamStub): camera / head tracking, constant uploads, stereo replay
        auto base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
        static const unsigned char kJob[6] = {0xFF, 0x90, 0x40, 0x02, 0x00, 0x00};
        ULONGLONG t0 = GetTickCount64();
        for (;;) {
            MEMORY_BASIC_INFORMATION m{};
            if (VirtualQuery(base + 0x106A8E, &m, sizeof m) && (m.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) && memcmp(base + 0x106A8E, kJob, 6) == 0) break;
            if (GetTickCount64() - t0 > 120000) { LOGW("EE: game code never became ready; game hooks NOT installed (flat screen only)"); return 0; }
            Sleep(20);
        }
        Sleep(300);
        LOGI("EE: game code ready after %llu ms; installing game hooks", (unsigned long long)(GetTickCount64() - t0));
        bool ok = so4vr::So4CameraInstall();
        ok = so4vr::EeRenderInstall() && ok;
        ok = so4vr::So4StereoInstall() && ok;
        LOGI("EE: game hooks %s (render mode %d)", ok ? "installed" : "PARTLY FAILED", int(so4vr::g_config.activeMode));
        return 0;
    }, nullptr, 0, nullptr));
    if (so4vr::g_config.debugEeProbe) CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        // wait until the game code is executable (SteamStub), then install the probe hooks
        auto base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
        for (int i = 0; i < 6000; ++i) {
            MEMORY_BASIC_INFORMATION m{};
            if (VirtualQuery(base + 0x94B50, &m, sizeof m) && (m.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) break;
            Sleep(20);
        }
        Sleep(500);
        LOGI("EE probe: installing (%s)", so4vr::EeProbeInstall(g_gameDir) ? "ok" : "FAILED");
        return 0;
    }, nullptr, 0, nullptr));
    (void)self;
    return;
#else
    if (exeIsEe) { LOGW("this is the 4K/HD edition build of the mod (xinput1_3.dll); the Enhanced Remaster needs xinput9_1_0.dll"); return; }
#endif
    so4vr::RdocInit(g_gameDir);
    so4vr::CameraScanStart(g_gameDir);
    // Game-code hooks wait until SteamStub has decrypted .text (this DLL is loaded before the exe's entry point,
    // so the code is still encrypted during DllMain; hooking it then fails or gets overwritten).
    if (exeMatches && (so4vr::off::kRofCameraMapped || so4vr::off::kRofStereoMapped))
        CloseHandle(CreateThread(nullptr, 0, DeferredGameHooks, nullptr, 0, nullptr));
    if (so4vr::InstallD3DHooks()) LOGI("bootstrap hooks installed");
    // Camera, render resolution and TAA hooks are game-specific; SO4's are not found yet.
    if (exeMatches && so4vr::off::kCameraKnown) { so4vr::CameraProbeInstall(); so4vr::RenderResInstall(); so4vr::ImageQualityInstall(); }
    else if (!exeMatches) LOGW("game-specific hooks skipped: offsets are only valid for the known Resonance of Fate.exe build");
    else LOGI("flat-screen build: camera / stereo hooks not available yet");
    (void)self;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(inst);
        // Only instrument the game itself; any other process loading this file just gets XInput passthrough.
        if (IsTargetProcess()) Startup(inst);
        break;
    case DLL_PROCESS_DETACH:
        // reserved != nullptr means the process is exiting: don't touch hooks, other threads are gone.
        if (!reserved) so4vr::RemoveD3DHooks();
        LOGI("unloading");
        so4vr::Log::Close();
        break;
    }
    return TRUE;
}
