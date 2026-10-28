#pragma once
struct ID3D11DeviceContext; struct ID3D11Resource; struct ID3D11DepthStencilView;

namespace so4vr {
// Foveated rendering (NVIDIA VRS). Immediate context, on every render-target bind: enables reduced shading
// in the periphery for the game's scene passes, off for everything else.
void VrsOnSetRT(ID3D11DeviceContext* ctx, ID3D11Resource* rt0, ID3D11DepthStencilView* dsv);
// Makes sure VRS is off before our own passes and at the end of the frame.
void VrsEndOfFrame(ID3D11DeviceContext* ctx);
}
