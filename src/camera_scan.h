#pragma once
#include <string>
namespace so4vr {
// Diagnostic: finds live camera objects by their RTTI vtables and writes them to so4_camscan<N>.bin.
// Triggered by creating so4_scan_now.txt in the game folder (polled twice a second).
void CameraScanStart(const std::wstring& gameDir);
}
