#pragma once
namespace so4vr {
// Logs the current call stack once per tag (SO6.exe frames as RVAs, others as module+offset).
// Only active when DebugStacks = 1 in config.txt. `maxTimes` limits how often a tag is logged.
void LogStackOnce(const char* tag, int maxTimes = 1);
}
