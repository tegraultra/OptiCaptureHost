<div align="center">

# OptiCaptureHost

### Frame generation and modern upscaling for **any** game window — now open source.
**FSR 4 · XeSS · Multi Frame Generation up to 6x · DLSS / FSR 3-4 output scaling** — without touching the game.

[![Latest release](https://img.shields.io/github/v/release/REPLACE_OWNER/REPLACE_REPO?style=for-the-badge&label=download)](../../releases/latest)
[![Build](https://img.shields.io/github/actions/workflow/status/REPLACE_OWNER/REPLACE_REPO/build.yml?style=for-the-badge&label=build)](../../actions)
[![Platform](https://img.shields.io/badge/Windows-10%20%2F%2011%20x64-0078d4?style=for-the-badge)](#requirements)
[![License](https://img.shields.io/badge/license-GPL--3.0-blue?style=for-the-badge)](LICENSE)
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

The project has two programs:

| Program | Job |
|---|---|
| `OptiCaptureLauncher.exe` | The control panel: pick a game, profiles, frame generation, limiter, output scaling, writes `OptiScaler.ini`. |
| `OptiCaptureHost.exe` | The capture + overlay + upscaling worker the launcher starts. |

## Highlights

- **Works on almost anything that runs in a window** — DX9, DX11, DX12, Vulkan, OpenGL, emulators.
- **Multi Frame Generation from 2x up to 6x** — Intel XeFG and the DLSSG-based modes.
- **One-click profiles** — *NO FG*, *x2 / x3 / x4 (NEW GPU)*, *x2 / x3 / x4 (OLD GPU)*: frame generation, frame limiter and present mode set together.
- **Output scaling** — run the game at a lower resolution, let the host scale it to your monitor:

  | Method | What it is |
  |---|---|
  | **DLSS** | NVIDIA DLSS Super Resolution (RTX, needs `nvngx_dlss.dll`) |
  | **FSR 3 / 4** | AMD FidelityFX upscaler (any GPU, needs `amd_fidelityfx_dx12.dll`) |
  | FSR 1 | EASU + RCAS, no extra files |
  | NIS | NVIDIA Image Scaling, no extra files |
  | Bilinear | Plain scaling |

  Levels: **Native (1x)**, **Quality (1.5x)**, **Balanced (1.7x)**, **Performance (2x)**.
- **The host measures motion itself.** A window capture has no motion vectors, so the host estimates them from consecutive frames on the GPU (block matching, sub-pixel refinement, temporal steadiness, a reactive mask). Two versions can be chosen in the launcher.
- **Opti Upscaler selector** — FSR 3.1.5, FSR 4.1.2, DLSS 3.7, DittoSR, XeSS, DLSS; picking one writes `OptiScaler.ini` and copies that upscaler's folder next to the exes.
- Built-in frame limiter, low-latency present modes, VSync on/off/auto, configurable hotkeys, live status panel.

## Requirements

- Windows 10 (2004+) or Windows 11, 64-bit
- A GPU with DirectX 12 (Intel Arc, NVIDIA, AMD)
- Game in **Borderless Fullscreen or Windowed** mode (not Exclusive Fullscreen)
- OptiScaler files (not included, see below)
- Optional: RivaTuner Statistics Server (frame limiter), an NVIDIA RTX card (DLSS)

## Quick start (users)

1. Download `OptiCaptureHost-<version>-win64.zip` from the [latest release](../../releases/latest) into a folder of its own.
2. Add OptiScaler to the same folder: OptiScaler renamed to `dxgi.dll`, its `OptiScaler.ini`, and the files of your frame generation / upscaler.
3. Optional: `amd_fidelityfx_dx12.dll` (FSR 3/4) and `nvngx_dlss.dll` (DLSS) next to the exes.
4. Start the game in Borderless / Windowed mode, run `OptiCaptureLauncher.exe`, click the game, choose a profile, press **START**.

> The runtimes of AMD, NVIDIA, Intel and OptiScaler are theirs and are **not included**. Get them from their official sources.

## Building from source

You need **Windows**, **Visual Studio 2022** (Desktop development with C++) and **CMake 3.20+**.

```powershell
git clone https://github.com/REPLACE_OWNER/REPLACE_REPO.git
cd REPLACE_REPO
cmake -S . -B build -A x64
cmake --build build --config Release
# build\Release\OptiCaptureHost.exe, build\Release\OptiCaptureLauncher.exe
```

**DLSS support is optional at build time.** NVIDIA's NGX SDK is not stored in this repository (it has its own licence). The GitHub Actions
workflow downloads it from NVIDIA's public repository into `third_party/ngx` before building. If you build locally, copy NVIDIA's headers into
`third_party/ngx/include` and `nvsdk_ngx_s.lib` into `third_party/ngx/lib`; without them CMake builds the host **without DLSS** and the log says so.

**Continuous builds:** every push runs [`build.yml`](.github/workflows/build.yml). On `main` it publishes a release named after
`src/version.h` (tag `vX.Y.Z`); older releases are kept.

## Project layout

```
src/
  main.cpp               host: capture, D3D12 overlay, upscaling passes, FSR/DLSS integration
  launcher.cpp           launcher UI (GDI+), profiles, OptiScaler.ini writer
  settings.h             all settings, OptiCaptureHost.ini load/save
  version.h              the one place for the version number
  upscale_shader.h       FSR 1 (EASU/RCAS) shaders        (AMD, MIT)
  nis_shader.h/_config.h NVIDIA Image Scaling             (NVIDIA, MIT)
  fsr_flow_shader.h      motion vectors, version 1 (block search)
  fsr_flow2_shader.h     per-pixel pass used by version 3
  fsr_flow3_shader.h     motion vectors, version 3 (version 1 + temporal steadiness)
  f5low/                 motion vectors, version 4: dense optical flow + trust mask (see NOTICE.md)
third_party/ffx/         AMD FidelityFX API headers       (MIT)
```

### Where to work

- **Motion estimation** (the biggest quality lever for DLSS / FSR 3): `fsr_flow*_shader.h`. New ideas go in a **new, switchable module**, with the old one kept — see how version 3 is wired (`MotionVersion` in `settings.h`, the launcher selector).
- **Launcher options:** add an id in `launcher.cpp`, draw it in `Paint`, handle it in `OnClick`, store it in `settings.h`.
- Keep `motionVectorScale` at `{1, -1}`; the vectors point from a pixel to where it was in the previous frame.

### Contributing

Pull requests and issues are welcome. Please:
1. Keep changes focused; one feature or fix per PR.
2. Say what you tested (GPU, game, upscaler). The CI only checks that it builds.
3. For quality work attach a short before/after clip and `OptiCaptureHost.log`.
4. Don't add third-party binaries (DLLs from AMD, NVIDIA, Intel, OptiScaler) to the repository.

## Good to know

- **Latency:** the capture adds one hop; VSync off at START gives the lowest latency.
- **Mouse:** with output scaling on, the pointer is not remapped.
- **Temporal upscalers on a capture:** there is no game jitter and no true depth, so DLSS / FSR 3 work from measured motion and look a bit softer than in-game. FSR 4 copes best. This part is experimental.
- **Online games:** capturing and resizing a window can still upset some anti-cheat systems. Use at your own risk.

## Troubleshooting

| Problem | Try |
|---|---|
| Launcher says the host stopped | Open `OptiCaptureHost.log` next to the exes; the last lines say why. |
| Output scaling falls back to FSR 1 | The log names the missing file (`nvngx_dlss.dll` / `amd_fidelityfx_dx12.dll`). |
| Black or frozen picture | The game must be Borderless / Windowed. |
| No frame limiter | Install RivaTuner Statistics Server. |

## Third-party

| Component | Licence |
|---|---|
| FSR 1 EASU/RCAS shader code, FidelityFX API headers (AMD) | MIT |
| NVIDIA Image Scaling (NIS) shader code | MIT |
| `src/f5low/` optical flow, scene-cut and trust-mask code (from [OptiScaler-F5-DLSSNR-Multipass](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass)) | GPL-3.0, see [NOTICE.md](NOTICE.md) |
| NVIDIA NGX / DLSS SDK headers | NVIDIA's licence; **downloaded at build time, not in this repo** |
| OptiScaler, AMD / NVIDIA / Intel runtimes | their own licences; not included |

All trademarks belong to their owners. This project is not affiliated with or endorsed by any of them.

## Support

Telegram **[@MKJH_APPS](https://t.me/MKJH_APPS)** · bugs and ideas: [issues](../../issues) (attach `OptiCaptureHost.log`).

## License

OptiCaptureHost is free software under the **GNU General Public License v3.0** ([LICENSE](LICENSE)), © MK-JH.
Version 4 of the motion vectors includes GPL-3.0 code from OptiScaler-F5-DLSSNR-Multipass, so the whole program is distributed under the GPL-3.0:
you may use, study, change and share it, and anything you distribute that is based on it must stay GPL-3.0 with its source available.
Third-party components keep their own licences (table above).

---

<div align="center">Made by <b>MK-JH</b> · <a href="https://t.me/MKJH_APPS">t.me/MKJH_APPS</a></div>
