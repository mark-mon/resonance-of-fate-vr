RESONANCE OF FATE VR - version 1.1.0
====================================

A native VR mod for Resonance of Fate on Steam. The game renders each eye with its own camera (true stereo, not
alternate-eye rendering), the camera follows your head, the HUD sits on its own panel and there is an
in-headset settings menu.

NEW IN 1.1.0: support for the Enhanced Edition (the 2026 "Enhanced Remaster" update, Direct3D 12).
The 4K/HD Edition build is the unchanged 1.0.0.

WHICH FOLDER?
  Enhanced Edition\   for the current Steam version (Enhanced Remaster update, 2026)   -> xinput9_1_0.dll
  4K-HD Edition\      for the older 4K/HD Edition 1.0.0.4 (2019)                       -> xinput1_3.dll
Copy BOTH files from ONE folder only. Each DLL checks the exe and stays off on the other edition.

REQUIREMENTS
- Resonance of Fate on Steam (either edition above)
- A PC VR headset with SteamVR (OpenXR runtime = SteamVR)
- Foveated rendering (optional): Enhanced Edition - a GPU with Direct3D 12 variable rate shading tier 2
  (NVIDIA RTX, AMD RX 6000 or newer, Intel Arc); 4K/HD Edition - an NVIDIA RTX card

INSTALL
1. Copy the two DLLs from the folder for your edition into the game folder, e.g.
     D:\SteamLibrary\steamapps\common\Resonance of Fate
   (next to "Resonance of Fate.exe").
2. Start SteamVR, then start the game from Steam.
3. On the first start the mod writes config.txt (all settings, with comments) and rofvr.log into that folder.
   Updating from an Enhanced Edition preview: keep your config.txt.

UNINSTALL
Delete the two DLLs, config.txt and rofvr.log from the game folder.

CONTROLS
- Settings menu: hold L3 + R3 (click both sticks) for 0.3 s, or press Insert.
    D-pad / left stick: select and change; A: toggle / run; Y: undo; LB / RB: page; B (or Backspace): close.
    The panel appears in front of where you look and stays fixed in space. Changes are saved to config.txt.
- F8: switch between the flat virtual screen and stereo
- F9: recentre the view
- F10: hide / show the HUD panel
- F12: mark the log (e.g. right after something looked wrong) - then send rofvr.log

DEFAULTS
- RenderMode = Native: full per-eye stereo. Menus and the title screen use the flat virtual screen.
- RenderResolution = Auto: 16:9 at your headset's recommended per-eye height. Lower RenderScale (or use the
  menu) for more fps; the game runs at up to 60 fps and the mod warps each frame to your head on every refresh.
- HorizonLock = 0: the game's downward-looking cameras are kept, so the characters are in front of you.
- Motion blur is off in VR; Sharpen and Foveation (Fixed / Gaze, Low / Medium / High) are optional.

KNOWN LIMITS
- The game runs at up to 60 fps; on a 90 Hz+ headset frames are reprojected by the mod in between.
- Enhanced Edition: HudLayer = 0 (HUD inside the eye images) is not supported - keep HudLayer = 1.
  The game's TAA is not separated per eye yet; edges may show slight ghosting.
- Tested mainly in the field / Base areas. Battles, the world map and cutscenes have had less testing -
  please report problems with rofvr.log.
- SteamVR may keep showing an older app name it remembered for this game; that is cosmetic.

CREDITS / LICENSES
Dear ImGui (MIT), MinHook (BSD 2-Clause), Khronos OpenXR loader (Apache 2.0) - see the licenses folder.
