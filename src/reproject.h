#pragma once
#include <d3d11.h>
#include <openxr/openxr.h>

namespace so4vr {

// Rotational reprojection ("timewarp") done by the mod on the XR thread's own device.
// Warps an image rendered from orientation `srcRot` with FOV `srcFov` so it is correct for
// orientation `dstRot` / FOV `dstFov`. Orientations are OpenXR quaternions in the same space.
class Reprojector {
public:
    bool Init(ID3D11Device* dev);
    // `src` must have D3D11_BIND_SHADER_RESOURCE; `dst` must allow a render target view in `dstFormat`.
    // `decodeSrgb`: source holds sRGB-encoded values in a UNORM format and `dst` is an _SRGB target.
    bool Draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* dst, DXGI_FORMAT dstFormat,
              UINT width, UINT height, const XrFovf& srcFov, const XrQuaternionf& srcRot,
              const XrFovf& dstFov, const XrQuaternionf& dstRot, bool decodeSrgb);
    // Same, for one eye of a side-by-side image: reads source columns [srcU0, srcU0 + srcUW) (0..1) and writes
    // `width` x `height` pixels at column `dstX` of `dst`.
    bool DrawRect(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* dst, DXGI_FORMAT dstFormat,
                  UINT dstX, UINT width, UINT height, float srcU0, float srcUW, const XrFovf& srcFov, const XrQuaternionf& srcRot,
                  const XrFovf& dstFov, const XrQuaternionf& dstRot, bool decodeSrgb, bool unbind = true);
    void ReleaseViews();
    bool Ready() const { return ready_; }

private:
    ID3D11Device* dev_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11Buffer* cb_ = nullptr;
    ID3D11RasterizerState* rs_ = nullptr;
    ID3D11BlendState* bs_ = nullptr;
    ID3D11DepthStencilState* ds_ = nullptr;
    struct SrvEntry { ID3D11Texture2D* tex; ID3D11ShaderResourceView* srv; };
    struct RtvEntry { ID3D11Texture2D* tex; ID3D11RenderTargetView* rtv; };
    SrvEntry srvs_[8] = {};
    RtvEntry rtvs_[8] = {};
    bool ready_ = false;

    ID3D11ShaderResourceView* GetSrv(ID3D11Texture2D* t);
    ID3D11RenderTargetView* GetRtv(ID3D11Texture2D* t, DXGI_FORMAT fmt);
};

} // namespace so4vr
