// SO4 HUD layer: GPU helpers.
// The game's UI jobs (everything after the PostProcessCombiner job) draw into the final image. so4_stereo.cpp
// runs those UI commands once over black and once over white; from the pair a premultiplied HUD texture with
// real coverage is built (colour = over-black, alpha = 1 - (white - black)), which the XR thread shows as a
// quad layer. The headset eyes themselves carry no HUD.
#include "so4_hud.h"
#include "temporal.h"
#include "config.h"
#include "log.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace so4vr {
namespace {

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

ID3D11Texture2D* g_tex[2] = {};                 // UI over black / over white (copies of the final image)
ID3D11ShaderResourceView* g_srv[2] = {};
ID3D11Texture2D* g_matte = nullptr;             // premultiplied colour + coverage
ID3D11RenderTargetView* g_matteRtv = nullptr;
ID3D11ShaderResourceView* g_matteSrv = nullptr;
UINT g_w = 0, g_h = 0; DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
bool g_valid[2] = {};
ULONGLONG g_matteTick = 0;

ID3DDeviceContextState* g_dcs = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_psMatte = nullptr;
ID3D11PixelShader* g_psCopy = nullptr;
ID3D11BlendState* g_opaque = nullptr;
ID3D11BlendState* g_premul = nullptr;
ID3D11SamplerState* g_samp = nullptr;
ID3D11RasterizerState* g_rs = nullptr;
bool g_shaderFailed = false;

std::atomic<uint64_t> s_mattes{0}, s_failed{0}, s_reused{0}, s_composites{0};

const char* kShaders = R"(
Texture2D tb : register(t0);
Texture2D tw : register(t1);
SamplerState s : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vsMain(uint id : SV_VertexID) {
    V o; float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
float4 psMatte(V i) : SV_Target {
    float3 b = tb.SampleLevel(s, i.uv, 0).rgb;    // UI over black = c*a
    float3 w = tw.SampleLevel(s, i.uv, 0).rgb;    // UI over white = c*a + (1-a)
    float a = saturate(1.0 - dot(w - b, float3(0.3333, 0.3334, 0.3333)));
    return float4(min(b, a.xxx), a);
}
float4 psCopy(V i) : SV_Target { return tb.SampleLevel(s, i.uv, 0); }
)";

bool EnsureShaders(ID3D11Device* dev) {
    if (g_dcs) return true;
    if (g_shaderFailed) return false;
    g_shaderFailed = true;
    ID3D11Device1* dev1 = nullptr; dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1);
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = comp ? (pD3DCompile)GetProcAddress(comp, "D3DCompile") : nullptr;
    bool ok = dev1 && compile;
    ID3DBlob* b = nullptr;
    auto ps = [&](const char* entry, ID3D11PixelShader** out) {
        if (!ok) return;
        ok = SUCCEEDED(compile(kShaders, strlen(kShaders), "so4hud", nullptr, nullptr, entry, "ps_5_0", 0, 0, &b, nullptr)) &&
             SUCCEEDED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, out));
        Rel(b);
    };
    if (ok) {
        ok = SUCCEEDED(compile(kShaders, strlen(kShaders), "so4hud", nullptr, nullptr, "vsMain", "vs_5_0", 0, 0, &b, nullptr)) &&
             SUCCEEDED(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_vs));
        Rel(b);
    }
    ps("psMatte", &g_psMatte);
    ps("psCopy", &g_psCopy);
    { D3D11_BLEND_DESC d{}; d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL; ok = ok && SUCCEEDED(dev->CreateBlendState(&d, &g_opaque)); }
    {
        D3D11_BLEND_DESC d{};
        auto& t = d.RenderTarget[0];
        t.BlendEnable = TRUE; t.SrcBlend = D3D11_BLEND_ONE; t.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; t.BlendOp = D3D11_BLEND_OP_ADD;
        t.SrcBlendAlpha = D3D11_BLEND_ZERO; t.DestBlendAlpha = D3D11_BLEND_ONE; t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok = ok && SUCCEEDED(dev->CreateBlendState(&d, &g_premul));
    }
    { D3D11_SAMPLER_DESC d{}; d.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; d.MaxLOD = D3D11_FLOAT32_MAX; ok = ok && SUCCEEDED(dev->CreateSamplerState(&d, &g_samp)); }
    { D3D11_RASTERIZER_DESC d{}; d.FillMode = D3D11_FILL_SOLID; d.CullMode = D3D11_CULL_NONE; d.DepthClipEnable = TRUE; ok = ok && SUCCEEDED(dev->CreateRasterizerState(&d, &g_rs)); }
    if (ok) {
        D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel(), chosen{};
        ok = SUCCEEDED(dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), &chosen, &g_dcs));
    }
    if (dev1) dev1->Release();
    if (!ok) { LOGW("HUD layer: shader setup failed, HUD stays in the image"); return false; }
    g_shaderFailed = false;
    LOGI("HUD layer: shaders ready");
    return true;
}

bool g_texFailed = false;
bool EnsureTextures(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& src, DXGI_FORMAT viewFmt) {
    if (g_matte && g_w == src.Width && g_h == src.Height && g_fmt == src.Format) return true;
    if (g_texFailed && g_w == src.Width && g_h == src.Height && g_fmt == src.Format) return false;
    for (int i = 0; i < 2; ++i) { Rel(g_srv[i]); Rel(g_tex[i]); g_valid[i] = false; }
    Rel(g_matteRtv); Rel(g_matteSrv); Rel(g_matte); g_matteTick = 0;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = src.Width; d.Height = src.Height; d.MipLevels = 1; d.ArraySize = 1; d.Format = src.Format;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bool ok = true;
    for (int i = 0; i < 2 && ok; ++i)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = viewFmt; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_tex[i])) && SUCCEEDED(dev->CreateShaderResourceView(g_tex[i], &sv, &g_srv[i]));
        if (g_tex[i]) TemporalMarkModTexture(g_tex[i]);
    }
    // XR quad layer: premultiplied RGBA8, encoded like the game's view of the final image (UNORM or sRGB)
    d.Format = (viewFmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || viewFmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_matte)) && SUCCEEDED(dev->CreateRenderTargetView(g_matte, nullptr, &g_matteRtv)) &&
         SUCCEEDED(dev->CreateShaderResourceView(g_matte, nullptr, &g_matteSrv));
    if (g_matte) TemporalMarkModTexture(g_matte);
    if (!ok) {
        LOGE("HUD layer: texture creation failed (%ux%u format %d, view %d)", src.Width, src.Height, src.Format, viewFmt);
        g_texFailed = true; g_w = src.Width; g_h = src.Height; g_fmt = src.Format;
        for (int i = 0; i < 2; ++i) { Rel(g_srv[i]); Rel(g_tex[i]); }
        Rel(g_matteRtv); Rel(g_matteSrv); Rel(g_matte);
        return false;
    }
    g_w = src.Width; g_h = src.Height; g_fmt = src.Format; g_texFailed = false;
    LOGI("HUD layer: textures %ux%u (source format %d, view %d)", g_w, g_h, g_fmt, viewFmt);
    return true;
}

// Fullscreen triangle into `rtv` with our own pipeline state (the game's state is swapped out and back).
void Draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, ID3D11PixelShader* ps, ID3D11BlendState* blend,
          ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, UINT w, UINT h) {
    ID3D11DeviceContext1* c1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1)) || !c1) return;
    ID3DDeviceContextState* prev = nullptr;
    c1->SwapDeviceContextState(g_dcs, &prev);
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetBlendState(blend, nullptr, 0xffffffff);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_rs);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = {a, b};
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetSamplers(0, 1, &g_samp);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[2] = {};
    ctx->PSSetShaderResources(0, 2, nulls);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
    c1->SwapDeviceContextState(prev, nullptr);
    if (prev) prev->Release();
    c1->Release();
}

} // namespace

bool So4HudPrepare(ID3D11Device* dev, ID3D11Texture2D* finalTex, DXGI_FORMAT viewFmt) {
    if (!dev || !finalTex || !EnsureShaders(dev)) return false;
    D3D11_TEXTURE2D_DESC d; finalTex->GetDesc(&d);
    if (d.SampleDesc.Count != 1) return false;
    g_valid[0] = g_valid[1] = false;
    if (viewFmt == DXGI_FORMAT_UNKNOWN) viewFmt = d.Format;
    return EnsureTextures(dev, d, viewFmt);
}

void So4HudStore(ID3D11DeviceContext* ctx, ID3D11Texture2D* finalTex, int bg) {
    if (!g_tex[bg & 1] || !finalTex) return;
    ctx->CopyResource(g_tex[bg & 1], finalTex);
    g_valid[bg & 1] = true;
}

bool So4HudBuildMatte(ID3D11DeviceContext* ctx) {
    if (!g_valid[0] || !g_valid[1] || !g_matteRtv) { s_failed++; return false; }
    Draw(ctx, g_matteRtv, g_psMatte, g_opaque, g_srv[0], g_srv[1], g_w, g_h);
    g_matteTick = GetTickCount64();
    s_mattes++;
    return true;
}

ID3D11Texture2D* So4HudTexture(bool builtThisFrame) {
    if (!g_matte || !g_matteTick) return nullptr;
    if (builtThisFrame) return g_matte;
    if (GetTickCount64() - g_matteTick < 500) { s_reused++; return g_matte; }   // a frame without a fresh matte: keep the last one briefly
    return nullptr;
}

void So4HudComposite(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv) {
    if (!rtv || !g_matteSrv || !g_matteTick || !g_dcs) return;
    Draw(ctx, rtv, g_psCopy, g_premul, g_matteSrv, nullptr, g_w, g_h);
    s_composites++;
}

ID3D11Texture2D* So4HudDebugTex(int bg) { return g_valid[bg & 1] ? g_tex[bg & 1] : nullptr; }

void So4HudReport(char* buf, unsigned size) {
    snprintf(buf, size, "HUD layer (SO4): %llu mattes, %llu failed, %llu reused last, %llu desktop composites",
             (unsigned long long)s_mattes.exchange(0), (unsigned long long)s_failed.exchange(0),
             (unsigned long long)s_reused.exchange(0), (unsigned long long)s_composites.exchange(0));
}

} // namespace so4vr
