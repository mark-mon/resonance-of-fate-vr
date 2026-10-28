#pragma once

namespace so4vr {

// Stage 3a: verifies the statically found camera layout at runtime.
// Installs the engine's own pre/post-Prepare callbacks (no code patching) and reads the
// main camera every frame. Read-only unless DebugLensShiftX/Y is set in config.txt.
bool CameraProbeInstall();

// Called from the Present hook (render thread); logs a camera summary periodically.
void CameraProbeOnPresent();

} // namespace so4vr
