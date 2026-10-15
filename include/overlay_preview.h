#pragma once

// Preview without RaceMenu: shows a texture on one of RaceMenu's overlay shapes ("Body [Ovl3]") by swapping the shape's
// material directly, the way RaceMenu itself applies a texture override, and remembers what was there so it can be put
// back. Nothing goes through RaceMenu's functions and nothing is stored in its co-save.
//
// On RaceMenu 0.4.16 (SE) SlaveTatsNG reaches RaceMenu through its legacy address tables, and calling them many times a
// second (hovering) damaged the game's string pool. Saving, Loading a preset and Remove still go through SlaveTats.
//
// ⚠ Show / RestoreAll are NOT used: tried as the preview (2026-10-15) and reverted - with it, tattoos just saved
// stopped showing. The preview goes through RaceMenu again (tattoo_model's painter). Only LogShapes is used, by
// Setup > Log overlays.
//
// Game thread only.

namespace slavetats_ng
{
	namespace overlay_preview
	{
		// a_texture: path under Data\Textures ("Actors\Character\slavetats\x.dds"), empty = show the slot as empty.
		// Colors are 0xRRGGBB. Returns false when the actor has no such overlay shape (RaceMenu hasn't built it).
		bool Show(RE::Actor* a_actor, const char* a_area, int a_slot, const std::string& a_texture, int a_color, int a_glow, bool a_gloss);

		// Puts every shape changed since the last restore back as it was; false when there was none
		bool RestoreAll();

		// Something redrew the overlays for real (a synchronize): what was saved is no longer what is there
		void Forget();

		// Diagnostic: logs what each overlay shape of an area really shows (diffuse texture, alpha, culled), slots
		// 0 .. a_count-1, and whether a preview currently holds it
		void LogShapes(RE::Actor* a_actor, const char* a_area, int a_count);
	}
}
