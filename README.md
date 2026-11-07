# Resonance of Fate VR

A native VR mod for **Resonance of Fate** on Steam - both the **Enhanced Edition** (2026 "Enhanced Remaster" update, Direct3D 12) and the older **4K/HD Edition** (Direct3D 11). Each eye is rendered by the game with its own
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
- Motion blur off in VR, sharpening, foveated rendering (fixed or eye-tracked; Enhanced Edition: Direct3D 12
  variable rate shading tier 2 on any vendor, 4K/HD Edition: NVIDIA).

## Install (players)
Download the release zip and copy the two DLLs from the folder for your edition next to `Resonance of Fate.exe`:

| Edition | Folder in the zip | Files |
|---|---|---|
| Enhanced Edition (current Steam version) | `Enhanced Edition\` | `xinput9_1_0.dll`, `openxr_loader.dll` |
| 4K/HD Edition 1.0.0.4 (2019) | `4K-HD Edition\` | `xinput1_3.dll`, `openxr_loader.dll` |

Start SteamVR, then the game. See [`release/README.txt`](release/README.txt) for controls and settings. Each DLL
checks the executable and stays off on the other edition.

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

### Enhanced Edition (Direct3D 12)
Same source tree, built with `-DROFVR_EE` into `xinput9_1_0.dll` (`scripts/build.sh ee`; `scripts/build.sh hd`
builds the 4K/HD `xinput1_3.dll`). The game logic is unchanged but the exe was rebuilt with a new compiler and the
renderer moved to Direct3D 12. A bridge (`src/d3d12_bridge.cpp`) shares textures and a fence with a private D3D11
device that drives the existing OpenXR code. The replay stereo works over D3D12 as well: the eyes are copied inside
the engine's own command list (`src/ee_render.cpp`), the constant uploads are patched at the engine's upload
function, and foveation uses D3D12 VRS tier 2. Because the game thread has already written its next camera update
by the time the eyes replay, the frame takes the head record the replays actually render (otherwise the eyes are
one head pose ahead of the pose the frame is shown with - jitter when turning).

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
