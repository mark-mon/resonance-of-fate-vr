#pragma once
#include <windows.h>
#include <d3d12.h>
#include <cstdint>

namespace so4vr {
// Enhanced Edition (D3D12) render helpers (ee_render.cpp).
struct EeRtState {   // render thread: what the engine's command list has bound (only when its methods are hooked)
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* rt[2] = {};
    SIZE_T rtv0 = 0;                       // CPU descriptor handle of render target 0
    DXGI_FORMAT fmt[2] = {};
    unsigned n = 0;
    uint64_t sets = 0, draws = 0;
};
void EeRenderOnDevice(ID3D12Device* d);                         // from D3D12CreateDevice: hooks CreateRenderTargetView
ID3D12Resource* EeRtvResource(SIZE_T handle, DXGI_FORMAT* fmt);  // RTV handle -> resource (null if unknown)
ID3D12GraphicsCommandList* EeEngineList();                      // the engine's currently open command list
bool EeHookListMethods(ID3D12GraphicsCommandList* l, DWORD renderTid);
EeRtState& EeRt();

// ---- native stereo (EE) ----
#ifndef EE_VT_COMBINER
#define EE_VT_COMBINER 0x17E8B30   // EE PostProcessCombiner (job fn 0x383F20, 1 draw into the 1280x720 R10G10B10A2 final target; test 073)
#endif
#ifndef EE_VT_MOTIONBLUR
#define EE_VT_MOTIONBLUR 0x17F79B8   // EE motion blur job (fn 0x10EB740: tests +0x715 & 9 like old 0x7F58C0 +0x70D)
#endif
struct HeadFrame;
void EeNoteRenderThread(DWORD tid);
const uint8_t* EeLastViewBlock();   // CPU per-view block of the last pass-1 upload (render thread)   // render thread id (constant uploads are patched only there)
bool EeCaptureEye(int e);             // copy the bound render target 0 (the combiner output) into eye e
void EeStereoReady(const HeadFrame& hf, bool hud);
bool EeHudClear(int bg);              // clear the bound final target to black (0) / white (1) in the engine's list
bool EeHudCapture(int bg);            // copy it (UI drawn over that background) for the HUD matte
bool EeRenderInstall();               // constant upload hook (0xCC060) + frame submit hook (0xD09E0 is hooked by so4_stereo)
void EeBeforeSubmit();                // from the frame end hook, before the game submits its lists
void EeRenderReport(char* buf, size_t size);
void EeVrsReport(char* buf, size_t size);
} // namespace so4vr
