#pragma once
#include <windows.h>
#include <dxgi.h>
#include <d3d11.h>
#include <cstddef>
#include <openxr/openxr.h>

namespace so4vr {

// Stage 2: shows the game's final frame on a virtual screen (an OpenXR quad layer).
// Everything runs from the game's Present call on its render thread.

// Called from the Present hook, before the frame goes to the desktop window.
void XrOnPresent(IDXGISwapChain* sc);
// Called after ResizeBuffers so the XR swapchain is rebuilt at the new size.
void XrOnResize();

// Render thread (native stereo): hands over this frame's two eye images (same size and format) with the
// head record they were rendered for. The Present that follows then skips its own (mono) copy.
struct HeadFrame;
void XrPublishStereo(ID3D11Texture2D* left, ID3D11Texture2D* right, const HeadFrame& hf, ID3D11Texture2D* hud = nullptr);
// F10: show/hide the HUD panel.
void XrToggleHud();
// Render thread: the settings menu image while it is open (null = closed).
void XrPublishMenu(ID3D11Texture2D* tex);

// Main thread: both eyes' poses (XR LOCAL space) and FOVs predicted for when a frame being
// prepared now will be displayed. False if no XR session is running or tracking is lost.
bool XrLocateEyesForGameFrame(XrView views[2], int64_t* baseTime = nullptr, int64_t* poseTime = nullptr);

// Render thread: one-line summary of what was submitted since the last call.
void XrReport(char* buf, size_t size);

// Latest eye-gaze direction relative to the head (tangents, x right, y up); false if none in the last 250 ms.
bool XrGazeTangents(float& tx, float& ty);

} // namespace so4vr
