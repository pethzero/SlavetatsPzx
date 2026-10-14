#pragma once

// Tattoo data and actions shared by the in-game UIs (the SKSE Menu Framework pages in ui_menu.cpp and the
// FLICK Tattoo Browser in tattoo_browser.cpp).
//
// Threading: everything here except the game-thread internals is UI-thread state. Both UIs draw from the
// D3D Present hook, so they share one thread. SlaveTats/JContainers/NiOverride work runs on the game thread
// (SKSE task) and its results come back through DrainQueue(), which each UI calls once per frame.

namespace slavetats_ng
{
	namespace tattoo_model
	{
		using std::string;

		inline constexpr int         kAreaCount = 4;
		inline constexpr const char* kAreas[kAreaCount] = { "Body", "Face", "Hands", "Feet" };
		inline constexpr const char* kPageNames[kAreaCount] = { "Body", "Head", "Hand", "Foot" };
		inline constexpr const char* kNoTattoo = "[No Tattoo]";

		inline bool IEquals(const string& a_lhs, const string& a_rhs)
		{
			return _stricmp(a_lhs.c_str(), a_rhs.c_str()) == 0;
		}

		struct ILess
		{
			bool operator()(const string& a_lhs, const string& a_rhs) const
			{
				return _stricmp(a_lhs.c_str(), a_rhs.c_str()) < 0;
			}
		};

		// ------------------------------ Data -----------------------------------------------------------------

		struct TattooDef
		{
			string name;
			string credit;
			string texture;  // relative to Data\Textures\Actors\Character\slavetats
			int    color = 0;
			int    glow = 0;
			bool   glossy = false;
		};
		using Sections = std::map<string, std::vector<TattooDef>, ILess>;

		struct Catalog
		{
			std::array<Sections, kAreaCount> areas;
			std::size_t                      count = 0;
			bool                             loaded = false;
		};

		enum class SlotKind
		{
			kEmpty,
			kTattoo,
			kExternal  // overlay used by another mod (not a SlaveTats texture)
		};

		// What is applied to the target in one overlay slot (from .SlaveTats.applied)
		struct SlotState
		{
			SlotKind kind = SlotKind::kEmpty;
			string   section;
			string   name;
			int      color = 0;
			int      glow = 0;
			bool     glossy = false;
			bool     locked = false;
		};

		// A slot as shown in the UI: applied state + pending edits
		struct Slot
		{
			SlotState applied;
			string    section;  // pending selection, empty = [No Tattoo]
			string    pattern;
			string    browse;  // group shown in the pattern grid (doesn't change the selection)
			float     ink[3] = { 0.0f, 0.0f, 0.0f };
			float     glow[3] = { 0.0f, 0.0f, 0.0f };
			bool      glossy = false;
			string    error;
		};

		enum class TargetMode
		{
			kPlayer,
			kConsole,
			kCurrent
		};

		// ------------------------------ UI thread state ------------------------------------------------------

		extern RE::FormID                                TargetID;
		extern string                                    TargetName;
		extern bool                                      HasTarget;
		extern Catalog                                   TattooCatalog;
		extern std::array<std::vector<Slot>, kAreaCount> Slots;
		extern std::array<bool, kAreaCount>              SlotsKnown;
		extern int                                       Working;  // game thread tasks in flight
		extern string                                    Status;

		// ------------------------------ Frame pump -----------------------------------------------------------

		// Applies finished game-thread work to the state above. Call once per frame from the UI thread.
		void DrainQueue();

		// ------------------------------ Helpers --------------------------------------------------------------

		void ToFloat(int a_rgb, float a_out[3]);
		int  ToRGB(const float a_color[3]);
		bool JContainersReady();
		int  TotalSlots();
		bool IContains(const string& a_text, const char* a_needle);

		// Path SlaveTats puts on the overlay, relative to Data\Textures
		string PreviewPath(const TattooDef& a_tattoo);

		const TattooDef* FindTattoo(int a_area, const string& a_section, const string& a_name);
		const TattooDef* Selected(int a_area, const Slot& a_slot);

		// Pending edits differ from what is applied
		bool HasChanges(const Slot& a_slot);

		// Pending edits = what is applied
		void ResetPending(Slot& a_slot);

		// Picking a pattern loads its default colors, like the MCM does when the tattoo is added
		void SelectTattoo(Slot& a_slot, const string& a_section, const TattooDef* a_tattoo);

		// ------------------------------ Actions (queued to the game thread) ----------------------------------

		void LoadTarget(TargetMode a_mode);
		void CompileTattoos();  // "Add/remove tattoos": rescan the tattoo packs
		void ApplySlot(int a_area, int a_index);
		void RemoveSlot(int a_area, int a_index);

		// One tattoo of a whole look (a preset): which slot it was in and how it was colored
		struct LookTattoo
		{
			int    area = 0;
			int    slot = -1;  // the slot it goes back to when that one is free, else the first free slot
			string section;
			string name;
			int    color = 0;
			int    glow = 0;
			bool   glossy = false;
		};

		// What the target wears now, from the slot states (all four areas must be known). Locked tattoos and
		// other mods' overlays are left out: a look never takes them off, so it doesn't carry them either.
		std::vector<LookTattoo> CurrentLook();

		// Takes off every SlaveTats tattoo the target wears (except locked ones) and puts a_look on instead, then
		// a_done runs on the UI thread with how many went on and a line for each one that didn't.
		void ApplyLook(std::vector<LookTattoo> a_look, std::function<void(int a_applied, std::vector<string> a_failed)> a_done);

		// ------------------------------ Preview --------------------------------------------------------------
		// Shows a slot's pending selection (pattern, ink, glow, gloss) on the target WITHOUT touching the SlaveTats
		// records: the overlay is applied directly. EndPreview() puts every slot back to what is saved (a forced
		// synchronize_tattoos). Saving or removing ends a preview on its own, since both synchronize.

		// a_tattoo: the pattern to show (e.g. the one under the mouse), nullptr = the slot's pending selection.
		// Ink/glow/gloss come from the slot's pending edits. UI thread.
		void PreviewSlot(int a_area, int a_index, const TattooDef* a_tattoo = nullptr);
		// A whole look (a preset) on the target, in the slots ApplyLook would put it in, without touching the records.
		// Slots the look doesn't use are shown empty; locked slots and other mods' overlays are left alone. UI thread.
		void PreviewLook(const std::vector<LookTattoo>& a_look);
		void EndPreview();                          // any thread; no-op when nothing is previewed
		bool IsPreviewing();
	}
}
