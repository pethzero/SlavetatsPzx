#pragma once

#include "tattoo_model.h"

// Presets: a whole look (every SlaveTats tattoo the target wears, with its slot and colors) saved to a JSON file.
//
//   Data/SKSE/Plugins/SlavetatsUIPzx/Presets/Preset01.json ... Preset12.json   the 12 slots of the PRESET tab
//   Data/SKSE/Plugins/SlavetatsUIPzx/Presets/Shared/*.json                    presets from other people (or mods)
//
// A preset only names its tattoos (section + name), so it works with anyone who has the same tattoo packs.
// Everything here is UI-thread state; loading goes through tattoo_model::ApplyLook (game thread).

namespace slavetats_ng
{
	namespace tattoo_presets
	{
		using std::string;

		inline constexpr int kSlotCount = 12;

		struct Preset
		{
			string                               name;
			string                               character;  // who wore it when it was saved
			string                               saved;      // local time, "YYYY-MM-DD HH:MM"
			std::vector<tattoo_model::LookTattoo> tattoos;
		};

		struct PresetFile
		{
			std::filesystem::path path;
			string                file;    // file name, for display
			bool                  exists = false;
			string                error;   // the file is there but can't be read
			Preset                preset;
		};

		extern std::array<PresetFile, kSlotCount> Mine;
		extern std::vector<PresetFile>            Shared;
		extern string                             Message;  // result of the last action, shown under the buttons
		extern bool                               MessageIsError;

		string Folder();
		string SharedFolder();

		// Reads both folders again (small files: done on the UI thread when the tab opens or on Refresh)
		void Rescan();

		// Saves what the target wears now into slot a_slot (0-11) as a_name
		bool SaveCurrent(int a_slot, const string& a_name);
		bool Rename(int a_slot, const string& a_name);
		bool Delete(int a_slot);
		bool CopyToSlot(const Preset& a_preset, int a_slot);  // a shared preset into one of the 12 slots

		// Puts the preset on the target. What it wore before is kept, so UndoLoad() can put it back.
		void Load(const Preset& a_preset);
		bool CanUndoLoad();
		void UndoLoad();

		// Tattoos of the preset that aren't in the installed tattoo packs
		int MissingCount(const Preset& a_preset);
		const tattoo_model::TattooDef* FirstInstalled(const Preset& a_preset);  // for the thumbnail
	}
}
