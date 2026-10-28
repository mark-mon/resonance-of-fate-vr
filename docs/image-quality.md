- **2026-10-24 (v0.9.5, untested): TAA options.** Found by static analysis:
  - `Renderer (*(0x248FA88)) + 0xB41B` is the engine's own **TAA-enabled** byte. It starts at 1 (0x10C7EF) and is also set by the engine's property setter 0xDFAD80 (property 0x3C).
    - Renderer::Prepare (0x165949) compares it with the TAA object's flag and switches the object (0x51CE70) when they differ. With TAA off it skips the jitter sequence.
    - Scene shader setup (0x3C393F) picks its non-TAA variants from the same byte.
    - `TAA = false` writes 0 there from the pre-Prepare callback. The mod writes 1 back only if it was the one that cleared it.
  - **Jitter source:** 0x3A92C8 JitterNext(taaObj) returns a pointer to {x, y, 0, 1}. That's a Halton 2/3 sequence in pixels (±0.5). Its only caller is Prepare (0x165974), which copies the result into camera +0x660.
    - `TaaJitter` (0–1) hooks it and returns a scaled copy. That scales the projection jitter and the TAA's own un-jitter together.
  - **Sharpening:** `Sharpen` (0–1) runs a CAS-style contrast-adaptive sharpen on the headset images (both eyes, or the Mono frame) before they are handed to the XR thread. The game's frame is not touched.
  - Still to do: a "mild TAA" with a tunable history weight would need the TAA resolve shader identified (PostPrs/AA/TAA, PostPrs/Temporal/*) and replaced, as in GranblueVR. That needs a capture on the test PC.

## Test results (2026-10-24, test PC: 3080 Ti, SteamVR null headset)
- **TAA = false:** the switch reads 0 and the camera jitter is 0. The image renders correctly in both eyes. On static rocks it has about 62% more edge energy (fine detail) than the game's default, with some stair-stepping on silhouettes.
- **TaaJitter = 0:** TAA stays on and the camera jitter is 0. About 37% more edge energy than the default.
- **Sharpen = 0.6:** about 44% more edge energy on the same frame, with no halos. It runs on the headset images only.

# Foveated rendering (v0.9.6)
- **Quad views were rejected for this mod.** The right eye is a replay of the left eye's frame, so every view has the same size. Each replay costs 6–9 ms of CPU, and extra views can't be rendered smaller.
- **NVIDIA VRS was chosen instead** (`vrs.cpp`, written from the GranblueVR notes in vr-mod-toolkit.md).
  - NVAPI is loaded through nvapi_QueryInterface, without the SDK lib.
  - Each eye has its own R8_UINT shading-rate image with one texel per 16×16 tile and three rings (full / mid / outer rate).
  - It is bound on the immediate context while the render target matches the eye render size and a depth buffer is bound, and unbound otherwise.
  - Pass 1 uses the left pattern; the replay (`NativeInReplay`) uses the right pattern.
  - The centre is each eye's optical axis, worked out from the eye's field-of-view tangents. Those are cached at StereoBegin, because the stereo state isn't active during render-target binds.
- **Settings:** `Foveation = Off | Fixed | Gaze`, `FoveationStrength = Low | Medium | High` (Low 2x1/2x2, Medium 2x2/4x2, High 2x2/4x4), `FoveationInner`, `FoveationOuter`.
- **Gaze mode:** XR_EXT_eye_gaze_interaction (action set, pose action, `xrLocateSpace` against VIEW space, then tangents) moves the pattern centre. It falls back to Fixed when no sample has arrived for 250 ms. `DebugFakeGaze` draws a circling test gaze.
- **Measured** (Native, 2000×2000 per eye, game capped at 60 fps):

  | Setting | fps |
  |---|---|
  | Off | 46 |
  | Fixed / High | 58 |
  | Fixed / Medium | 58–60 |

  Both eyes get their own pattern, and in Gaze mode it follows the gaze in each eye.
- Not yet tested with a real eye tracker. The null driver reports no eye tracking.
