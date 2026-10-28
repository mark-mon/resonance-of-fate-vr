#pragma once
#include <string>
namespace so4vr {
// Optional RenderDoc in-application capture (diagnostics). RenderDocPath in config.txt enables it.
void RdocInit(const std::wstring& gameDir);
// Render thread, each Present: triggers one capture once enough gameplay frames have run.
void RdocOnPresent(unsigned long long gameplayFrames);
}
