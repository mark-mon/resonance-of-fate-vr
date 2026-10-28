// In-headset settings menu (vrkit) for the SO6 VR mod: registers config.txt settings with the menu and
// hands the menu texture to the XR thread. Open with L3+R3 (hold) or Insert.
#include "temporal.h"
#include "menu_options.h"
#include "config.h"
#include "game_offsets.h"
#include "so4_headtrack.h"
#include "log.h"
#include "render_res.h"
#include "head_tracking.h"
#include "xr_runtime.h"
#include "vrkit_options.h"
#include "vrkit_menu.h"
#include <windows.h>
#include <string>
#include <cstdio>

#define SO4VR_MENU_TITLE "Resonance of Fate VR"

namespace so4vr {
void DumpEyeBmp(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const wchar_t* path, bool alpha);
namespace {

bool g_inited = false, g_failed = false;
std::wstring g_cfgPath;

// String mirrors for settings the config stores as numbers.
std::string g_fovStr, g_fovStrength, g_modeStr, g_resStr, g_reprojStr, g_anchorStr;

const char* ModeStr(int m) { return m == Config::Flat ? "Flat" : m == Config::Mono ? "Mono" : m == Config::AER ? "AER" : "Native"; }

void LogFn(const char* m) { LOGI("%s", m); }

// Frame rate for the status line (measured between Update calls = presented frames).
double g_fps = 0;
LARGE_INTEGER g_last{}, g_qpf{};
int g_frames = 0;

std::string Status() {
    uint32_t w = 0, h = 0;
    RenderResCurrent(w, h);
    char b[96];
    if (w && h) snprintf(b, sizeof(b), "%.0f fps | %ux%u per eye", g_fps, w, h);
    else snprintf(b, sizeof(b), "%.0f fps | flat screen", g_fps);
    return b;
}

void Register() {
    auto& o = vrkit::Options::Get();
    Config& c = g_config;

    // The menu offers Flat and Native only. Mono (not release quality) still works when set by hand in
    // config.txt; it is then listed too, so opening the menu does not silently change the mode.
    g_modeStr = ModeStr(c.renderMode == Config::AER ? Config::Mono : c.renderMode);
    const bool showMono = g_modeStr == "Mono";
    std::vector<std::string> modes = showMono ? std::vector<std::string>{"Flat", "Mono", "Native"} : std::vector<std::string>{"Flat", "Native"};
    auto& mode = o.AddChoice("View", "RenderMode", "Render mode", &g_modeStr, modes,
        "Flat: virtual screen. Native: full stereo, each eye rendered by the game with its own camera\n(F8 toggles, F9 recentres).",
        [] {
            int m = g_modeStr == "Flat" ? Config::Flat : g_modeStr == "Mono" ? Config::Mono : Config::Native;
            g_config.renderMode = m;
            g_config.activeMode = m;
            LOGI("menu: render mode -> %s", g_modeStr.c_str());
        });
    mode.choiceLabels = showMono ? std::vector<std::string>{"Flat screen", "Mono (head-tracked)", "Native stereo"}
                                    : std::vector<std::string>{"Flat screen", "Native stereo"};

    o.AddFloat("View", "WorldScale", "World scale", &c.worldScale, 0.05f, 10.0f, 0.05f, 2,
        "1 = real-world scale for head movement and eye separation.\nLarger values exaggerate depth and head movement.");
    o.AddBool("View", "HorizonLock", "Level horizon", &c.horizonLock,
        "Remove the game camera's downward tilt; you then look down with your head to see the characters.\nOff (default): the view keeps the game's camera angle.");
    g_reprojStr = c.reprojection == 1 ? "Mod" : "Runtime";
    auto& rp = o.AddChoice("View", "Reprojection", "Reprojection", &g_reprojStr, {"Mod", "Runtime"},
        "Who corrects each frame for head movement between rendering and display.\nMod is recommended.",
        [] { g_config.reprojection = g_reprojStr == "Mod" ? 1 : 0; });
    rp.choiceLabels = {"Mod (recommended)", "SteamVR / driver"};
    o.AddBool("View", "PredictionAuto", "Automatic pose prediction", &c.predictionAuto,
        "Measures how late frames reach the headset and predicts the head pose for exactly that.");
    o.AddFloat("View", "PredictionFrames", "Pose prediction (frames)", &c.predictionFrames, 0.0f, 5.0f, 0.25f, 2,
        "How far ahead the head pose is predicted, in headset frames.\nLower if the view lags, raise if it overshoots.").visible = [] { return !g_config.predictionAuto; };
    o.AddFloat("View", "FovOverscan", "Field of view overscan", &c.fovOverscan, 0.0f, 0.5f, 0.05f, 2,
        "Extra image rendered around each eye so turning your head shows no black edges.\n0.15 = 15% more.");
    o.AddAction("View", "Recentre view (F9)", [] { So4HtRequestRecenter(); }, "Makes the current head position and direction the centre.");

    // Resolution
    {
    std::vector<std::string> res = {"Auto", "Game"};
    if (c.renderResMode == Config::ResExplicit) {
        char b[32]; snprintf(b, sizeof(b), "%ux%u", c.renderResW, c.renderResH);
        res.push_back(b);
        g_resStr = b;
    } else g_resStr = c.renderResMode == Config::ResGame ? "Game" : "Auto";
    auto& rr = o.AddChoice("Display", "RenderResolution", "Render resolution", &g_resStr, res,
        "Auto: 16:9 at the headset's recommended per-eye height.\nGame: the game's own graphics resolution setting.",
        [] {
            Config& c = g_config;
            unsigned w = 0, h = 0;
            if (g_resStr == "Auto") c.renderResMode = Config::ResAuto;
            else if (g_resStr == "Game") c.renderResMode = Config::ResGame;
            else if (sscanf(g_resStr.c_str(), "%ux%u", &w, &h) == 2) { c.renderResMode = Config::ResExplicit; c.renderResW = w; c.renderResH = h; }
        });
    rr.choiceLabels = {"Auto (headset)", "Game setting"};
    if (res.size() > 2) rr.choiceLabels.push_back(res[2]);
    o.AddFloat("Display", "RenderScale", "Render scale", &c.renderScale, 0.5f, 2.0f, 0.05f, 2,
        "Multiplies the render resolution. Lower for more fps, higher for sharpness.\nApplied a moment after changing.");

    // Image
    if (off::kCameraKnown) o.AddBool("Image", "TAA", "Temporal anti-aliasing (TAA)", &c.taa,
        "The game's TAA. Off = sharpest image, but edges shimmer.\nApplies from the next frame.");
    if (off::kCameraKnown) o.AddFloat("Image", "TaaJitter", "TAA jitter", &c.taaJitter, 0.0f, 1.0f, 0.05f, 2,
        "How far TAA shifts the camera each frame (1 = game default).\nLower = less blur and smearing, edges less smooth.").visible = [] { return g_config.taa; };
    }
    o.AddBool("Image", "MotionBlur", "Motion blur", &c.motionBlur, "The game's camera motion blur in VR.\nOff recommended: it smears every head movement.");
    o.AddFloat("Image", "Sharpen", "Sharpening", &c.sharpen, 0.0f, 1.0f, 0.05f, 2,
        "Sharpens the headset image (0 = off).");

    // Foveated rendering
    g_fovStr = c.foveation == 2 ? "Gaze" : c.foveation == 1 ? "Fixed" : "Off";
    auto& fv = o.AddChoice("Foveation", "Foveation", "Foveated rendering", &g_fovStr, {"Off", "Fixed", "Gaze"},
        "Shade the outer part of each eye at a reduced rate for more fps (NVIDIA RTX).\nGaze: the sharp area follows your eyes (eye-tracking headsets).",
        [] { g_config.foveation = g_fovStr == "Gaze" ? 2 : g_fovStr == "Fixed" ? 1 : 0; });
    fv.choiceLabels = {"Off", "Fixed (centre of each lens)", "Eye-tracked"};
    static const char* kStr[] = {"Low", "Medium", "High"};
    g_fovStrength = kStr[c.foveationStrength < 0 || c.foveationStrength > 2 ? 1 : c.foveationStrength];
    o.AddChoice("Foveation", "FoveationStrength", "Strength", &g_fovStrength, {"Low", "Medium", "High"},
        "How coarse the outer areas are shaded. Higher = more fps, blurrier edges.",
        [] { g_config.foveationStrength = g_fovStrength == "Low" ? 0 : g_fovStrength == "High" ? 2 : 1; });
    o.AddFloat("Foveation", "FoveationInner", "Full-quality radius", &c.foveationInner, 0.1f, 1.5f, 0.05f, 2,
        "Radius of the full-quality centre (1.0 = half the image width).");
    o.AddFloat("Foveation", "FoveationOuter", "Lowest-rate radius", &c.foveationOuter, 0.1f, 2.0f, 0.05f, 2,
        "Beyond this radius the lowest rate is used; between the two a middle rate.");

    // HUD (Native stereo)
    {
    o.AddBool("HUD", "HudLayer", "HUD on its own panel", &c.hudLayer,
        "Native stereo: show the game's HUD on a flat panel instead of inside both eye images.\nApplies from the next frame.");
    o.AddFloat("HUD", "HudDistance", "Distance (m)", &c.hudDistance, 0.3f, 10.0f, 0.05f, 2, "How far away the HUD panel is.");
    o.AddFloat("HUD", "HudWidth", "Width (m)", &c.hudWidth, 0.2f, 10.0f, 0.05f, 2, "Width of the HUD panel; its height follows the HUD's shape.");
    o.AddFloat("HUD", "HudOffsetY", "Height offset (m)", &c.hudOffsetY, -2.0f, 2.0f, 0.02f, 2, "Up (+) or down (-) from eye level.");
    g_anchorStr = c.hudAnchor == 1 ? "Head" : "World";
    auto& an = o.AddChoice("HUD", "HudAnchor", "Anchor", &g_anchorStr, {"World", "Head"},
        "World: fixed in front of you (Recentre moves it). Head: follows your head.",
        [] { g_config.hudAnchor = g_anchorStr == "Head" ? 1 : 0; });
    an.choiceLabels = {"World (fixed)", "Head (follows)"};
    o.AddAction("HUD", "Show / hide HUD panel (F10)", [] { XrToggleHud(); });
    }

    // Menu panel itself
    vrkit::menu::AddPlacementOptions(o, c.menu);

    // Advanced
    o.AddInt("Advanced", "StatsIntervalSec", "Log statistics every (s)", &c.statsIntervalSec, 0, 120, 5,
        "Seconds between performance lines in rofvr.log (0 = off).");
}

} // namespace

void MenuOnPresent(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (g_failed || !dev || !ctx) return;
    if (!g_inited) {
        g_inited = true;
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        g_cfgPath = exe; g_cfgPath = g_cfgPath.substr(0, g_cfgPath.find_last_of(L'\\')) + L"\\config.txt";
        vrkit::SetLog(&LogFn);
        Register();
        vrkit::menu::Settings s;
        s.title = SO4VR_MENU_TITLE;
        s.configPath = g_cfgPath;
        s.statusLine = &Status;
        if (!vrkit::menu::Init(dev, ctx, s)) { g_failed = true; LOGE("settings menu: init failed"); return; }
        QueryPerformanceFrequency(&g_qpf); QueryPerformanceCounter(&g_last);
        LOGI("settings menu ready (hold L3+R3 or press Insert)");
    }
    ++g_frames;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double dt = double(now.QuadPart - g_last.QuadPart) / double(g_qpf.QuadPart);
    if (dt >= 0.5) { g_fps = g_frames / dt; g_frames = 0; g_last = now; }

    static int retry = 0;
    if (++retry % 300 == 1) vrkit::input::InstallHooks();   // the game may load XInput late
    bool open = vrkit::menu::Update();
    { static bool tagged = false; if (!tagged && vrkit::menu::Texture()) { tagged = true; TemporalMarkModTexture(vrkit::menu::Texture()); } }
    XrPublishMenu(open ? vrkit::menu::Texture() : nullptr);
    // Diagnostic: write the menu image once, a second after it first opens.
    static int openFrames = 0; static bool dumped = false;
    openFrames = open ? openFrames + 1 : 0;
    if (g_config.debugMenuDump && !dumped && openFrames == 60) {
        dumped = true;
        std::wstring dir = g_cfgPath.substr(0, g_cfgPath.find_last_of(L'\\'));
        DumpEyeBmp(ctx, vrkit::menu::Texture(), (dir + L"\\rofvr_menu_rgb.bmp").c_str(), false);
        DumpEyeBmp(ctx, vrkit::menu::Texture(), (dir + L"\\rofvr_menu_alpha.bmp").c_str(), true);
    }
}

} // namespace so4vr
