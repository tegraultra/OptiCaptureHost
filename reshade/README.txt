OptiCaptureHost - ReShade depth (optional, experimental)            by MK-JH  https://t.me/MKJH_APPS

What it is
  OptiCaptureHost normally uses a flat depth for its own output scaling (FSR 3 / FSR 4 / DLSS), because a captured picture has no depth.
  With this helper the game's real depth buffer, found by ReShade, is shared with the host. It is only used by the host's own
  output scaling modes. Choose "Depth" in the launcher header: "Flat" is the normal mode and changes nothing.

What it needs
  * A Direct3D 11 or Direct3D 12 game, 64 bit.
  * The latest ReShade, the version "with add-on support" (reshade.me), installed into that game.
  * No online game with an anti-cheat: injecting ReShade can get you banned. Use single-player games only.

Steps
  1. Install ReShade into the game (with add-on support).
  2. Copy OptiCaptureDepth.addon64 next to the game's exe (where ReShade's dxgi.dll / d3d11.dll / d3d12.dll is).
  3. Copy OptiCaptureDepth.fx into ReShade's "Shaders" folder (reshade-shaders\Shaders).
  4. Start the game, open ReShade's menu (Home key), and tick "OptiCaptureDepth" on. Check that the depth is found with ReShade's own
     "DisplayDepth" effect (reversed / upside down / logarithmic settings apply here as well).
  5. In the launcher choose Depth -> ReShade, pick the game, choose Output scaling (FSR 3/4 or DLSS) and START.
  The launcher's Depth window shows what the host sees: waiting / receiving / not updating.

Good to know
  * The host copies the depth only when it has the same size as the captured picture (a game window with the picture size).
  * The depth is not matched frame by frame with the capture (the capture is 1-2 frames late), so it may be slightly ahead.
  * Nothing here changes OptiScaler's frame generation; it only feeds the host's own FSR 3/4 / DLSS output scaling.
  * If something does not work, send OptiCaptureHost.log and ReShade.log.

Licence: the add-on is part of OptiCaptureHost (GPL-3.0, (c) MK-JH). It was built with ReShade's public headers
(BSD-3-Clause, (c) Patrick Mours): see ReShade-LICENSE.txt.
