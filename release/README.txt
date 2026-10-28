RESONANCE OF FATE VR - version 1.0.0 (initial release)
=======================================================

A native VR mod for Resonance of Fate 4K/HD Edition (Steam). The game renders each eye with its own camera
(true stereo, not alternate-eye rendering), the camera follows your head, the HUD sits on its own panel and
there is an in-headset settings menu.

REQUIREMENTS
- Resonance of Fate 4K/HD Edition, Steam version 1.0.0.4 (the mod checks the exe and stays off for other builds)
- A PC VR headset with SteamVR (OpenXR runtime = SteamVR)
- A D3D11 GPU; foveated rendering needs an NVIDIA RTX card

INSTALL
1. Copy xinput1_3.dll and openxr_loader.dll into the game folder, e.g.
     D:\SteamLibrary\steamapps\common\Resonance of Fate
   (next to "Resonance of Fate.exe").
2. Start SteamVR, then start the game from Steam.
3. On the first start the mod writes config.txt (all settings, with comments) and rofvr.log into that folder.

UNINSTALL
Delete xinput1_3.dll, openxr_loader.dll, config.txt and rofvr.log from the game folder.

CONTROLS
- Settings menu: hold L3 + R3 (click both sticks) for 0.3 s, or press Insert.
    D-pad / left stick: select and change; A: toggle / run; Y: undo; LB / RB: page; B (or Backspace): close.
    The panel appears in front of where you look and stays fixed in space (Menu page: distance, width,
    height, or make it follow your head). Changes are saved to config.txt when the menu closes.
- F8: switch between the flat virtual screen and stereo
- F9: recentre the view
- F10: hide / show the HUD panel
- F12: mark the log (e.g. right after something looked wrong) - then send rofvr.log

DEFAULTS
- RenderMode = Native: full per-eye stereo. Menus and the title screen use the flat virtual screen.
- RenderResolution = Auto: 16:9 at your headset's recommended per-eye height. Lower RenderScale (or use the
  menu) for more fps; the game is capped at 60 fps and the mod warps each frame to your head on every refresh.
- HorizonLock = 0: the game's downward-looking cameras are kept, so the characters are in front of you.
- Motion blur is off in VR; Sharpen and Foveation (Fixed / Gaze) are optional.

KNOWN LIMITS
- The game runs at up to 60 fps; on a 90 Hz+ headset frames are reprojected by the mod in between.
- Tested on the field / Base areas with an RTX 3080 Ti and an RTX 5090 (Pimax). Battles, the world map and
  cutscenes have had less testing - please report problems with rofvr.log.
- SteamVR may keep showing an older app name it remembered for this game; that is cosmetic.

CREDITS / LICENSES
Dear ImGui (MIT), MinHook (BSD 2-Clause), Khronos OpenXR loader (Apache 2.0) - see the licenses folder.
