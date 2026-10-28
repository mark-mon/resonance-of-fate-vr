#include "vrkit_input.h"
#include "vrkit_options.h"
#include <windows.h>
#include <string>
#include <xinput.h>
#include "MinHook.h"

namespace vrkit { namespace input {

namespace {
Settings g_set;
const char* g_extName = "";
using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
struct Hooked { const wchar_t* dll; void* target; GetStateFn orig; GetStateFn origEx; void* targetEx; };
Hooked g_mods[3] = { { L"xinput1_4.dll" }, { L"xinput1_3.dll" }, { L"xinput9_1_0.dll" } };
GetStateFn g_read = nullptr;                // an original XInputGetState for our own polling
// Private copy of System32\xinput1_4.dll under another name (temp folder): Steam Input hooks the XInput DLLs
// the game loads (by name) and hides physical pads from them; a renamed copy reads the hardware directly
// (same approach as the FF XII mod).
GetStateFn g_private = nullptr;
GetStateFn LoadPrivateXInput() {
    wchar_t sys[MAX_PATH], tmp[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH); DWORD t = GetTempPathW(MAX_PATH, tmp);
    if (!n || n > MAX_PATH - 20 || !t || t > MAX_PATH - 32) return nullptr;
    std::wstring src = std::wstring(sys) + L"\\xinput1_4.dll", dst = std::wstring(tmp) + L"vrkit_xinput1_4.dll";
    WIN32_FILE_ATTRIBUTE_DATA a{}, b{};
    bool same = GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &a) && GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &b) &&
                a.nFileSizeLow == b.nFileSizeLow && a.nFileSizeHigh == b.nFileSizeHigh &&
                CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0;
    if (!same && !CopyFileW(src.c_str(), dst.c_str(), FALSE)) { Log("vrkit: could not copy xinput1_4.dll to %ls (error %lu)", dst.c_str(), GetLastError()); }
    HMODULE m = LoadLibraryW(dst.c_str());
    if (!m) { Log("vrkit: could not load the private XInput copy %ls (error %lu)", dst.c_str(), GetLastError()); return nullptr; }
    GetStateFn f = (GetStateFn)GetProcAddress(m, "XInputGetState");
    Log("vrkit: private XInput copy %ls %s", dst.c_str(), f ? "loaded" : "has no XInputGetState");
    return f;
}
volatile bool g_block = false, g_chordHeld = false;
DWORD g_fakePacket[4] = {};

volatile bool g_untilRelease = false;   // after closing: keep blocking until the pad is released
// The pad state the GAME received (its own XInput call through our hook, or the host's report from the game's
// pad code). With Steam Input (PlayStation / Switch / generic pads) the pad exists only as Steam's virtual
// XInput pad inside the game's calls; reading XInput ourselves then finds nothing.
SRWLOCK g_extLock = SRWLOCK_INIT; XINPUT_STATE g_ext = {}; DWORD g_extTick = 0; const char* g_extSrc = "";
void NoteExternal(const XINPUT_STATE& st, const char* src) {
    AcquireSRWLockExclusive(&g_extLock); g_ext = st; g_extTick = GetTickCount(); g_extSrc = src; ReleaseSRWLockExclusive(&g_extLock);
}
DWORD Filter(DWORD r, DWORD user, XINPUT_STATE* st) {
    if (r == ERROR_SUCCESS && st) NoteExternal(*st, "the game's XInput calls");
    if (r == ERROR_SUCCESS && st && g_untilRelease && !g_block) {
        const XINPUT_GAMEPAD& p = st->Gamepad;
        if (!p.wButtons && p.bLeftTrigger < 30 && p.bRightTrigger < 30) g_untilRelease = false;
    }
    // while all chord buttons are down together, the game doesn't see them (no stray L3 / R3 actions)
    if (r == ERROR_SUCCESS && st && g_set.chordButtons && (st->Gamepad.wButtons & g_set.chordButtons) == g_set.chordButtons)
        st->Gamepad.wButtons &= ~g_set.chordButtons;
    if (r == ERROR_SUCCESS && st && (g_block || g_chordHeld || g_untilRelease)) {
        DWORD pkt = ++g_fakePacket[user & 3];
        ZeroMemory(st, sizeof(*st));
        st->dwPacketNumber = pkt;
    }
    return r;
}
// One hook function per DLL (each needs its own trampoline).
template <int N> DWORD WINAPI HkGet(DWORD u, XINPUT_STATE* s) { return Filter(g_mods[N].orig(u, s), u, s); }
template <int N> DWORD WINAPI HkGetEx(DWORD u, XINPUT_STATE* s) { return Filter(g_mods[N].origEx(u, s), u, s); }
void* const kHk[3] = { (void*)HkGet<0>, (void*)HkGet<1>, (void*)HkGet<2> };
void* const kHkEx[3] = { (void*)HkGetEx<0>, (void*)HkGetEx<1>, (void*)HkGetEx<2> };

// polling state
WORD g_prevButtons = 0; bool g_prevKeys[256] = {};
DWORD g_chordStart = 0; bool g_chordFired = false;
DWORD g_repeatAt[4] = {}; uint32_t g_repeatDir = 0;
} // namespace

void Configure(const Settings& s) { g_set = s; }

bool InstallHooks() {
    bool any = false;
    for (int k = 0; k < 3; ++k) {
        Hooked& h = g_mods[k];
        HMODULE m = GetModuleHandleW(h.dll);
        if (!m) continue;
        if (!h.target) {
            void* p = (void*)GetProcAddress(m, "XInputGetState");
            if (p && MH_CreateHook(p, kHk[k], (void**)&h.orig) == MH_OK && MH_EnableHook(p) == MH_OK) { h.target = p; Log("vrkit: hooked XInputGetState in %ls", h.dll); }
            void* px = (void*)GetProcAddress(m, (LPCSTR)100);   // XInputGetStateEx (ordinal 100, includes the guide button)
            if (px && px != p && MH_CreateHook(px, kHkEx[k], (void**)&h.origEx) == MH_OK && MH_EnableHook(px) == MH_OK) h.targetEx = px;
        }
        any = any || h.target;
    }
    // Read the pad through the newest XInput the game uses (1_4 before 1_3 before 9_1_0): Steam Input and
    // some wrappers only serve the newer DLLs, and a proxy xinput9_1_0 (such as the VR mod itself) may be
    // loaded long before the game loads xinput1_4.
    for (int k = 0; k < 3; ++k) if (g_mods[k].orig) { g_read = g_mods[k].orig; break; }
    if (!g_read) {   // the game hasn't loaded XInput (yet): read the pad ourselves
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput1_3.dll");
        if (m) g_read = (GetStateFn)GetProcAddress(m, "XInputGetState");
    }
    return any;
}

void SetBlocking(bool b) { if (g_block && !b) g_untilRelease = true; g_block = b; }
volatile bool g_chordDown = false;   // all chord buttons down right now (before the hold time)
bool GameInputBlocked() { return g_block || g_chordHeld || g_untilRelease || g_chordDown; }
volatile LONG g_injected = 0;
void Inject(uint32_t b) { InterlockedOr(&g_injected, (LONG)b); }
volatile int g_padUser = -1;
int PadUser() { return g_padUser; }
void SetExternalState(const void* st) { if (st) NoteExternal(*static_cast<const XINPUT_STATE*>(st), "the game's pad code"); }

uint32_t Poll() {
    uint32_t out = (uint32_t)InterlockedExchange(&g_injected, 0);
    // --- gamepad (first connected)
    XINPUT_STATE st = {}; bool pad = false;
    int padUser = -1;
    // Try every hooked XInput (newest first), then the fallback reader: a pad may only be visible through one.
    GetStateFn readers[5] = {}; int nr = 0;
    static bool privateTried = false;
    if (!privateTried) { privateTried = true; g_private = LoadPrivateXInput(); }
    if (g_private) readers[nr++] = g_private;   // first: the hardware, past Steam Input
    for (int k = 0; k < 3; ++k) if (g_mods[k].orig) readers[nr++] = g_mods[k].orig;
    if (nr == (g_private ? 1 : 0) && g_read) readers[nr++] = g_read;
    int which = -1;
    for (int r = 0; r < nr && !pad; ++r)
        for (DWORD u = 0; u < 4 && !pad; ++u) { ZeroMemory(&st, sizeof(st)); pad = readers[r](u, &st) == ERROR_SUCCESS; if (pad) { which = r; padUser = (int)u; } }
    g_padUser = padUser;
    {   // the state the game itself received (recent = within 250 ms): used when we can't see the pad ourselves,
        // and its buttons are merged in otherwise (Steam Input's virtual pad, wrappers)
        AcquireSRWLockShared(&g_extLock);
        const bool recent = g_extTick && GetTickCount() - g_extTick <= 250;
        XINPUT_STATE ext = g_ext; const char* src = g_extSrc;
        ReleaseSRWLockShared(&g_extLock);
        if (recent) {
            if (!pad) { st = ext; pad = true; which = 100; padUser = 0; g_extName = src; }
            else {
                st.Gamepad.wButtons |= ext.Gamepad.wButtons;
                if (ext.Gamepad.bLeftTrigger > st.Gamepad.bLeftTrigger) st.Gamepad.bLeftTrigger = ext.Gamepad.bLeftTrigger;
                if (ext.Gamepad.bRightTrigger > st.Gamepad.bRightTrigger) st.Gamepad.bRightTrigger = ext.Gamepad.bRightTrigger;
                auto big = [](SHORT a, SHORT b) { return (b < 0 ? -(int)b : (int)b) > (a < 0 ? -(int)a : (int)a) ? b : a; };
                st.Gamepad.sThumbLX = big(st.Gamepad.sThumbLX, ext.Gamepad.sThumbLX); st.Gamepad.sThumbLY = big(st.Gamepad.sThumbLY, ext.Gamepad.sThumbLY);
            }
        }
    }
    {   // log once whenever the pad source (or its absence) changes, so missing gamepad input can be diagnosed
        static int lastWhich = -2;
        if (which != lastWhich) {
            lastWhich = which;
            int k = -1, c = g_private ? 0 : -1;
            for (int m = 0; m < 3; ++m) if (g_mods[m].orig) { ++c; if (c == which) { k = m; break; } }
            if (which == 100) Log("vrkit: gamepad seen only through %s (e.g. Steam Input's virtual pad)", g_extName);
            else if (pad && g_private && which == 0) Log("vrkit: gamepad read via the private xinput1_4 copy, user %d", padUser);
            else if (pad) Log("vrkit: gamepad read via %ls, user %d", k >= 0 ? g_mods[k].dll : L"fallback", padUser);
            else Log("vrkit: no XInput gamepad found (%d readers)", nr);
        }
    }
    WORD b = pad ? st.Gamepad.wButtons : 0;
    float lt = pad ? st.Gamepad.bLeftTrigger / 255.f : 0.f, rt = pad ? st.Gamepad.bRightTrigger / 255.f : 0.f;
    // stick as d-pad
    if (pad) {
        const SHORT dz = 16000;
        if (st.Gamepad.sThumbLY > dz) b |= XINPUT_GAMEPAD_DPAD_UP;
        if (st.Gamepad.sThumbLY < -dz) b |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (st.Gamepad.sThumbLX > dz) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (st.Gamepad.sThumbLX < -dz) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    }
    // after closing, blocking ends once the pad is released (also for games that never call XInput)
    if (g_untilRelease && !g_block && pad && !st.Gamepad.wButtons && st.Gamepad.bLeftTrigger < 30 && st.Gamepad.bRightTrigger < 30)
        g_untilRelease = false;
    if (g_untilRelease && !pad) g_untilRelease = false;
    WORD pressed = b & ~g_prevButtons;
    g_prevButtons = b;
    // --- open / close chord (default L3 + R3)
    DWORD now = GetTickCount();
    bool chord = pad && (g_set.chordButtons || g_set.chordTriggers);
    if (g_set.chordButtons) chord = chord && (st.Gamepad.wButtons & g_set.chordButtons) == g_set.chordButtons;
    if (g_set.chordTriggers) chord = chord && lt > g_set.triggerThreshold && rt > g_set.triggerThreshold;
    g_chordDown = chord;
    if (chord) {
        if (!g_chordStart) g_chordStart = now;
        if (!g_chordFired && now - g_chordStart >= (DWORD)g_set.chordHoldMs) { g_chordFired = true; out |= Toggle; }
        g_chordHeld = g_chordFired;   // after it fired, keep the game from seeing the triggers until released
    } else { g_chordStart = 0; g_chordFired = false; g_chordHeld = false; }
    // --- keyboard (every key is queried each frame so its edge state stays right). Esc is not used: the
    // game would see it too (keyboard input is not blocked) and most games open their own menu on it.
    auto key = [&](int vk) { bool d = (GetAsyncKeyState(vk) & 0x8000) != 0; bool p = d && !g_prevKeys[vk]; g_prevKeys[vk] = d; return p; };
    auto held = [&](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool kTog = g_set.keyboardToggleVk ? key(g_set.keyboardToggleVk) : false;
    bool kRet = key(VK_RETURN), kBack = key(VK_BACK), kPgUp = key(VK_PRIOR), kPgDn = key(VK_NEXT), kDel = key(VK_DELETE);
    if (kTog) out |= Toggle;
    if ((pressed & XINPUT_GAMEPAD_A) || kRet) out |= Accept;
    if ((pressed & XINPUT_GAMEPAD_B) || kBack) out |= Back;
    if ((pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) || kPgUp) out |= PagePrev;
    if ((pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) || kPgDn) out |= PageNext;
    if ((pressed & XINPUT_GAMEPAD_Y) || kDel) out |= Reset;
    // directions with auto-repeat (first repeat after 400 ms, then every 90 ms)
    uint32_t dirs = 0;
    if (b & XINPUT_GAMEPAD_DPAD_UP || held(VK_UP)) dirs |= Up;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN || held(VK_DOWN)) dirs |= Down;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT || held(VK_LEFT)) dirs |= Left;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT || held(VK_RIGHT)) dirs |= Right;
    const uint32_t bits[4] = { Up, Down, Left, Right };
    for (int k = 0; k < 4; ++k) {
        if (!(dirs & bits[k])) { g_repeatAt[k] = 0; continue; }
        if (!g_repeatAt[k]) { out |= bits[k]; g_repeatAt[k] = now + 400; }
        else if ((int)(now - g_repeatAt[k]) >= 0) { out |= bits[k]; g_repeatAt[k] = now + 90; }
    }
    return out;
}

} } // namespace vrkit::input
