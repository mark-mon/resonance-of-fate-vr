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

## Enhanced Edition / Enhanced Remaster (2026-10-29..30, work in progress)
- New exe (37,080,008 bytes, PE timestamp 1792059323) after the "Enhanced Edition" update. Direct3D 12, imports
  XINPUT9_1_0 (so the EE build is xinput9_1_0.dll, built with -DROFVR_EE; the 4K/HD build xinput1_3.dll is
  unchanged and keeps the old behaviour). SteamStub .bind, but .text is plaintext on disk. New compiler; the
  game classes are the same (~1400 RTTI vtables match the old exe). Old->EE function map: work_ee/fmap.pkl.
- Stage 1 (done): D3D12 -> D3D11 bridge (d3d12_bridge.cpp): private D3D11 device on the same adapter, shared
  texture + shared fence, back buffer copied on the game's queue; feeds the existing XR runtime. Flat screen in
  the headset at 60 fps.
- EE RVAs: UpdateCamera 0x94B50 (camera layout as before: +0x120 view, +0x420 proj, +0x4E0 VP); render command
  processor 0x106940; constant upload 0xCC060 (rcx renderer, rdx buffer object, r8 desc {src +0, offset +0xC,
  size +0x10}; memcpy into the 1 GB UPLOAD ring at 0xCC207); wrapper 0xE34C0(this, slot, size, data).
- Per-view block (test 069): size 0x8B0 in the EE (0x8A0 before), uploaded from 0xDB783 / 0xDB8E3 (function
  0xDB51B) on the render thread, ~50,000 per second; the camera view rows are still at +0x340. (A 0x8A0 upload also
  still exists, ~480 per second.) Found with a hardware write watch on a ring slot (test 068); a CPU-side search for
  the uploaded bytes matched garbage and flooded the log to 765 MB (removed; the log is now capped at 64 MB).
- Render thread (tests 072-073): processor 0x106940(q, int*, bool*), packet jump table 0x107410; ring +0xC8
  (0x2001 x 0x28), read +0xC4, write +0xC0, callback +0x98 (args +0xA0/+0xA8), event +0x88. Type 0 job:
  call [rax+0x240] at 0x106A8E (FF 90 40 02 00 00); type 4 job2 vt+0x248. Frame submit 0xD09E0 (closes the open
  list with 0xD0580, ExecuteCommandLists) from 0xA1970 (types 10 / 19); 0x1062E0 is NOT per frame.
  Renderer global 0x2545A90 -> +0x298C30 device object -> +0x1C28D0 open ID3D12GraphicsCommandList (all of a
  frame's lists are submitted together at the frame end, so per-eye replays record into the same frame).
- Gameplay frame ~383 jobs (Base): shadow/depth jobs, scene into a 1280x720 R16G16B16A16F target, post jobs,
  bloom 320x180, then the COMBINER vt 0x17E8B30 (job fn 0x383F20; old 0xA26790 / 0x3FE150) draws 1 quad into the
  1280x720 R10G10B10A2 final target, then UI jobs (vt 0x1711090, 0x17119A0, 0x17143E0, 0x17103A0) on top.
- Camera in the EE: same layout up to +0x5FF; flags +0x118 (old +0x110); from the lens shift on everything +0x10
  (shift 0x660, aspect 0x6F4, lens* 0x720, flags 0x7BA, film aspect/W/H 0x7D8/0x7DC/0x7E0; projection builder
  0x93620). Rigid view inverse 0x134A0 (hooked); the scaled inverse is inlined (UpdateCamera +0x119 & 3 path),
  so for it the head goes into the world matrix around the update. The 2D/UI cameras (vt 0x17E9FE8, m00 2/1280)
  do NOT carry the ortho flag bit -> rejected by projection (m00 < 0.05 or no perspective terms). The main field
  camera (CCameraCH vt 0x16EEDD0) is updated ~4x per frame: a still view's match goes stale after 4 records ->
  re-matched (EE only).
- Native stereo (test 078, stereo preview 1): the old replay engine (so4_stereo.cpp, #ifdef ROFVR_EE) runs over
  D3D12: constant uploads patched in a copy at 0xCC060 (ee_render.cpp), eyes copied (barrier RT -> COPY_SOURCE)
  into shared D3D12 textures inside the engine's own command list after each eye's replay, published at Present
  through the D3D11 bridge (XrPublishStereo). Every frame stereo, ~56 fps at 1280x720 per eye (replay 4.4 ms,
  patching 6.1 ms per stereo frame). Jobs flagged for completion (+0x190 bit 19) are never replayed.
  Not ported yet: HUD layer, render resolution (SetScreenConfig), TAA per-eye history, motion blur skip, VRS.

### EE head tracking / render resolution (tests 079-082, stereo preview 2)
- User test of preview 1 (Pimax/lighthouse, 3980x3424 recommended): stereo ran, but the image moved with the head and
  was very blurry. Causes: (1) UpdateCamera rebuilds the view only when +0x7BA bit 5 is set -> So4HtPre sets 0x22;
  (2) the camera's own world update (CCameraCH vt+0x90 = 0x94AD0 -> node update 0x17180: +0x118 &= 0xE2,
  +0x119 |= 3) selects the INLINED "scaled" inverse inside UpdateCamera although the world is orthonormal -> the
  rigid-inverse hook never ran ("views WITHOUT head" ~100%). Fix: hook 0x94AD0; for the tracked camera with an
  orthonormal world clear +0x119 bits 0-1 after it, restore them after UpdateCamera -> rigid path 0x134A0 (hooked).
  Test 082: head in every view, fake head sweep turns the image.
- Render resolution (test 080): SetScreenConfig 0xA1560(cfg [0x2547AD8], final desc 16 bytes -> +0x2C, scene desc
  20 bytes -> +0x3C); rebuild byte queue +0x500F1 (set by 0x108A50, handled by 0x1062E0). 4808x2704 per eye on the
  null headset, ~53 fps, eye images sharp.
- Brightness (user, preview 2): the EE final target is R10G10B10A2_UNORM holding gamma-encoded colour; there is no
  sRGB variant, so the XR swapchain took it as linear -> far too bright. PickSwapchainFormat maps R10G10B10A2 to
  R8G8B8A8_UNORM_SRGB (drawn with sRGB decode by the reprojection pass; a plain draw when not reprojecting). Test 083:
  XR swapchain format 29.
- HUD (test 083, preview 3): at the combiner the frame's UI commands (until command 3/6/10/19) are replayed twice
  through the fake queue after ClearRenderTargetView (black / white) of the final target in the engine's list (RTV
  handle from the OMSetRenderTargets hook); both copied into shared textures (eye slots 2/3), matte built with the
  4K/HD code (so4_hud) on the bridge's D3D11 device at Present -> HUD quad layer. 4808x2704, ~48-50 fps.
- Motion blur (EE): job vt 0x17F79B8 (fn 0x10EB740, tests +0x715 & 9 like the old 0x7F58C0 +0x70D & 5), skipped in VR
  (test 085: skipped every frame).
- Foveated rendering (EE, tests 084-085): D3D12 VRS tier 2 instead of NVAPI. In the OMSetRenderTargets hook (render
  thread): scene pass = RT0 at the render size + a DSV -> ID3D12GraphicsCommandList5::RSSetShadingRate(1X1,
  {PASSTHROUGH, OVERRIDE}) + RSSetShadingRateImage(per-eye R8_UINT image, 16-pixel tiles on the 3080 Ti); other binds
  and the frame end unbind. Pattern uploaded through an upload buffer + CopyTextureRegion in the engine's list (old
  buffers released 8 frames later). Test 085 (High): eye corners 100% 4x4-uniform blocks, centre full rate, both eyes;
  51-55 fps vs 48-50 without.
- Head-turn jitter (user, preview 4, 90 fps on the Pimax): the main camera is updated ~4x per game frame and So4HtPre
  located the head for each update -> records of one frame had different poses; the replays rendered record +1..+3
  views while the frame was presented with record 0's pose ("replay views by record: [0] 0, +1 646908, +2 27944,
  +3 8160"). EE: the located pose is reused for updates < 4 ms after the previous one (one pose per burst). Test
  086 (fake head sweep): pose reused ~800 / 5 s, all replay views from one record carrying the frame's pose.
- Head-turn jitter, real cause (tests 087-091, frame sequence dumps rof_dump_seq.txt -> 40 frames at 1/8 + csv of
  each frame's record; image shift by phase correlation vs fx * tan(head yaw delta)): the eye images were one head
  pose AHEAD of the record the frame was presented with (image shift(i) = predicted shift(i+1)). The replays
  rebuild the per-view block (0xDB51B, CPU block = its rbx argument) from live view data that the game thread has
  already advanced to its next camera update by then (replay views by record: all +1). Not the camera object
  (holding it during the replays changed nothing, test 089), and UpdateCamera does not run inside the replays (090).
  Fix (EE, ReplayLiveRecord = 1): before the replays, read the view rows (+0x340) of the last pass-1 per-view CPU
  block, find the head record with that view (+1..+8) and make it the frame's record (patch centre, eye matrices,
  published pose). Test 091: replay views all [0], image motion vs pose rms 0.40 px at 1/8 scale (quantisation).
  Kept but off: ReplayHoldCamera (089). Pass-1 view restore only applies when the live record was not taken.
