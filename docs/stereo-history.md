# Stereo history, ghosting and stability fixes (v0.9.7, 2026-10-24)

## Right-eye TAA ghosting (speckled outlines on nearby characters)
- **Symptom:** in Native stereo, nearby characters showed dithered or speckled outlines, worst indoors. Entering a menu cleared it for a few seconds.
- **Cause:** the game draws near characters with a dithered fade and relies on TAA to resolve it. TAA (shader 1306, in 0x1B13E4) compares against a linear-depth pair, `obj+0x450[+0x673]` (current) and `[+0x673 ^ 1]` (previous). The linear depth is written by 0x150298 (shader 1312), but only when `(+0x66F & 0x11) == 1`; it then sets bit 0x10 ("done this frame"). In the replay that bit was still set, so the right eye skipped the linear-depth draw. Its TAA then used a stale texture (last written over 600 frames earlier) as current depth and the left eye's depth as previous. History was rejected and the dither showed through. Found with a RenderDoc capture (agent test 115, `rdc_fullres.py`): the replay's TAA draw had its pair inputs in swapped order, and no 1312 draw came before it.
- **Fix:** `ReplayRestoreMask` bit 8 (default mask is now 14). Before the replay, only `+0x673` and `+0x66F` of the stateful object are put back to their frame-start values. After the replay they are set to the values pass 1 left. Restoring the whole object, which is mask bit 1, still breaks TAA.
- **Result** (hallway, test PC), speckles near the characters:

  | | Left eye | Right eye |
  |---|---|---|
  | Before | 500–720 | 750–1430 |
  | After | ~390 | ~370 |

  With TAA off, both eyes show about 1,350.

## Per-eye history for every temporal texture (`temporal.cpp`, `StereoHistoryAll`)
- **Detection** happens during pass 1. A screen-sized texture that is read through a shader resource view before anything writes it in the frame, and that the game wrote within the last 60 frames, is history.
  - Hooks: PS/VS/CS shader resource binds (reads); render targets, depth targets, UAVs, Copy and Clear (writes).
  - UAV binds count as reads too (`StereoHistoryUav`).
- **Per-eye copies:** for each detected texture, a left copy is saved after pass 1 and a right copy is saved after the replay, and each eye gets its own copy loaded back.
  - This runs *after* the right eye is captured.
  - The final image and the composite pair are excluded. Swapping the final image made both eyes identical.
- **Found in the hallway:** the R16F linear-depth pair, plus an R8 2176² texture with mips (likely occlusion). `DebugHistLog = N` lists every texture pass 1 used.

## Crashes
1. **Video memory exhaustion (user crash, "Aska exit -1"):**
   - Single non-stereo frames between stereo frames made the shared frame textures switch between 9552×4112 and 3840×2160 every frame. That was 1,016 reallocations of about 470 MB each.
   - Fix: flat frames are held back for 0.75 s after a stereo frame, and shared-texture size changes are limited to one every 2 s.
2. **Portrait render size crash:**
   - SO6+0xDD36F (visibility code, Hybrid Research Vessel hallways) crashes when the render image is taller than wide.
   - Reproduced on the test PC's null headset (1856×2056). The same size turned landscape and Flat mode both work.
   - Many headsets recommend portrait per-eye sizes. Fix: `render_res.cpp` widens portrait sizes to square.

## HUD white flashes (minimap and portraits)
- Frames without a black/white matte (no replay: menus, transitions, skipped frames) fell back to the raw HUD, whose alpha makes the minimap white.
- Fix: in stereo, the last matte (up to 0.5 s old) is reused. Counters are in the HUD stats line.
