# Resonance of Fate VR (rofvr) - findings

Game: "Resonance of Fate.exe" 4K/HD Edition ver.1.0.0.4 (Steam app 645730), 22,071,664 bytes, PE timestamp
1548819852 (2019-01-30). 64-bit D3D11, tri-Ace ASKA engine (same family as Star Ocean: The Last Hope).
Image base at runtime 0x7FF723FE0000 on the test PC (ASLR; all addresses below are RVAs).

## Protection / loading
- Only SteamStub (.bind). The decrypted image (memory snapshot, test 002) has normal sections
  (.text 0x1000, .rdata 0x9E2000, .data 0xBFC000, .pdata 0x17FA000). No second layer.
- The mod is xinput1_3.dll (static import, ordinals 2/3/4). DllMain runs BEFORE SteamStub decrypts .text,
  so game-code hooks are installed from a thread that waits until known code bytes match AND the page is
  executable again (dllmain.cpp DeferredGameHooks; ~0.4-0.7 s after start).
- RTTI: only 1,452 classes have type info; Aska::Camera, PostProcessCombiner etc. have none. Matched against
  the SO4 dump by code shape instead (re/match.py, re/findmatch.py, re/side.py).

## D3D11 / renderer (same layout as SO4 up to ~0x26xxxx)
- Renderer global 0x1714AC0 (SO4 0xF76C70). +0x24A9B8 device, +0x24A9C0 factory, +0x24A9C8 swapchain,
  +0x24A9D0 swap chain desc. Device creation 0x2924F0.
- Immediate context global 0x1716C10 (SO4 0xF77310). Screen config global 0x1716C08 (SO4 0xF76C98).
- Swapchain 1280x720 R8G8B8A8_UNORM, 1 buffer, DISCARD; window class "AskaWnd".

## Render thread (identical to SO4 apart from offsets)
- Command processor 0x2A2FA0 process(queue, int* counter, bool* flag). Ring at +0xB8, 0x2001 x 0x28 bytes,
  read index +0xB4 (last consumed), write +0xB0. Pending callback +0x88. Types as SO4: 0 job, 1, 2 set RT +
  clear (0x3984D0), 3 resolve (0x398F00), 4 job2, 6 release game thread, 7, 10 frame end, 17, 18, 19.
- Type 0: job->vt[0x248/8](job, arg, count) at 0x2A30C3 (FF 90 48 02 00 00) - SO4 used vt+0x250.
- Frame end 0x28DE80 (SO4 0x0D2E0) -> present 0x293350.
- Final composite job = PostProcessCombiner vtable 0xA26790 (job fn 0x3FE150; SO4 0x90F038 / 0x9D200).
- Motion blur job = PostProcessMotionBlur vtable 0xA48170 (job fn 0x7F58C0; SO4 0x9296C8), skipped in head-tracked modes (MotionBlur = 0).

## Per-view constants
- Writer 0x28FC70 (SO4 0x506D0), uploader 0x295D80 (UpdateSubresource; SO4 0x56BA0).
- Dirty flags renderer+0x26DF7C (SO4 +0x270B7C): bit0 -> 0x80-byte block +0x26E010, bit3 -> per-view block.
- Per-view blocks are 0x8A0 = 2208 bytes (SO4 2192). Three uploads from 0x28FC70 (return addresses):
  0x28FD09 (renderer+0x26DF7C bit3, CPU copy +0x26E930: lights / shadow views), 0x28FDA0 (0x26DF78 bit0),
  0x28FDC5 (0x26DF78 bit3, CPU copy +0x26FA70: the MAIN CAMERA block). Main camera block layout (test 020):
  +0x340 view rows (3 x float4, row-major, translation in w), +0x4C0 camera position + 0x4CC fov (deg).
  +0xC0 is empty for the main camera (SO4 had a view there); both offsets are checked.

## Camera (Aska camera, vtable 0xA27680; field camera class CCameraCH, vtable 0x9E9CD8)
- UpdateCamera 0x2746F0 (SO4 0x3CC20). Field layout identical to SO4 (+0x30 world, +0x110 flags, +0x120 view,
  +0x420 proj, +0x4E0 VP, +0x560 pos, +0x5D0 proj no jitter, +0x650 lens shift, +0x6E4 aspect, +0x710 lens*,
  +0x7C8/7CC/7D0 film), EXCEPT the flags byte at +0x7AA (SO4 +0x7AB). Vtable slots are 8 bytes lower.
- View inverse: rigid 0x1B9AF0, scaled 0xD50F0 (src world, dst view).
- Title camera: (0, 0, -1400), identity rotation, m00 2.414 (skipped as a menu camera by the SO4 rule).
- Field (Base interior, test 020): CCameraCH at (357, 200, -743), pitched ~8 deg down, m00 1.374 (~72 deg h-FOV),
  static between frames (fixed camera; many records with identical views -> "ambiguous" matches are normal).
  The SO4 main-camera pick (most-updated perspective camera / seen in the render upload) chooses it.

## Render resolution
- SetScreenConfig 0x28DDD0 (cfg, final desc, scene desc) (SO4 0xD1F0); queue rebuild flag +0x500E1 (SO4 +0x500D1).

## Native stereo (tests 021-026)
- The SO4 replay mechanism works unchanged with the mapped offsets.
- StereoBothReplay = 1 (pass 1 = centre, both eyes replayed) is REQUIRED: with 0 the left eye (pass 1 patched
  from the first view upload on) is badly over-lit: lighting uploads come before the main view upload (test 023).
- Performance: the upload patcher scanned every float position; constant buffers keep float4x4 16-byte aligned,
  so PatchAligned = 1 scans only those (same patch counts): 30 -> 57-60 fps on the 3080 Ti at 1280x720,
  replay 7.4 -> 4.0 ms CPU, patching 10.9 -> 4.7 ms per stereo frame (test 024). At headset size
  (null HMD 2244x2352 -> Auto 4808x2704 per eye) 52-60 fps (test 025).
- ROF frames have ~515 render jobs per pass (SO4 ~114) and ~2000 draws.
- One sporadic right-eye artifact seen once (test 021, staircase lit pure white for one dump); not in later dumps.
- HUD matte works (prompts, markers); motion blur job skipped in stereo.
- ReplayDupFix (SO4's torn-upload fix) is WRONG for RoF and is off by default since 0.1.5: RoF re-uploads the
  main per-view block ~180 times per eye per frame within jobs, same view at +0x340 but different contents
  (per-object/light parameters). The fix replaced them with stale copies (~110k per 10 s): left-eye white hair
  outline (test 029 A vs B; user report 2026-10-27) and probably the random one-frame flickers.
- Glitch detector: head records advance ~3 per frame in RoF (several UpdateCamera calls), the SO4 detector
  required +1 steps and never ran; 0.1.5 accepts gaps up to 6 and adds a per-tile score ("glitch scan:" lines).
- User test on the Pimax (0.1.0, 7000x3936 per eye, 5090): 55-60 fps; SteamVR dropped the refresh to
  45/30/15 Hz at times (late frames).

## Render resolution (test 025)
- SetScreenConfig from the game thread works: "render resolution: 4808x2704 [VR] (game's own 1280x720)" applied
  at the title already, no crash, HUD intact.

## Input (test automation)
- The game reads pads only through a pad object that its input system enables; with no physical pad it never
  calls XInput at all (not even XInputGetCapabilities), so the fake pad (DebugFakePad) cannot drive it.
  WM_DEVICECHANGE did not trigger a rescan.
- The pad object (ctor 0x7DF7E0, vtable 0xA2FFC0; poll 0x7DFBF0, find-active 0x7DFDE0) is never polled
  without a pad (hooks in rof_input.cpp log it).
- Keyboard: WM_KEYDOWN lParam scan codes -> table 0xA2F640 (ASCII-like codes) -> key state at kb+0x2A0.
- Navigation that works (rof.ps1 Enter-ROFGame, tests 010-020): Enter (scan code) skips the intro; at the title
  left click + right click in the client area, long Enter, long Space, then Enter as a virtual key (repeat until
  the orange LOAD header is seen); a LEFT CLICK = confirm anywhere; W/A/S/D = directions; Data 1 -> click ->
  "Load save data?" (cursor on No) -> W -> click -> ~8 s -> click ("Load complete.") -> field (Base).

## One-frame white flashes (tests 029-053, fixed in 0.1.8)
- Symptom (user, Pimax): now and then one eye shows a surface / object pure white (or white silhouette edges) for
  one frame; either eye. The XR-thread detector (DebugGlitchDetect = 1, tile score, thumbnails
  rofvr_glitch_NN_*.bmp) caught 50-80 per 3000 frames on the test walk with a fake head sweep.
- Signature (tests 032-043): the glitched eye has ONE EXTRA per-view block upload (0x8A0) compared with the other
  eye. Its object matrix (+0x00, VP * World; also +0x40) was built with a NEWER camera than the rest of that
  replay (other rows identical) -> part of that eye drawn with another frame's matrices -> depth / G-buffer
  mismatch -> white.
- Ruled out: TAA history (no history textures tracked), packet memory (job args; generic job fn 0x1EF9A0 draws
  `count` packets of 0x260 bytes with POINTERS to game data - the packets never changed), the main camera
  object (+0x4E0 VP changes mid-replay only a few times per 10 s), the game thread that calls UpdateCamera
  (gating it, even SuspendThread during the replays: no change), Map-type leaks (the game only uses
  WRITE_DISCARD), cross-eye leak of our patched matrices (never seen), delta-test tolerance.
  With delta patching off (DebugNoDelta = 1) there were no detections - but the object matrices then are not
  stereo at all, so that only showed that the object matrices are what differs.
- Fix (ReplayRawFromPass1 = 1, ReplayPass1Nearest = 1, default): pass 1 (the game's own frame, before the
  combiner) stores every per-view block with its job index (So4StereoBeforeUpload). In each eye's replay, the
  upload is matched to the pass-1 blocks of the same job: all rows except +0x00..+0x6F and the view rows
  +0x340..+0x36F equal (hash first) -> the closest one's +0x00..+0x6F is used; no such match -> the pass-1 block
  whose object matrices are clearly the closest (best * 4 < runner-up, best < 2% of the matrix size).
  Then the normal patching runs. Results: 0 detections in 7200 frames (052); 053 (25 deg head sweep at 0.3 Hz,
  outdoor area) only low-score detections (max 29 vs 50-200 before) that show no white surfaces (head-motion
  noise). No fps cost (diagnostics off: same fps with the fix on and off).
- Rejected attempts left in the code, all OFF: ReplayCameraGate, ReplayPacketSnapshot, ReplayMapShadow,
  ReplayCrossEyeFix, ReplayPass1MaxRows (whole-block substitution), DebugSuspendGame, DebugNoDelta.
- Other fixes found on the way: the mod's XR thread (own D3D11 device) shares the hooked context vtable; its
  reprojection Map/Unmap/Draw went through the stereo hooks (raced g_mapRes / patch state; a crash in 037).
  Hooks now act only on the game device's context (GameCtx). Per-position diagnostics in the patch loop must
  stay out: they doubled the patching cost (11.5 vs 4.7-7 ms per stereo frame).
- The "job draw mismatch" lines (+-1 draws per job between pass 1 / left / right) are normal in RoF.

## Gamepad in the settings menu (0.1.9, 2026-10-28)
- User: the vrkit menu worked with the keyboard but not the controller.
- RoF reads pads ONLY through XInput (xinput1_3 imported by ordinal; no DirectInput / raw input / HID / Steam
  Input API use - SteamController006 is only the steam_api context's interface list). Steam Input pads
  (PlayStation, Switch, generic) are therefore Steam's virtual XInput pad inside the game's calls; vrkit read
  XInput directly and found no pad. The user's log (0.1.0 session) showed "vrkit: no XInput gamepad found",
  the game's pad object never polled and the proxy never called (no "system xinput loaded").
- 0.1.9: vrkit notes the state the game receives (its hooked XInputGetState calls + new
  input::SetExternalState, fed by rof_input.cpp from the pad object after each poll: raw XINPUT_STATE at
  +0x16C) and uses it when it can't read a pad itself (merged otherwise). While the menu is open the pad
  object's decoded state (+0x9C..+0xA7) and raw state (+0x170..+0x17B) are zeroed. Also (as in the FF XII mod)
  vrkit reads the pad first through a private, renamed copy of System32\xinput1_4.dll (%TEMP%\vrkit_xinput1_4.dll)
  that Steam Input's hooks don't touch. Not yet tested in a game.

## 1.0.0 initial release (2026-10-28)
- User: "this game works great" with 0.1.10 (menu gamepad via the private xinput1_4 copy). 1.0.0 = 0.1.10 +
  tidied default config text (diagnostic keys removed from the template, ReplayRawFromPass1 listed).
  Release zip: builds\release_1.0.0\ResonanceOfFateVR_1.0.0.zip (DLLs, README, licenses).
