# SlavetatsPzx

SlaveTats as a C++/SKSE plugin, with a modern in-game tattoo editor.

Built on [SlaveTatsNG](https://github.com/nopse0/SlaveTatsNG) by nopse0: the tattoo logic, the JContainers data and the
Papyrus API are SlaveTatsNG's, so mods that use SlaveTats keep working. SlavetatsPzx adds two UIs on top of it:

* **Mod Control Panel pages** (SKSE Menu Framework) for setup and quick edits.
* **Tattoo Browser** (FLICK), a full-height panel beside your character with DDS thumbnails, a free camera and live preview.

Both read and write the same SlaveTats records as the MCM (`.SlaveTats.applied`), so everything stays in sync.

## Features

### Tattoo Browser

* Docked to the left edge of the screen, the character stands beside it. The world keeps running; no game menu is
  opened underneath, so nothing can be clicked or equipped by accident.
* Body / Face / Hands / Feet tabs, slot overview with thumbnails (2 x 3 per page), then a slot editor with
  group chips, search across every group, and a thumbnail grid of the patterns.
* Ink, Glow and Glossy per slot, Save / Replace / Remove / Undo. Locked tattoos and slots used by other mods are
  shown but can't be edited.
* **Preview** (Setup page): hover a pattern and it appears on the character, like trying clothes on. Nothing is saved
  until Save/Replace; Undo, leaving the slot or closing the browser puts the saved tattoos back.
* Camera and character controls, outside the panel:

  | Mouse | Does |
  |---|---|
  | Left drag | Free camera: orbit around the character, tilt up/down |
  | Right drag | Turn the character |
  | Middle drag | Move the camera left/right/up/down |
  | Wheel | Zoom |

  Closing the browser puts the camera, zoom and the character's heading back where they were.

### Mod Control Panel (SKSE Menu Framework)

* **Setup**: choose the target (player, or the actor selected in the console), rescan tattoo packs
  ("Add/remove tattoos"), open the Tattoo Browser, set its hotkey, toggle Preview.
* **Body / Head / Hand / Foot** pages: slot list with thumbnails and the same editor as the browser.

### Thumbnails

DDS textures are read through the game's resource system, so loose files and BSAs both work, and all BC formats
(BC1-BC7) are supported. The browser caches 256 px PNG thumbnails in `Data/SKSE/Plugins/SlavetatsPzx/Thumbs/`, so
each texture is decoded once.

## Requirements

* Skyrim SE 1.5.97 / AE, with [SKSE](https://skse.silverlock.org/)
* [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
* [JContainers](https://www.nexusmods.com/skyrimspecialedition/mods/16495)
* [RaceMenu](https://www.nexusmods.com/skyrimspecialedition/mods/19080) (SKEE)
* [SKSE Menu Framework](https://github.com/QTR-Modding/SKSE-Menu-Framework-3): for the Mod Control Panel pages
* [FLICK](https://www.nexusmods.com/skyrimspecialedition/mods/181603) (FLICK NG on 1.5.97): for the Tattoo Browser
* SlaveTats tattoo packs (`Data/Textures/Actors/Character/slavetats/*.json`)

The menu and the browser are optional: without SKSE Menu Framework or FLICK the plugin still works as SlaveTatsNG.

## Installation

1. Install the requirements.
2. Install the release zip with your mod manager. It contains `SKSE/Plugins/SlavetatsPzx.dll` (and its `.pdb` for
   crash logs).
3. `SlavetatsPzx.dll` **replaces** `SlaveTatsNG.dll`. Don't install both, or the plugin loads twice. Keep
   SlaveTatsNG's other files (scripts and `Data/SKSE/Plugins/SlaveTatsNG/SlaveTatsNG.ini`, `skee_addresses.json`),
   which this plugin reads too.

## Usage

1. Open the Mod Control Panel (SKSE Menu Framework, F1 by default) > **SlavetatsPzx** > **Setup**.
2. Press **Select Player** (or select an NPC in the console and press **Select Console Target**).
3. Press **Open Tattoo Browser**, or set a hotkey with **Set hotkey** and press it in gameplay.
4. Pick a tab, click a slot, choose a group and a pattern, set the colors, press **Save Tattoo**.

After installing or removing tattoo packs, press **Add/remove tattoos** on the Setup page.

## Settings

`Data/SKSE/Plugins/SlavetatsPzx.ini` (written by the menu; with MO2 it ends up in `overwrite`):

```ini
[Browser]
iHotkey = 0           ; DirectInput scan code of the browser hotkey, 0 = none
fCameraShift = 60     ; sideways camera offset while the browser is open; negative = other side, 0 = off
bPreview = false      ; hovering a pattern shows it on the character without saving
```

SlaveTatsNG's own settings stay in `Data/SKSE/Plugins/SlaveTatsNG/SlaveTatsNG.ini`.

## Building

* Visual Studio 2022 or newer, with "Desktop development with C++"
* [vcpkg](https://github.com/microsoft/vcpkg), with `VCPKG_ROOT` set (defaults to `C:\vcpkg`)

CommonLibSSE-NG, DirectXTex and the other dependencies come from the vcpkg manifest (`vcpkg.json`,
`vcpkg-configuration.json`). The first build downloads and builds them.

```
build.bat                                      # release -> build\release-msvc\SlavetatsPzx.dll
build.bat debug                                # debug   -> build\debug-msvc
build.bat release "D:\MO2\mods\SlavetatsPzx"   # also copy the DLL/PDB + config into a mod folder
```

`build.ps1` takes the same options (`-Preset`, `-Clean`, `-OutputDir`).

### Source layout

| Path | What |
|---|---|
| `src/tattoo_model.*` | Tattoo catalog, slot state and the SlaveTats actions shared by both UIs |
| `src/ui_menu.cpp` | SKSE Menu Framework pages |
| `src/tattoo_browser.cpp` | FLICK Tattoo Browser, camera and turntable, preview |
| `src/dds_preview.*`, `src/tattoo_thumbs.*` | DDS thumbnails (D3D11 for the menu, PNG cache for FLICK) |
| `extern/FUCK_API.h` | FLICK's plugin API header |
| `extern/imgui/` | Dear ImGui 1.92.8 headers, types only (FLICK owns the ImGui context) |
| everything else | SlaveTatsNG |

## Credits

* [SlaveTatsNG](https://github.com/nopse0/SlaveTatsNG) by nopse0, and the original SlaveTats
* [SKSE Menu Framework](https://github.com/QTR-Modding/SKSE-Menu-Framework-3) by QTR-Modding
* FLICK by Fuzzles
* [Fitting Room](https://www.nexusmods.com/skyrimspecialedition/mods/185342), whose editor inspired the browser's
  layout and whose notes on FLICK's quirks saved a lot of time (no code was copied)
* [Dear ImGui](https://github.com/ocornut/imgui) (MIT), [DirectXTex](https://github.com/microsoft/DirectXTex) (MIT)
* [VMTHook](https://github.com/gfreivasc/VMTHook)

## License

[Apache-2.0](LICENSE), same as SlaveTatsNG. `extern/imgui` is MIT ([LICENSE.txt](extern/imgui/LICENSE.txt)).
