// Proxy for xinput1_3.dll. StarOceanTheLastHope.exe imports this DLL statically, so placing our
// build in the game folder gets us loaded before the game's own code runs.
// Every export (names 2-8, unnamed ordinals 100-103) is forwarded to the real system DLL
// (System32\xinput1_3.dll from the DirectX runtime; xinput1_4.dll if that is missing).
#include <windows.h>
#include "log.h"
#include "config.h"
#include <atomic>
#include <string>

namespace {

using PFN_GetState        = DWORD (WINAPI*)(DWORD, void*);
using PFN_SetState        = DWORD (WINAPI*)(DWORD, void*);
using PFN_GetCapabilities = DWORD (WINAPI*)(DWORD, DWORD, void*);
using PFN_GetDSoundGuids  = DWORD (WINAPI*)(DWORD, GUID*, GUID*);
using PFN_Enable          = void  (WINAPI*)(BOOL);
using PFN_GetBattery      = DWORD (WINAPI*)(DWORD, BYTE, void*);
using PFN_GetKeystroke    = DWORD (WINAPI*)(DWORD, DWORD, void*);
using PFN_WaitGuide       = DWORD (WINAPI*)(DWORD, DWORD, void*);
using PFN_UserIndex       = DWORD (WINAPI*)(DWORD);

HMODULE             g_real = nullptr;
PFN_GetState        p_GetState = nullptr;
PFN_SetState        p_SetState = nullptr;
PFN_GetCapabilities p_GetCapabilities = nullptr;
PFN_GetDSoundGuids  p_GetDSoundGuids = nullptr;
PFN_Enable          p_Enable = nullptr;
PFN_GetBattery      p_GetBattery = nullptr;
PFN_GetKeystroke    p_GetKeystroke = nullptr;
PFN_GetState        p_GetStateEx = nullptr;
PFN_WaitGuide       p_WaitGuide = nullptr;
PFN_UserIndex       p_CancelGuide = nullptr;
PFN_UserIndex       p_PowerOff = nullptr;
INIT_ONCE           g_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK LoadReal(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 32) return TRUE;
    std::wstring dir(path);
    g_real = LoadLibraryW((dir + L"\\xinput1_3.dll").c_str());
    const char* which = "xinput1_3.dll";
    if (!g_real) { g_real = LoadLibraryW((dir + L"\\xinput1_4.dll").c_str()); which = "xinput1_4.dll"; }
    if (!g_real) { LOGE("failed to load system xinput1_3.dll / xinput1_4.dll (err %lu)", GetLastError()); return TRUE; }
    p_GetState        = (PFN_GetState)GetProcAddress(g_real, "XInputGetState");
    p_SetState        = (PFN_SetState)GetProcAddress(g_real, "XInputSetState");
    p_GetCapabilities = (PFN_GetCapabilities)GetProcAddress(g_real, "XInputGetCapabilities");
    p_GetDSoundGuids  = (PFN_GetDSoundGuids)GetProcAddress(g_real, "XInputGetDSoundAudioDeviceGuids");
    p_Enable          = (PFN_Enable)GetProcAddress(g_real, "XInputEnable");
    p_GetBattery      = (PFN_GetBattery)GetProcAddress(g_real, "XInputGetBatteryInformation");
    p_GetKeystroke    = (PFN_GetKeystroke)GetProcAddress(g_real, "XInputGetKeystroke");
    p_GetStateEx      = (PFN_GetState)GetProcAddress(g_real, (LPCSTR)100);
    p_WaitGuide       = (PFN_WaitGuide)GetProcAddress(g_real, (LPCSTR)101);
    p_CancelGuide     = (PFN_UserIndex)GetProcAddress(g_real, (LPCSTR)102);
    p_PowerOff        = (PFN_UserIndex)GetProcAddress(g_real, (LPCSTR)103);
    LOGI("system %s loaded at %p", which, g_real);
    return TRUE;
}

inline void EnsureReal() { InitOnceExecuteOnce(&g_once, LoadReal, nullptr, nullptr); }

} // namespace

extern "C" {

// Diagnostic (DebugFakePad): pad 0 reports keyboard-driven buttons, so automated tests can press gamepad-only
// actions. F5 = RB, F6 = LB, F7 = RT, F2 = Start, F3 = Back, I/K/J/L = D-pad, Numpad 8/2/4/6 = left stick, 7/9 right stick, 1/0/3/5 = A/B/X/Y.
static std::atomic<unsigned> s_getStateCalls{0};
static bool s_fakeArmed = false;   // the fake pad appears only after F5 is first pressed (menus stay keyboard)
DWORD WINAPI Proxy_XInputGetState(DWORD user, void* state) {
    EnsureReal();
    DWORD r = p_GetState ? p_GetState(user, state) : ERROR_DEVICE_NOT_CONNECTED;
    { static std::atomic<unsigned> n{0}; unsigned k = ++n; if (k == 1) LOGI("XInputGetState first call (user %lu, real %lu)", user, r); }
    if (so4vr::g_config.debugFakePad && !s_fakeArmed && ((GetAsyncKeyState(VK_F5) & 0x8000) || so4vr::g_config.debugFakePadAlways)) s_fakeArmed = true;
    if (so4vr::g_config.debugFakePad && s_fakeArmed && user == 0 && state) {
        unsigned n = ++s_getStateCalls;
        if (n == 1 || (n % 3000) == 0) LOGI("fake pad: XInputGetState calls %u (real result %lu)", n, r);
        static DWORD packet = 0;
        uint8_t* st = static_cast<uint8_t*>(state);
        if (r != ERROR_SUCCESS) memset(st, 0, 16);
        *reinterpret_cast<DWORD*>(st) = ++packet;
        WORD& b = *reinterpret_cast<WORD*>(st + 4);
        auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        if (down(VK_F5)) b |= 0x0200;
        if (down(VK_F6)) b |= 0x0100;
        if (down(VK_F7)) st[7] = 255;
        if (down(VK_NUMPAD1)) b |= 0x1000;   // A
        if (down(VK_NUMPAD0)) b |= 0x2000;   // B
        if (down(VK_NUMPAD3)) b |= 0x4000;   // X
        if (down(VK_NUMPAD5)) b |= 0x8000;   // Y
        if (down(VK_F2)) b |= 0x0010;        // START
        if (down(VK_F3)) b |= 0x0020;        // BACK
        if (down('I')) b |= 0x0001; if (down('K')) b |= 0x0002;   // D-pad up / down
        if (down('J')) b |= 0x0004; if (down('L')) b |= 0x0008;   // D-pad left / right
        short& lx = *reinterpret_cast<short*>(st + 8); short& ly = *reinterpret_cast<short*>(st + 10);
        if (down(VK_NUMPAD8)) ly = 32767; if (down(VK_NUMPAD2)) ly = -32767;
        if (down(VK_NUMPAD4)) lx = -32767; if (down(VK_NUMPAD6)) lx = 32767;
        short& rx = *reinterpret_cast<short*>(st + 12);
        if (down(VK_NUMPAD7)) rx = -32767; if (down(VK_NUMPAD9)) rx = 32767;   // right stick (camera turn)
        r = ERROR_SUCCESS;
    }
    return r;
}

DWORD WINAPI Proxy_XInputSetState(DWORD user, void* vib) {
    EnsureReal();
    return p_SetState ? p_SetState(user, vib) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetCapabilities(DWORD user, DWORD flags, void* caps) {
    EnsureReal();
    DWORD r = p_GetCapabilities ? p_GetCapabilities(user, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
    if (so4vr::g_config.debugFakePad && so4vr::g_config.debugFakePadAlways) s_fakeArmed = true;   // the game only polls pads whose capabilities succeed
    { static std::atomic<unsigned> n{0}; unsigned k = ++n; if (k == 1 || k % 2000 == 0) LOGI("XInputGetCapabilities(%lu) call %u -> real %lu (fake pad %d)", user, k, r, (int)(so4vr::g_config.debugFakePad && s_fakeArmed)); }
    if (so4vr::g_config.debugFakePad && s_fakeArmed && user == 0 && caps && r != ERROR_SUCCESS) {
        memset(caps, 0, 20); static_cast<uint8_t*>(caps)[0] = 1; static_cast<uint8_t*>(caps)[1] = 1;   // gamepad
        *reinterpret_cast<WORD*>(static_cast<uint8_t*>(caps) + 4) = 0xF3FF;
        r = ERROR_SUCCESS;
    }
    return r;
}

DWORD WINAPI Proxy_XInputGetDSoundAudioDeviceGuids(DWORD user, GUID* render, GUID* capture) {
    EnsureReal();
    return p_GetDSoundGuids ? p_GetDSoundGuids(user, render, capture) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetStateEx(DWORD user, void* state) {
    EnsureReal();
    if (!p_GetStateEx) return Proxy_XInputGetState(user, state);
    return p_GetStateEx(user, state);
}

void WINAPI Proxy_XInputEnable(BOOL enable) { EnsureReal(); if (p_Enable) p_Enable(enable); }

DWORD WINAPI Proxy_XInputGetBatteryInformation(DWORD user, BYTE type, void* info) {
    EnsureReal();
    return p_GetBattery ? p_GetBattery(user, type, info) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputGetKeystroke(DWORD user, DWORD reserved, void* ks) {
    EnsureReal();
    return p_GetKeystroke ? p_GetKeystroke(user, reserved, ks) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputWaitForGuideButton(DWORD user, DWORD flags, void* listen) {
    EnsureReal();
    return p_WaitGuide ? p_WaitGuide(user, flags, listen) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputCancelGuideButtonWait(DWORD user) {
    EnsureReal();
    return p_CancelGuide ? p_CancelGuide(user) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Proxy_XInputPowerOffController(DWORD user) {
    EnsureReal();
    return p_PowerOff ? p_PowerOff(user) : ERROR_DEVICE_NOT_CONNECTED;
}

} // extern "C"
