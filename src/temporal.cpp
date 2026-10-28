// Per-eye temporal history for Native stereo (generalises StereoHistoryFix to every history texture).
//
// Many effects keep data from the previous frame (TAA, the game's TemporalReprojection / TemporalFilter
// passes, ambient occlusion and reflection denoisers, ...). With replay stereo both eyes render into the same
// textures, so without care the right eye blends the LEFT eye's history from the previous frame (and vice
// versa). Near objects have the most parallax between the eyes, so they get ghosted, speckled outlines.
//
// Detection (pass 1 only): a texture that is read through a shader resource view before anything writes it
// in this frame, and that the game does write (this frame or recently), holds history. Once seen it stays
// in the set until it is not used for a while. Per frame, for every texture in the set:
//   after pass 1 (left eye):  save it as the left copy, load the right copy (from the previous frame)
//   after the replay (right):  save it as the right copy, load the left copy back
// so each eye always sees its own previous frame. This works for in-place and ping-pong (A/B) histories.
#include "temporal.h"
#include "config.h"
#include "log.h"
#include "render_res.h"
#include "native_stereo.h"
#include <windows.h>
#include <d3d11.h>
#include <unordered_map>
#include <vector>
#include <cstdio>

namespace so4vr {
namespace {

struct Info {
    uint64_t lastWrite = 0;      // frame number of the last write (0 = never)
    uint64_t lastUse = 0;        // frame number of the last read or write
    uint64_t writtenFrame = 0;   // == g_frame when written in the current frame
    uint64_t candStart = 0;      // stability window: first frame it looked like history
    uint32_t candCount = 0;      // frames in the window it looked like history
    bool readBeforeWrite = false;
};
struct Hist {
    ID3D11Texture2D* game = nullptr;   // referenced
    ID3D11Texture2D* copy[2] = {};     // left / right copies
    bool valid[2] = {};
    uint64_t lastSeen = 0;
    UINT w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};

std::unordered_map<ID3D11Resource*, Info> g_info;   // raw pointers: identity only, pruned regularly
std::vector<Hist> g_hist;
std::vector<ID3D11Resource*> g_blocked;   // identity only: textures handled elsewhere (never added again)
uint64_t g_frame = 1;
bool g_tracking = false;          // pass 1 of a stereo frame
uint32_t g_renderW = 0, g_renderH = 0;
uint64_t s_swaps = 0, s_added = 0, s_dropped = 0;
bool g_pass1Done = false, g_replayPending = false;

// Textures the mod itself creates (HUD, menu, sharpening, shared frames, ...) are tagged and never treated as
// game history: swapping in older copies of them showed stale, flickering UI.
// {5A3C9E21-7B4D-4F1A-9C2E-6D8B0F1A2C3E}
const GUID kModTexture = {0x5a3c9e21, 0x7b4d, 0x4f1a, {0x9c, 0x2e, 0x6d, 0x8b, 0x0f, 0x1a, 0x2c, 0x3e}};
DWORD g_trackThread = 0;          // only the render thread's pass 1 is tracked

bool IsModTexture(ID3D11Resource* r) {
    UINT size = 0;
    return SUCCEEDED(r->GetPrivateData(kModTexture, &size, nullptr)) && size > 0;
}

bool Eligible(ID3D11Resource* r, D3D11_TEXTURE2D_DESC* out = nullptr) {
    if (!r) return false;
    if (IsModTexture(r)) return false;
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    D3D11_TEXTURE2D_DESC d; static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
    if (d.ArraySize != 1 || d.SampleDesc.Count != 1 || (d.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)) return false;
    if (d.Usage != D3D11_USAGE_DEFAULT || !(d.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_DEPTH_STENCIL))) return false;
    // Screen-sized (full down to quarter resolution); excludes 1x1 exposure (shared on purpose), LUTs, probes.
    if (!g_renderW || d.Width * 5 < g_renderW || d.Width > g_renderW + g_renderW / 8) return false;
    if (out) *out = d;
    return true;
}

bool IsHist(ID3D11Resource* r) {
    for (auto& h : g_hist) if (h.game == r) return true;
    return false;
}

void AddHist(ID3D11Resource* r, const D3D11_TEXTURE2D_DESC& d) {
    if (IsHist(r) || NativeIsHistoryFixTexture(r)) return;
    for (auto* b : g_blocked) if (b == r) return;
    if (g_hist.size() >= (size_t)g_config.stereoHistoryMax) return;
    Hist h; h.game = static_cast<ID3D11Texture2D*>(r); h.game->AddRef();
    h.w = d.Width; h.h = d.Height; h.fmt = d.Format; h.lastSeen = g_frame;
    g_hist.push_back(h);
    s_added++;
    LOGI("stereo history: per-eye copies for %ux%u format %d (bind 0x%x, mips %u) - %zu textures",
         d.Width, d.Height, int(d.Format), d.BindFlags, d.MipLevels, g_hist.size());
}

void Release(Hist& h) {
    for (auto& c : h.copy) if (c) { c->Release(); c = nullptr; }
    if (h.game) { h.game->Release(); h.game = nullptr; }
}

bool EnsureCopy(ID3D11DeviceContext* ctx, Hist& h, int e) {
    if (h.copy[e]) return true;
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_TEXTURE2D_DESC d; h.game->GetDesc(&d);
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = 0;
    d.MiscFlags &= D3D11_RESOURCE_MISC_RESOURCE_CLAMP;   // plain copy target
    if (d.Format == DXGI_FORMAT_R24G8_TYPELESS || d.Format == DXGI_FORMAT_R32G8X24_TYPELESS || d.Format == DXGI_FORMAT_R32_TYPELESS ||
        d.Format == DXGI_FORMAT_R16_TYPELESS || d.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || d.Format == DXGI_FORMAT_D32_FLOAT ||
        d.Format == DXGI_FORMAT_D16_UNORM || d.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
        d.BindFlags = D3D11_BIND_DEPTH_STENCIL;          // depth formats: keep a bind flag that is legal for them
    HRESULT hr = dev->CreateTexture2D(&d, nullptr, &h.copy[e]);
    if (SUCCEEDED(hr)) TemporalMarkModTexture(h.copy[e]);
    dev->Release();
    if (FAILED(hr)) { LOGW("stereo history: copy texture creation failed (0x%08lx)", hr); return false; }
    return true;
}

// Writes or reads by the game, from the context hooks.
inline void NoteWrite(ID3D11Resource* r) {
    if (!r) return;
    Info& i = g_info[r];
    i.lastWrite = g_frame; i.lastUse = g_frame; i.writtenFrame = g_frame;
}

} // namespace

bool TemporalActive() { return g_tracking && GetCurrentThreadId() == g_trackThread; }

void TemporalMarkModTexture(ID3D11Resource* r) {
    if (!r) return;
    uint8_t one = 1;
    r->SetPrivateData(kModTexture, 1, &one);
}

// Promote only textures that look like history in a good share of frames (every frame, or every other frame
// for ping-pong pairs). UI layers that are occasionally read before being redrawn do not qualify.
static void Candidate(ID3D11Resource* r, Info& i) {
    D3D11_TEXTURE2D_DESC d;
    if (!Eligible(r, &d)) return;
    i.readBeforeWrite = true;
    if (!i.candStart || g_frame - i.candStart > 64) { i.candStart = g_frame; i.candCount = 0; }
    if (++i.candCount >= 24) AddHist(r, d);
}

void TemporalOnRead(ID3D11Resource* r) {
    if (!TemporalActive() || !r) return;
    auto it = g_info.find(r);
    if (it == g_info.end()) {
        D3D11_TEXTURE2D_DESC d;
        if (!Eligible(r, &d)) { g_info[r]; return; }   // remember as "not interesting" cheaply (lastWrite 0)
        it = g_info.emplace(r, Info{}).first;
    }
    Info& i = it->second;
    i.lastUse = g_frame;
    if (i.writtenFrame != g_frame && i.lastWrite && g_frame - i.lastWrite <= 60 && !i.readBeforeWrite) {
        Candidate(r, i);
    }
    for (auto& h : g_hist) if (h.game == r) { h.lastSeen = g_frame; break; }
}

void TemporalOnWrite(ID3D11Resource* r) {
    if (!TemporalActive() || !r) return;
    NoteWrite(r);
    for (auto& h : g_hist) if (h.game == r) { h.lastSeen = g_frame; break; }
}

// A UAV can be read and written in place (compute denoisers, TAA): if the first access this frame is a UAV
// bind and the game wrote the texture recently, treat it as history too.
void TemporalOnUavView(ID3D11View* v) {
    if (!TemporalActive() || !v) return;
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    if (!r) return;
    r->Release();
    if (g_config.stereoHistoryUav) {
        auto it = g_info.find(r);
        if (it != g_info.end()) {
            Info& i = it->second;
            if (i.writtenFrame != g_frame && i.lastWrite && g_frame - i.lastWrite <= 60 && !i.readBeforeWrite) {
                Candidate(r, i);
            }
        }
    }
    TemporalOnWrite(r);
}

void TemporalOnWriteView(ID3D11View* v) {
    if (!TemporalActive() || !v) return;
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    if (r) { r->Release(); TemporalOnWrite(r); }
}

void TemporalOnReadView(ID3D11View* v) {
    if (!TemporalActive() || !v) return;
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    if (r) { r->Release(); TemporalOnRead(r); }
}

// Render thread, start of pass 1 of a stereo frame.
void TemporalBeginFrame(bool stereo) {
    uint32_t w = 0, h = 0; RenderResCurrent(w, h);
    if (w != g_renderW || h != g_renderH) {   // targets were reallocated: start over
        for (auto& x : g_hist) Release(x);
        if (!g_hist.empty()) LOGI("stereo history: render size changed, %zu textures forgotten", g_hist.size());
        g_hist.clear(); g_info.clear(); g_blocked.clear();
        g_renderW = w; g_renderH = h;
    }
    ++g_frame;
    for (auto& kv : g_info) kv.second.readBeforeWrite = false;
    g_tracking = stereo && g_config.stereoHistoryAll && g_config.stereoHistoryFix;
    g_trackThread = GetCurrentThreadId();
    if ((g_frame & 255) == 0) {   // prune
        for (auto it = g_info.begin(); it != g_info.end();) { if (g_frame - it->second.lastUse > 600) it = g_info.erase(it); else ++it; }
        for (size_t k = 0; k < g_hist.size();) {
            if (g_frame - g_hist[k].lastSeen > 300) { Release(g_hist[k]); g_hist.erase(g_hist.begin() + k); s_dropped++; }
            else ++k;
        }
    }
}

// Diagnostic (DebugHistLog = N): list every screen-sized texture pass 1 touched, once, after N tracked frames.
static void DumpTouched() {
    static uint64_t tracked = 0;
    if (!g_config.debugHistLog || ++tracked != (uint64_t)g_config.debugHistLog) return;
    LOGI("stereo history diagnostic: screen-sized textures used in pass 1 of frame %llu (render %ux%u):", (unsigned long long)g_frame, g_renderW, g_renderH);
    int n = 0;
    for (auto& kv : g_info) {
        const Info& i = kv.second;
        if (i.lastUse != g_frame) continue;
        D3D11_TEXTURE2D_DESC d;
        if (!Eligible(kv.first, &d)) continue;
        LOGI("  %p %ux%u fmt %d mips %u bind 0x%x: written this frame %d, read before write %d, last write %lld frames ago%s%s",
             (void*)kv.first, d.Width, d.Height, int(d.Format), d.MipLevels, d.BindFlags, i.writtenFrame == g_frame ? 1 : 0,
             i.readBeforeWrite ? 1 : 0, i.lastWrite ? (long long)(g_frame - i.lastWrite) : -1LL,
             IsHist(kv.first) ? " [per-eye]" : "", NativeIsHistoryFixTexture(kv.first) ? " [composite pair/final]" : "");
        if (++n > 80) break;
    }
}

// End of pass 1 (left eye): stop collecting.
void TemporalEndPass1() {
    if (g_tracking) DumpTouched();
    g_pass1Done = g_tracking;
    g_tracking = false;
}

// Just before the replay: keep the left result, give the right eye its own history.
void TemporalBeforeReplay(ID3D11DeviceContext* ctx) {
    if (!g_pass1Done || !ctx) return;
    g_pass1Done = false;
    for (size_t k = 0; k < g_hist.size();) {   // drop textures identified as the composite pair / final image later
        if (NativeIsHistoryFixTexture(g_hist[k].game)) {
            LOGI("stereo history: %ux%u format %d is handled elsewhere (final image / composite history), removed", g_hist[k].w, g_hist[k].h, int(g_hist[k].fmt));
            g_blocked.push_back(g_hist[k].game);
            Release(g_hist[k]); g_hist.erase(g_hist.begin() + k);
        } else ++k;
    }
    for (auto& h : g_hist) {
        if (!EnsureCopy(ctx, h, 0) || !EnsureCopy(ctx, h, 1)) continue;
        ctx->CopyResource(h.copy[0], h.game); h.valid[0] = true;
        if (h.valid[1]) ctx->CopyResource(h.game, h.copy[1]);
        s_swaps++;
    }
    g_replayPending = true;
}

// After the replay (right eye): keep the right result and put the left history back for the next frame.
// If the replay failed, the right copies would be stale next time, so they are dropped.
void TemporalAfterReplay(ID3D11DeviceContext* ctx, bool ok) {
    if (!g_replayPending || !ctx) return;
    g_replayPending = false;
    for (auto& h : g_hist) {
        if (!h.copy[0] || !h.copy[1] || !h.valid[0]) continue;
        if (ok) { ctx->CopyResource(h.copy[1], h.game); h.valid[1] = true; }
        else h.valid[1] = false;
        ctx->CopyResource(h.game, h.copy[0]);
    }
}

// SO4 three-pass frames: pass 1 renders the centre camera (not shown), then eye 0 and eye 1 are replays.
// Before each eye's replay its own copy from the previous frame is loaded; after it, the result is saved.
// The game textures are left holding eye 1's result (what pass 1 of the next frame reads - not shown anyway).
void TemporalBeforeEye(ID3D11DeviceContext* ctx, int e) {
    if (!ctx || e < 0 || e > 1) return;
    if (e == 0) { if (!g_pass1Done) return; g_pass1Done = false; g_replayPending = true; }
    if (!g_replayPending) return;
    for (auto& h : g_hist) {
        if (!EnsureCopy(ctx, h, 0) || !EnsureCopy(ctx, h, 1)) continue;
        if (h.valid[e]) { ctx->CopyResource(h.game, h.copy[e]); s_swaps++; }
    }
}
void TemporalAfterEye(ID3D11DeviceContext* ctx, int e, bool ok) {
    if (!ctx || !g_replayPending || e < 0 || e > 1) return;
    for (auto& h : g_hist) {
        if (!h.copy[e]) continue;
        if (ok) { ctx->CopyResource(h.copy[e], h.game); h.valid[e] = true; }
        else h.valid[e] = false;
    }
    if (e == 1 || !ok) g_replayPending = false;
}

void TemporalReport(char* buf, size_t size) {
    unsigned long long bytes = 0;
    for (auto& h : g_hist) bytes += 2ull * h.w * h.h * 8;   // rough (assumes 8 bytes/texel)
    snprintf(buf, size, "stereo history: %zu textures (added %llu, dropped %llu), %llu swaps, copies ~%llu MB",
             g_hist.size(), (unsigned long long)s_added, (unsigned long long)s_dropped, (unsigned long long)s_swaps, bytes >> 20);
    s_swaps = 0;
}

} // namespace so4vr
