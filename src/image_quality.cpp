// Image quality: the game's TAA (on/off, jitter amount) and a sharpening pass for the headset image.
//
// SO6.exe (known build), from static analysis:
//   Renderer (= *(0x248FA88)) + 0xB41B  u8 "TAA enabled". Initialised to 1 (0x10C7EF); also set by the engine's
//            property setter 0xDFAD80 (property 0x3C). Renderer::Prepare (0x165949) compares it with the TAA
//            object's flag and switches the TAA object (0x51CE70) when they differ; with TAA off it skips the
//            jitter sequence, and the scene shaders pick their non-TAA variants (0x3C393F).
//   0x3A92C8 JitterNext(taaObj) -> float4* {x, y, 0, 1}: the next sample of the jitter sequence (Halton 2/3,
//            in pixels, +-0.5). Only caller: Renderer::Prepare 0x165974, which copies it into the main
//            camera (+0x660) before the projection is built. Scaling it here scales the jitter everywhere
//            consistently (projection and the TAA's own un-jitter).
#include "temporal.h"
#include "image_quality.h"
#include "config.h"
#include "hook_util.h"
#include "safe_mem.h"
#include "log.h"
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <cstring>
#include <string>

namespace so4vr {
void DumpEyeBmp(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const wchar_t* path, bool alpha);
namespace {

constexpr uintptr_t kRendererPtr = 0x248FA88;
constexpr uintptr_t kRendererTaaOn = 0xB41B;
constexpr uintptr_t kFnJitterNext = 0x3A92C8;

uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

using PFN_JitterNext = float* (*)(void*);
PFN_JitterNext o_JitterNext = nullptr;
float g_jitter[4];

float* hk_JitterNext(void* taa) {
    float* j = o_JitterNext(taa);
    float s = g_config.taaJitter;
    static int n = 0;
    if (j && (++n == 300 || n == 301)) LOGI("TAA jitter sample (%.3f, %.3f) x %.2f", j[0], j[1], s);
    if (!j || s >= 0.999f) return j;
    if (s < 0.0f) s = 0.0f;
    g_jitter[0] = j[0] * s; g_jitter[1] = j[1] * s; g_jitter[2] = j[2]; g_jitter[3] = j[3];
    return g_jitter;   // the caller only reads it (copies 16 bytes into the camera)
}

bool g_forcedOff = false;   // we turned the game's TAA off (so we turn it back on, and only then)

// ---- sharpening (contrast-adaptive, after AMD FidelityFX CAS) ----
const char* kCas = R"(
Texture2D<float4> t : register(t0);
cbuffer C : register(b0) { float peak; int2 maxXY; float pad; };
struct V { float4 pos : SV_Position; };
V vsMain(uint id : SV_VertexID) {
    V o; float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
float3 L(int2 p) { return t.Load(int3(clamp(p, int2(0, 0), maxXY), 0)).rgb; }
float4 psMain(V i) : SV_Target {
    int2 p = int2(i.pos.xy);
    float4 e4 = t.Load(int3(p, 0));
    float3 a = L(p + int2(-1, -1)), b = L(p + int2(0, -1)), c = L(p + int2(1, -1));
    float3 d = L(p + int2(-1, 0)),  e = e4.rgb,              f = L(p + int2(1, 0));
    float3 g = L(p + int2(-1, 1)),  h = L(p + int2(0, 1)),  k = L(p + int2(1, 1));
    float3 mn = min(min(min(d, e), min(f, b)), h);
    mn += min(mn, min(min(a, c), min(g, k)));
    float3 mx = max(max(max(d, e), max(f, b)), h);
    mx += max(mx, max(max(a, c), max(g, k)));
    float3 amp = saturate(min(mn, 2.0 - mx) / max(mx, 1e-5));
    amp = sqrt(amp);
    float3 w = amp * peak;                         // peak < 0
    float3 o = (b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w);
    return float4(saturate(o), e4.a);
}
)";

struct SharpSlot {
    ID3D11Texture2D* out = nullptr; ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Texture2D* in = nullptr;  ID3D11ShaderResourceView* srv = nullptr;   // copy of src when src has no SRV binding
    ID3D11Texture2D* srvFor = nullptr;                                         // texture the srv was made for (referenced)
    UINT w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};
SharpSlot g_slot[2];
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11RasterizerState* g_rs = nullptr;
ID3D11BlendState* g_bs = nullptr;
ID3DDeviceContextState* g_dcs = nullptr;
bool g_failed = false;

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

DXGI_FORMAT ViewFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

bool EnsurePipeline(ID3D11Device* dev) {
    if (g_dcs) return true;
    if (g_failed) return false;
    g_failed = true;
    ID3D11Device1* dev1 = nullptr; dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1);
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = comp ? (pD3DCompile)GetProcAddress(comp, "D3DCompile") : nullptr;
    bool ok = dev1 && compile;
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    ok = ok && SUCCEEDED(compile(kCas, strlen(kCas), "cas", nullptr, nullptr, "vsMain", "vs_5_0", 0, 0, &vsb, nullptr));
    ok = ok && SUCCEEDED(compile(kCas, strlen(kCas), "cas", nullptr, nullptr, "psMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err));
    if (err) { LOGW("sharpen shader: %s", (const char*)err->GetBufferPointer()); err->Release(); }
    ok = ok && SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs));
    ok = ok && SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps));
    Rel(vsb); Rel(psb);
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rd, &g_rs));
    D3D11_BLEND_DESC bld{}; bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(dev->CreateBlendState(&bld, &g_bs));
    if (ok) {
        D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel(), chosen{};
        ok = SUCCEEDED(dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), &chosen, &g_dcs));
    }
    if (dev1) dev1->Release();
    if (!ok) { LOGW("sharpen: unavailable (shader or state creation failed)"); return false; }
    g_failed = false;
    LOGI("sharpen: ready");
    return true;
}

bool EnsureSlot(ID3D11Device* dev, SharpSlot& s, ID3D11Texture2D* src, const D3D11_TEXTURE2D_DESC& sd) {
    if (s.out && (s.w != sd.Width || s.h != sd.Height || s.fmt != sd.Format)) {
        Rel(s.out); Rel(s.rtv); Rel(s.srv); Rel(s.srvFor); Rel(s.in);
    }
    DXGI_FORMAT vf = ViewFormat(sd.Format);
    if (!s.out) {
        D3D11_TEXTURE2D_DESC d = sd;
        d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc = {1, 0}; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = 0; d.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &s.out))) return false;
        TemporalMarkModTexture(s.out);
        D3D11_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = vf; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateRenderTargetView(s.out, &rv, &s.rtv))) { Rel(s.out); return false; }
        s.w = sd.Width; s.h = sd.Height; s.fmt = sd.Format;
    }
    // Source view: straight on src when it can be bound as a shader resource, else on a copy.
    ID3D11Texture2D* viewTex = src;
    bool needCopy = !(sd.BindFlags & D3D11_BIND_SHADER_RESOURCE) || sd.SampleDesc.Count != 1 || sd.MipLevels != 1 || sd.ArraySize != 1;
    if (needCopy) {
        if (!s.in) {
            D3D11_TEXTURE2D_DESC d = sd;
            d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc = {1, 0}; d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = 0; d.MiscFlags = 0;
            if (FAILED(dev->CreateTexture2D(&d, nullptr, &s.in))) return false;
            TemporalMarkModTexture(s.in);
        }
        viewTex = s.in;
    }
    if (s.srvFor != viewTex) {
        Rel(s.srv); Rel(s.srvFor);
        D3D11_SHADER_RESOURCE_VIEW_DESC v{}; v.Format = vf; v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(viewTex, &v, &s.srv))) return false;
        s.srvFor = viewTex; s.srvFor->AddRef();
    }
    return true;
}

} // namespace

bool ImageQualityInstall() {
    return Hook(reinterpret_cast<void*>(Base() + kFnJitterNext), (void*)&hk_JitterNext, (void**)&o_JitterNext, "TaaJitterNext");
}

void ImageQualityPrePrepare() {
    uintptr_t r = 0;
    if (!SafeLoad(Base() + kRendererPtr, r) || !r) return;
    uint8_t cur = 0;
    if (!SafeLoad(r + kRendererTaaOn, cur)) return;
    bool wantOff = !g_config.taa && g_config.activeMode != Config::Flat;
    static int frames = 0;
    if (++frames % 1800 == 600) {   // diagnostic: what the main camera's jitter is now
        uintptr_t root = 0, cam = 0; float jit[2] = {};
        if (SafeLoad(Base() + 0x248F750, root) && root && SafeLoad(root + 0x590, cam) && cam) SafeRead(jit, reinterpret_cast<void*>(cam + 0x660), 8);
        LOGI("TAA: switch %u (want %s), camera jitter (%.3f, %.3f)", cur, wantOff ? "off" : "game", jit[0], jit[1]);
    }
    if (wantOff && cur != 0) {
        uint8_t z = 0; SafeWrite(reinterpret_cast<void*>(r + kRendererTaaOn), &z, 1);
        if (!g_forcedOff) LOGI("TAA: off (game TAA switch)");
        g_forcedOff = true;
    } else if (!wantOff && g_forcedOff) {
        uint8_t one = 1; SafeWrite(reinterpret_cast<void*>(r + kRendererTaaOn), &one, 1);
        g_forcedOff = false;
        LOGI("TAA: on");
    }
}

ID3D11Texture2D* SharpenForHeadset(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, int slot) {
    float amount = g_config.sharpen;
    if (!ctx || !src || amount <= 0.001f || slot < 0 || slot > 1) return src;
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    if (!dev) return src;
    D3D11_TEXTURE2D_DESC sd; src->GetDesc(&sd);
    SharpSlot& s = g_slot[slot];
    bool ok = EnsurePipeline(dev) && EnsureSlot(dev, s, src, sd);
    dev->Release();
    if (!ok) return src;
    ID3D11DeviceContext1* c1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1)) || !c1) return src;
    if (s.srvFor == s.in) ctx->CopySubresourceRegion(s.in, 0, 0, 0, 0, src, 0, nullptr);

    if (amount > 1.0f) amount = 1.0f;
    struct { float peak; int maxX, maxY; float pad; } cb{ -1.0f / (8.0f - 3.0f * amount), int(sd.Width) - 1, int(sd.Height) - 1, 0 };
    ID3DDeviceContextState* prev = nullptr;
    c1->SwapDeviceContextState(g_dcs, &prev);
    ctx->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
    D3D11_VIEWPORT vp{0, 0, float(sd.Width), float(sd.Height), 0, 1};
    ctx->OMSetRenderTargets(1, &s.rtv, nullptr);
    ctx->OMSetBlendState(g_bs, nullptr, 0xffffffff);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_rs);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_cb);
    ctx->PSSetShaderResources(0, 1, &s.srv);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
    c1->SwapDeviceContextState(prev, nullptr);
    if (prev) prev->Release();
    c1->Release();
    // Diagnostic: with DebugDumpEyesAt set, write the sharpened left image once (after as many sharpened frames).
    static unsigned calls = 0;
    if (slot == 0 && g_config.debugDumpEyesAt > 0 && ++calls == unsigned(g_config.debugDumpEyesAt)) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe; dir = dir.substr(0, dir.find_last_of(L'\\'));
        DumpEyeBmp(ctx, s.out, (dir + L"\\rofvr_sharp_left.bmp").c_str(), false);
        DumpEyeBmp(ctx, src, (dir + L"\\rofvr_unsharp_left.bmp").c_str(), false);
    }
    static bool logged = false;
    if (!logged) { logged = true; LOGI("sharpen: active (%ux%u, format %d, amount %.2f)", sd.Width, sd.Height, int(sd.Format), amount); }
    return s.out;
}

} // namespace so4vr
