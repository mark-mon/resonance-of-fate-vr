// Aim reticle in the 3D image.
//
// While D.U.M.A. is aimed (RB held) the game draws a crosshair at the centre of the screen. It is UI, so it
// ends up on the flat HUD panel, a metre or two in front of the player, while the target is far away in the
// 3D scene. Looking at the target, the crosshair is seen double (and in World anchor it is not even where the
// camera points). This finds the crosshair in the HUD image, takes it off the panel and draws it into each
// eye image at a fixed distance along the camera's aim direction, so it lines up with the world in stereo.
//
// Detection: a small square at the centre of the HUD image is read back (two frames late, without stalls).
// The crosshair is opaque at its centre and symmetric left/right and top/bottom; other HUD elements that pass
// through the centre (labels, prompts) are not.
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <atomic>
#include "config.h"
#include "log.h"
#include "temporal.h"
#include "reticle.h"

namespace so4vr {
namespace {

template <typename T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Views {
    ID3D11Texture2D* tex = nullptr;   // identity only
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    void Reset() { tex = nullptr; Rel(srv); Rel(rtv); }
};
Views g_hudV, g_eyeV[2];

constexpr int kRing = 3;
ID3D11Texture2D* g_stage[kRing] = {};
bool g_stageFull[kRing] = {};
int g_stageSize = 0;
DXGI_FORMAT g_stageFmt = DXGI_FORMAT_UNKNOWN;
unsigned g_frame = 0;

bool g_on = false;
int g_miss = 0;

ID3DDeviceContextState* g_dcs = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11SamplerState* g_samp = nullptr;
ID3D11RasterizerState* g_rs = nullptr;
bool g_failed = false;

std::atomic<unsigned long long> s_frames{0}, s_detected{0}, s_drawn{0}, s_reads{0};

const char* kShader = R"(
cbuffer C : register(b0) { float4 uvRect; };
Texture2D t : register(t0);
SamplerState s : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vsMain(uint id : SV_VertexID) {
    V o; float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
float4 psMain(V i) : SV_Target { return t.SampleLevel(s, uvRect.xy + i.uv * uvRect.zw, 0); }
)";

DXGI_FORMAT ViewFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}
bool Is8888(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return true;
    default: return false;
    }
}

bool EnsureViews(ID3D11Device* dev, Views& v, ID3D11Texture2D* t, bool needSrv, bool needRtv) {
    if (v.tex == t && (!needSrv || v.srv) && (!needRtv || v.rtv)) return true;
    v.Reset();
    D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
    DXGI_FORMAT f = ViewFormat(d.Format);
    if (needSrv) {
        if (!(d.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = f; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(t, &sd, &v.srv))) return false;
    }
    if (needRtv) {
        if (!(d.BindFlags & D3D11_BIND_RENDER_TARGET)) { v.Reset(); return false; }
        D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = f; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateRenderTargetView(t, &rd, &v.rtv))) { v.Reset(); return false; }
    }
    v.tex = t;
    return true;
}

bool EnsurePipeline(ID3D11Device* dev) {
    if (g_dcs) return true;
    if (g_failed) return false;
    g_failed = true;
    ID3D11Device1* dev1 = nullptr; dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1);
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = comp ? (pD3DCompile)GetProcAddress(comp, "D3DCompile") : nullptr;
    bool ok = dev1 && compile;
    ID3DBlob *vsb = nullptr, *psb = nullptr;
    ok = ok && SUCCEEDED(compile(kShader, strlen(kShader), "reticle", nullptr, nullptr, "vsMain", "vs_5_0", 0, 0, &vsb, nullptr));
    ok = ok && SUCCEEDED(compile(kShader, strlen(kShader), "reticle", nullptr, nullptr, "psMain", "ps_5_0", 0, 0, &psb, nullptr));
    ok = ok && SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs));
    ok = ok && SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps));
    Rel(vsb); Rel(psb);
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
    D3D11_BLEND_DESC bl{};   // premultiplied colour over the eye image; keep the eye image's alpha
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(dev->CreateBlendState(&bl, &g_blend));
    D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER; sd.MaxLOD = D3D11_FLOAT32_MAX;   // border = transparent
    ok = ok && SUCCEEDED(dev->CreateSamplerState(&sd, &g_samp));
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rd, &g_rs));
    if (ok) {
        D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel(), chosen{};
        ok = SUCCEEDED(dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), &chosen, &g_dcs));
    }
    if (dev1) dev1->Release();
    if (!ok) { LOGW("reticle: drawing unavailable"); return false; }
    g_failed = false;
    return true;
}

bool EnsureStaging(ID3D11Device* dev, int size, DXGI_FORMAT fmt) {
    if (g_stageSize == size && g_stageFmt == fmt && g_stage[0]) return true;
    for (auto*& s : g_stage) Rel(s);
    for (bool& f : g_stageFull) f = false;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = UINT(size); d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto*& s : g_stage) {
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &s))) { for (auto*& x : g_stage) Rel(x); return false; }
        TemporalMarkModTexture(s);
    }
    g_stageSize = size; g_stageFmt = fmt;
    return true;
}

// Crosshair test on the read-back centre square (8-bit RGBA/BGRA, alpha in byte 3).
bool Detect(const uint8_t* p, UINT pitch, int n) {
    auto op = [&](int x, int y) { return x >= 0 && y >= 0 && x < n && y < n && p[size_t(y) * pitch + size_t(x) * 4 + 3] > 96; };
    auto opNear = [&](int x, int y) { return op(x, y) || op(x - 1, y) || op(x + 1, y) || op(x, y - 1) || op(x, y + 1); };
    int c = n / 2, centre = 0;
    for (int y = c - 3; y <= c + 2; ++y) for (int x = c - 3; x <= c + 2; ++x) centre += op(x, y);
    if (centre < 6) return false;
    int cnt = 0, lr = 0, tb = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            if (op(x, y)) { ++cnt; lr += opNear(n - 1 - x, y); tb += opNear(x, n - 1 - y); }
    float frac = float(cnt) / float(n * n);
    if (frac < 0.01f || frac > 0.35f) return false;
    // Anything touching the square's border is a bigger element passing through, not the crosshair.
    int edge = 0;
    for (int i = 0; i < n; ++i) edge += op(i, 0) + op(i, n - 1) + op(0, i) + op(n - 1, i);
    if (edge > n / 8) return false;
    return lr >= cnt * 0.85f && tb >= cnt * 0.85f;
}

void MulV(const float* M, const float v[4], float o[4]) {
    for (int r = 0; r < 4; ++r) o[r] = M[r * 4 + 0] * v[0] + M[r * 4 + 1] * v[1] + M[r * 4 + 2] * v[2] + M[r * 4 + 3] * v[3];
}

} // namespace

void ReticleOnStereoFrame(ID3D11DeviceContext* ctx, ID3D11Texture2D* hud, ID3D11Texture2D* const eyes[2],
                          const float* centreVI, const float* const eyeVI[2]) {
    if (!g_config.reticleInScene || !ctx || !hud || !eyes[0] || !eyes[1]) { g_on = false; g_miss = 0; return; }
    s_frames++;
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    if (!dev) return;
    D3D11_TEXTURE2D_DESC hd; hud->GetDesc(&hd);
    const int n = std::max(32, std::min(320, int(hd.Height * 0.09f + 0.5f) & ~1));
    const int x0 = int(hd.Width) / 2 - n / 2, y0 = int(hd.Height) / 2 - n / 2;

    // 1. Detection, from the square copied two frames ago; then copy this frame's square (before it is cleared).
    if (Is8888(hd.Format) && EnsureStaging(dev, n, hd.Format)) {
        int rd = int((g_frame + 1) % kRing);   // oldest slot
        if (g_stageFull[rd]) {
            D3D11_MAPPED_SUBRESOURCE m{};
            HRESULT hr = ctx->Map(g_stage[rd], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (SUCCEEDED(hr)) {
                bool hit = Detect(static_cast<const uint8_t*>(m.pData), m.RowPitch, n);
                ctx->Unmap(g_stage[rd], 0);
                g_stageFull[rd] = false;
                s_reads++;
                if (hit) { s_detected++; if (!g_on) LOGI("reticle: crosshair found, drawing it in the 3D image"); g_on = true; g_miss = 0; }
                else if (g_on && ++g_miss >= 3) { g_on = false; LOGI("reticle: crosshair gone"); }
            }
        }
        int wr = int(g_frame % kRing);
        D3D11_BOX box{UINT(x0), UINT(y0), 0, UINT(x0 + n), UINT(y0 + n), 1};
        ctx->CopySubresourceRegion(g_stage[wr], 0, 0, 0, 0, hud, 0, &box);
        g_stageFull[wr] = true;
        ++g_frame;
    }

    // 2. While the crosshair is up: draw it into both eyes, then take it off the HUD panel.
    if (g_on && EnsurePipeline(dev) && EnsureViews(dev, g_hudV, hud, true, true) &&
        EnsureViews(dev, g_eyeV[0], eyes[0], false, true) && EnsureViews(dev, g_eyeV[1], eyes[1], false, true)) {
        // Aim point: the centre camera's screen centre, at ReticleDistance along that ray.
        const float* IP = centreVI + 48;
        const float* IV = centreVI + 16;
        float ndc[4] = {0, 0, 0.5f, 1}, pv[4];
        MulV(IP, ndc, pv);
        if (pv[3] != 0) { pv[0] /= pv[3]; pv[1] /= pv[3]; pv[2] /= pv[3]; }
        float len = std::sqrt(pv[0] * pv[0] + pv[1] * pv[1] + pv[2] * pv[2]);
        float dist = g_config.reticleDistance * 100.0f * g_config.worldScale;   // game units are cm
        float pc[4] = {len > 0 ? pv[0] / len * dist : 0, len > 0 ? pv[1] / len * dist : 0, len > 0 ? pv[2] / len * dist : dist, 1};
        float pw[4]; MulV(IV, pc, pw);

        ID3D11DeviceContext1* c1 = nullptr;
        if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1)) && c1) {
            ID3DDeviceContextState* prev = nullptr;
            c1->SwapDeviceContextState(g_dcs, &prev);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                float r[4] = {float(x0) / hd.Width, float(y0) / hd.Height, float(n) / hd.Width, float(n) / hd.Height};
                memcpy(m.pData, r, 16); ctx->Unmap(g_cb, 0);
            }
            ctx->OMSetBlendState(g_blend, nullptr, 0xffffffff);
            ctx->RSSetState(g_rs);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(g_vs, nullptr, 0);
            ctx->PSSetShader(g_ps, nullptr, 0);
            ctx->PSSetConstantBuffers(0, 1, &g_cb);
            ctx->PSSetShaderResources(0, 1, &g_hudV.srv);
            ctx->PSSetSamplers(0, 1, &g_samp);
            for (int e = 0; e < 2; ++e) {
                const float* VP = eyeVI[e] + 64;
                float clip[4]; MulV(VP, pw, clip);
                if (clip[3] <= 1e-6f) continue;
                D3D11_TEXTURE2D_DESC ed; eyes[e]->GetDesc(&ed);
                float u = clip[0] / clip[3] * 0.5f + 0.5f, v = 0.5f - clip[1] / clip[3] * 0.5f;
                // Same angular size as on screen: scale by the eye's projection relative to the centre camera's.
                float sx = std::fabs(eyeVI[e][32] / centreVI[32]), sy = std::fabs(eyeVI[e][37] / centreVI[37]);
                float w = n * sx * float(ed.Width) / hd.Width, h = n * sy * float(ed.Height) / hd.Height;
                D3D11_VIEWPORT vp{u * ed.Width - w * 0.5f, v * ed.Height - h * 0.5f, w, h, 0, 1};
                ctx->OMSetRenderTargets(1, &g_eyeV[e].rtv, nullptr);
                ctx->RSSetViewports(1, &vp);
                ctx->Draw(3, 0);
            }
            ID3D11ShaderResourceView* nullSrv = nullptr;
            ctx->PSSetShaderResources(0, 1, &nullSrv);
            ID3D11RenderTargetView* nullRtv = nullptr;
            ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
            const float zero[4] = {0, 0, 0, 0};
            D3D11_RECT rc{x0, y0, x0 + n, y0 + n};
            c1->ClearView(g_hudV.rtv, zero, &rc, 1);
            c1->SwapDeviceContextState(prev, nullptr);
            if (prev) prev->Release();
            c1->Release();
            s_drawn++;
        }
    }
    dev->Release();
}

void ReticleReport(char* buf, size_t size) {
    snprintf(buf, size, "reticle: %s, %llu stereo frames, %llu reads, crosshair in %llu, drawn in 3D %llu",
             g_config.reticleInScene ? "on" : "off", (unsigned long long)s_frames.exchange(0), (unsigned long long)s_reads.exchange(0),
             (unsigned long long)s_detected.exchange(0), (unsigned long long)s_drawn.exchange(0));
}

} // namespace so4vr
