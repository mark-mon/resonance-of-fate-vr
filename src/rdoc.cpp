#include <windows.h>
#include <string>
#include "rdoc.h"
#include "renderdoc_app.h"
#include "config.h"
#include "log.h"

namespace so4vr {
namespace {
RENDERDOC_API_1_6_0* g_api = nullptr;
int g_captured = 0;
}

void RdocInit(const std::wstring& gameDir) {
    if (g_config.renderDocPath.empty()) return;
    std::wstring dll = g_config.renderDocPath + L"\\renderdoc.dll";
    HMODULE m = LoadLibraryW(dll.c_str());
    if (!m) { LOGE("RenderDoc: could not load %ls (error %lu)", dll.c_str(), GetLastError()); return; }
    auto get = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(m, "RENDERDOC_GetAPI"));
    if (!get || get(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&g_api)) != 1 || !g_api) {
        LOGE("RenderDoc: API 1.6.0 not available"); g_api = nullptr; return;
    }
    char tmpl[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, (gameDir + L"\\rofvr_capture").c_str(), -1, tmpl, sizeof(tmpl), nullptr, nullptr);
    g_api->SetCaptureFilePathTemplate(tmpl);
    g_api->MaskOverlayBits(0, 0);
    LOGI("RenderDoc loaded; will capture after %d gameplay frames to %s_*.rdc", g_config.renderDocCaptureFrame, tmpl);
}

void RdocOnPresent(unsigned long long gameplayFrames) {
    if (!g_api || g_captured >= g_config.renderDocCaptureCount) return;
    if (gameplayFrames >= (unsigned long long)g_config.renderDocCaptureFrame + 60ull * g_captured) {
        g_api->TriggerCapture();
        ++g_captured;
        LOGI("RenderDoc: capture %d triggered (gameplay frame %llu)", g_captured, gameplayFrames);
    }
}
}
