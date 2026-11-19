# Notice

OptiCaptureHost is © MK-JH (licence: [LICENSE](LICENSE)). The source repository and `OptiCaptureHost-<version>-win64.zip` contain only OptiCaptureHost's own
code and the components under "In the source" below.

## In the source

- FSR 1 shader code and the FidelityFX API headers (`third_party/ffx`): AMD, MIT.
- NVIDIA Image Scaling shader code: NVIDIA, MIT.
- The GitHub mark on the launcher's "Source code" button is the Octicons `mark-github` shape from GitHub (https://primer.style/foundations/icons, MIT). GitHub and the GitHub logo are trademarks of GitHub, Inc.; the button only links to this project's repository.
- NVIDIA NGX / DLSS SDK headers: NVIDIA's licence; downloaded at build time and not part of this repository.

## OptiScaler (not included in the small zip)

OptiScaler is not part of this repository. It is used as `dxgi.dll` and has to be downloaded by the user from the
[OptiScaler-F5-DLSSNR-Multipass](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass) repository (janblade's fork of OptiScaler), which is licensed under the **GNU General Public License v3.0**. It is used unmodified by
OptiCaptureHost; only the file name is `dxgi.dll`. Their licence texts, copyright notices and credits are in those repositories
(and in that fork's `docs/CREDITS.md`) and apply to those files.

## Full package (third-party files next to the release)

The optional full package contains the third-party files below **unmodified**, together with their licence texts (folder `licenses` in the package). They are not
written or licensed by MK-JH; each keeps its own licence and copyright, and OptiCaptureHost's licence does not apply to them. If you own one of them and want it
removed or its notice changed, open an issue.

| File | What it is | Licence / source |
|---|---|---|
| `dxgi.dll`, `OptiScaler.ini` | OptiScaler (janblade fork) | GPL-3.0, https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass |
| `amd_fidelityfx_dx12.dll`, FSR 4 files | AMD FidelityFX upscaler (FSR 3 / FSR 4). `amd_fidelityfx_dx12.dll` is AMD's `amd_fidelityfx_upscaler_dx12.dll` only renamed, contents unchanged | AMD's licence (in the FidelityFX SDK package), official download: https://gpuopen.com/download-fsr-sdk/ |
| `nvngx_dlss.dll` | NVIDIA DLSS Super Resolution | NVIDIA RTX SDKs licence (`licenses/NVIDIA-RTX-SDK-LICENSE_DLSS.txt`, from https://github.com/NVIDIA/DLSS): distributed unmodified, only together with this application, never stand-alone; the licence must stay with the files; no NVIDIA endorsement is claimed |
| NVIDIA Streamline files | NVIDIA Streamline | MIT (`licenses/NVIDIA-Streamline-LICENSE_MIT.txt`, from https://github.com/NVIDIAGameWorks/Streamline), keep the copyright notice |
| Intel XeSS files (upscaler and frame generation) | Intel XeSS | Intel Simplified Software License, binary and unmodified, from Intel's official SDK |
| `dlss-enabler-headless.dll` | DLSS Enabler (headless), by its author | permission page: https://www.nexusmods.com/site/mods/757 (credit the author, do not modify, not for sale); built on dlssg-to-fsr3 (Nukem9), OptiScaler and others |

### Not included: download these yourself

These files are **not** in the package or the repository. Download them from their own sources and put them next to the program:

| File | Where to get it |
|---|---|
| DittoSR (folder `DittoSR`) | https://github.com/Vancete/DittoSR/releases (check the licence there) |
| `nvngx_dlssnr.dll` (DLSS 5 runtime), optional, version 310.9.16, needed only for DLSS 5 | NVIDIA has no public download for it; get it yourself from a source you trust and check that you may use it. It is not distributed here. |

This product uses NVIDIA DLSS. NVIDIA, NVIDIA RTX, DLSS and GeForce are trademarks of NVIDIA Corporation.

All trademarks belong to their owners. This project is not affiliated with or endorsed by any of them.
