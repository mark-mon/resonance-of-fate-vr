#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstring>
#include "reproject.h"
#include "log.h"

namespace so4vr {
namespace {

const char* kShader = R"(
cbuffer C : register(b0) {
    float4 srcFov;   // tangents: left, right, up, down
    float4 dstFov;
    float4 m0;       // rows of M: maps a direction in the display frame to the render frame
    float4 m1;
    float4 m2;
    float4 opts;     // x: decode sRGB, y/z: source column range (start, width; z = 0: whole image), w: half texel
};
Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

V vsMain(uint id : SV_VertexID) {
    V o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 SrgbToLinear(float3 c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float4 psMain(V i) : SV_Target {
    // Direction through this output pixel in the display-time head frame (OpenXR: -Z forward).
    float x = lerp(dstFov.x, dstFov.y, i.uv.x);
    float y = lerp(dstFov.z, dstFov.w, i.uv.y);
    float3 d = float3(x, y, -1);
    float3 s = float3(dot(m0.xyz, d), dot(m1.xyz, d), dot(m2.xyz, d));
    if (s.z > -1e-4) return float4(0, 0, 0, 1);
    float sx = s.x / -s.z, sy = s.y / -s.z;
    float2 suv = float2((sx - srcFov.x) / (srcFov.y - srcFov.x), (srcFov.z - sy) / (srcFov.z - srcFov.w));
    if (any(suv < 0) || any(suv > 1)) return float4(0, 0, 0, 1);
    if (opts.z > 0) suv.x = opts.y + clamp(suv.x * opts.z, opts.w, opts.z - opts.w);
    float3 c = tex.SampleLevel(samp, suv, 0).rgb;
    if (opts.x > 0.5) c = SrgbToLinear(c);
    return float4(c, 1);
}
)";

struct Consts { float srcFov[4], dstFov[4], m0[4], m1[4], m2[4], opts[4]; };

void QuatToMat(const XrQuaternionf& q, float r[9]) {
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    r[0] = 1 - 2 * (yy + zz); r[1] = 2 * (xy - wz);     r[2] = 2 * (xz + wy);
    r[3] = 2 * (xy + wz);     r[4] = 1 - 2 * (xx + zz); r[5] = 2 * (yz - wx);
    r[6] = 2 * (xz - wy);     r[7] = 2 * (yz + wx);     r[8] = 1 - 2 * (xx + yy);
}

template <typename T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

} // namespace

bool Reprojector::Init(ID3D11Device* dev) {
    if (ready_) return true;
    dev_ = dev;
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = comp ? (pD3DCompile)GetProcAddress(comp, "D3DCompile") : nullptr;
    if (!compile) { LOGE("reprojection: d3dcompiler_47.dll not available"); return false; }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(compile(kShader, strlen(kShader), "reproject", nullptr, nullptr, "vsMain", "vs_5_0", 0, 0, &vsb, &err))) {
        LOGE("reprojection: VS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        Rel(err); return false;
    }
    if (FAILED(compile(kShader, strlen(kShader), "reproject", nullptr, nullptr, "psMain", "ps_5_0", 0, 0, &psb, &err))) {
        LOGE("reprojection: PS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        Rel(err); Rel(vsb); return false;
    }
    bool ok = SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_)) &&
              SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_));
    Rel(vsb); Rel(psb);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ok = ok && SUCCEEDED(dev->CreateSamplerState(&sd, &sampler_));
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Consts);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &cb_));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rd, &rs_));
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(dev->CreateBlendState(&bl, &bs_));
    D3D11_DEPTH_STENCIL_DESC dsd{};
    ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&dsd, &ds_));
    if (!ok) { LOGE("reprojection: resource creation failed"); return false; }
    ready_ = true;
    LOGI("reprojection (mod timewarp) ready");
    return true;
}

ID3D11ShaderResourceView* Reprojector::GetSrv(ID3D11Texture2D* t) {
    for (auto& e : srvs_) if (e.tex == t) return e.srv;
    for (auto& e : srvs_) {
        if (!e.tex) {
            if (FAILED(dev_->CreateShaderResourceView(t, nullptr, &e.srv))) return nullptr;
            e.tex = t;
            return e.srv;
        }
    }
    ReleaseViews();
    return GetSrv(t);
}

ID3D11RenderTargetView* Reprojector::GetRtv(ID3D11Texture2D* t, DXGI_FORMAT fmt) {
    for (auto& e : rtvs_) if (e.tex == t) return e.rtv;
    for (auto& e : rtvs_) {
        if (!e.tex) {
            D3D11_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = fmt;
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            if (FAILED(dev_->CreateRenderTargetView(t, &rd, &e.rtv))) {
                if (FAILED(dev_->CreateRenderTargetView(t, nullptr, &e.rtv))) return nullptr;
            }
            e.tex = t;
            return e.rtv;
        }
    }
    ReleaseViews();
    return GetRtv(t, fmt);
}

void Reprojector::ReleaseViews() {
    for (auto& e : srvs_) { Rel(e.srv); e.tex = nullptr; }
    for (auto& e : rtvs_) { Rel(e.rtv); e.tex = nullptr; }
}

bool Reprojector::Draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* dst, DXGI_FORMAT dstFormat,
                       UINT width, UINT height, const XrFovf& srcFov, const XrQuaternionf& srcRot,
                       const XrFovf& dstFov, const XrQuaternionf& dstRot, bool decodeSrgb) {
    return DrawRect(ctx, src, dst, dstFormat, 0, width, height, 0.0f, 0.0f, srcFov, srcRot, dstFov, dstRot, decodeSrgb);
}

bool Reprojector::DrawRect(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* dst, DXGI_FORMAT dstFormat,
                           UINT dstX, UINT width, UINT height, float srcU0, float srcUW, const XrFovf& srcFov, const XrQuaternionf& srcRot,
                           const XrFovf& dstFov, const XrQuaternionf& dstRot, bool decodeSrgb, bool unbind) {
    if (!ready_) return false;
    ID3D11ShaderResourceView* srv = GetSrv(src);
    ID3D11RenderTargetView* rtv = GetRtv(dst, dstFormat);
    if (!srv || !rtv) return false;

    // M = Rs^T * Rd: display-frame direction -> world -> render-frame direction.
    float rs[9], rd[9], m[9];
    QuatToMat(srcRot, rs);
    QuatToMat(dstRot, rd);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m[r * 3 + c] = rs[0 * 3 + r] * rd[0 * 3 + c] + rs[1 * 3 + r] * rd[1 * 3 + c] + rs[2 * 3 + r] * rd[2 * 3 + c];

    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return false;
    Consts* k = static_cast<Consts*>(map.pData);
    auto tans = [](const XrFovf& f, float o[4]) { o[0] = tanf(f.angleLeft); o[1] = tanf(f.angleRight); o[2] = tanf(f.angleUp); o[3] = tanf(f.angleDown); };
    tans(srcFov, k->srcFov);
    tans(dstFov, k->dstFov);
    k->m0[0] = m[0]; k->m0[1] = m[1]; k->m0[2] = m[2]; k->m0[3] = 0;
    k->m1[0] = m[3]; k->m1[1] = m[4]; k->m1[2] = m[5]; k->m1[3] = 0;
    k->m2[0] = m[6]; k->m2[1] = m[7]; k->m2[2] = m[8]; k->m2[3] = 0;
    k->opts[0] = decodeSrgb ? 1.0f : 0.0f; k->opts[1] = srcU0; k->opts[2] = srcUW; k->opts[3] = 0;
    if (srcUW > 0) { D3D11_TEXTURE2D_DESC sd; src->GetDesc(&sd); k->opts[3] = sd.Width ? 0.5f / float(sd.Width) : 0.0f; }
    ctx->Unmap(cb_, 0);

    D3D11_VIEWPORT vp{float(dstX), 0, float(width), float(height), 0, 1};
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rs_);
    float bf[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(bs_, bf, 0xffffffff);
    ctx->OMSetDepthStencilState(ds_, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_, nullptr, 0);
    ctx->PSSetShader(ps_, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &sampler_);
    ctx->PSSetConstantBuffers(0, 1, &cb_);
    ctx->Draw(3, 0);
    if (!unbind) return true;
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
    return true;
}

} // namespace so4vr
