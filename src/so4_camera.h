#pragma once
#include <cstdint>
namespace so4vr {
// SO4 camera discovery: hooks the engine's UpdateCamera (Aska camera, RVA 0x3CC20) and tracks which
// camera objects it updates. Enabled with CameraHook = 1 in config.txt (experimental).
bool So4CameraInstall();
void So4CameraReport(char* buf, unsigned size);   // one-line summary for the stats log
uintptr_t So4MainCamera();                        // best guess for the main 3D camera, or 0
// Render thread: every constant-buffer upload (how: 0 UpdateSubresource, 1 Map). Used by the upload capture.
void So4UploadTap(void* buffer, const void* data, unsigned bytes, unsigned bufferBytes, int how);
// Render thread, each Present: arms/finishes the one-frame upload capture (request file so4_upl_now.txt).
void So4CameraOnPresent();
}
