#pragma once
#include <cstdint>

// Addresses inside StarOceanTheLastHope.exe (Steam 4K & Full HD Remaster, PE timestamp 1517901502),
// as RVAs from the module base. Source: memory dump analysis (see FINDINGS.md).
namespace so4vr::off {

// ---- SO4 (verified at runtime, test 004) ----
constexpr uintptr_t kSo4Renderer        = 0xF76C70;  // CRenderer*
constexpr uintptr_t kSo4ImmContext      = 0xF77310;  // ID3D11DeviceContext* (immediate)
constexpr uintptr_t kSo4RendDevice      = 0x24A9B8;  // renderer + : ID3D11Device*
constexpr uintptr_t kSo4RendFactory     = 0x24A9C0;  // renderer + : IDXGIFactory*
constexpr uintptr_t kSo4RendSwapChain   = 0x24A9C8;  // renderer + : IDXGISwapChain*
constexpr uintptr_t kSo4FnCreateDevice  = 0x532C0;   // bool CreateDevice(CRenderer*)
constexpr uintptr_t kSo4PresentCall     = 0x5429A;   // call [rax+0x40] = swapchain->Present(sync, 0)

// Camera / view not found yet: head-tracked and stereo modes are disabled until these exist.
constexpr bool kCameraKnown = false;

// ---- Resonance of Fate (rofvr): which game-specific SO4 modules have their addresses mapped to ROF ----
constexpr bool kRofCameraMapped = true;    // UpdateCamera 0x2746F0, view inverse 0x1B9AF0 / 0xD50F0, flags +0x7AA
constexpr bool kRofStereoMapped = true;     // processor 0x2A2FA0, job site 0x2A30C3, frame end 0x28DE80, combiner vt 0xA26790
// Per-view constant block (renderer +0x26E930 CPU copy, uploaded by 0x295D80 from 0x28FC70; SO4: 2192 bytes)
constexpr unsigned kViewBlockBytes = 0x8A0;          // 2208
constexpr unsigned kViewPosA = 0xC0, kViewPosB = 0x340;   // camera view rows (SO4 layout; ROF main camera blocks: +0x340, test 020)
constexpr unsigned kViewPosMain = 0x340;                  // ROF: where the main camera view sits in its per-view blocks

// ---- Divine Force (SO6) offsets below are NOT valid for SO4. They are kept only so the shared
// modules compile; nothing reads them while kCameraKnown is false. ----

// ---- globals ----
constexpr uintptr_t kSceneRoot          = 0x248F750; // Aska scene/world root pointer
constexpr uintptr_t kSceneRoot_MainCam  = 0x590;     // root + 0x590 -> main Aska camera
constexpr uintptr_t kRenderer           = 0x248FA88; // Renderer*
constexpr uintptr_t kCbPrePrepare       = 0x257DE40; // void(*)(void*) called before Renderer::Prepare (null by default)
constexpr uintptr_t kCbPostPrepare      = 0x257DE48; // void(*)(void*) tail-called after Renderer::Prepare (null by default)
constexpr uintptr_t kViewInfoRing       = 0;        // SO6 0x253CB38; unknown for SO4 // *ring + 0x10 + idx*0x530 = ViewInfo snapshot
constexpr uintptr_t kViewInfoWriteIdx   = 0x253D3F8; // int
constexpr uintptr_t kViewInfoReadIdx    = 0x253D400; // int
constexpr uintptr_t kViewInfoStride     = 0x530;

// ---- functions ----
constexpr uintptr_t kFnUpdateCamera     = 0x18130C;  // void UpdateCamera(AskaCamera*)
constexpr uintptr_t kFnRendererPrepare  = 0x165888;
constexpr uintptr_t kVtAskaCamera       = 0x1536B68; // vtable of the Aska camera object
constexpr uintptr_t kFnAdvanceRenderView = 0x29D5E8; // void(): render side moves to the next ViewInfo slot
                                                     // (counter 0x253D410++, read idx = counter & 1)

// ---- Aska camera object (0x8C0 bytes) ----
constexpr uintptr_t kCamWorld           = 0x30;   // float4x4 row-major, translation in [3],[7],[11]
constexpr uintptr_t kCamFlags110        = 0x110;  // bit0 world dirty, bit2 view valid
constexpr uintptr_t kCamView            = 0x120;  // float4x4 = inverse(world)
constexpr uintptr_t kCamProj            = 0x420;  // float4x4 with TAA jitter
constexpr uintptr_t kCamViewProj        = 0x4E0;  // float4x4
constexpr uintptr_t kCamPos             = 0x560;  // float4
constexpr uintptr_t kCamProjNoJitter    = 0x5D0;  // float4x4
constexpr uintptr_t kCamLensShift       = 0x650;  // float2, film-back units
constexpr uintptr_t kCamJitter          = 0x660;  // float2
constexpr uintptr_t kCamAspect6F4       = 0x6F4;  // float
constexpr uintptr_t kCamFocal70C        = 0x70C;  // float (= m00)
constexpr uintptr_t kCamNear            = 0x710;  // float
constexpr uintptr_t kCamFar             = 0x714;  // float
constexpr uintptr_t kCamLens            = 0x720;  // LensParams*; +0x10 = full FOV (radians)
constexpr uintptr_t kCamFlags7C2        = 0x7C2;  // uint8: bit0 ortho, bit1 projection dirty, bit5 view dirty
constexpr uintptr_t kCamDepthNear       = 0x7D8;  // float (1.0 = reversed-Z)
constexpr uintptr_t kCamDepthFar        = 0x7DC;  // float
constexpr uintptr_t kCamFilmAspect      = 0x7E0;  // float
constexpr uintptr_t kCamFilmW           = 0x7E4;  // float
constexpr uintptr_t kCamFilmH           = 0x7E8;  // float
constexpr uintptr_t kLensFov            = 0x10;   // float radians

// ---- ViewInfo snapshot (0x530 bytes) ----
constexpr uintptr_t kViView             = 0x000;
constexpr uintptr_t kViProj             = 0x080;
constexpr uintptr_t kViViewProj         = 0x100;
constexpr uintptr_t kViCamPos           = 0x4D0;
constexpr uintptr_t kViFov              = 0x510;
constexpr uintptr_t kViAspect           = 0x514;
constexpr uintptr_t kViNear             = 0x518;
constexpr uintptr_t kViFar              = 0x51C;

} // namespace so4vr::off
