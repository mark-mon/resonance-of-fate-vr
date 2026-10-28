#pragma once
#include <d3d11.h>

namespace so4vr {

// TAA control (the engine's own TAA switch and camera jitter amount) and sharpening of the headset image.
bool ImageQualityInstall();            // DLL load: hooks the TAA jitter source
void ImageQualityPrePrepare();         // Prepare thread, before Renderer::Prepare: applies TAA on/off
// Returns a sharpened copy of src (owned by this module, valid until the next call with the same slot),
// or src itself when sharpening is off or unavailable. slot: 0 left / mono, 1 right.
ID3D11Texture2D* SharpenForHeadset(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, int slot);

} // namespace so4vr
