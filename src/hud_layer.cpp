// HUD layer: the game draws its HUD/UI straight into the final image after compositing the 3D scene.
// In head-tracked modes those draws are redirected into a transparent texture of their own, which the XR
// thread shows as a quad layer at a fixed place in front of the player. In the second-eye replay the
// HUD draws are skipped (the HUD is the same for both eyes).
//
// Pass structure (from RenderDoc): ... scene -> [bind final RT] composite draw -> HUD draws -> end of frame.
// The first draw into the final RT is the composite; everything after it in the pass is UI.
#include "temporal.h"
#include "hud_layer.h"
#include "config.h"
#include "log.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <unordered_map>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace so4vr {

bool NativeIsFinalRT(ID3D11Resource* r);   // native_stereo.cpp: is this the game's final image target?
bool NativeInReplay();

bool g_hudFrame = false;
bool g_hudActive = false;

namespace {

enum State { Idle = 0, Armed = 1, Redirect = 2, Skip = 3 };
State g_state = Idle;
bool g_doneThisPass = false;

ID3D11Texture2D* g_hudTex = nullptr;
ID3D11RenderTargetView* g_hudRtv = nullptr;
ID3D11ShaderResourceView* g_hudSrv = nullptr;
ID3D11Texture2D* g_scratchTex = nullptr;          // replay: HUD draws go here (results discarded)
ID3D11RenderTargetView* g_scratchRtv = nullptr;
ID3D11ShaderResourceView* g_scratchSrv = nullptr;
ID3D11Texture2D* g_matteTex = nullptr;            // HUD colour + coverage from the black/white pair
ID3D11RenderTargetView* g_matteRtv = nullptr;
ID3D11ShaderResourceView* g_matteSrv = nullptr;
bool g_matteThisFrame = false;
uint64_t g_lastMatteTick = 0;    // when a black/white matte was last built
uint64_t s_hudMatteReused = 0, s_hudRawFallback = 0;
ID3D11RenderTargetView* g_curRtv = nullptr;         // target used while redirecting (HUD or scratch)
ID3D11RenderTargetView* g_gameFinalRtv = nullptr;   // the game's view of the final RT (referenced)
bool g_hudReady = false;
bool g_hudThisFrame = false;
bool g_curIsFinal = false;
bool g_redirectScratch = false;
int g_finalBindsThisPass = 0;      // the bound RT0 is the final image (or our HUD target in its place)   // this frame's HUD was captured (desktop composite)

// Blend state tracking: what the game asked for, and alpha-accumulating copies used while redirecting.
ID3D11BlendState* g_gameBlend = nullptr;   // identity only
float g_gameBlendFactor[4] = {1, 1, 1, 1};
UINT g_gameSampleMask = 0xffffffff;
std::unordered_map<ID3D11BlendState*, ID3D11BlendState*> g_blendMap;
ID3D11BlendState* g_nullBlendMod = nullptr;

std::atomic<unsigned long long> s_hudFrames{0}, s_hudDraws{0}, s_skippedDraws{0}, s_desktop{0};
unsigned long long g_drawsThisPass = 0;

// Desktop composite (HUD back over the final image for the monitor), drawn in its own context state.
ID3DDeviceContextState* g_dcs = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11PixelShader* g_psMatte = nullptr;
ID3D11BlendState* g_opaqueBlend = nullptr;
ID3D11BlendState* g_premulBlend = nullptr;
ID3D11SamplerState* g_samp = nullptr;
ID3D11RasterizerState* g_rs = nullptr;
bool g_blitFailed = false;

template <typename T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

ID3D11Resource* ResOf(ID3D11View* v) {
    if (!v) return nullptr;
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    if (r) r->Release();   // identity only
    return r;
}

// Blend state with the same colour blending, but alpha accumulated as coverage (premultiplied output).
ID3D11BlendState* AlphaVersion(ID3D11DeviceContext* ctx, ID3D11BlendState* orig) {
    if (!orig) {
        if (!g_nullBlendMod) {
            D3D11_BLEND_DESC d{};
            d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
            if (dev) { dev->CreateBlendState(&d, &g_nullBlendMod); dev->Release(); }
        }
        return g_nullBlendMod;
    }
    auto it = g_blendMap.find(orig);
    if (it != g_blendMap.end()) return it->second;
    D3D11_BLEND_DESC d; orig->GetDesc(&d);
    int n = d.IndependentBlendEnable ? 8 : 1;
    for (int i = 0; i < n; ++i) {
        auto& rt = d.RenderTarget[i];
        if (rt.RenderTargetWriteMask & 7) rt.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
        if (rt.BlendEnable) {
            bool additive = rt.DestBlend == D3D11_BLEND_ONE && rt.BlendOp == D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = additive ? D3D11_BLEND_ZERO : D3D11_BLEND_ONE;
            rt.DestBlendAlpha = additive ? D3D11_BLEND_ONE : D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        }
    }
    {
        D3D11_BLEND_DESC od; orig->GetDesc(&od);
        const auto& a = od.RenderTarget[0]; const auto& m = d.RenderTarget[0];
        if (g_blendMap.size() < 24)
            LOGI("HUD layer: blend state #%zu: enable %d src %d dst %d op %d | alpha src %d dst %d op %d | mask 0x%x -> alpha src %d dst %d mask 0x%x (a2c %d indep %d)",
                 g_blendMap.size(), a.BlendEnable, a.SrcBlend, a.DestBlend, a.BlendOp, a.SrcBlendAlpha, a.DestBlendAlpha, a.BlendOpAlpha,
                 a.RenderTargetWriteMask, m.SrcBlendAlpha, m.DestBlendAlpha, m.RenderTargetWriteMask, od.AlphaToCoverageEnable, od.IndependentBlendEnable);
    }
    ID3D11BlendState* mod = nullptr;
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    if (dev) { dev->CreateBlendState(&d, &mod); dev->Release(); }
    if (!mod) { mod = orig; orig->AddRef(); }
    g_blendMap[orig] = mod;
    return mod;
}

bool EnsureHudTarget(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* gameRtv) {
    ID3D11Resource* r = ResOf(gameRtv);
    D3D11_RESOURCE_DIMENSION dim; if (!r) return false; r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    D3D11_TEXTURE2D_DESC fd; static_cast<ID3D11Texture2D*>(r)->GetDesc(&fd);
    D3D11_RENDER_TARGET_VIEW_DESC rd; gameRtv->GetDesc(&rd);
    if (g_hudTex) {
        D3D11_TEXTURE2D_DESC hd; g_hudTex->GetDesc(&hd);
        if (hd.Width == fd.Width && hd.Height == fd.Height && hd.Format == fd.Format) return true;
        Rel(g_hudRtv); Rel(g_hudSrv); Rel(g_hudTex); Rel(g_scratchRtv); Rel(g_scratchTex); Rel(g_scratchSrv); Rel(g_matteRtv); Rel(g_matteSrv); Rel(g_matteTex);
    }
    D3D11_TEXTURE2D_DESC td = fd;
    td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1; td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0; td.MiscFlags = 0;
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    if (!dev) return false;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_hudTex));
    if (ok) TemporalMarkModTexture(g_hudTex);
    if (ok) {
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; rd.Texture2D.MipSlice = 0;
        ok = SUCCEEDED(dev->CreateRenderTargetView(g_hudTex, &rd, &g_hudRtv));
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = rd.Format; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        ok = ok && SUCCEEDED(dev->CreateShaderResourceView(g_hudTex, &sd, &g_hudSrv));
        ok = ok && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_scratchTex));
        if (ok) TemporalMarkModTexture(g_scratchTex);
        ok = ok && SUCCEEDED(dev->CreateRenderTargetView(g_scratchTex, &rd, &g_scratchRtv));
        ok = ok && SUCCEEDED(dev->CreateShaderResourceView(g_scratchTex, &sd, &g_scratchSrv));
        ok = ok && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_matteTex));
        if (ok) TemporalMarkModTexture(g_matteTex);
        ok = ok && SUCCEEDED(dev->CreateRenderTargetView(g_matteTex, &rd, &g_matteRtv));
        ok = ok && SUCCEEDED(dev->CreateShaderResourceView(g_matteTex, &sd, &g_matteSrv));
    }
    dev->Release();
    if (!ok) { LOGE("HUD layer: could not create the HUD target"); Rel(g_hudRtv); Rel(g_hudSrv); Rel(g_hudTex); Rel(g_scratchRtv); Rel(g_scratchTex); Rel(g_scratchSrv); Rel(g_matteRtv); Rel(g_matteSrv); Rel(g_matteTex); return false; }
    LOGI("HUD layer: HUD target %ux%u format %d (view format %d)", td.Width, td.Height, td.Format, rd.Format);
    return true;
}

void BeginRedirect(ID3D11DeviceContext* ctx, bool scratch) {
    ID3D11RenderTargetView* rtvs[8] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(8, rtvs, &dsv);
    bool hasRt = rtvs[0] != nullptr, isFin = hasRt && NativeIsFinalRT(ResOf(rtvs[0]));
    bool ok = isFin && EnsureHudTarget(ctx, rtvs[0]);
    if (!ok) { static int logs = 0; if (logs < 10) { ++logs; LOGI("HUD layer: redirect not started (rt bound %d, is final %d, scratch %d)", int(hasRt), int(isFin), int(scratch)); } }
    if (ok) {
        Rel(g_gameFinalRtv);
        g_gameFinalRtv = rtvs[0]; g_gameFinalRtv->AddRef();
        g_curRtv = scratch ? g_scratchRtv : g_hudRtv;
        // HUD on black in pass 1, the same HUD on white in the replay: the difference gives its coverage.
        const float zero[4] = {0, 0, 0, 0}, one[4] = {1, 1, 1, 1};
        ctx->ClearRenderTargetView(g_curRtv, scratch ? one : zero);
        g_state = Redirect;
        g_redirectScratch = scratch;
        ID3D11RenderTargetView* sub[8]; memcpy(sub, rtvs, sizeof(sub));
        sub[0] = g_curRtv;
        UINT n = 0; for (UINT i = 0; i < 8; ++i) if (rtvs[i]) n = i + 1;
        ctx->OMSetRenderTargets(n, sub, dsv);           // passes through our hook unchanged (not the final RT)
        ctx->OMSetBlendState(g_gameBlend, g_gameBlendFactor, g_gameSampleMask);   // re-set: the hook swaps in the alpha version
    } else {
        g_state = Idle;
    }
    for (auto* v : rtvs) if (v) v->Release();
    if (dsv) dsv->Release();
}

void BuildMatte(ID3D11DeviceContext* ctx);

void EndRedirect(ID3D11DeviceContext* ctx) {
    g_state = Idle;   // hooks pass through from here on
    ID3D11RenderTargetView* rtvs[8] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(8, rtvs, &dsv);
    if (rtvs[0] && rtvs[0] == g_curRtv && g_gameFinalRtv) {
        ID3D11RenderTargetView* sub[8]; memcpy(sub, rtvs, sizeof(sub));
        sub[0] = g_gameFinalRtv;
        UINT n = 0; for (UINT i = 0; i < 8; ++i) if (rtvs[i]) n = i + 1;
        ctx->OMSetRenderTargets(n, sub, dsv);
    }
    for (auto* v : rtvs) if (v) v->Release();
    if (dsv) dsv->Release();
    ctx->OMSetBlendState(g_gameBlend, g_gameBlendFactor, g_gameSampleMask);   // what the game believes is bound
    if (g_redirectScratch) { if (g_hudThisFrame) BuildMatte(ctx); return; }   // replay: HUD over white -> coverage
    g_hudReady = true;
    g_hudThisFrame = true;
    s_hudFrames++;
    {   // diagnostic: draws per HUD frame for a stretch of frames (flicker hunting)
        static int logged = 0, frames = 0;
        ++frames;
        if (g_config.debugDumpEyesAt > 0 && frames > 300 && logged < 24) { ++logged; LOGI("HUD frame: %llu draws redirected", g_drawsThisPass); }
    }
}

const char* kBlit = R"(
Texture2D t : register(t0);
SamplerState s : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vsMain(uint id : SV_VertexID) {
    V o; float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
float4 psMain(V i) : SV_Target { return t.SampleLevel(s, i.uv, 0); }
Texture2D tw : register(t1);
float4 psMatte(V i) : SV_Target {
    float3 b = t.SampleLevel(s, i.uv, 0).rgb;    // HUD over black  = c*a
    float3 w = tw.SampleLevel(s, i.uv, 0).rgb;   // HUD over white  = c*a + (1-a)
    float a = saturate(1.0 - dot(w - b, float3(0.3333, 0.3334, 0.3333)));
    return float4(min(b, a.xxx), a);             // premultiplied colour, coverage
}
)";

bool EnsureBlit(ID3D11DeviceContext* ctx) {
    if (g_dcs) return true;
    if (g_blitFailed) return false;
    g_blitFailed = true;
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    if (!dev) return false;
    ID3D11Device1* dev1 = nullptr; dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1);
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = comp ? (pD3DCompile)GetProcAddress(comp, "D3DCompile") : nullptr;
    bool ok = dev1 && compile;
    ID3DBlob *vsb = nullptr, *psb = nullptr;
    ok = ok && SUCCEEDED(compile(kBlit, strlen(kBlit), "hudblit", nullptr, nullptr, "vsMain", "vs_5_0", 0, 0, &vsb, nullptr));
    ok = ok && SUCCEEDED(compile(kBlit, strlen(kBlit), "hudblit", nullptr, nullptr, "psMain", "ps_5_0", 0, 0, &psb, nullptr));
    ok = ok && SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs));
    ok = ok && SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps));
    Rel(psb);
    ok = ok && SUCCEEDED(compile(kBlit, strlen(kBlit), "hudblit", nullptr, nullptr, "psMatte", "ps_5_0", 0, 0, &psb, nullptr));
    ok = ok && SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_psMatte));
    Rel(vsb); Rel(psb);
    { D3D11_BLEND_DESC ob{}; ob.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL; ok = ok && SUCCEEDED(dev->CreateBlendState(&ob, &g_opaqueBlend)); }
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(dev->CreateBlendState(&bd, &g_premulBlend));
    D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    ok = ok && SUCCEEDED(dev->CreateSamplerState(&sd, &g_samp));
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rd, &g_rs));
    if (ok) {
        D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel(), chosen{};
        ok = SUCCEEDED(dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), &chosen, &g_dcs));
    }
    if (dev1) dev1->Release();
    dev->Release();
    if (!ok) { LOGW("HUD layer: desktop HUD composite unavailable"); return false; }
    g_blitFailed = false;
    return true;
}

void BuildMatte(ID3D11DeviceContext* ctx) {
    if (!g_matteRtv || !g_scratchSrv || !g_hudSrv || !EnsureBlit(ctx)) return;
    ID3D11DeviceContext1* c1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1)) || !c1) return;
    ID3DDeviceContextState* prev = nullptr;
    c1->SwapDeviceContextState(g_dcs, &prev);
    D3D11_TEXTURE2D_DESC hd; g_hudTex->GetDesc(&hd);
    D3D11_VIEWPORT vp{0, 0, float(hd.Width), float(hd.Height), 0, 1};
    ctx->OMSetRenderTargets(1, &g_matteRtv, nullptr);
    ctx->OMSetBlendState(g_opaqueBlend, nullptr, 0xffffffff);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_rs);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(g_psMatte, nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = {g_hudSrv, g_scratchSrv};
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetSamplers(0, 1, &g_samp);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[2] = {};
    ctx->PSSetShaderResources(0, 2, nulls);
    c1->SwapDeviceContextState(prev, nullptr);
    if (prev) prev->Release();
    c1->Release();
    g_matteThisFrame = true;
    g_lastMatteTick = GetTickCount64();
}

} // namespace

void HudFrameStart(bool gameplayCamera) {
    g_hudFrame = g_config.hudLayer && HeadTrackedMode() && gameplayCamera;
    g_doneThisPass = false;
    g_drawsThisPass = 0;
    g_state = Idle;
    g_hudActive = false;
    g_hudThisFrame = false;
    g_matteThisFrame = false;
    g_curIsFinal = false;
}

void HudOnSetRT(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const*& r, ID3D11RenderTargetView** scratch) {
    if (!g_hudFrame) return;
    bool isFinal = n > 0 && r && r[0] && (r[0] == g_hudRtv || r[0] == g_scratchRtv || NativeIsFinalRT(ResOf(r[0])));
    bool wasFinal = g_curIsFinal;
    g_curIsFinal = isFinal;
    if (isFinal) ++g_finalBindsThisPass;
    if (!isFinal) {
        // Leaving the HUD target while redirecting (UI drawn into its own offscreen targets): give those
        // draws the game's own blend state, unmodified.
        if (g_state == Redirect && wasFinal) ctx->OMSetBlendState(g_gameBlend, g_gameBlendFactor, g_gameSampleMask);
        return;
    }
    if (g_state == Idle && !g_doneThisPass) { g_state = Armed; g_hudActive = true; return; }
    if (g_state == Redirect && g_curRtv) {
        if (r[0] != g_curRtv) {
            UINT m = n > 8 ? 8 : n;
            for (UINT i = 0; i < m; ++i) scratch[i] = r[i];
            scratch[0] = g_curRtv;
            r = scratch;
        }
        if (!wasFinal) ctx->OMSetBlendState(g_gameBlend, g_gameBlendFactor, g_gameSampleMask);   // back on the HUD: alpha version
    }
}

ID3D11BlendState* HudOnSetBlend(ID3D11DeviceContext* ctx, ID3D11BlendState* bs, const FLOAT* factor, UINT mask) {
    g_gameBlend = bs;
    if (factor) memcpy(g_gameBlendFactor, factor, sizeof(g_gameBlendFactor)); else { for (float& f : g_gameBlendFactor) f = 1.0f; }
    g_gameSampleMask = mask;
    if (g_state != Redirect || !g_curIsFinal || !g_config.hudAlphaFix) return bs;
    return AlphaVersion(ctx, bs);
}

bool HudBeforeDraw() {
    // Replay: skip only what would land on the final image; UI drawn into offscreen targets (minimap,
    // menu backgrounds) still runs, because the game's own double-buffering expects it every pass.
    if (g_state == Skip && g_curIsFinal) { s_skippedDraws++; return true; }
    if (g_state == Redirect) { if (g_redirectScratch) s_skippedDraws++; else { s_hudDraws++; g_drawsThisPass++; } }
    return false;
}

void HudAfterDraw(ID3D11DeviceContext* ctx) {
    if (g_state != Armed || !g_curIsFinal) return;   // the composite is the first draw *on the final image*
    // The draw that just ran was the composite. Everything after it in this pass is UI.
    g_doneThisPass = true;
    // Replay: the HUD is the same for both eyes, but its draws still run (into a scratch target) so that
    // GPU-side side effects the game depends on next frame (queries, stencil, counters) stay as they were.
    BeginRedirect(ctx, NativeInReplay());
    g_hudActive = g_state != Idle;
}

void HudEndPass(ID3D11DeviceContext* ctx) {
    {
        static int logs = 0, calls = 0;
        if (g_config.debugDumpEyesAt > 0 && g_hudFrame && ++calls > 400 && logs < 16) {
            ++logs;
            LOGI("HUD end of pass: frame flag %d, state %d, done %d, final binds %d, cur final %d", int(g_hudFrame), int(g_state), int(g_doneThisPass), g_finalBindsThisPass, int(g_curIsFinal));
        }
        g_finalBindsThisPass = 0;
    }
    if (g_state == Redirect && ctx) EndRedirect(ctx);
    g_state = Idle;
    g_doneThisPass = false;
    g_hudActive = false;
}

ID3D11Texture2D* HudTakeFrame() {
    if (!g_hudReady) return nullptr;
    g_hudReady = false;
    if (g_matteThisFrame) return g_matteTex;
    // No matte this frame (no second-eye pass: skipped frame, menu, transition). The raw HUD has the game's
    // alpha, which is wrong for parts of it (the minimap shows white), so in stereo show the last matte
    // instead of flickering to the raw image.
    if (g_matteTex && g_lastMatteTick && GetTickCount64() - g_lastMatteTick < 500) { s_hudMatteReused++; return g_matteTex; }
    s_hudRawFallback++;
    return g_hudTex;
}

ID3D11Texture2D* HudTexture() {
    if (!g_hudThisFrame) return nullptr;
    if (g_matteThisFrame || (g_matteTex && g_lastMatteTick && GetTickCount64() - g_lastMatteTick < 500)) return g_matteTex;
    return g_hudTex;
}

bool HudFrameHasHud() { return g_hudThisFrame; }

void HudCompositeToDesktop(ID3D11DeviceContext* ctx) {
    if (!g_config.hudOnDesktop || !g_hudThisFrame || !g_hudSrv || !g_gameFinalRtv || !ctx) return;
    if (!EnsureBlit(ctx)) return;
    ID3D11DeviceContext1* c1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1)) || !c1) return;
    ID3DDeviceContextState* prev = nullptr;
    c1->SwapDeviceContextState(g_dcs, &prev);
    D3D11_TEXTURE2D_DESC hd; g_hudTex->GetDesc(&hd);
    D3D11_VIEWPORT vp{0, 0, float(hd.Width), float(hd.Height), 0, 1};
    ctx->OMSetRenderTargets(1, &g_gameFinalRtv, nullptr);
    ctx->OMSetBlendState(g_premulBlend, nullptr, 0xffffffff);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_rs);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ID3D11ShaderResourceView* hs = g_matteThisFrame ? g_matteSrv : g_hudSrv;
    ctx->PSSetShaderResources(0, 1, &hs);
    ctx->PSSetSamplers(0, 1, &g_samp);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    c1->SwapDeviceContextState(prev, nullptr);
    if (prev) prev->Release();
    c1->Release();
    s_desktop++;
}

void HudReport(char* buf, size_t size) {
    snprintf(buf, size, "HUD layer: %llu frames, %llu HUD draws redirected, %llu replay HUD draws to scratch, %llu desktop composites",
             (unsigned long long)s_hudFrames.exchange(0), (unsigned long long)s_hudDraws.exchange(0),
             (unsigned long long)s_skippedDraws.exchange(0), (unsigned long long)s_desktop.exchange(0));
    size_t k = strlen(buf);
    if (k < size) snprintf(buf + k, size - k, ", no matte: %llu reused last, %llu raw", (unsigned long long)s_hudMatteReused, (unsigned long long)s_hudRawFallback);
    s_hudMatteReused = s_hudRawFallback = 0;
}

} // namespace so4vr
