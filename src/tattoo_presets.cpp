#include "tattoo_presets.h"
#include <nlohmann/json.hpp>

// Preset files. The format is plain JSON so people can share and hand-edit them:
//
//   {
//     "format": "SlavetatsUIPzx preset", "version": 1,
//     "name": "Night look", "character": "Lydia", "saved": "2026-10-14 20:40",
//     "tattoos": [ { "area": "Body", "slot": 0, "section": "Bimbo", "name": "Back Text",
//                    "color": "#1A237E", "glow": "#000000", "glossy": false } ]
//   }
//
// Areas are SlaveTats' ("Body", "Face", "Hands", "Feet"); colors are "#RRGGBB" (plain numbers are read too).

using namespace slavetats_ng::tattoo_model;
using json = nlohmann::json;

namespace slavetats_ng
{
	namespace tattoo_presets
	{
		namespace
		{
			constexpr const char* kFolder = "Data/SKSE/Plugins/SlavetatsUIPzx/Presets";
			constexpr const char* kSharedFolder = "Data/SKSE/Plugins/SlavetatsUIPzx/Presets/Shared";
			constexpr const char* kFormat = "SlavetatsUIPzx preset";
			constexpr int         kVersion = 1;

			// What the target wore before the last Load, for UndoLoad
			std::optional<std::vector<LookTattoo>> Backup;
			RE::FormID                             BackupTarget = 0;

			std::filesystem::path SlotPath(int a_slot)
			{
				return std::filesystem::path(kFolder) / fmt::format("Preset{:02}.json", a_slot + 1);
			}

			string FileName(const std::filesystem::path& a_path)
			{
				auto u8 = a_path.filename().u8string();
				return string(u8.begin(), u8.end());
			}

			string Now()
			{
				std::time_t t = std::time(nullptr);
				std::tm     tm{};
				localtime_s(&tm, &t);
				char buf[32] = {};
				std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
				return buf;
			}

			string ColorText(int a_rgb) { return fmt::format("#{:06X}", a_rgb & 0xFFFFFF); }

			int ParseColor(const json& a_value)
			{
				if (a_value.is_number_integer())
					return a_value.get<int>() & 0xFFFFFF;
				if (!a_value.is_string())
					return 0;
				string s = a_value.get<string>();
				if (!s.empty() && s[0] == '#')
					s.erase(0, 1);
				else if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
					s.erase(0, 2);
				try {
					return static_cast<int>(std::stoul(s, nullptr, 16) & 0xFFFFFF);
				} catch (...) {
					return 0;
				}
			}

			int AreaIndex(const string& a_area)
			{
				for (int a = 0; a < kAreaCount; a++) {
					if (IEquals(a_area, kAreas[a]))
						return a;
				}
				return -1;
			}

			// Reads one file; a missing file is fine (an empty slot), a broken one sets error
			PresetFile ReadFile(const std::filesystem::path& a_path)
			{
				PresetFile file;
				file.path = a_path;
				file.file = FileName(a_path);
				std::error_code ec;
				if (!std::filesystem::exists(a_path, ec))
					return file;
				file.exists = true;

				std::ifstream in(a_path, std::ios::binary);
				json          doc = json::parse(in, nullptr, false);
				if (doc.is_discarded() || !doc.is_object()) {
					file.error = "not a valid preset file (JSON)";
					return file;
				}
				// value() throws on a wrong type (a hand-edited file): a broken file is reported, never fatal
				try {
					auto& preset = file.preset;
					preset.name = doc.value("name", string());
					preset.character = doc.value("character", string());
					preset.saved = doc.value("saved", string());
					if (preset.name.empty()) {
						auto stem = a_path.stem().u8string();
						preset.name = string(stem.begin(), stem.end());
					}

					if (auto it = doc.find("tattoos"); it != doc.end() && it->is_array()) {
						for (auto& t : *it) {
							if (!t.is_object())
								continue;
							int area = AreaIndex(t.value("area", string()));
							if (area < 0)
								continue;
							LookTattoo tattoo;
							tattoo.area = area;
							tattoo.slot = t.value("slot", -1);
							tattoo.section = t.value("section", string());
							tattoo.name = t.value("name", string());
							tattoo.color = t.contains("color") ? ParseColor(t["color"]) : 0;
							tattoo.glow = t.contains("glow") ? ParseColor(t["glow"]) : 0;
							tattoo.glossy = t.value("glossy", false);
							if (!tattoo.name.empty())
								preset.tattoos.push_back(std::move(tattoo));
						}
					}
				} catch (const std::exception& e) {
					file.error = fmt::format("not a valid preset file ({})", e.what());
					file.preset = {};
				}
				return file;
			}

			bool WriteFile(const std::filesystem::path& a_path, const Preset& a_preset)
			{
				json tattoos = json::array();
				for (auto& t : a_preset.tattoos) {
					tattoos.push_back({ { "area", kAreas[t.area] }, { "slot", t.slot }, { "section", t.section }, { "name", t.name },
						{ "color", ColorText(t.color) }, { "glow", ColorText(t.glow) }, { "glossy", t.glossy } });
				}
				json doc = { { "format", kFormat }, { "version", kVersion }, { "name", a_preset.name }, { "character", a_preset.character },
					{ "saved", a_preset.saved }, { "tattoos", std::move(tattoos) } };

				std::error_code ec;
				std::filesystem::create_directories(a_path.parent_path(), ec);
				std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
				if (!out) {
					logger::warn("Presets: could not write {}", a_path.string());
					return false;
				}
				// Tattoo names come from the packs and aren't always valid UTF-8: replace rather than throw
				out << doc.dump(2, ' ', false, json::error_handler_t::replace);
				return static_cast<bool>(out);
			}

			void Report(string a_message, bool a_error = false)
			{
				Message = std::move(a_message);
				MessageIsError = a_error;
			}

			bool ValidSlot(int a_slot) { return a_slot >= 0 && a_slot < kSlotCount; }

			void Apply(std::vector<LookTattoo> a_look, string a_what)
			{
				Report(fmt::format("Loading {}...", a_what));
				ApplyLook(std::move(a_look), [a_what](int a_applied, std::vector<string> a_failed) {
					if (a_failed.empty()) {
						Report(fmt::format("{}: {} tattoo{} on.", a_what, a_applied, a_applied == 1 ? "" : "s"));
						return;
					}
					string text = fmt::format("{}: {} on, {} not:", a_what, a_applied, a_failed.size());
					for (auto& line : a_failed)
						text += "\n  " + line;
					Report(std::move(text), true);
				});
			}
		}

		std::array<PresetFile, kSlotCount> Mine;
		std::vector<PresetFile>            Shared;
		string                             Message;
		bool                               MessageIsError = false;

		string Folder() { return kFolder; }
		string SharedFolder() { return kSharedFolder; }

		void Rescan()
		{
			for (int i = 0; i < kSlotCount; i++)
				Mine[i] = ReadFile(SlotPath(i));

			Shared.clear();
			std::error_code ec;
			for (auto it = std::filesystem::directory_iterator(kSharedFolder, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
				if (!it->is_regular_file(ec))
					continue;
				auto ext = it->path().extension().string();
				if (_stricmp(ext.c_str(), ".json") == 0)
					Shared.push_back(ReadFile(it->path()));
			}
			std::sort(Shared.begin(), Shared.end(), [](const PresetFile& a, const PresetFile& b) {
				return _stricmp(a.preset.name.c_str(), b.preset.name.c_str()) < 0;
			});
		}

		bool SaveCurrent(int a_slot, const string& a_name)
		{
			if (!ValidSlot(a_slot))
				return false;
			for (int a = 0; a < kAreaCount; a++) {
				if (!SlotsKnown[a]) {
					Report("The tattoos are still being read, try again in a moment.", true);
					return false;
				}
			}
			Preset preset;
			preset.name = a_name.empty() ? fmt::format("Preset {}", a_slot + 1) : a_name;
			preset.character = TargetName;
			preset.saved = Now();
			preset.tattoos = CurrentLook();
			if (!WriteFile(SlotPath(a_slot), preset)) {
				Report(fmt::format("Could not write {}.", SlotPath(a_slot).string()), true);
				return false;
			}
			Mine[a_slot] = ReadFile(SlotPath(a_slot));
			Report(fmt::format("Saved \"{}\" ({} tattoo{}).", preset.name, preset.tattoos.size(), preset.tattoos.size() == 1 ? "" : "s"));
			return true;
		}

		bool Rename(int a_slot, const string& a_name)
		{
			if (!ValidSlot(a_slot) || !Mine[a_slot].exists || !Mine[a_slot].error.empty() || a_name.empty())
				return false;
			Preset preset = Mine[a_slot].preset;
			preset.name = a_name;
			if (!WriteFile(SlotPath(a_slot), preset)) {
				Report(fmt::format("Could not write {}.", SlotPath(a_slot).string()), true);
				return false;
			}
			Mine[a_slot] = ReadFile(SlotPath(a_slot));
			Report(fmt::format("Renamed to \"{}\".", a_name));
			return true;
		}

		bool Delete(int a_slot)
		{
			if (!ValidSlot(a_slot))
				return false;
			std::error_code ec;
			std::filesystem::remove(SlotPath(a_slot), ec);
			if (ec) {
				Report(fmt::format("Could not delete {}.", SlotPath(a_slot).string()), true);
				return false;
			}
			string name = Mine[a_slot].preset.name;
			Mine[a_slot] = ReadFile(SlotPath(a_slot));
			Report(fmt::format("Deleted \"{}\".", name));
			return true;
		}

		bool CopyToSlot(const Preset& a_preset, int a_slot)
		{
			if (!ValidSlot(a_slot))
				return false;
			if (!WriteFile(SlotPath(a_slot), a_preset)) {
				Report(fmt::format("Could not write {}.", SlotPath(a_slot).string()), true);
				return false;
			}
			Mine[a_slot] = ReadFile(SlotPath(a_slot));
			Report(fmt::format("Copied \"{}\" to slot {}.", a_preset.name, a_slot + 1));
			return true;
		}

		void Load(const Preset& a_preset)
		{
			for (int a = 0; a < kAreaCount; a++) {
				if (!SlotsKnown[a]) {
					Report("The tattoos are still being read, try again in a moment.", true);
					return;
				}
			}
			Backup = CurrentLook();
			BackupTarget = TargetID;
			Apply(a_preset.tattoos, fmt::format("\"{}\"", a_preset.name));
		}

		bool CanUndoLoad() { return Backup.has_value() && BackupTarget == TargetID; }

		void UndoLoad()
		{
			if (!CanUndoLoad())
				return;
			auto look = std::move(*Backup);
			Backup.reset();
			Apply(std::move(look), "Undo load");
		}

		int MissingCount(const Preset& a_preset)
		{
			if (!TattooCatalog.loaded)
				return 0;
			int missing = 0;
			for (auto& t : a_preset.tattoos) {
				if (!FindTattoo(t.area, t.section, t.name))
					missing++;
			}
			return missing;
		}

		const TattooDef* FirstInstalled(const Preset& a_preset)
		{
			for (auto& t : a_preset.tattoos) {
				if (auto def = FindTattoo(t.area, t.section, t.name); def && !def->texture.empty())
					return def;
			}
			return nullptr;
		}
	}
}
