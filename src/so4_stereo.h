#pragma once
#include <cstdint>
struct ID3D11DeviceContext;
struct ID3D11DeviceContext;
namespace so4vr {
// SO4 native stereo (RenderMode = Native).
bool So4StereoInstall();                 // job call-site patch + command processor / frame-end hooks
void So4StereoReport(char* buf, unsigned size);
// Constant-buffer upload patching while an eye pass runs (render thread).
bool So4StereoPatching();
ID3D11DeviceContext* So4StereoGameContext();   // the game device's immediate context (null until known)
void So4StereoGateWait();   // game thread: wait while the eye replays run (ReplayCameraGate)
void So4StereoPatch(void* data, unsigned bytes);
void So4StereoScanUpload(const void* data, unsigned bytes);   // diagnostic (StereoDiag >= 5)
void So4StereoBeforeUpload(const void* data, unsigned bytes, unsigned bufferBytes, unsigned offset = 0);
bool So4StereoInReplay();
int  So4StereoCurrentEye();                       // eye being replayed (0/1), -1 outside the eye replays
bool So4StereoEyeTan(int e, float tanLRUD[4]);    // eye e's field of view (tangents) of the current stereo frame
void So4StereoDumpDiag(unsigned long long seq, int eye, float score);
}
