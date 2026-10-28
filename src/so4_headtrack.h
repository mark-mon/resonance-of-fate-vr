#pragma once
#include <cstdint>
#include "head_tracking.h"   // HeadFrame

namespace so4vr {
// SO4 head tracking (RenderMode = Mono): the headset pose and FOV drive the game's main camera.
// Game thread, from the UpdateCamera hook (main camera only):
bool So4HtPre(uintptr_t cam);    // true = head pose/FOV prepared; call So4HtPost after the original
void So4HtPost(uintptr_t cam);
void So4HtIdle(uintptr_t cam);   // main camera updated without head tracking (restores the game's own view once)
// View-inverse hook: if `src` is the main camera's world matrix during a tracked update, replaces it
// with the head-posed world matrix (written to `tmp`) and returns tmp; otherwise returns src.
const float* So4HtWorldForView(const float* src, float* tmp);
// Render thread: a 2192-byte per-view constant upload (view rows at +0xC0).
void So4HtOnViewUpload(const void* data, unsigned bytes, unsigned offset = 0);   // offset: where data starts in the buffer
// Render thread, at Present: the head record of the frame being presented.
bool So4HtTakeFrame(HeadFrame& out);
// Render thread: the record matched so far this frame, without consuming it.
bool So4HtPeekFrame(HeadFrame& out);
// The oldest head record newer than `seq` (the frame after a rendered one, as a guess before its view upload).
bool So4HtRecordAfter(uint64_t seq, HeadFrame& out);
bool So4HtRecordBySeq(uint64_t seq, HeadFrame& out);
void So4HtRequestRecenter();
bool So4HtRecenterRef(float& yaw, XrVector3f& pos);
void So4HtReport(char* buf, unsigned size);
// Game thread: every perspective camera's view after UpdateCamera (to find which camera is actually rendered).
void So4HtNoteCameraView(uintptr_t cam);
// The camera whose view was last seen in a per-view upload (0 = unknown) and when (GetTickCount64).
uintptr_t So4HtRenderedCamera(uint64_t* tick = nullptr);
// Diagnostics: copies up to `max` (camera, view[12]) entries; returns the count.
// Render thread: the head record matched by the per-view upload just processed (consumed).
bool So4HtTakeUploadMatch(HeadFrame& out);
bool So4HtNearestRecord(const float* v, uint64_t& seq, float& diff, uint64_t& newest, uint64_t& oldest);
int So4HtCopyCamViews(uint64_t* cams, float* views, int max);
}
