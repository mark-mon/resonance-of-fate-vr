#pragma once
#include <d3d11.h>

namespace so4vr {
bool So4HudPrepare(ID3D11Device* dev, ID3D11Texture2D* finalTex, DXGI_FORMAT viewFmt);   // viewFmt: the game RTV format        // start of a HUD capture
void So4HudStore(ID3D11DeviceContext* ctx, ID3D11Texture2D* finalTex, int bg);   // 0 = UI over black, 1 = over white
bool So4HudBuildMatte(ID3D11DeviceContext* ctx);
ID3D11Texture2D* So4HudTexture(bool builtThisFrame);                      // premultiplied HUD for the XR quad, or null
void So4HudComposite(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv);   // draw the HUD over `rtv` (monitor)
ID3D11Texture2D* So4HudDebugTex(int bg);
void So4HudReport(char* buf, unsigned size);
}
