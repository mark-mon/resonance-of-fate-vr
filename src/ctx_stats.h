#pragma once
#include <d3d11.h>
#include <cstddef>

namespace so4vr {
void CtxMapReport(char* buf, size_t n);   // constant buffer Map types + raw restores since the last call

// Instruments the immediate context, deferred contexts and command-list traffic so we can see
// how the engine submits its frame (direct drawing vs. multithreaded command lists).
void CtxStatsInstall(ID3D11Device* dev);

// Also watch the immediate context of any additional device the game creates.
void CtxStatsWatchDevice(ID3D11Device* dev);

// Calls `cb` (render thread) at the first render-target bind after each CtxStatsArmFrameStart().
void CtxStatsSetFrameStartCallback(void (*cb)());
void CtxStatsArmFrameStart();

// Called once per Present: folds this frame's counters into the running interval totals.
void CtxStatsEndFrame();

// Writes the per-frame averages over `frames` frames into `buf` and resets the interval totals.
void CtxStatsReport(char* buf, size_t size, double frames);

// Native stereo: compute UAV resources (small ones) are snapshotted the first time they are bound in a
// frame; RestoreUavSnapshots() puts them back so a second-eye pass starts from the same GPU state.
void CtxRestoreUavSnapshots(ID3D11DeviceContext* ctx);
void CtxNewFrameUavSnapshots();
// Re-issues this frame's structured/raw buffer uploads (light lists etc.) before the second-eye pass.
void CtxReapplyBufferUploads(ID3D11DeviceContext* ctx);
ID3D11DeviceContext* CtxImmediate();

} // namespace so4vr
