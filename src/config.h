#pragma once
#include "vrkit_menu_placement.h"
#include <string>

namespace so4vr {

// Settings loaded from config.txt in the game folder. Unknown keys are ignored;
// missing keys keep these defaults. A default config.txt is written if none exists.
struct Config {
    bool enabled = true;          // master switch: false = pure XInput passthrough, no hooks
    bool verboseLog = false;      // extra per-event logging
    int  statsIntervalSec = 10;   // how often to log frame statistics (0 = off)

    // Stage 2: virtual screen in the headset
    bool  vrOutput = true;        // send the game image to the headset via OpenXR
    float screenDistance = 2.5f;  // metres in front of the seated position
    float screenWidth = 3.2f;     // metres; height follows the game's aspect ratio
    float screenHeight = 0.0f;    // metres above (+) or below (-) eye level at session start
    // Stage 3a: camera probe
    int   cameraProbeIntervalSec = 2; // seconds between camera log lines (0 = off)
    float debugLensShiftX = 0.0f;     // diagnostic: shift the picture (fraction of half width); 0 = off
    float debugLensShiftY = 0.0f;

    // Stage 3b: head tracking
    // Render modes. Flat = virtual screen; Mono = head-tracked single view (both eyes same image);
    // AER = alternate-eye rendering; Native = true per-eye stereo. AER/Native not implemented yet.
    enum Mode { Flat = 0, Mono = 1, AER = 2, Native = 3 };
    int   renderMode = Native;   // as configured in config.txt
    int   activeMode = Flat;     // what is running now (F8 toggles between Flat and renderMode)
    enum ResMode { ResAuto = 0, ResGame = 1, ResExplicit = 2 };
    int   renderResMode = ResAuto;
    unsigned renderResW = 0, renderResH = 0;
    float renderScale = 1.0f;
    bool  taa = true;             // the game's temporal anti-aliasing (its own engine switch)
    float taaJitter = 1.0f;       // TAA camera jitter amount, 0..1 (lower = less shimmer-blur, more aliasing)
    float sharpen = 0.0f;
    int   foveation = 0;            // 0 = off, 1 = fixed, 2 = follows the eye-tracking gaze (NVIDIA VRS)
    bool  debugFakeGaze = false;    // diagnostic: circling fake gaze for tests without an eye tracker
    int   foveationStrength = 1;    // 0 Low, 1 Medium, 2 High
    float foveationInner = 0.45f;   // full-rate radius (1.0 = half the image width)
    float foveationOuter = 0.80f;   // beyond this radius: lowest rate         // sharpening of the headset image, 0..1 (counters TAA softness)
    bool  hudLayer = true;        // head-tracked modes: HUD on its own fixed layer instead of in the eye images
    bool  hudOnDesktop = true;    // draw the HUD back over the game image on the monitor
    float hudDistance = 1.5f;     // metres
    float hudWidth = 1.5f;        // metres (height follows the HUD's aspect)
    float hudOffsetY = -0.05f;    // metres, up from eye level
    int   hudAnchor = 0;
    bool  reticleInScene = true;  // draw the aim crosshair in the 3D image instead of on the HUD panel
    float reticleDistance = 10.0f; // metres along the aim direction
    vrkit::menu::Placement menu;  // settings menu panel: distance, width, height offset, anchor (World/Head)
    bool  debugMenuDump = false;
    bool  mouseFix = true;
    bool  queryWait = false;
    bool  patchAligned = true;          // stereo: scan constant uploads for camera matrices only at 16-byte positions
    bool  patchFullResOnly = true;
    bool  replayGpuSync = false;
    bool  hudAlphaFix = false;    // old method: change the HUD's alpha blending (breaks the minimap); the matte is used instead // stereo camera shift only in passes near eye resolution      // diagnostic: wait for GPU query results instead of returning 'not ready'        // 1:1 cursor mapping at VR render sizes (stops the camera drifting)          // 0 = World (fixed in front of you, recentred with F9), 1 = Head (follows your head)
    bool  restoreCameraAfterPrepare = false; // 1 = put the game camera back right after frame preparation
    float worldScale = 1.0f;      // 1 = real scale; >1 makes head movement move the camera further
    bool  horizonLock = false;     // remove the game camera's pitch/roll so the horizon stays level
    bool  stereoBothReplay = true; // native stereo: render both eyes as replays (pass 1 = centre camera, unpatched)
    bool  patchPrevCamera = false; // native stereo: also replace exact copies of the previous head record's matrices
    bool  motionBlur = false;     // the game's camera motion blur (head-tracked modes); off = skip its render job
    bool  predictionAuto = true;  // measure how late frames are shown and predict the pose for exactly that
    float fovOverscan = 0.15f;    // render this much more field of view (tangent fraction) than the lenses show
    float predictionFrames = 4.0f;// how far ahead (in headset frames) to predict the head pose
    float lensShiftYSign = 1.0f;
    int   frameMatchOffset = 0;
    int   reprojection = 1;           // 0 = runtime (SteamVR) reprojects; 1 = the mod warps each frame itself
    float debugLayerYawDeg = 0.0f;
    float debugFakeHeadYawDeg = 0.0f; // diagnostic: fake head turning (amplitude in degrees) for null-headset tests
    float debugFakeHeadHz = 0.25f;
    float debugRenderViewYawDeg = 0.0f; // diagnostic: rotate the render-side view by this many degrees
    int   debugReplay = 0;              // experiment: execute each frame a second time (1 = whole command range, 2 = render jobs only)
    float debugReplayYawDeg = 0.0f;
    bool  debugReplayViewSetup = true;
    bool  replayRestoreState = true;
    int   debugDesktopEye = 1;           // native stereo: which eye the monitor shows (0 = left, 1 = right)
    bool  stereoHistoryFix = true;       // per-eye copies of the scene/TAA history read across frames
    bool  stereoHistoryAll = false;      // ... and of every other history texture (found automatically)
    bool  stereoHistoryUav = true;       // also detect history updated in place through UAVs
    int   stereoHistoryMax = 16;
    int   debugHistLog = 0;
    bool  debugDumpKey = false;
    bool  debugFakePadAlways = false;    // DebugFakePad = 2: fake pad active from the start (no F5 needed)
    bool  debugFakePad = false;          // diagnostic: keyboard-driven pad 0 (F5 = RB)          // diagnostic: F11 writes both eye images              // diagnostic: list the textures pass 1 uses after this many tracked frames         // safety limit on how many textures get per-eye copies
    bool  debugLatTrack = false;
    int   replayRestoreMask = 14;    // 1 stateful passes (breaks TAA history), 2 globals, 4 small UAVs, 8 TAA depth state
    int   debugDumpEyesAt = 0;           // native stereo: write both eye images as BMP at this gameplay frame (0 = off)
    bool  replayReuploadBuffers = true;  // re-issue the frame's structured buffer uploads before the second-eye pass
    unsigned replayUavMaxBytes = 256;    // compute buffers up to this size are restored before the second-eye pass
    unsigned replayUavMaxTexels = 64;   // compute textures up to this many texels likewise     // restore stateful post-process objects before the second-eye pass   // experiment: rebuild the view constants before the replay     // experiment: rotate the view for the second execution
    std::wstring renderDocPath;         // diagnostic: folder with renderdoc.dll (empty = off)
    int   renderDocCaptureFrame = 600;  // capture after this many gameplay frames
    int   renderDocCaptureCount = 1;
    bool  debugStacks = false;        // diagnostic: log call stacks of key render events (a few times each)    // diagnostic: fake head turning frequency    // diagnostic: rotate the pose reported to SteamVR by this many degrees   // pair each image with the pose of frame (matched + offset); diagnostic  // flip (-1) if the picture is vertically off-centre in the headset

    bool  d3dMultithreadProtect = false;
    int   cameraHook = 0;
    unsigned debugSwapW = 0, debugSwapH = 0;
    int   debugPresentSync = -1;
    bool  htNewestMatch = false;
    int   debugDumpSeq = 0;
    int   debugFrameScan = 0;
    bool  debugGlitchDetect = false;  // diagnostic: detect one-frame glitches (XR thread) and log the frame's details
    bool  deltaSkipOrtho = true;
    bool  replayCrossEyeFix = false;    // stereo: raw matrices already patched for the other eye are converted to this eye
    float deltaRelTol = 0.0f;          // stereo: relative tolerance of the VP*W test (0 = old fixed 1e-3)
    bool  replayRawFromPass1 = true;
    bool  replayPass1Nearest = true;   // ... else the object matrices of the clearly closest pass-1 block of the job
    int   replayPass1MaxRows = 0;      // ... whole pass-1 block when at most this many 16-byte rows differ (closest, unambiguous)   // stereo replays: object matrices of per-view blocks from pass 1 (RoF one-frame white surfaces)
    bool  debugSuspendGame = false;    // diagnostic: suspend the game thread while the eye replays run
    bool  debugNoDelta = false;        // diagnostic: no affine-delta patching of matrices (exact copies only)
    bool  replayMapShadow = false;     // RoF: Map(WRITE/NO_OVERWRITE) of a constant buffer gets the game's raw contents back, not our per-eye patch
    bool  replayPacketSnapshot = false; // RoF: replays draw pass 1's copies of the jobs' draw packets (game thread reuses that memory; one-frame white surfaces)
    bool  replayCameraGate = false;    // RoF: hold the game thread's camera updates while the two eye replays run (one-frame white surfaces, tests 031-034)
    bool  replayDupFix = false;       // RoF: OFF - the main per-view block is legitimately re-uploaded many times per job with different contents; the SO4 fix replaced them (left-eye hair outline, test 029)
    // (SO4 notes) // stereo: a per-view block re-uploaded within one replay job keeps the job's first copy      // stereo: never apply the camera delta to orthographic clip matrices (shadow cascades)         // diagnostic: thumbnails of every stereo frame for N seconds (so4vr_scan_NN.raw)           // diagnostic: dump N quarter-size left-eye images per replay-view state
    bool  replayViewFix = true;       // stereo: per-view blocks the replay builds from a newer head record get this frame's eye matrices
    bool  exactUploadView = true;     // stereo: patch against the exact uploaded view instead of the matched record's      // diagnostic: match a frame to the newest record within tolerance (0.3.3 behaviour)      // experiment: override the game's Present sync interval (-1 = game's own)
    bool  debugRenderAll = false;   // experiment: render size also into the other screen config fields   // experiment: force the swapchain size (DebugSwapSize = WxH)
    int   stereoDiag = 0;         // SO4 development: install the stereo hooks (render jobs) - StereoDiag = 1            // SO4 experiment: hook UpdateCamera to find the main camera // D3D11 multithread protection; off: it reroutes the game's calls past our hooks
};

extern Config g_config;

// True when the current mode drives the game camera from the headset.
inline bool HeadTrackedMode() { return g_config.activeMode != Config::Flat; }
const char* ModeName(int mode);

void LoadConfig(const std::wstring& gameDir);

} // namespace so4vr
