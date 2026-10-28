# Resonance of Fate VR

A native VR mod for **Resonance of Fate 4K/HD Edition** (Steam). Each eye is rendered by the game with its own
camera (true stereo, not alternate-eye rendering), the camera follows your head, the HUD sits on its own panel and
there is an in-headset settings menu.

## Features
- **Native stereo**: the frame's render commands are replayed once per eye with that eye's camera; both eyes are
  full-quality, same frame.
- **Head tracking** on the game's fixed / overhead cameras (their downward tilt is kept by default).
- **Headset resolution** rendering (16:9 at the headset's recommended per-eye height, scalable).
- **HUD on its own OpenXR layer** (world- or head-anchored), **settings menu** (Dear ImGui via vrkit), fixed in
  space where it opens; works with keyboard and gamepad (also with Steam Input).
- **Mod reprojection** of every frame to the current head pose (the game is capped at 60 fps).
- Motion blur off in VR, sharpening, NVIDIA foveated rendering (fixed or eye-tracked).

## Install (players)
Download the release zip, copy `xinput1_3.dll` and `openxr_loader.dll` next to `Resonance of Fate.exe`, start
SteamVR, then the game. See [`release/README.txt`](release/README.txt) for controls and settings.
Requires the Steam version 1.0.0.4 of the game (the mod checks the executable and stays off for other builds).

| Control | |
|---|---|
| Hold L3 + R3 / Insert | settings menu (D-pad, A, Y undo, LB/RB pages, B close) |
| F8 | flat screen <-> stereo |
| F9 | recentre |
| F10 | HUD panel on/off |
| F12 | mark the log (for bug reports: send `rofvr.log`) |

## How it works (short)
The mod is an `xinput1_3.dll` proxy, loaded by the game at startup. Game-code hooks are installed once SteamStub
has decrypted the executable. On the render thread it hooks the engine's command processor (tri-Ace ASKA engine,
the same family as Star Ocean: The Last Hope): pass 1 renders the game's centre camera; at the final composite job
the frame's commands are replayed for the left and the right eye, and every per-view constant upload is patched
with that eye's matrices. Object matrices of the replays are taken from pass 1, because other game threads already
write the next frame's data while the eyes replay (this caused one-frame white flashes). The eye images go to an
OpenXR projection layer; the HUD is separated with a black/white matte onto its own quad layer.

Details, addresses and the test history: [`FINDINGS.md`](FINDINGS.md).

## Building
Cross-compiled on Linux with clang-cl / lld-link against the MSVC STL and Windows SDK:
```
scripts/setup_env.sh /path/to/toolchain64bit.zip   # LLVM 22, MSVC STL + Windows SDK, MinHook, OpenXR SDK
scripts/build.sh                                    # -> build/xinput1_3.dll (+ openxr_loader.dll, .pdb)
```

## Layout
```
src/          the mod (dllmain, xinput proxy, D3D11/DXGI hooks, stereo replay (so4_stereo.cpp), camera and head
              tracking, render resolution, HUD layer, OpenXR runtime (xr_runtime.cpp), menu options, config)
vrkit/        shared in-headset settings menu library (ImGui) and gamepad / keyboard input
third_party/  RenderDoc API header
scripts/      build environment and build script
tools/        test automation (rof.ps1) and constant-upload analysis scripts
re/           reverse-engineering helpers (need a dump of the game, not included)
docs/         notes; handoff_notes.txt = project status for the next developer
release/      README shipped in the release zip
```

Third-party components: [`THIRD_PARTY.md`](THIRD_PARTY.md).
