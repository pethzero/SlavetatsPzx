# SlavetatsUIPzx

SlaveTats as a C++/SKSE plugin, with a modern in-game tattoo editor.

Built on [SlaveTatsNG](https://github.com/nopse0/SlaveTatsNG) by nopse0: the tattoo logic, the JContainers data and the
Papyrus API are SlaveTatsNG's, so mods that use SlaveTats keep working. SlavetatsUIPzx adds two UIs on top of it:

* **Tattoo Browser** (FLICK): a full-height panel beside your character, with DDS thumbnails, a free camera and live
  preview.
* **Mod Control Panel pages** (SKSE Menu Framework): setup and quick edits.

Both read and write the same SlaveTats records as the MCM (`.SlaveTats.applied`), so everything stays in sync.

> **UI inspired by [slavetats-ui](https://github.com/momoyuki/slavetats-ui) by momoyuki**, the prototype this design
> grew from.
>
> **This build targets Skyrim SE 1.5.97.** Playing another version (AE 1.6.x, VR...)?
> **[Use slavetats-ui instead](https://github.com/momoyuki/slavetats-ui).**

## Download

**[Latest release](https://github.com/pethzero/SlavetatsUIPzx/releases/latest)**: install the zip with your mod
manager. All versions are on the [releases page](https://github.com/pethzero/SlavetatsUIPzx/releases), and what
changed in each one is in the [CHANGELOG](CHANGELOG.md).

## Features

### Tattoo Browser

* **Docked to the left edge**, the character centered in the space beside it. The camera frames the shot by itself
  when the browser opens, so there is nothing to set up first.
* **The world keeps running.** No game menu is opened underneath, so nothing can be clicked or equipped by accident,
  and the game never gets a click while the browser is open.
* **Slot overview**: Body / Face / Hands / Feet tabs, every overlay slot as a card with its thumbnail (2 x 3 per page).
  Empty slots say *Add*; slots used by other mods are shown as *External - Locked*.
* **Slot editor**: group chips (with pattern counts), a search box across every group, and a thumbnail grid of the
  patterns. Below the grid, one row tells you what you picked and what the slot holds now, then Ink / Glow / Glossy,
  then **Save / Replace / Remove / Undo** with **Back** on the right.
* **Preview** (turned on in Setup): hover a pattern and it appears on the character, like trying clothes on. Nothing is
  saved until Save/Replace; Undo, Back or closing the browser puts the saved tattoos back.
* **Camera and character**, with the mouse outside the panel:

  | Mouse | Does |
  |---|---|
  | Left drag | Free camera: orbit around the character, tilt up/down |
  | Right drag | Turn the character |
  | Middle drag | Move the camera left/right/up/down |
  | Wheel | Zoom |

  **Recenter** (next to Refresh) goes back to the starting shot. Closing the browser puts the camera, the zoom and the
  character's heading back where they were.

### Mod Control Panel (SKSE Menu Framework)

* **Setup**: choose the target (the player, or the actor selected in the console), rescan tattoo packs
  (*Add/remove tattoos*), open the Tattoo Browser, set its hotkey, turn Preview on or off.
* **Body / Head / Hand / Foot** pages: slot list with thumbnails and the same editor as the browser.

### Thumbnails

DDS textures are read through the game's resource system, so loose files and BSAs both work, and every BC format
(BC1-BC7) is supported. The browser caches 256 px PNG thumbnails in `Data/SKSE/Plugins/SlavetatsUIPzx/Thumbs/`, so
each texture is decoded once; the first time a group is opened they appear after a moment.

## ⚠ Warnings

Read these before installing. **Back up your saves** the first time you use it.

### Camera

The Tattoo Browser takes over the third-person camera while it is open (framing, orbit, tilt, zoom) and puts it back
when it closes.

* **SmoothCam**: supported. The browser borrows the camera through SmoothCam's own API while it is open and hands it
  back afterwards, so the two don't fight. `SlavetatsUIPzx.log` says `got the SmoothCam interface` when it is found.
* **Other camera mods** (Improved Camera, True Directional Movement's camera options, Customizable Camera, ...) have
  no such API and may still move the camera at the same time: jitter, a drifting camera, or a shot that ends up in the
  wrong place. If that happens, set `bAutoFrame = false`, or turn the other mod's camera features off while browsing.
* **Don't open the browser** in combat, on horseback, while sitting or crafting, or during scenes and animations
  (OStim, SexLab and the like). Those modes own the camera and the character's heading, and the browser fights them.
* **If the camera looks wrong after closing** (stuck to one side, odd zoom), open and close the browser once more,
  switch view (F) twice, or reload. Nothing about the camera is saved, so a reload always fixes it.
* The browser switches to third-person view while it is open, and back to first person on close if you were in it.

### Gameplay

* **The game is not paused** while the browser is open. You can't move or attack (the browser keeps the input), but
  the world keeps running, so don't browse with enemies around.
* **Preview** applies the hovered or selected pattern to the character for real, just without a SlaveTats record.
  **Don't save the game while previewing**: RaceMenu stores overlays in its own co-save, so a previewed pattern would
  be saved with it (opening and closing the browser again cleans it up).
* Don't edit tattoos in the SlaveTats MCM and in the browser at the same time; close one before using the other.

## Requirements

* Skyrim SE **1.5.97**, with [SKSE](https://skse.silverlock.org/). Other versions: use
  [slavetats-ui](https://github.com/momoyuki/slavetats-ui).
* [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
* [JContainers](https://www.nexusmods.com/skyrimspecialedition/mods/16495)
* [RaceMenu](https://www.nexusmods.com/skyrimspecialedition/mods/19080) (SKEE)
* [SlaveTatsNG](https://www.loverslab.com/files/file/35989-slavetatsng/)'s scripts and config files (see Installation)
* SlaveTats tattoo packs (`Data/Textures/Actors/Character/slavetats/*.json`)
* Optional, for the UIs:
  * [FLICK](https://www.nexusmods.com/skyrimspecialedition/mods/181603) (FLICK NG on 1.5.97): the Tattoo Browser
  * [SKSE Menu Framework](https://github.com/QTR-Modding/SKSE-Menu-Framework-3): the Mod Control Panel pages
  * [SmoothCam](https://github.com/mwilsnd/SkyrimSE-SmoothCam): supported, not required

Without FLICK or SKSE Menu Framework the plugin still works, as SlaveTatsNG does.

## Installation

1. Install the requirements.
2. Install the release zip with your mod manager. It contains `SKSE/Plugins/SlavetatsUIPzx.dll`, and its `.pdb` for
   crash logs.
3. **Remove `SlaveTatsNG.dll`**: `SlavetatsUIPzx.dll` contains the whole SlaveTatsNG plugin, and both together would
   load it twice. Keep SlaveTatsNG's other files: its scripts and `Data/SKSE/Plugins/SlaveTatsNG/` (`SlaveTatsNG.ini`,
   `skee_addresses.json`), which this plugin reads.

### Updating

* **From SlavetatsPzx** (before ui-v3-1): remove the old `SlavetatsPzx.dll` and `.pdb`, for the same reason. Your
  browser settings carry over from `SlavetatsPzx.ini`, and the old thumbnail folder `Data/SKSE/Plugins/SlavetatsPzx/`
  can be deleted.
* **From ui-v3 / ui-v3-1**: the fixed `fCameraShift` is gone (the camera frames itself now) and is removed from the ini
  the next time the menu saves it. If you had changed `fOpenZoom`, check it against the default below.

## Usage

1. Open the Mod Control Panel (SKSE Menu Framework, F1 by default) > **SlavetatsUIPzx** > **Setup**.
2. Press **Select Player** (or select an NPC in the console and press **Select Console Target**).
3. Press **Open Tattoo Browser**, or set a hotkey with **Set hotkey** and press it in gameplay.
4. Pick a tab, click a slot, choose a group and a pattern, set the colors, press **Save Tattoo**.

After installing or removing tattoo packs, press **Add/remove tattoos** on the Setup page.

## Settings

`Data/SKSE/Plugins/SlavetatsUIPzx.ini`, written by the menu (with MO2 it ends up in `overwrite`):

```ini
[Browser]
iHotkey = 0              ; DirectInput scan code of the browser hotkey, 0 = none (set it in Setup)
bPreview = false         ; hovering a pattern shows it on the character without saving (Setup checkbox)
bAutoFrame = true        ; center the character in the area right of the browser when it opens
fCameraExtraShift = 0    ; extra sideways camera offset (the whole offset when bAutoFrame = false), negative = other side
fOpenZoom = 0.2          ; zoom the camera starts at: 0 = closest, 1 = farthest, -1 = keep the current zoom
fOpenHeight = -25        ; camera height the browser starts at (negative = lower)
```

**Tuning the starting shot**: too far away, lower `fOpenZoom`; head or feet cut off, change `fOpenHeight`; character
not centered beside the panel, nudge it with `fCameraExtraShift`. **Recenter** uses the same values, so edit the ini,
then reopen the browser to try them.

SlaveTatsNG's own settings stay in `Data/SKSE/Plugins/SlaveTatsNG/SlaveTatsNG.ini`.

## Building

* Visual Studio 2022 or newer, with "Desktop development with C++"
* [vcpkg](https://github.com/microsoft/vcpkg), with `VCPKG_ROOT` set (defaults to `C:\vcpkg`)

CommonLibSSE-NG, DirectXTex and the other dependencies come from the vcpkg manifest (`vcpkg.json`,
`vcpkg-configuration.json`). The first build downloads and builds them, so it needs a working connection to GitHub and
GitLab.

```
build.bat                                        # release -> build\release-msvc\SlavetatsUIPzx.dll
build.bat debug                                  # debug   -> build\debug-msvc
build.bat release "D:\MO2\mods\SlavetatsUIPzx"   # also copy the DLL/PDB + config into a mod folder
```

`build.ps1` takes the same options (`-Preset`, `-Clean`, `-OutputDir`). With `-OutputDir` it also removes an old
`SlavetatsPzx.dll` from the mod folder.

### Source layout

| Path | What |
|---|---|
| `src/tattoo_model.*` | Tattoo catalog, slot state and the SlaveTats actions shared by both UIs |
| `src/tattoo_browser.cpp` | FLICK Tattoo Browser: layout, camera framing and turntable, preview, SmoothCam |
| `src/ui_menu.cpp` | SKSE Menu Framework pages |
| `src/dds_preview.*`, `src/tattoo_thumbs.*` | DDS thumbnails (D3D11 for the menu, PNG cache for FLICK) |
| `extern/FUCK_API.h` | FLICK's plugin API header |
| `extern/SmoothCamAPI.h` | SmoothCam's modder API header (borrowing the camera) |
| `extern/imgui/` | Dear ImGui 1.92.8 headers, types only (FLICK owns the ImGui context) |
| everything else | SlaveTatsNG |

Releases are tagged `ui-vMAJOR-MINOR` (`ui-v4-0`, `ui-v4-1`, ...).

## Credits

* [slavetats-ui](https://github.com/momoyuki/slavetats-ui) by momoyuki: the UI prototype this design is inspired by
* [SlaveTatsNG](https://github.com/nopse0/SlaveTatsNG) by nopse0, and the original SlaveTats
* FLICK by Fuzzles
* [SKSE Menu Framework](https://github.com/QTR-Modding/SKSE-Menu-Framework-3) by QTR-Modding
* [SmoothCam](https://github.com/mwilsnd/SkyrimSE-SmoothCam) by mwilsnd, for its modder API (`extern/SmoothCamAPI.h`)
* [Fitting Room](https://www.nexusmods.com/skyrimspecialedition/mods/185342), whose editor inspired the browser's
  layout and whose notes on FLICK's quirks saved a lot of time (no code was copied)
* [Dear ImGui](https://github.com/ocornut/imgui) (MIT), [DirectXTex](https://github.com/microsoft/DirectXTex) (MIT)
* [VMTHook](https://github.com/gfreivasc/VMTHook)

## License

[Apache-2.0](LICENSE), same as SlaveTatsNG. `extern/imgui` is MIT ([LICENSE.txt](extern/imgui/LICENSE.txt)).
