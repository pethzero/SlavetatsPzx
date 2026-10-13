# Changes

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