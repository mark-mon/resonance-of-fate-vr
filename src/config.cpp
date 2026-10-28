#include <windows.h>
#include "config.h"
#include "log.h"
#include "game_offsets.h"
#include <cstdio>
#include <cstring>
#include <cctype>

namespace so4vr {

Config g_config;

static const char* kDefaultConfig =
    "# Resonance of Fate VR mod (rofvr) - configuration\n"
    "# Lines are key = value. Lines starting with # are comments.\n"
    "\n"
    "# Master switch. 0 = mod does nothing (controller passthrough only).\n"
    "Enabled = 1\n"
    "\n"
    "# Extra detail in rofvr.log (1 = on).\n"
    "VerboseLog = 0\n"
    "\n"
    "# Seconds between frame-statistics lines in rofvr.log (0 = off).\n"
    "StatsIntervalSec = 10\n"
    "\n"
    "# --- Headset output ---\n"
    "# 1 = show the game in the headset through OpenXR (SteamVR).\n"
    "VROutput = 1\n"
    "\n"
    "# Virtual screen placement, in metres, relative to your seated position when the game\n"
    "# connected to the headset. Use SteamVR's 'Reset seated position' to recentre.\n"
    "ScreenDistance = 2.5\n"
    "ScreenWidth = 3.2\n"
    "ScreenHeight = 0.0\n"
    "\n"
    "# How the game is shown in the headset:\n"
    "#   Flat   = a flat virtual screen in front of you\n"
    "#   Native = full per-eye stereo: the game renders each eye with its own camera\n"
    "# Menus / title use the flat screen. Hotkeys in game: F8 switches between Flat and this mode,\n"
    "# F9 recentres the view, F10 hides/shows the HUD panel.\n"
    "RenderMode = Native\n"
    "\n"
    "# Resolution the game renders each eye at (Native):\n"
    "#   Auto = 16:9 at the headset's recommended per-eye height (the game's HUD only works at 16:9)\n"
    "#   Game = the resolution chosen in the game's graphics options\n"
    "#   WxH  = a fixed size, e.g. 3200x1800 (keep it 16:9, other shapes break the minimap)\n"
    "RenderResolution = Auto\n"
    "# Multiplies the resolution above (0.5 - 2.0). Lower for more fps, higher for sharpness.\n"
    "RenderScale = 1.0\n"
    "\n"
    "# --- Image ---\n"
    "# The game's camera motion blur in VR. 0 = off (every head movement would be smeared).\n"
    "MotionBlur = 0\n"
    "# Sharpening of the headset image (0.0 = off, 1.0 = strong). Counters TAA softness.\n"
    "Sharpen = 0.0\n"
    "\n"
    "# --- Foveated rendering (NVIDIA RTX GPUs) ---\n"
    "# Fixed = shade the outer part of each eye's image at a reduced rate for more fps (centred on each lens).\n"
    "# Gaze  = the full-quality area follows your eyes (headsets with eye tracking; falls back to Fixed).\n"
    "# Off   = full rate everywhere.\n"
    "Foveation = Off\n"
    "# How coarse the outer areas are: Low, Medium or High.\n"
    "FoveationStrength = Medium\n"
    "# Radius of the full-quality centre and where the lowest rate starts (1.0 = half the image width).\n"
    "FoveationInner = 0.45\n"
    "FoveationOuter = 0.80\n"
    "\n"
    "# --- HUD (Native) ---\n"
    "# 1 = show the game's HUD on its own flat panel in front of you, 0 = put it into both eye images.\n"
    "HudLayer = 1\n"
    "# Panel distance and width in metres, and height offset from eye level (negative = lower).\n"
    "HudDistance = 1.5\n"
    "HudWidth = 1.5\n"
    "HudOffsetY = -0.05\n"
    "# World = fixed in front of you (F9 recentres it), Head = follows your head.\n"
    "HudAnchor = World\n"
    "# F10 hides/shows the HUD panel. The monitor always shows the HUD.\n"
    "\n"
    "# --- Settings menu (Insert, or hold L3+R3) ---\n"
    "# Panel distance and width in metres, and height offset from eye level.\n"
    "MenuDistance = 1.0\n"
    "MenuWidth = 0.75\n"
    "MenuOffsetY = -0.05\n"
    "# World = fixed in space where it opened (turn your head to read it), Head = follows your head.\n"
    "MenuAnchor = World\n"
    "\n"
    "# 1 = real-world scale for head movement. Larger values exaggerate head movement.\n"
    "WorldScale = 1.0\n"
    "\n"
    "# 0 = keep the game camera's downward tilt (recommended: Resonance of Fate's cameras look down on the\n"
    "#     characters from above, so you see them when you look straight ahead).\n"
    "# 1 = keep the horizon level (the tilt is removed; look down with your head to see the characters).\n"
    "HorizonLock = 0\n"
    "\n"
    "# Who corrects each frame for head movement between rendering and display:\n"
    "#   Mod     = the mod warps every frame to your current head direction (recommended)\n"
    "#   Runtime = leave it to SteamVR / the headset driver\n"
    "Reprojection = Mod\n"
    "\n"
    "# Head pose prediction, in headset frames. Lower if the view lags, raise if it overshoots.\n"
    "PredictionFrames = 4.0\n"
    "# 1 = measure how late frames reach the headset and predict the head pose for exactly that (ignores\n"
    "# PredictionFrames). 0 = use PredictionFrames.\n"
    "PredictionAuto = 1\n"
    "# Extra field of view rendered around each eye (0.15 = 15%), so turning your head never shows black\n"
    "# edges while the headset waits for the next game frame. 0 = exactly the lens field of view.\n"
    "FovOverscan = 0.15\n"
    "\n"
    "# --- Advanced / diagnostics ---\n"
    "# Seconds between camera readouts in rofvr.log (0 = off).\n"
    "CameraProbeIntervalSec = 2\n"
    "\n"
    "\n"
    "# If you see a one-frame flash in one eye, press F12 right after it (marks rofvr.log) and send the log.\n"
    "# 1 = both eyes use the object positions of the game's own pass (fixes one-frame white flashes).\n"
    "ReplayRawFromPass1 = 1\n";

static std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

static bool EqNoCase(const std::string& a, const char* b) {
    return _stricmp(a.c_str(), b) == 0;
}

const char* ModeName(int mode) {
    switch (mode) {
    case Config::Flat: return "Flat";
    case Config::Mono: return "Mono";
    case Config::AER: return "AER";
    case Config::Native: return "Native";
    default: return "?";
    }
}

void LoadConfig(const std::wstring& gameDir) {
    std::wstring path = gameDir + L"\\config.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) {
        LOGI("config.txt not found, writing defaults");
        FILE* w = nullptr;
        if (_wfopen_s(&w, path.c_str(), L"w") == 0 && w) { fputs(kDefaultConfig, w); fclose(w); }
        return;
    }
    char buf[512];
    int lineNo = 0;
    while (fgets(buf, sizeof(buf), f)) {
        ++lineNo;
        std::string line = Trim(buf);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) { LOGW("config.txt line %d ignored (no '=')", lineNo); continue; }
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        size_t hash = val.find('#');
        if (hash != std::string::npos) val = Trim(val.substr(0, hash));
        int iv = atoi(val.c_str());
        // booleans: 1/0, true/false, yes/no, on/off (the in-game menu writes true/false)
        bool bv = iv != 0 || EqNoCase(val, "true") || EqNoCase(val, "yes") || EqNoCase(val, "on");
        float fv = (float)atof(val.c_str());
        if      (EqNoCase(key, "Enabled"))          g_config.enabled = bv;
        else if (EqNoCase(key, "VerboseLog"))       g_config.verboseLog = bv;
        else if (EqNoCase(key, "StatsIntervalSec")) g_config.statsIntervalSec = iv < 0 ? 0 : iv;
        else if (EqNoCase(key, "VROutput"))         g_config.vrOutput = bv;
        else if (EqNoCase(key, "ScreenDistance"))   g_config.screenDistance = fv < 0.3f ? 0.3f : fv;
        else if (EqNoCase(key, "ScreenWidth"))      g_config.screenWidth = fv < 0.2f ? 0.2f : fv;
        else if (EqNoCase(key, "ScreenHeight"))     g_config.screenHeight = fv;
        else if (EqNoCase(key, "CameraProbeIntervalSec")) g_config.cameraProbeIntervalSec = iv < 0 ? 0 : iv;
        else if (EqNoCase(key, "DebugLensShiftX"))  g_config.debugLensShiftX = fv;
        else if (EqNoCase(key, "DebugLensShiftY"))  g_config.debugLensShiftY = fv;
        else if (EqNoCase(key, "RenderResolution")) {
            unsigned w = 0, h = 0;
            if (EqNoCase(val, "Auto")) g_config.renderResMode = Config::ResAuto;
            else if (EqNoCase(val, "Game")) g_config.renderResMode = Config::ResGame;
            else if (sscanf(val.c_str(), "%ux%u", &w, &h) == 2 && w >= 320 && h >= 240) { g_config.renderResMode = Config::ResExplicit; g_config.renderResW = w; g_config.renderResH = h; }
            else LOGW("config: RenderResolution '%s' not understood (Auto, Game or WxH)", val.c_str());
        }
        else if (EqNoCase(key, "HudLayer")) g_config.hudLayer = bv;
        else if (EqNoCase(key, "HudOnDesktop")) g_config.hudOnDesktop = bv;
        else if (EqNoCase(key, "ReticleInScene")) g_config.reticleInScene = bv;
        else if (EqNoCase(key, "ReticleDistance")) g_config.reticleDistance = fv < 0.5f ? 0.5f : (fv > 200.0f ? 200.0f : fv);
        else if (EqNoCase(key, "HudDistance")) g_config.hudDistance = fv < 0.2f ? 0.2f : (fv > 20.0f ? 20.0f : fv);
        else if (EqNoCase(key, "HudWidth")) g_config.hudWidth = fv < 0.1f ? 0.1f : (fv > 20.0f ? 20.0f : fv);
        else if (EqNoCase(key, "HudOffsetY")) g_config.hudOffsetY = fv;
        else if (EqNoCase(key, "MenuDistance")) g_config.menu.distance = fv < 0.3f ? 0.3f : (fv > 5.0f ? 5.0f : fv);
        else if (EqNoCase(key, "HudAlphaFix")) g_config.hudAlphaFix = bv;
        else if (EqNoCase(key, "ReplayGpuSync")) g_config.replayGpuSync = bv;
        else if (EqNoCase(key, "PatchFullResOnly")) g_config.patchFullResOnly = bv;
        else if (EqNoCase(key, "QueryWait")) g_config.queryWait = bv;
        else if (EqNoCase(key, "PatchAligned")) g_config.patchAligned = bv;
        else if (EqNoCase(key, "MouseFix")) g_config.mouseFix = bv;
        else if (EqNoCase(key, "DebugMenuDump")) g_config.debugMenuDump = bv;
        else if (EqNoCase(key, "MenuOffsetY")) g_config.menu.offsetY = fv < -2.0f ? -2.0f : (fv > 2.0f ? 2.0f : fv);
        else if (EqNoCase(key, "MenuAnchor")) g_config.menu.anchor = EqNoCase(val, "Head") ? 1 : 0;
        else if (EqNoCase(key, "MenuWidth")) g_config.menu.width = fv < 0.2f ? 0.2f : (fv > 4.0f ? 4.0f : fv);
        else if (EqNoCase(key, "HudAnchor")) g_config.hudAnchor = EqNoCase(val, "Head") ? 1 : 0;
        else if (EqNoCase(key, "TAA")) g_config.taa = bv;
        else if (EqNoCase(key, "StereoHistoryAll")) g_config.stereoHistoryAll = bv;
        else if (EqNoCase(key, "StereoHistoryUav")) g_config.stereoHistoryUav = bv;
        else if (EqNoCase(key, "DebugHistLog")) g_config.debugHistLog = iv;
        else if (EqNoCase(key, "DebugDumpKey")) g_config.debugDumpKey = bv;
        else if (EqNoCase(key, "DebugFakePad")) { g_config.debugFakePad = bv; g_config.debugFakePadAlways = atoi(val.c_str()) >= 2; }
        else if (EqNoCase(key, "StereoHistoryMax")) g_config.stereoHistoryMax = iv < 0 ? 0 : (iv > 64 ? 64 : iv);
        else if (EqNoCase(key, "Foveation")) g_config.foveation = (EqNoCase(val, "Gaze") || EqNoCase(val, "Eye") || iv == 2) ? 2 : (EqNoCase(val, "Fixed") || EqNoCase(val, "true") || EqNoCase(val, "on") || iv == 1) ? 1 : 0;
        else if (EqNoCase(key, "DebugFakeGaze")) g_config.debugFakeGaze = bv;
        else if (EqNoCase(key, "FoveationStrength")) g_config.foveationStrength = EqNoCase(val, "Low") ? 0 : EqNoCase(val, "High") ? 2 : 1;
        else if (EqNoCase(key, "FoveationInner")) g_config.foveationInner = fv < 0.05f ? 0.05f : (fv > 2.0f ? 2.0f : fv);
        else if (EqNoCase(key, "FoveationOuter")) g_config.foveationOuter = fv < 0.05f ? 0.05f : (fv > 3.0f ? 3.0f : fv);
        else if (EqNoCase(key, "TaaJitter")) g_config.taaJitter = fv < 0.0f ? 0.0f : (fv > 1.0f ? 1.0f : fv);
        else if (EqNoCase(key, "Sharpen")) g_config.sharpen = fv < 0.0f ? 0.0f : (fv > 1.0f ? 1.0f : fv);
        else if (EqNoCase(key, "RenderScale")) g_config.renderScale = fv < 0.25f ? 0.25f : (fv > 3.0f ? 3.0f : fv);
        else if (EqNoCase(key, "RenderMode")) {
            if      (EqNoCase(val, "Flat"))   g_config.renderMode = Config::Flat;
            else if (EqNoCase(val, "Mono"))   g_config.renderMode = Config::Mono;
            else if (EqNoCase(val, "AER"))    g_config.renderMode = Config::AER;
            else if (EqNoCase(val, "Native")) g_config.renderMode = Config::Native;
            else if (iv >= 0 && iv <= 3 && !val.empty() && isdigit((unsigned char)val[0])) g_config.renderMode = iv;
            else LOGW("config.txt line %d: RenderMode '%s' not recognised (use Flat or Native)", lineNo, val.c_str());
        }
        else if (EqNoCase(key, "RestoreCameraAfterPrepare")) g_config.restoreCameraAfterPrepare = bv;
        else if (EqNoCase(key, "WorldScale"))       g_config.worldScale = fv < 0.05f ? 0.05f : fv;
        else if (EqNoCase(key, "HorizonLock"))      g_config.horizonLock = bv;
        else if (EqNoCase(key, "StereoBothReplay")) g_config.stereoBothReplay = bv;
        else if (EqNoCase(key, "PatchPrevCamera"))  g_config.patchPrevCamera = bv;
        else if (EqNoCase(key, "MotionBlur"))       g_config.motionBlur = bv;
        else if (EqNoCase(key, "PredictionAuto"))   g_config.predictionAuto = bv;
        else if (EqNoCase(key, "FovOverscan"))      g_config.fovOverscan = fv < 0 ? 0 : (fv > 0.5f ? 0.5f : fv);
        else if (EqNoCase(key, "PredictionFrames")) g_config.predictionFrames = fv < 0 ? 0 : fv;
        else if (EqNoCase(key, "FrameMatchOffset")) g_config.frameMatchOffset = iv < -2 ? -2 : (iv > 2 ? 2 : iv);
        else if (EqNoCase(key, "Reprojection")) {
            if (EqNoCase(val, "Runtime") || val == "0") g_config.reprojection = 0;
            else if (EqNoCase(val, "Mod") || val == "1") g_config.reprojection = 1;
            else LOGW("config.txt line %d: Reprojection '%s' not recognised (use Runtime or Mod)", lineNo, val.c_str());
        }
        else if (EqNoCase(key, "DebugLayerYawDeg")) g_config.debugLayerYawDeg = fv;
        else if (EqNoCase(key, "DebugFakeHeadYawDeg")) g_config.debugFakeHeadYawDeg = fv;
        else if (EqNoCase(key, "DebugFakeHeadHz"))  g_config.debugFakeHeadHz = fv;
        else if (EqNoCase(key, "DebugRenderViewYawDeg")) g_config.debugRenderViewYawDeg = fv;
        else if (EqNoCase(key, "DebugReplay"))      g_config.debugReplay = iv;
        else if (EqNoCase(key, "ReplayUavMaxBytes")) g_config.replayUavMaxBytes = unsigned(iv < 0 ? 0 : iv);
        else if (EqNoCase(key, "ReplayUavMaxTexels")) g_config.replayUavMaxTexels = unsigned(iv < 0 ? 0 : iv);
        else if (EqNoCase(key, "ReplayReuploadBuffers")) g_config.replayReuploadBuffers = bv;
        else if (EqNoCase(key, "DebugDumpEyesAt")) g_config.debugDumpEyesAt = iv;
        else if (EqNoCase(key, "DebugDesktopEye")) g_config.debugDesktopEye = iv;
        else if (EqNoCase(key, "StereoHistoryFix")) g_config.stereoHistoryFix = bv;
        else if (EqNoCase(key, "DebugLatTrack")) g_config.debugLatTrack = bv;
        else if (EqNoCase(key, "ReplayRestoreMask")) g_config.replayRestoreMask = iv;
        else if (EqNoCase(key, "ReplayRestoreState")) g_config.replayRestoreState = bv;
        else if (EqNoCase(key, "DebugReplayViewSetup")) g_config.debugReplayViewSetup = bv;
        else if (EqNoCase(key, "DebugReplayYawDeg")) g_config.debugReplayYawDeg = fv;
        else if (EqNoCase(key, "RenderDocPath")) {
            wchar_t w[MAX_PATH]; MultiByteToWideChar(CP_UTF8, 0, val.c_str(), -1, w, MAX_PATH); g_config.renderDocPath = w;
        }
        else if (EqNoCase(key, "RenderDocCaptureFrame")) g_config.renderDocCaptureFrame = iv;
        else if (EqNoCase(key, "RenderDocCaptureCount")) g_config.renderDocCaptureCount = iv;
        else if (EqNoCase(key, "DebugStacks"))      g_config.debugStacks = bv;
        else if (EqNoCase(key, "LensShiftYSign"))   g_config.lensShiftYSign = fv < 0 ? -1.0f : 1.0f;
        else if (EqNoCase(key, "D3DMultithreadProtect")) g_config.d3dMultithreadProtect = bv;
        else if (EqNoCase(key, "CameraHook"))       g_config.cameraHook = iv;
        else if (EqNoCase(key, "ExactUploadView")) g_config.exactUploadView = bv;
        else if (EqNoCase(key, "ReplayViewFix")) g_config.replayViewFix = bv;
        else if (EqNoCase(key, "ReplayDupFix")) g_config.replayDupFix = bv;
        else if (EqNoCase(key, "ReplayCameraGate")) g_config.replayCameraGate = bv;
        else if (EqNoCase(key, "ReplayPacketSnapshot")) g_config.replayPacketSnapshot = bv;
        else if (EqNoCase(key, "ReplayMapShadow")) g_config.replayMapShadow = bv;
        else if (EqNoCase(key, "DebugNoDelta")) g_config.debugNoDelta = bv;
        else if (EqNoCase(key, "DebugSuspendGame")) g_config.debugSuspendGame = bv;
        else if (EqNoCase(key, "ReplayRawFromPass1")) g_config.replayRawFromPass1 = bv;
        else if (EqNoCase(key, "ReplayPass1MaxRows")) g_config.replayPass1MaxRows = iv;
        else if (EqNoCase(key, "ReplayPass1Nearest")) g_config.replayPass1Nearest = bv;
        else if (EqNoCase(key, "DeltaRelTol")) g_config.deltaRelTol = fv;
        else if (EqNoCase(key, "ReplayCrossEyeFix")) g_config.replayCrossEyeFix = bv;
        else if (EqNoCase(key, "DeltaSkipOrtho")) g_config.deltaSkipOrtho = bv;
        else if (EqNoCase(key, "DebugGlitchDetect")) g_config.debugGlitchDetect = bv;
        else if (EqNoCase(key, "DebugFrameScan")) g_config.debugFrameScan = iv;
        else if (EqNoCase(key, "DebugDumpSeq")) g_config.debugDumpSeq = iv;
        else if (EqNoCase(key, "HtNewestMatch")) g_config.htNewestMatch = bv;
        else if (EqNoCase(key, "DebugPresentSync")) g_config.debugPresentSync = atoi(val.c_str());
        else if (EqNoCase(key, "DebugSwapSize")) { unsigned w = 0, h = 0; if (sscanf(val.c_str(), "%ux%u", &w, &h) == 2) { g_config.debugSwapW = w; g_config.debugSwapH = h; } else g_config.debugSwapW = g_config.debugSwapH = 0; }
        else if (EqNoCase(key, "DebugRenderAll"))   g_config.debugRenderAll = bv;
        else if (EqNoCase(key, "StereoDiag"))       g_config.stereoDiag = iv;
        else LOGW("config.txt line %d: unknown key '%s'", lineNo, key.c_str());
    }
    fclose(f);
    // Mono still works when set by hand in config.txt (undocumented: not release quality, hidden from the menu).
    if (g_config.renderMode == Config::AER)
        LOGW("RenderMode AER is not implemented yet; running as Mono");
    if (g_config.renderMode == Config::AER) { LOGW("RenderMode AER is not supported; using Native"); g_config.renderMode = Config::Native; }
    g_config.activeMode = g_config.renderMode;
    LOGI("config: Enabled=%d VerboseLog=%d StatsIntervalSec=%d VROutput=%d Screen dist=%.2f width=%.2f height=%.2f MTProtect=%d CamProbe=%ds LensShift=(%.2f,%.2f) RenderMode=%s RestoreCam=%d WorldScale=%.2f HorizonLock=%d Prediction=%.2f ShiftYSign=%.0f FrameMatchOffset=%d Reprojection=%s DebugLayerYaw=%.1f FakeHead=%.1fdeg@%.2fHz",
         g_config.enabled, g_config.verboseLog, g_config.statsIntervalSec, g_config.vrOutput,
         g_config.screenDistance, g_config.screenWidth, g_config.screenHeight, g_config.d3dMultithreadProtect,
         g_config.cameraProbeIntervalSec, g_config.debugLensShiftX, g_config.debugLensShiftY,
         ModeName(g_config.renderMode), g_config.restoreCameraAfterPrepare, g_config.worldScale, g_config.horizonLock, g_config.predictionFrames, g_config.lensShiftYSign, g_config.frameMatchOffset,
         g_config.reprojection ? "Mod" : "Runtime", g_config.debugLayerYawDeg,
         g_config.debugFakeHeadYawDeg, g_config.debugFakeHeadHz);
}

} // namespace so4vr
