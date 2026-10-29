# OptiCaptureHost

Capture any game window (Windows Graphics Capture) and show it in an overlay that
[OptiScaler](https://github.com/optiscaler/OptiScaler) can attach to, so FSR 4 and XeSS multi frame generation work on
**any** game without hooking or modifying it.

by **MK-JH** - Telegram: https://t.me/MKJH_APPS

## Files

- `OptiCaptureLauncher.exe` - the menu: pick the game, VSync on/off, change shortcuts, Start/Stop.
- `OptiCaptureHost.exe` - the capture host (started by the launcher).
- `OptiCaptureHost.ini` - created by the launcher; the host re-reads it while running.

## Setup

1. Put both exes in one folder together with your OptiScaler files: OptiScaler renamed to **`dxgi.dll`** (keep that name),
   `OptiScaler.ini`, the FSR 4 INT8 DLL, `libxess_fg.dll`, `libxell.dll`, `XeFGUnlock.asi` (`LoadAsiPlugins=true`).
2. Run the game in **Borderless / Windowed** (not Exclusive Fullscreen).
3. Run `OptiCaptureLauncher.exe`, click the game, press **START**.

## Shortcuts (defaults, all changeable in the launcher)

| Action | Default |
|---|---|
| Stop capture | Ctrl+Shift+F12 |
| Game / menu focus | Ctrl+Shift+F11 |
| OptiScaler menu | Ctrl+Shift+F10 |
| Show the launcher | Ctrl+Shift+F9 |

"OptiScaler menu key" must match `ShortcutKey` in `OptiScaler.ini` (Insert by default).

## VSync

Switch it in the launcher at any time. For real tearing (lowest latency) start the capture with VSync **off**; the
tearing capability is chosen when the capture starts. Your monitor refresh rate caps the output frame rate.

## Build

Push to GitHub: the Actions workflow builds both exes and publishes `OptiCaptureHost-win64.zip` under Releases
(`build-latest`). Local: `cmake -S . -B build -A x64` then `cmake --build build --config Release`.

## Notes

FSR 4 here runs 1:1 (no jitter): anti-aliasing / shimmer reduction, not resolution upscaling. Latency: one capture hop.
OptiScaler, XeSS and FSR keep their own licences and are not included.

## Output scaling (2.9.0, experimental)

Run the game in a smaller window and let the host scale the picture up to the whole monitor before OptiScaler sees it.
Choose FSR 1 (EASU + RCAS), NIS or bilinear and a level (Quality 1.5x, Balanced 1.7x, Performance 2x) in the launcher. START
shrinks the game window to monitor size / level (any monitor, 1080p to 4K) and the host scales it back up; STOP puts the window back.
The Resize button does the shrinking without starting. Settings are in `[Upscale]` of OptiCaptureHost.ini
(`Method` 0 off / 1 bilinear / 2 FSR 1 / 3 NIS, `Level` 0..2, `Sharpness` x100, 0 = sharpest, default 25). Applies at the next start.
The mouse is not remapped: the pointer lands where it is on the real desktop, not on the scaled picture.

## DLSS (experimental)
Output scaling has a **DLSS** entry (Native / Quality / Balanced / Performance) next to FSR3/4. It needs an NVIDIA RTX card and NVIDIA's `nvngx_dlss.dll`,
placed next to `OptiCaptureHost.exe` (or in a `dlss` folder there). It uses the same measured motion as FSR3/4. The model is chosen automatically
(L when upscaling, E at 1:1); `[Upscale] DlssPreset` in `OptiCaptureHost.ini` overrides it (11 = K, lighter on RTX 20/30; 13 = M). If the runtime or card is missing, the host falls back to FSR 1.
