#pragma once
#include <cstdint>
struct ID3D11DeviceContext; struct ID3D11Resource; struct ID3D11Texture2D; struct ID3D11DepthStencilView;

namespace so4vr {

// Native stereo groundwork: records the render jobs (command type 0 of the render thread's queue)
// of each frame so they can be executed a second time with a different view before Present.

bool NativeInstall();                 // patches the job call site; call once at startup
void NativeEndOfFrame(uintptr_t vi);  // render thread, end of frame (before AdvanceRenderView); vi = ViewInfo read slot
void NativeReport(char* buf, size_t size);
unsigned long long NativeGameplayFrames();
// Diagnostic: offer uploaded constant data (render thread) for the camera-matrix search.
// While a frame is being replayed for the other eye: replaces the frame's camera matrices in uploaded constants.
// Diagnostic: after AdvanceRenderView, watch which code reads the new ViewInfo read slot during one frame.
void NativeAfterAdvance(uintptr_t vi);
bool NativePatchActive();
bool NativeInReplay();
// Native stereo: tangents (left, right, up, down) of eye e's render frustum this frame; false when not in stereo.
bool NativeEyeFov(int e, float tanLRUD[4]);
// The texture pair the composite history fix (StereoHistoryFix) already handles.
bool NativeIsHistoryFixTexture(ID3D11Resource* r);
// True when frames are executed a second time (DebugReplay or RenderMode = Native).
bool NativeReplayEnabled();
// Mono with a VR render resolution: this frame's final image (render thread, consumed by XrOnPresent), or null.
ID3D11Texture2D* NativeTakeMonoFrame();
void NativePatchUpload(void* data, unsigned bytes);
// Stereo: the game's final composite reads the previous frame's post-processed scene (ping-pong
// targets), so each eye needs its own copy. These hooks find the two targets each pass.
extern bool g_latTrack;          // fast-path gate for the hooks below
extern bool g_latWantSrv;
void NativeOnSetRT(ID3D11Resource* rt0);
// Camera patching applies only to passes rendering at (or near) the eye resolution; offscreen renders
// (minimap, portraits, probes) keep their own cameras.
void NativeOnPassTarget(ID3D11Resource* rt0, ID3D11DepthStencilView* dsv);
void NativeOnDraw(ID3D11DeviceContext* ctx);
void NativeScanUpload(const void* data, unsigned bytes, unsigned bufferBytes, const char* how);  // frames drawn with the gameplay camera so far

} // namespace so4vr
