#pragma once
#include <d3d11.h>
#include <cstddef>

namespace so4vr {
// Render thread, once per stereo frame before it is published: moves the game's aim crosshair from the HUD
// panel into both eye images (centreVI / eyeVI: V, InvV, P, InvP, VP, InvVP as 16-float row-major matrices).
void ReticleOnStereoFrame(ID3D11DeviceContext* ctx, ID3D11Texture2D* hud, ID3D11Texture2D* const eyes[2],
                          const float* centreVI, const float* const eyeVI[2]);
void ReticleReport(char* buf, size_t size);
}
