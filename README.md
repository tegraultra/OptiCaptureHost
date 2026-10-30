<div align="center">

# OptiCaptureHost

### Frame generation and modern upscaling for **any** game window.
**FSR 4 · XeSS · Multi Frame Generation up to 6x · DLSS / FSR 3-4 output scaling** — without touching the game.

[![Latest release](https://img.shields.io/github/v/release/REPLACE_OWNER/REPLACE_REPO?style=for-the-badge&label=download)](../../releases/latest)
[![Platform](https://img.shields.io/badge/Windows-10%20%2F%2011%20x64-0078d4?style=for-the-badge)](#requirements)
[![Telegram](https://img.shields.io/badge/Telegram-MKJH__APPS-26A5E4?style=for-the-badge&logo=telegram&logoColor=white)](https://t.me/MKJH_APPS)

**by MK-JH**

</div>

---

## What is this?

[OptiScaler](https://github.com/optiscaler/OptiScaler) can add frame generation and better upscaling to games, but only to games it can hook.
Many games can't be hooked: old engines, DX11-only titles, launchers with anti-tamper, emulators, video players.

**OptiCaptureHost takes a different road.** It captures the game's window with Windows Graphics Capture and shows it in its own
Direct3D 12 flip-model overlay. To OptiScaler that overlay looks like a normal modern game, so it can do its work on it.
The game itself is **never injected, patched or modified**.

```
 your game  ──(window capture)──▶  OptiCaptureHost  ──▶  D3D12 overlay  ──▶  OptiScaler  ──▶  your monitor
 (untouched)                       motion + upscaling      (flip model)      FG / FSR 4 / XeSS
```

## Highlights

- **Works on almost anything that runs in a window** — DX9, DX11, DX12, Vulkan, OpenGL, emulators.
- **Multi Frame Generation from 2x up to 6x** — Intel XeFG and the DLSSG-based modes, picked with one click.
- **One-click profiles** — *NO FG*, *x2 / x3 / x4 (NEW GPU)* and *x2 / x3 / x4 (OLD GPU)*. A profile sets frame generation, the frame limiter, the present mode and output scaling together.
- **Output scaling with temporal upscalers** — run the game at a lower resolution and let the host scale it to your monitor:

  | Method | What it is |
  |---|---|
  | **DLSS** | NVIDIA DLSS Super Resolution (RTX cards, needs `nvngx_dlss.dll`) |
  | **FSR 3 / 4** | AMD FidelityFX upscaler (any GPU, needs `amd_fidelityfx_dx12.dll`) |
  | FSR 1 | EASU + RCAS, no extra files |
  | NIS | NVIDIA Image Scaling, no extra files |
  | Bilinear | Plain scaling |

  Levels: **Native (1x)**, **Quality (1.5x)**, **Balanced (1.7x)**, **Performance (2x)**. 
- **The host measures the motion itself.** DLSS and FSR need motion vectors, and a window capture has none, so the host estimates them from consecutive frames on the GPU (block matching with sub-pixel refinement and a confidence mask).
- **Sharpness slider** with its own RCAS pass, applied after the upscaler.
- **Automatic window handling** — START shrinks the game window to the right render size, STOP restores it, and if the game changes its resolution in its own settings the size is put back.
- **Frame limiter built in** — up to 500 fps through RivaTuner, and OptiScaler's own limit set to your monitor's refresh rate at every start.
- **Low-latency present modes**, VSync on/off (real tearing with VSync off), configurable hotkeys.
- **Live status panel** with frame pacing numbers, warnings as dialogs you can silence with "don't show again".
- **Reset all** puts every option back to its default.

## Requirements

- Windows 10 (2004+) or Windows 11, 64-bit
- A GPU with DirectX 12 (Intel Arc, NVIDIA, AMD)
- Game running in **Borderless Fullscreen or Windowed** mode (not Exclusive Fullscreen)
- OptiScaler files (see below)
- Optional: **RivaTuner Statistics Server** for the frame limiter
- Optional: an **NVIDIA RTX** card for DLSS

## Quick start

1. **Download** `OptiCaptureHost-win64.zip` from the [latest release](../../releases/latest) and unpack it into a folder of its own.
2. **Add OptiScaler** to the same folder: OptiScaler renamed to `dxgi.dll`, its `OptiScaler.ini`, the FSR 4 DLL you use, and the frame generation files for your mode (`libxess_fg.dll`, `libxell.dll`, `XeFGUnlock.asi` for Intel XeFG).
3. *(Optional)* For **FSR 3 / 4** put AMD's `amd_fidelityfx_dx12.dll` next to the exes. For **DLSS** put NVIDIA's `nvngx_dlss.dll` next to the exes (or in a `dlss` folder).
4. Start the game in Borderless / Windowed mode.
5. Run **`OptiCaptureLauncher.exe`**, click your game in the list, choose a profile, and press **START**.

> The runtimes of AMD, NVIDIA, Intel and OptiScaler are theirs and are **not included** in this download. Get them from their official sources.

## Profiles

| Profile | Frame generation | Output scaling |
|---|---|---|
| NO FG MOD | off | DLSS Quality |
| x2 / x3 / x4 FG MOD (NEW GPU) | Intel MFG (XeFG) at 2x / 3x / 4x | DLSS Quality |
| x2 / x3 / x4 FG MOD (OLD GPU) | DLSSG-based MFG at 2x / 3x / 4x | DLSS Quality |

Every profile also sets the frame limiter to the refresh rate (or 1/2, 1/3, 1/4 of it) and the low-latency present mode. You can change any single option afterwards.
If DLSS isn't available (no RTX card or no `nvngx_dlss.dll`), the host falls back to FSR 1 and says so in the log.

## Hotkeys (defaults, all changeable)

| Action | Default |
|---|---|
| Stop capture | `Ctrl+Shift+F12` |
| Game / menu focus | `Ctrl+Shift+F11` |
| OptiScaler menu | `Ctrl+Shift+F10` |
| Show the launcher | `Ctrl+Shift+F9` |

The "OptiScaler menu" key must match `ShortcutKey` in `OptiScaler.ini`.

## Good to know

- **Latency:** the capture adds one hop. Use VSync **off** at START for real tearing and the lowest latency; your monitor's refresh rate still caps the output.
- **Mouse:** with output scaling on, the pointer is not remapped. It lands where it is on the real desktop, not on the scaled picture.
- **Temporal upscalers on a capture:** there is no game jitter and no true depth, so DLSS / FSR 3-4 here work from measured motion. Expect them to be a bit softer than in-game DLSS; that is what the sharpness slider is for. This part is **experimental** and improving.
- **Online games:** the game isn't modified, but capturing a window and resizing it can still upset some anti-cheat systems. Use it in single-player or at your own risk.

## Troubleshooting

| Problem | Try |
|---|---|
| Launcher says the host stopped | Open `OptiCaptureHost.log` next to the exes; the last lines say why. |
| Output scaling falls back to FSR 1 | The log names the missing file (`nvngx_dlss.dll` / `amd_fidelityfx_dx12.dll`) or an unsupported card. |
| Black or frozen picture | Make sure the game is Borderless / Windowed, not Exclusive Fullscreen. |
| No frame limiter | Install RivaTuner Statistics Server. |
| Shimmer on fine detail | Raise the sharpness a little, or use Native / Quality instead of Performance. |

## Support and feedback

- Telegram channel: **[@MKJH_APPS](https://t.me/MKJH_APPS)**
- Bugs and ideas: open an [issue](../../issues) and attach `OptiCaptureHost.log`.

## Credits

Built on top of the work of the [OptiScaler](https://github.com/optiscaler/OptiScaler) team. FSR and FidelityFX are AMD technologies, DLSS is NVIDIA's, XeSS and XeFG are Intel's.
All trademarks belong to their owners. This project is not affiliated with or endorsed by any of them.

## License

OptiCaptureHost is **free for personal, non-commercial use**. It is distributed as a binary; the source code is not published.
Redistribution, modification, reverse engineering and commercial use need written permission from the author. See [LICENSE](LICENSE.txt).
Third-party components keep their own licences and are not part of this download.

---

<div align="center">Made with care by <b>MK-JH</b> · <a href="https://t.me/MKJH_APPS">t.me/MKJH_APPS</a></div>
