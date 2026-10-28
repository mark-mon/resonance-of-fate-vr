// Resonance of Fate: gamepad object diagnostics / test support.
// The game creates one pad object at startup (ctor 0x7DF7E0, vtable 0xA2FFC0, 0x188 bytes):
//   vt+0x50 = 0x7DFDE0 "find active pad": XInputGetCapabilities/XInputGetState on users 0..3, picks the one
//             pressing a start button (+0x98 mask) -> +0x90 active user
//   vt+0x58 = 0x7DFBF0 poll: if +0x168 (no device) is 0, XInputGetState(+0x89) and decode buttons into +0x9C...
// With no physical pad +0x168 stays set and the game never calls XInput, so the test pad (DebugFakePad) is
// invisible. With DebugFakePad = 2 the poll hook clears the flag so the game reads pad 0 through our proxy.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include "MinHook.h"
#include "hook_util.h"
#include "log.h"
#include "config.h"
#include "vrkit_input.h"

namespace so4vr {
namespace {
constexpr uintptr_t kFnPadPoll = 0x7DFBF0, kFnPadFind = 0x7DFDE0;
using Fn = char(__fastcall*)(void* pad);
Fn o_Poll = nullptr, o_Find = nullptr;
std::atomic<uint64_t> s_poll{0}, s_find{0}, s_forced{0}, s_blocked{0};
char __fastcall hk_Poll(void* pad) {
    auto p = static_cast<uint8_t*>(pad);
    uint64_t n = ++s_poll;
    if (n == 1 || n % 3600 == 0)
        LOGI("pad poll %llu: pad %p no-device %u user %u connected %u active %u", (unsigned long long)n, pad, p[0x168], p[0x89], p[0x8A], p[0x90]);
    if (g_config.debugFakePad && g_config.debugFakePadAlways && p[0x168]) { p[0x168] = 0; p[0x89] = 0; p[0x8A] = 1; s_forced++; }
    char r = o_Poll(pad);
    // The settings menu reads what the game's pad code got (works with Steam Input's virtual pad too), and
    // while the menu is open the game sees a neutral pad here as well.
    if (!p[0x168]) {
        vrkit::input::SetExternalState(p + 0x16C);   // XINPUT_STATE (packet, buttons, triggers, sticks)
        if (vrkit::input::GameInputBlocked()) { memset(p + 0x9C, 0, 0xA8 - 0x9C); memset(p + 0x170, 0, 0x17C - 0x170); s_blocked++; }
    }
    return r;
}
char __fastcall hk_Find(void* pad) {
    uint64_t n = ++s_find;
    if (n == 1 || n % 3600 == 0) LOGI("pad find-active %llu (active user %u)", (unsigned long long)n, static_cast<uint8_t*>(pad)[0x90]);
    return o_Find(pad);
}
} // namespace

bool RofInputInstall() {
    uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    bool ok = Hook(reinterpret_cast<void*>(b + kFnPadPoll), (void*)hk_Poll, (void**)&o_Poll, "ROF pad poll");
    ok = Hook(reinterpret_cast<void*>(b + kFnPadFind), (void*)hk_Find, (void**)&o_Find, "ROF pad find-active") && ok;
    return ok;
}
void RofInputReport(char* buf, unsigned size) {
    snprintf(buf, size, "pad: polls %llu, find-active %llu, forced %llu, blocked for the menu %llu", (unsigned long long)s_poll.load(), (unsigned long long)s_find.load(), (unsigned long long)s_forced.load(), (unsigned long long)s_blocked.load());
}
} // namespace so4vr
