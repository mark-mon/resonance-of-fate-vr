#pragma once
#include "MinHook.h"
#include "log.h"

namespace so4vr {

inline void* VtableEntry(void* comObj, int index) { return (*reinterpret_cast<void***>(comObj))[index]; }

enum class HookResult { Ok, AlreadyHooked, Failed };

// Creates and enables a MinHook detour. AlreadyHooked means another detour already owns `target`
// (e.g. two COM vtables sharing one implementation); `original` is then left untouched.
inline HookResult HookEx(void* target, void* detour, void** original, const char* name) {
    if (!target) { LOGE("hook %s: null target", name); return HookResult::Failed; }
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s == MH_ERROR_ALREADY_CREATED) return HookResult::AlreadyHooked;
    if (s != MH_OK) { LOGE("hook %s: create failed (%s)", name, MH_StatusToString(s)); return HookResult::Failed; }
    s = MH_EnableHook(target);
    if (s != MH_OK) { LOGE("hook %s: enable failed (%s)", name, MH_StatusToString(s)); return HookResult::Failed; }
    LOGI("hooked %s at %p", name, target);
    return HookResult::Ok;
}

inline bool Hook(void* target, void* detour, void** original, const char* name) {
    return HookEx(target, detour, original, name) != HookResult::Failed;
}

} // namespace so4vr
