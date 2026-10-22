# QuarryDLSS

NVIDIA **DLSS Super Resolution** and **DLSS Frame Generation** for **The Quarry** (PC, Steam
build 11105655, Unreal Engine 4.26), as a ReShade add-on. It replaces the game's own temporal
upscaler with DLSS, following the approach of
[days-gone-dlss](https://github.com/teemupynnonen/days-gone-dlss), and can add generated
frames on top through NVIDIA Streamline.

Version **1.1.1**.

## Requirements

- An NVIDIA RTX graphics card and a current driver. Frame generation needs an RTX 40 or 50
  series card; 3x, 4x and Dynamic need an RTX 50 series card.
- [ReShade](https://reshade.me) **6.8 or newer, the version with full add-on support**.
- The game running on **DirectX 12**: add `-dx12` to its Steam launch options.
- For frame generation: **Hardware-accelerated GPU scheduling** on (Windows Settings >
  System > Display > Graphics).

## Install

1. Install ReShade with add-on support for
   `The Quarry\SMG026\Binaries\Win64\TheQuarry-Win64-Shipping.exe` (not the
   `TheQuarry.exe` launcher). Choose DirectX 10/11/12. No effect packs are needed.
2. Copy **all the files at the top level of the zip** into that same `Win64` folder:
   - `QuarryDLSS.addon64`, the add-on;
   - `nvngx_dlss.dll`, NVIDIA's DLSS Super Resolution runtime (310.9.1);
   - `nvngx_dlssg.dll`, `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`,
     `sl.reflex.dll` and `sl.pcl.dll`, NVIDIA's DLSS Frame Generation runtime (Streamline
     2.14.1). They're only loaded while frame generation is on.

   The `licenses` folder and the text files are for reference; they don't need copying.
3. In Steam, set the launch options to `-dx12`.
4. **In the NVIDIA App, turn off any DLSS Super Resolution model override for The
   Quarry.** It would replace the model this add-on asks for (see Known limits).

Upgrading from 1.0.0 or 1.1.0: copy the new files over the old ones. Your settings carry over,
except "Stabilise screen-space lighting", which 1.1.1 turns off for everyone (see Changes).

## Use

Press **Home** in game to open ReShade, then open the **Add-ons** tab and the
**QuarryDLSS** section:

- **DLSS Super Resolution**: on or off. Off gives you the game's own anti-aliasing at
  native resolution.
- The status line shows whether DLSS is active and at what resolutions.
- **Quality mode**: Ultra Quality (77%), Quality (67%), Balanced (58%), Performance
  (50%) or Ultra Performance (33%) of the output resolution per axis.
- **Frame generation**: Off, 2x, 3x, 4x or Dynamic. 2x to 4x present each rendered frame
  plus one to three generated ones. Dynamic picks the smallest multiplier that reaches
  your refresh rate, which is usually the best choice. Frame generation needs DLSS Super
  Resolution on; the line under it says whether it's active. **Turning it on from Off
  needs a game restart**; changing the mode or turning it off works straight away.
- **Advanced**: the DLSS model (J is recommended), a custom render scale, and
  "Stabilise screen-space lighting" (off by default; only useful with the SSGI/SSR pak mods).
- **Diagnostics**: for troubleshooting and bug reports only.

Settings are saved in `ReShade.ini` under `[QuarryDLSS]`.

## Known limits

- DirectX 12 only.
- Frame generation pauses for a moment on camera cuts, loading screens, window resizes
  and fullscreen switches (F11), and while DLSS Super Resolution is off.
- The frame generation model is NVIDIA's default, or the NVIDIA App's frame generation
  override if you've set one. DLSS Frame Generation has no presets to choose.
- The ReShade overlay itself is drawn before frame generation, so it's interpolated too.
- DLSS model **K**, which NVIDIA uses by default for Quality, smears some close-up
  cutscene shots, so the add-on asks for **J**. An NVIDIA App or driver model override
  for the game replaces that choice.
- Some cutscene cuts briefly flash white. The game does this itself, and it happens with
  DLSS off too.
- During loading and on camera-cut frames, the game's own upscaler is used for a frame.

## Changes in 1.1.1

- **Fixed a crash at startup** ("AllocateTexture ... DXGI_ERROR_DEVICE_REMOVED") when the
  game's scene colour isn't 16-bit float, for example with lower graphics settings or mods
  that change the scene colour format. The add-on now reads the game's textures in their own
  formats, and refuses a format it can't read instead of crashing.
- "Stabilise screen-space lighting" is now **off by default**, and switched off once for
  everyone upgrading. Turn it back on under Advanced if you use the SSGI/SSR mods.

## Reporting a problem

Send `QuarryDLSS.log` (and `QuarryDLSS.prev.log`) from the `Win64` folder, plus a
screenshot. In Diagnostics, **Verbose log** adds motion statistics, Streamline's own log
lines and frame pacing, which helps with smearing, ghosting or frame generation reports.

## Building (developers)

```powershell
.\build.ps1                     # -> bin\QuarryDLSS.addon64
.\build.ps1 -Test               # also builds and runs the DXBC parser tests
.\build.ps1 -Install            # also copies it (and nvngx_dlss.dll) into the game folder
.\build.ps1 -InstallStreamline  # copies the frame generation runtime into the game folder
.\build.ps1 -Package            # -> bin\release\QuarryDLSS-<version>.zip
```

Building needs Visual Studio 2022 or newer with the C++ x64 tools, and the Windows SDK's
`fxc.exe`. MinGW builds crash inside ReShade: MinGW returns API structs with a
different calling convention than MSVC. The NVIDIA DLLs for `-Install`, `-InstallStreamline`
and `-Package` come from `bin\x64` of the
[Streamline 2.14.1 release](https://github.com/NVIDIA-RTX/Streamline/releases), copied to
`third_party\streamline\bin\x64` (or pass `-StreamlineBin`). They aren't in this repository.

How the renderer was mapped, and every finding along the way, is in `docs/FINDINGS.md`.
`tools/analyze_capture.py` analyses the texture captures from the Diagnostics section.
It needs numpy and Pillow.

## Credits

This software uses NVIDIA DLSS and NVIDIA Streamline. NVIDIA, DLSS and Reflex are
trademarks of NVIDIA Corporation. See THIRD_PARTY_NOTICES.txt.
