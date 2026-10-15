# Changes

## SlavetatsUIPzx UI v5.1 (tag `ui-v5-1`)

* README: a note that Fitting Room's Shape and Overlays may cause problems next to this mod on SE 1.5.97 (both go
  through RaceMenu 0.4.16's older overlay functions). A precaution: nothing crashed in testing.
* **Crash while hovering presets.** Each preview redrew every overlay slot one by one (a full overlay update per
  slot), many times a second while the mouse moved; the crash logs point to memory corrupted under that load.
  Previews now:
  * only change the slots whose picture changes, in one overlay update;
  * wait for the mouse to settle (0.25 s for a whole look) and update at most four times a second, dropping
    requests that were already replaced;
  * take SlaveTats' overlay lock like a synchronize does, so the two never overlap.
* Moving off a preset, or switching tabs, puts back only the slots that differ instead of clearing and redrawing
  every overlay; the full redraw happens once, when the browser closes.
* **Pause world** (Setup checkbox, on by default, `bPauseWorld`): the world stops while the browser is open and your
  character keeps moving, like behind the inventory with Show Player In Inventory. An empty pausing menu of ours is
  opened under the browser (nothing in it to click). The pause stops the character too, so while paused the
  browser runs the character's animation itself and updates the camera by hand.
* The HUD and its "E  Take ..." prompt for whatever the camera points at are hidden while the browser is open: every
  open game menu except the cursor is faded out (TrueHUD's bars too), and the crosshair is told it points at
  nothing, so HUD mods' item cards (InfinityUI, moreHUD...) go away too.

## SlavetatsUIPzx UI v5.0 (tag `ui-v5-0`)

* **Presets**: a new **PRESET** tab in the Tattoo Browser saves a whole look (every SlaveTats tattoo in every area, with
  its slot, ink, glow and gloss) into one of **12 preset slots**, and **Load** puts it back on. Loading replaces the
  current tattoos; **Undo Load** puts them back. Rename and Delete too.
* **Try a preset on by hovering it**: with Preview on, the whole look shows on the character while the mouse is over
  a preset (one of yours or a shared one), without saving anything.
* **Presets page** in the Mod Control Panel, next to Body / Head / Hand / Foot: the same presets and actions.
* **Shared presets**: presets are JSON files (`Data/SKSE/Plugins/SlavetatsUIPzx/Presets/Preset01.json` ...
  `Preset12.json`) you can send to someone. Files dropped in `Presets/Shared/` show in the tab, ready to load or copy
  into a slot. Tattoos from packs you don't have are marked *not installed* and skipped.
* Locked tattoos and other mods' overlays are never saved in a preset, and loading never takes them off.

## SlavetatsUIPzx UI v4.1 (tag `ui-v4-1`)

* **Setup > Camera**, like Show Player In Inventory's settings: live sliders for the browser's camera (Offset X / Y / Z,
  Zoom, FOV, Rotation Z around the character, Rotation X tilt), with **Save Settings** and **Reset Defaults**. The browser opens at these values and Recenter returns to
  them. Defaults: X -72.5, Y 80, Z -13.5, zoom 0.2, FOV 55, rotation Z -20, rotation X 10, Preview on.
* The automatic framing from v4.0 is gone in favour of these fixed offsets; its ini keys (`bAutoFrame`,
  `fCameraExtraShift`, `fOpenHeight`, `fCameraShift`) are replaced by `fCameraOffsetX/Y/Z`, `fCameraFOV`, `fCameraRotation` and `fCameraPitch`.
* **Fixed**: the pattern grid shaking. Its scrollbar appeared and disappeared every frame (the column count follows
  the width the scrollbar takes); the grid now always keeps it, and the editor area never scrolls.

## SlavetatsUIPzx UI v4.0 (tag `ui-v4-0`)

* **Camera framing on open**: the character is centered in the screen area right of the browser, worked out from the
  panel width, the FOV and the camera distance, so there is nothing to adjust before browsing. Starts at a full-body
  zoom and height (`fOpenZoom`, `fOpenHeight`); `bAutoFrame`, `fCameraExtraShift` replace the fixed `fCameraShift`.
* **Recenter** button next to Refresh: back to the starting shot after orbiting, panning or zooming.
* **Slot editor footer**: the selected pattern and the slot's status share one row, and **Back** (was `< All slots`) sits on the right of the action row, freeing a row for the pattern grid.
* **Group chips**: the strip is sized to whole rows (up to 3, then it scrolls), so the last row is never cut off, with
  a divider before the patterns.
* README: credits [slavetats-ui](https://github.com/momoyuki/slavetats-ui) by momoyuki as the UI's inspiration, and
  points players on other game versions to it (this build targets 1.5.97).

## SlavetatsUIPzx UI v3.1 (tag `ui-v3-1`)

* **Renamed SlavetatsPzx -> SlavetatsUIPzx**: the DLL (`SlavetatsUIPzx.dll`), the menu section, the settings file
  (`Data/SKSE/Plugins/SlavetatsUIPzx.ini`, read from the old `SlavetatsPzx.ini` until it exists), the thumbnail cache
  and the GitHub repository. Remove the old `SlavetatsPzx.dll` when updating; `build.ps1 -OutputDir` does it for you.
* **SmoothCam compatibility**: the Tattoo Browser borrows the camera through SmoothCam's API while it is open and
  hands it back (sent to its goal position) on close, instead of fighting it every frame.
* README: warnings about camera mods, scenes, Preview and saving, and the rename.
## SlavetatsPzx UI v3 (tag `ui-v3`)

* **Tattoo Browser** on FLICK: a full-height panel docked to the left edge, the character beside it, the world still
  running. No game menu underneath, and the game never gets a click while it is open.
* Free camera outside the panel: left drag orbits and tilts, right drag turns the character, middle drag moves the
  camera, wheel zooms. Everything is restored on close.
* **Preview** (Setup page): hovering a pattern shows it on the character without saving, like trying clothes on.
* Thumbnails in the browser: DDS decoded with DirectXTex (BC1-BC7) into a PNG cache that FLICK loads.
* Hotkey for the browser (Setup > Set hotkey), saved in `Data/SKSE/Plugins/SlavetatsPzx.ini`.
* Shared `tattoo_model` behind both UIs.
## SlavetatsPzx UI v2 (tag `ui-v2`)

In-game menu (SKSE Menu Framework) redesign, on top of SlaveTatsNG 0.8.2:

* Body/Head/Hand/Foot pages are a master-detail layout: a compact slot list (thumbnail, applied pattern, status dot) and an editor for the selected slot.
* Groups are pill buttons with pattern counts, plus a search box across every group.
* Patterns are a thumbnail grid that adapts to the window width; selected and applied patterns are marked, hovering shows name, group, credit and texture path.
* DDS thumbnails (`dds_preview`): read through the game's resource system (loose files and BSAs), BC1-BC7 and common uncompressed formats, nearest mip to 256 px, decoded on a worker thread, LRU-cached on the GPU. Shown on a light gray checkerboard so black ink stays visible.
* Ink/Glow use a compact popup (SlaveTats MCM palette plus a fixed-size picker) instead of ImGui's font-sized color picker.

## SlavetatsPzx UI v1 (tag `ui-v1`)

* First SKSE Menu Framework menu: Setup / Body / Head / Hand / Foot pages with section, pattern, ink, glow and glossy per slot, Save / Replace / Remove, player or console target.

## 0.8.0

* The boolean field ``gloss`` in tattoos isn't used anymore, it has been replaced by two new float fields, ``glossiness`` and ``specularstrength``.

* Three other new fields have been added to tattoos: ``specularstrength``, ``emissivemult`` and ``glowtexture``.

* Besides the old ``apply_overlay`` function in ``SlaveTats.psc``, a new function ``apply_overlay_ex`` has been added to ``SlaveTatsNG.psc``:

  ```
  bool function apply_overlay_ex(Actor target, bool isFemale, string area, int slot, string path, int color, int glow, float glossiness = 0.0, string bump_path = "", float alpha = 1.0, float specular = 0.0, string glow_path = "", float emissive_mult = 1.0) global native
  ```
  It contains the above mentioned ``gloss`` change and the three new fields for the NiOverride parameters ``SpecularStrength``, ``EmissiveMult`` and ``Texture[glow/detail]``.
  The old Papyrus function in ``SlaveTats.psc`` with boolean ``gloss`` parameter still exists; if ``gloss = true``, it calls ``apply_overlay_ex`` with hardcoded ``glossiness = 5.0`` and ``specularstrength = 5.0`` parameters.

  ```
  bool function apply_overlay(Actor target, bool isFemale, string area, int slot, string path, int color, int glow, bool gloss, string bump = "", float alpha = 1.0) global native
  ```
  Note: ``apply_overlay/apply_overlay_ex`` only apply NiOverride overlays, they don't change the SlaveTats JContainer tattoos, so they shouldn't be called directly. Instead edit the SlaveTats tattoos with JContainer or SlaveTats functions, and then call ``synchronize_tattoos``.

* The tattoo version (``.SlaveTats.version``) of actors is changed from ``1.0`` to ``2.0``.