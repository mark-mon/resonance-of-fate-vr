#pragma once
#include <cstdint>

namespace so4vr {

bool RenderResInstall();                                  // hooks the game's graphics-resolution setting
void RenderResSetHmdRecommended(uint32_t w, uint32_t h);  // XR thread: runtime's recommended per-eye size
void RenderResTick();                                     // game thread, once per frame
void RenderResCurrent(uint32_t& w, uint32_t& h);          // the game's current render size
void RenderResRenderTick(uintptr_t queue);                // render thread: note the queue / current size (SO4)
void RenderResGameTick();                                 // game thread: apply the wanted size (SO4)

} // namespace so4vr
