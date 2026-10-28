#pragma once
#include <cstddef>
struct ID3D11DeviceContext; struct ID3D11Resource; struct ID3D11View;

namespace so4vr {
// Per-eye copies of every history texture (read before written in a frame) for Native stereo.
bool TemporalActive();
// Tag a texture the mod creates, so it is never mistaken for game history.
void TemporalMarkModTexture(ID3D11Resource* r);                       // collecting during pass 1 (fast gate for the hooks)
void TemporalOnRead(ID3D11Resource* r);
void TemporalOnWrite(ID3D11Resource* r);
void TemporalOnReadView(ID3D11View* v);
void TemporalOnWriteView(ID3D11View* v);
void TemporalOnUavView(ID3D11View* v);          // UAV bind: may read history in place
void TemporalBeginFrame(bool stereo);        // render thread, start of a frame (after StereoBegin)
void TemporalEndPass1();                     // end of pass 1 (left eye)
void TemporalBeforeReplay(ID3D11DeviceContext* ctx);
void TemporalAfterReplay(ID3D11DeviceContext* ctx, bool ok);
// SO4 (pass 1 = centre camera, then one replay per eye): eye e gets its own previous-frame history.
void TemporalBeforeEye(ID3D11DeviceContext* ctx, int eye);
void TemporalAfterEye(ID3D11DeviceContext* ctx, int eye, bool ok);
void TemporalReport(char* buf, size_t size);
}
