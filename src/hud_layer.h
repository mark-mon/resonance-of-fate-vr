#pragma once
#include <cstddef>
#include <d3d11.h>

namespace so4vr {

extern bool g_hudFrame;    // this frame redirects its HUD (fast gate for the OM hooks)
extern bool g_hudActive;   // a HUD state is live (fast gate for the draw hooks)

void HudFrameStart(bool gameplayCamera);   // render thread, start of each frame
// OMSetRenderTargets*: may replace the RTV array (points `r` at `scratch`, 8 entries).
void HudOnSetRT(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const*& r, ID3D11RenderTargetView** scratch);
ID3D11BlendState* HudOnSetBlend(ID3D11DeviceContext* ctx, ID3D11BlendState* bs, const FLOAT* factor, UINT mask);
bool HudBeforeDraw();                       // true = skip this draw
void HudAfterDraw(ID3D11DeviceContext* ctx);
void HudEndPass(ID3D11DeviceContext* ctx);  // end of pass 1 / end of the replay
ID3D11Texture2D* HudTakeFrame();            // this frame's HUD texture (once), or null
bool HudFrameHasHud();
ID3D11Texture2D* HudTexture();              // diagnostic: this frame's HUD texture (not consumed)
void HudCompositeToDesktop(ID3D11DeviceContext* ctx);   // draw the HUD back over the final image (monitor)
void HudReport(char* buf, size_t size);

} // namespace so4vr
