OptiCaptureHost - ReShade depth (optional, experimental)            by MK-JH  https://t.me/MKJH_APPS

What it is
  OptiCaptureHost normally uses a flat depth for its own output scaling (FSR 3 / FSR 4 / DLSS), because a captured picture has no depth.
  With this helper the game's real depth buffer, found by ReShade, is shared with the host. It is only used by the host's own
  output scaling modes. Choose "Depth" in the launcher header: selecting "ReShade" switches output scaling to DLSS · Quality,
  while selecting "Flat" is the normal mode and sets output scaling to Off.

What it needs
  * A 64-bit game using Direct3D 11 / Direct3D 12 (GPU shared texture) or Direct3D 9 / Direct3D 10 / OpenGL / Vulkan (CPU shared buffer).
  * ReShade (version 6+ "with add-on support" from reshade.me) already installed into that game.
  * No online game with an anti-cheat: injecting ReShade can get you banned. Use single-player games only.

Steps
  1. Install ReShade ("with add-on support") into the game first.
  2. Put the required files (such as OptiCaptureDepth.addon64 and reshade-shaders\Shaders\OptiCaptureDepth.fx) inside "ReShade depth"
     next to the launcher.
  3. Select your game in the launcher, open "Depth", and click "Install ReShade files". The launcher closes the game completely,
     copies the required files next to the game's exe, and launches the game again (while keeping the launcher open).
     (Alternatively, copy OptiCaptureDepth.addon64 next to the game's exe and OptiCaptureDepth.fx into reshade-shaders\Shaders manually.)
  4. In-game, open ReShade's menu (Home key) and tick "OptiCaptureDepth" on. Check that the depth is found with ReShade's own
     "DisplayDepth" effect (reversed / upside down / logarithmic settings apply here as well).
  5. In the launcher choose Depth -> ReShade, make sure Output scaling is set (FSR 3/4 or DLSS), and press START.
  The launcher's Depth window shows what the host sees: waiting / receiving / different size / cannot open / no new pictures.

Good to know
  * The host copies the depth only when it has the same size as the captured picture (a game window with the picture size).
  * On Direct3D 11 and 12 the depth is shared via GPU; on Direct3D 9, Direct3D 10, OpenGL, and Vulkan it falls back to a CPU shared
    memory buffer, which may add extra CPU/memory overhead at high resolutions.
  * The depth is not matched frame by frame with the capture (the capture is 1-2 frames late), so it may be slightly ahead.
  * Nothing here changes OptiScaler's frame generation; it only feeds the host's own FSR 3/4 / DLSS output scaling.
  * If something does not work, send OptiCaptureHost.log and ReShade.log.

Licence: the add-on is part of OptiCaptureHost (GPL-3.0, (c) MK-JH). It was built with ReShade's public headers
(BSD-3-Clause, (c) Patrick Mours): see ReShade-LICENSE.txt.