#pragma once
#include <cstdint>
#include <openxr/openxr.h>

namespace so4vr {

// Stage 3b: single-view head tracking.
//
// Main thread (engine pre/post-Prepare callbacks): before the game prepares a frame, the game
// camera is replaced by game camera x head pose and its lens is set to the headset's field of
// view; afterwards everything is restored so game logic never sees the change.
// Render thread (Present): the pose/FOV used for that frame is looked up so the OpenXR
// projection layer reports exactly what was rendered.

struct HeadFrame {
    float   view[16];   // camera view matrix after Prepare (used to match frames)
    float   viewVI[16]; // view written into this frame's ViewInfo snapshot (preferred match key)
    bool    haveVI;
    uint64_t seq;       // preparation sequence number
    XrPosef pose;       // head pose in the XR LOCAL space used for this frame
    XrFovf  fov;        // field of view the frame was rendered with (union of both eyes)
    // Native stereo: per-eye data for the same prediction.
    XrPosef eyePose[2]; // raw eye poses in the XR LOCAL space (for the projection layer)
    XrFovf  eyeFov[2];
    float   eyeOff[2][3]; // eye position relative to the rendered head camera, game view space (cm)
    float   eyeRot[4 * 2]; // eye orientation relative to the head camera, game convention quaternion (x,y,z,w) per eye
    float   proj[16];      // SO4: the camera projection (+0x420) this frame was rendered with
    float   viewNoHead[16]; // SO4: the game camera's own view (inverse world, no head) for this update
    int64_t baseTime = 0;  // XrTime: the latest display time when the pose was located (prediction base)
    int64_t poseTime = 0;  // XrTime the pose was predicted for
};

// Installs the view-matrix hook (call once at startup, after MinHook is initialised).
bool HeadTrackInstall();
// Called from the pre-Prepare callback with the main camera. Returns true if it took control.
bool HeadTrackPrePrepare(uintptr_t cam);
// Recentre reference (F9) in XR LOCAL space: heading and head position. False until the first recentre.
bool HeadTrackRecenterRef(float& yaw, XrVector3f& pos);
// Called from the post-Prepare callback; restores the camera and records the frame.
void HeadTrackPostPrepare(uintptr_t cam);
// Render thread: finds the record for the frame being presented. `renderedView` is the view
// matrix from the ViewInfo snapshot the renderer used (may be null).
bool HeadTrackTakeFrame(const float* renderedView, HeadFrame& out);
// Render thread: the view of the ViewInfo slot captured when the renderer last advanced to a new
// frame (exact frame identification). False if none since the last call.
bool HeadTrackTakeRenderView(float out[16]);
// Render thread: looks up (without consuming) the record whose view matches `renderedView`.
bool HeadTrackFindFrame(const float* renderedView, HeadFrame& out);
// Recentre the view on the next frame (yaw and position).
void HeadTrackRequestRecenter();
// Render thread, at Present: timing diagnostics (advance -> present interval).
void HeadTrackNotePresent();
// Diagnostics line for the log.
void HeadTrackReport(char* buf, size_t size);

} // namespace so4vr
