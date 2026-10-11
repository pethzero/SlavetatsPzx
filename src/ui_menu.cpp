#include "jcontainers_wrapper.h"
#include "nioverride_wrapper.h"
#include "constants.h"
#include "cache.h"
#include "tattoo.h"
#include "overlays.h"
#include "ui_menu.h"

// SKSEMenuFramework.h pulls in <windows.h>, keep it after the SlaveTatsNG headers
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include "SKSEMenuFramework.h"

using namespace slavetats_ng::jcwrapper;
using std::string;

// In-game menu (SKSE Menu Framework), the ImGui counterpart of SlaveTatsMCMMenu.psc.
// All SlaveTats/JContainers/NiOverride work runs on the game thread (SKSE task), the results are
// handed back to the UI thread, which only touches its own copies.

namespace slavetats_ng
{
	namespace ui_menu
	{
		namespace
		{
			constexpr int         kAreaCount = 4;
			constexpr const char* kAreas[kAreaCount] = { "Body", "Face", "Hands", "Feet" };
			constexpr const char* kPageNames[kAreaCount] = { "Body", "Head", "Hand", "Foot" };
			constexpr const char* kPool = "SlaveTatsNG-UI";
			constexpr const char* kNoTattoo = "[No Tattoo]";

			bool IEquals(const string& a_lhs, const string& a_rhs)
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
				float     ink[3] = { 0.0f, 0.0f, 0.0f };
				float     glow[3] = { 0.0f, 0.0f, 0.0f };
				bool      glossy = false;
				string    error;
			};

			struct ApplyRequest
			{
				int    area = 0;
				int    slot = 0;
				string section;
				string name;
				int    color = 0;
				int    glow = 0;
				bool   glossy = false;
				bool   write_gloss = false;  // only touch glossiness when the user changed it
			};

			// ------------------------------ UI thread state ------------------------------------------------------

			RE::FormID                                TargetID = 0;
			string                                    TargetName;
			bool                                      HasTarget = false;
			Catalog                                   TattooCatalog;
			std::array<std::vector<Slot>, kAreaCount> Slots;
			std::array<bool, kAreaCount>              SlotsKnown = {};
			int                                       Working = 0;  // game thread tasks in flight
			string                                    Status;

			std::mutex                         QueueLock;
			std::vector<std::function<void()>> Queue;

			// ------------------------------ Threading ------------------------------------------------------------

			void PostToUI(std::function<void()> a_fn)
			{
				std::lock_guard guard(QueueLock);
				Queue.push_back(std::move(a_fn));
			}

			void DrainQueue()
			{
				std::vector<std::function<void()>> queue;
				{
					std::lock_guard guard(QueueLock);
					queue.swap(Queue);
				}
				for (auto& fn : queue)
					fn();
			}

			// Runs a_work on the game thread; the function it returns is run on the UI thread afterwards
			void RunOnGameThread(std::function<std::function<void()>()> a_work)
			{
				Working++;
				SKSE::GetTaskInterface()->AddTask([a_work = std::move(a_work)]() {
					auto update = a_work();
					PostToUI([update = std::move(update)]() {
						Working--;
						if (update)
							update();
					});
				});
			}

			// ------------------------------ Helpers --------------------------------------------------------------

			void ToFloat(int a_rgb, float a_out[3])
			{
				a_out[0] = ((a_rgb >> 16) & 0xFF) / 255.0f;
				a_out[1] = ((a_rgb >> 8) & 0xFF) / 255.0f;
				a_out[2] = (a_rgb & 0xFF) / 255.0f;
			}

			int ToRGB(const float a_color[3])
			{
				auto channel = [](float v) { return std::clamp(static_cast<int>(std::lround(v * 255.0f)), 0, 255); };
				return (channel(a_color[0]) << 16) | (channel(a_color[1]) << 8) | channel(a_color[2]);
			}

			bool JContainersReady()
			{
				return jmap_object_func != nullptr;
			}

			int TotalSlots()
			{
				int total = 0;
				for (auto area : kAreas)
					total += SLOTS(area);
				return total;
			}

			// ------------------------------ Game thread ----------------------------------------------------------

			string GetStr(int a_obj, const char* a_key)
			{
				return string(JMap::getStr(a_obj, a_key).c_str());
			}

			// SlaveTats 2.0 uses "glossiness", older tattoos only have the boolean "gloss"
			bool IsGlossy(int a_tattoo)
			{
				if (JMap::hasKey(a_tattoo, "glossiness"))
					return JMap::getFlt(a_tattoo, "glossiness") > 0.0f;
				return JMap::getInt(a_tattoo, "gloss") > 0;
			}

			// Same values apply_overlay() uses for the old boolean gloss
			void SetGlossy(int a_tattoo, bool a_glossy)
			{
				JMap::setInt(a_tattoo, "gloss", a_glossy ? 1 : 0);
				JMap::setFlt(a_tattoo, "glossiness", a_glossy ? 5.0f : 0.0f);
				JMap::setFlt(a_tattoo, "specularStrength", a_glossy ? 5.0f : 0.0f);
			}

			RE::Actor* GetActor(RE::FormID a_id)
			{
				return a_id ? RE::TESForm::LookupByID<RE::Actor>(a_id) : nullptr;
			}

			Catalog ReadCatalog(int a_cache)
			{
				Catalog catalog;
				catalog.loaded = true;

				int domain = JMap::getObj(a_cache, "default");
				for (int a = 0; a < kAreaCount; a++) {
					int area = JMap::getObj(domain, kAreas[a]);
					if (area == 0)
						continue;

					for (RE::BSFixedString key = JMap::nextKey(area); key != ""; key = JMap::nextKey(area, key)) {
						int   list = JMap::getObj(area, key);
						auto& tattoos = catalog.areas[a][string(key.c_str())];
						int   count = JArray::count(list);
						for (int i = 0; i < count; i++) {
							int tattoo = JArray::getObj(list, i);
							tattoos.push_back({ GetStr(tattoo, "name"), GetStr(tattoo, "credit"),
								JMap::getInt(tattoo, "color"), JMap::getInt(tattoo, "glow"), IsGlossy(tattoo) });
							catalog.count++;
						}
					}
				}
				return catalog;
			}

			// Looked up again on every apply, the cache is rebuilt on load game and by "Add/remove tattoos"
			int FindInCache(int a_area, const string& a_section, const string& a_name)
			{
				int domain = JMap::getObj(acquire_cache(), "default");
				int list = JMap::getObj(JMap::getObj(domain, kAreas[a_area]), a_section.c_str());
				int count = JArray::count(list);
				for (int i = 0; i < count; i++) {
					int tattoo = JArray::getObj(list, i);
					if (IEquals(GetStr(tattoo, "name"), a_name))
						return tattoo;
				}
				return 0;
			}

			std::vector<SlotState> ReadSlots(RE::Actor* a_target, int a_area)
			{
				RE::BSFixedString area = kAreas[a_area];
				std::vector<SlotState> states(std::max(SLOTS(area), 0));

				int external = JValue::addToPool(JArray::object(), kPool);
				external_slots(a_target, area, external);

				for (int i = 0; i < static_cast<int>(states.size()); i++) {
					auto& state = states[i];
					if (JArray::findInt(external, i) >= 0) {
						state.kind = SlotKind::kExternal;
						continue;
					}
					int entry = get_applied_tattoo_in_slot(a_target, area, i);
					if (entry == 0)
						continue;

					state.kind = SlotKind::kTattoo;
					state.section = GetStr(entry, "section");
					state.name = GetStr(entry, "name");
					state.color = JMap::getInt(entry, "color");
					state.glow = JMap::getInt(entry, "glow");
					state.glossy = IsGlossy(entry);
					state.locked = JMap::getInt(entry, "locked") > 0;
				}

				JValue::cleanPool(kPool);
				return states;
			}

			// Same steps as the MCM (add_and_get_tattoo, then edit the applied entry), but synchronized right away
			string ApplyTattoo(RE::Actor* a_target, const ApplyRequest& a_req)
			{
				RE::BSFixedString area = kAreas[a_req.area];

				int  entry = get_applied_tattoo_in_slot(a_target, area, a_req.slot);
				bool same = entry != 0 && IEquals(GetStr(entry, "section"), a_req.section) && IEquals(GetStr(entry, "name"), a_req.name);

				if (!same) {
					int tattoo = FindInCache(a_req.area, a_req.section, a_req.name);
					if (tattoo == 0)
						return "This tattoo is not in the SlaveTats cache anymore, press \"Add/remove tattoos\" on the Setup page.";

					int copy = JValue::addToPool(JValue::shallowCopy(tattoo), kPool);
					JMap::removeKey(copy, "slot");  // the menu picks the slot, pre-specified slots are for mods
					entry = add_and_get_tattoo(a_target, copy, a_req.slot, false, true);
					JValue::cleanPool(kPool);

					if (entry == 0)
						return "SlaveTats refused the tattoo (locked slot, missing texture, or excluded/required by another tattoo).";
				}

				JMap::setInt(entry, "color", a_req.color);
				JMap::setInt(entry, "glow", a_req.glow);
				if (a_req.write_gloss)
					SetGlossy(entry, a_req.glossy);

				JFormDB::setInt(a_target, ".SlaveTats.updated", 1);
				synchronize_tattoos(a_target, true);
				return {};
			}

			string RemoveTattoo(RE::Actor* a_target, int a_area, int a_slot)
			{
				if (remove_tattoo_from_slot(a_target, kAreas[a_area], a_slot))
					return "The tattoo resists all of your efforts to remove it.";

				JFormDB::setInt(a_target, ".SlaveTats.updated", 1);
				synchronize_tattoos(a_target, true);
				return {};
			}

			// ------------------------------ UI thread ------------------------------------------------------------

			const TattooDef* Selected(int a_area, const Slot& a_slot)
			{
				auto& sections = TattooCatalog.areas[a_area];
				auto  it = sections.find(a_slot.section);
				if (it == sections.end())
					return nullptr;
				for (auto& tattoo : it->second) {
					if (IEquals(tattoo.name, a_slot.pattern))
						return &tattoo;
				}
				return nullptr;
			}

			// Pending edits = what is applied
			void ResetPending(Slot& a_slot)
			{
				a_slot.error.clear();
				if (a_slot.applied.kind == SlotKind::kTattoo) {
					a_slot.section = a_slot.applied.section;
					a_slot.pattern = a_slot.applied.name;
					ToFloat(a_slot.applied.color, a_slot.ink);
					ToFloat(a_slot.applied.glow, a_slot.glow);
					a_slot.glossy = a_slot.applied.glossy;
				} else {
					a_slot = Slot{ a_slot.applied };
				}
			}

			// a_reset: slot whose pending edits are replaced by the new state, -1 = all of them
			void SetSlots(int a_area, std::vector<SlotState> a_states, int a_reset = -1)
			{
				auto& slots = Slots[a_area];
				slots.resize(a_states.size());
				for (int i = 0; i < static_cast<int>(slots.size()); i++) {
					slots[i].applied = std::move(a_states[i]);
					if (a_reset < 0 || a_reset == i)
						ResetPending(slots[i]);
				}
				SlotsKnown[a_area] = true;
			}

			// Picking a pattern loads its default colors, like the MCM does when the tattoo is added
			void SelectTattoo(Slot& a_slot, const string& a_section, const TattooDef* a_tattoo)
			{
				a_slot.section = a_section;
				a_slot.pattern = a_tattoo ? a_tattoo->name : "";
				a_slot.error.clear();
				if (a_tattoo) {
					ToFloat(a_tattoo->color, a_slot.ink);
					ToFloat(a_tattoo->glow, a_slot.glow);
					a_slot.glossy = a_tattoo->glossy;
				}
			}

			enum class TargetMode
			{
				kPlayer,
				kConsole,
				kCurrent
			};

			void LoadTarget(TargetMode a_mode)
			{
				Status.clear();
				RunOnGameThread([a_mode, current = TargetID]() -> std::function<void()> {
					if (!JContainersReady())
						return [] { Status = "JContainers is not installed (or not initialized yet)."; };

					RE::Actor* actor = nullptr;
					if (a_mode == TargetMode::kPlayer) {
						actor = RE::PlayerCharacter::GetSingleton();
					} else if (a_mode == TargetMode::kConsole) {
						auto ref = RE::Console::GetSelectedRef();
						actor = ref ? ref->As<RE::Actor>() : nullptr;
						if (!actor)
							return [] { Status = "No actor selected in the console. Open the console (~) and click on someone first."; };
					} else {
						actor = GetActor(current);
					}
					if (!actor)
						return [] { Status = "The target is not available anymore."; };

					upgrade_tattoos(actor);

					auto   id = actor->GetFormID();
					string name = actor->GetDisplayFullName() ? actor->GetDisplayFullName() : "";
					if (name.empty())
						name = fmt::format("{:08X}", id);

					auto catalog = ReadCatalog(acquire_cache());

					std::array<std::vector<SlotState>, kAreaCount> states;
					for (int a = 0; a < kAreaCount; a++)
						states[a] = ReadSlots(actor, a);

					return [id, name, catalog = std::move(catalog), states = std::move(states)]() mutable {
						TargetID = id;
						TargetName = name;
						HasTarget = true;
						TattooCatalog = std::move(catalog);
						for (int a = 0; a < kAreaCount; a++)
							SetSlots(a, std::move(states[a]));
					};
				});
			}

			// "Add/remove tattoos" (RECOMPILE in the MCM): rescan the tattoo packs
			void CompileTattoos()
			{
				Status.clear();
				RunOnGameThread([]() -> std::function<void()> {
					if (!JContainersReady())
						return [] { Status = "JContainers is not installed (or not initialized yet)."; };

					auto catalog = ReadCatalog(compile_cache());
					return [catalog = std::move(catalog)]() mutable { TattooCatalog = std::move(catalog); };
				});
			}

			void ApplySlot(int a_area, int a_index)
			{
				auto& slot = Slots[a_area][a_index];
				auto  tattoo = Selected(a_area, slot);
				if (!tattoo)
					return;

				bool same = slot.applied.kind == SlotKind::kTattoo && IEquals(slot.applied.section, slot.section) && IEquals(slot.applied.name, slot.pattern);

				ApplyRequest req;
				req.area = a_area;
				req.slot = a_index;
				req.section = slot.section;
				req.name = tattoo->name;
				req.color = ToRGB(slot.ink);
				req.glow = ToRGB(slot.glow);
				req.glossy = slot.glossy;
				req.write_gloss = slot.glossy != (same ? slot.applied.glossy : tattoo->glossy);

				slot.error.clear();
				RunOnGameThread([id = TargetID, req]() -> std::function<void()> {
					auto actor = GetActor(id);
					if (!actor)
						return [] { Status = "The target is not available anymore."; };

					auto error = ApplyTattoo(actor, req);
					auto states = ReadSlots(actor, req.area);
					return [req, error, states = std::move(states)]() mutable {
						SetSlots(req.area, std::move(states), req.slot);
						if (req.slot < static_cast<int>(Slots[req.area].size()))
							Slots[req.area][req.slot].error = error;
					};
				});
			}

			void RemoveSlot(int a_area, int a_index)
			{
				auto& slot = Slots[a_area][a_index];
				if (slot.applied.locked) {
					slot.error = "The tattoo resists all of your efforts to remove it.";
					return;
				}

				slot.error.clear();
				RunOnGameThread([id = TargetID, a_area, a_index]() -> std::function<void()> {
					auto actor = GetActor(id);
					if (!actor)
						return [] { Status = "The target is not available anymore."; };

					auto error = RemoveTattoo(actor, a_area, a_index);
					auto states = ReadSlots(actor, a_area);
					return [a_area, a_index, error, states = std::move(states)]() mutable {
						SetSlots(a_area, std::move(states), a_index);
						if (a_index < static_cast<int>(Slots[a_area].size()))
							Slots[a_area][a_index].error = error;
					};
				});
			}

			// ------------------------------ Rendering ------------------------------------------------------------

			void RenderSlot(int a_area, int a_index)
			{
				auto& slot = Slots[a_area][a_index];
				auto& sections = TattooCatalog.areas[a_area];

				ImGuiMCP::PushID(a_index);
				ImGuiMCP::SeparatorText(fmt::format("Slot {}", a_index + 1).c_str());

				if (slot.applied.kind == SlotKind::kExternal) {
					ImGuiMCP::TextDisabled("External: this overlay is used by another mod.");
					ImGuiMCP::PopID();
					return;
				}

				bool locked = slot.applied.locked;
				ImGuiMCP::BeginDisabled(locked);
				if (ImGuiMCP::BeginTable("slot", 2, ImGuiMCP::ImGuiTableFlags_BordersInnerV | ImGuiMCP::ImGuiTableFlags_SizingStretchProp)) {
					ImGuiMCP::TableNextRow();

					// Left: pattern selection
					ImGuiMCP::TableNextColumn();
					const char* section_preview = slot.section.empty() ? kNoTattoo : slot.section.c_str();
					if (ImGuiMCP::BeginCombo("Section", section_preview)) {
						if (ImGuiMCP::Selectable(kNoTattoo, slot.section.empty())) {
							SelectTattoo(slot, {}, nullptr);
						}
						for (auto& [name, tattoos] : sections) {
							bool current = IEquals(slot.section, name);
							if (ImGuiMCP::Selectable(name.c_str(), current) && !current) {
								SelectTattoo(slot, name, tattoos.empty() ? nullptr : &tattoos.front());
							}
						}
						ImGuiMCP::EndCombo();
					}
					if (ImGuiMCP::IsItemHovered()) {
						ImGuiMCP::SetTooltip("Pick a tattoo collection to select your tattoo from.");
					}

					auto section_it = sections.find(slot.section);
					bool has_section = section_it != sections.end();
					auto selected = Selected(a_area, slot);

					ImGuiMCP::BeginDisabled(!has_section);
					const char* pattern_preview = selected ? selected->name.c_str() : (slot.pattern.empty() ? kNoTattoo : slot.pattern.c_str());
					if (ImGuiMCP::BeginCombo("Pattern", pattern_preview)) {
						if (has_section) {
							for (auto& tattoo : section_it->second) {
								bool current = &tattoo == selected;
								if (ImGuiMCP::Selectable(tattoo.name.c_str(), current) && !current) {
									SelectTattoo(slot, slot.section, &tattoo);
								}
								if (!tattoo.credit.empty() && ImGuiMCP::IsItemHovered()) {
									ImGuiMCP::SetTooltip("Tattoo by: %s", tattoo.credit.c_str());
								}
							}
						}
						ImGuiMCP::EndCombo();
					}
					ImGuiMCP::EndDisabled();
					if (selected && !selected->credit.empty() && ImGuiMCP::IsItemHovered()) {
						ImGuiMCP::SetTooltip("Tattoo by: %s", selected->credit.c_str());
					}

					// Right: colors
					ImGuiMCP::TableNextColumn();
					ImGuiMCP::ColorEdit3("Ink", slot.ink, ImGuiMCP::ImGuiColorEditFlags_NoInputs);
					if (ImGuiMCP::IsItemHovered()) {
						ImGuiMCP::SetTooltip("Pick the ink color for this tattoo.");
					}
					ImGuiMCP::SameLine();
					ImGuiMCP::ColorEdit3("Glow", slot.glow, ImGuiMCP::ImGuiColorEditFlags_NoInputs);
					if (ImGuiMCP::IsItemHovered()) {
						ImGuiMCP::SetTooltip("Pick a color for the tattoo to glow (black is no glow).");
					}
					ImGuiMCP::SameLine();
					ImGuiMCP::Checkbox("Glossy", &slot.glossy);
					if (ImGuiMCP::IsItemHovered()) {
						ImGuiMCP::SetTooltip("Checking this makes the tattoo slightly glossy, like skin.");
					}

					ImGuiMCP::EndTable();
				}

				// Actions
				bool can_apply = Selected(a_area, slot) != nullptr;
				ImGuiMCP::BeginDisabled(Working > 0);
				if (slot.applied.kind != SlotKind::kTattoo) {
					ImGuiMCP::BeginDisabled(!can_apply);
					if (ImGuiMCP::Button("Save Tattoo")) {
						ApplySlot(a_area, a_index);
					}
					ImGuiMCP::EndDisabled();
				} else {
					auto& applied = slot.applied;
					bool  changed = !IEquals(applied.section, slot.section) || !IEquals(applied.name, slot.pattern) ||
					               applied.color != ToRGB(slot.ink) || applied.glow != ToRGB(slot.glow) || applied.glossy != slot.glossy;

					ImGuiMCP::BeginDisabled(!can_apply || !changed);
					if (ImGuiMCP::Button("Replace")) {
						ApplySlot(a_area, a_index);
					}
					ImGuiMCP::EndDisabled();
					ImGuiMCP::SameLine();
					if (ImGuiMCP::Button("Remove")) {
						RemoveSlot(a_area, a_index);
					}
					ImGuiMCP::SameLine();
					ImGuiMCP::BeginDisabled(!changed);
					if (ImGuiMCP::Button("Undo")) {
						ResetPending(slot);
					}
					ImGuiMCP::EndDisabled();
					ImGuiMCP::SameLine();
					if (TattooCatalog.areas[a_area].contains(applied.section)) {
						ImGuiMCP::TextDisabled("Applied: %s / %s", applied.section.c_str(), applied.name.c_str());
					} else {
						ImGuiMCP::TextDisabled("Applied (not in installed packs): %s / %s", applied.section.c_str(), applied.name.c_str());
					}
				}
				ImGuiMCP::EndDisabled();
				ImGuiMCP::EndDisabled();

				if (locked) {
					ImGuiMCP::TextDisabled("Locked: this tattoo can only be removed by the mod that applied it.");
				}
				if (!slot.error.empty()) {
					ImGuiMCP::TextColored({ 1.0f, 0.4f, 0.4f, 1.0f }, "%s", slot.error.c_str());
				}

				ImGuiMCP::PopID();
			}

			void RenderArea(int a_area)
			{
				DrainQueue();

				if (!HasTarget) {
					ImGuiMCP::TextDisabled("Go to Setup and select a target.");
					return;
				}
				ImGuiMCP::Text("%s tattoos on %s", kPageNames[a_area], TargetName.c_str());
				if (Working > 0) {
					ImGuiMCP::SameLine();
					ImGuiMCP::TextDisabled("(working...)");
				}
				if (!SlotsKnown[a_area]) {
					ImGuiMCP::TextDisabled("Reading overlays...");
					return;
				}
				if (Slots[a_area].empty()) {
					ImGuiMCP::TextDisabled("No %s overlay slots (check RaceMenu/SKEE is installed and iNumOverlays in skee64.ini).", kAreas[a_area]);
					return;
				}
				for (int i = 0; i < static_cast<int>(Slots[a_area].size()); i++) {
					RenderSlot(a_area, i);
				}
			}

			void __stdcall RenderSetup()
			{
				DrainQueue();

				if (!JContainersReady()) {
					ImGuiMCP::TextColored({ 1.0f, 0.4f, 0.4f, 1.0f }, "SlaveTatsNG requires JContainers.");
				}
				if (TotalSlots() == 0) {
					ImGuiMCP::TextColored({ 1.0f, 0.4f, 0.4f, 1.0f }, "No overlay slots found, SlaveTatsNG requires RaceMenu (SKEE).");
				}

				ImGuiMCP::BeginDisabled(Working > 0);

				ImGuiMCP::SeparatorText("Target");
				if (ImGuiMCP::Button("Select Player")) {
					LoadTarget(TargetMode::kPlayer);
				}
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button("Select Console Target")) {
					LoadTarget(TargetMode::kConsole);
				}
				if (ImGuiMCP::IsItemHovered()) {
					ImGuiMCP::SetTooltip("Open the console (~), click on an NPC, close the console, then press this.");
				}
				ImGuiMCP::SameLine();
				ImGuiMCP::BeginDisabled(!HasTarget);
				if (ImGuiMCP::Button("Refresh")) {
					LoadTarget(TargetMode::kCurrent);
				}
				ImGuiMCP::EndDisabled();
				if (ImGuiMCP::IsItemHovered()) {
					ImGuiMCP::SetTooltip("Read the target's tattoos again (e.g. after another mod changed them).");
				}

				if (HasTarget) {
					ImGuiMCP::Text("Target: %s", TargetName.c_str());
				} else {
					ImGuiMCP::TextDisabled("No target selected");
				}

				ImGuiMCP::SeparatorText("Tattoos");
				if (ImGuiMCP::Button("Add/remove tattoos")) {
					CompileTattoos();
				}
				if (ImGuiMCP::IsItemHovered()) {
					ImGuiMCP::SetTooltip("After you install or uninstall tattoo packs, click this to rescan Data\\Textures\\Actors\\Character\\slavetats.");
				}
				ImGuiMCP::SameLine();
				if (TattooCatalog.loaded) {
					std::size_t sections = 0;
					for (auto& area : TattooCatalog.areas)
						sections += area.size();
					ImGuiMCP::Text("Found %zu tattoos in %zu sections", TattooCatalog.count, sections);
				} else {
					ImGuiMCP::TextDisabled("Not loaded yet (select a target)");
				}

				ImGuiMCP::EndDisabled();

				if (Working > 0) {
					ImGuiMCP::TextDisabled("Working...");
				}
				if (!Status.empty()) {
					ImGuiMCP::TextColored({ 1.0f, 0.6f, 0.3f, 1.0f }, "%s", Status.c_str());
				}
			}

			void __stdcall RenderBody() { RenderArea(0); }
			void __stdcall RenderHead() { RenderArea(1); }
			void __stdcall RenderHands() { RenderArea(2); }
			void __stdcall RenderFeet() { RenderArea(3); }
		}

		void Register()
		{
			if (!SKSEMenuFramework::IsInstalled()) {
				logger::info("SKSE Menu Framework is not installed, the in-game menu is disabled");
				return;
			}
			SKSEMenuFramework::SetSection("SlavetatsPzx");
			SKSEMenuFramework::AddSectionItem("Setup", RenderSetup);
			SKSEMenuFramework::AddSectionItem("Body Tattoos", RenderBody);
			SKSEMenuFramework::AddSectionItem("Head Tattoos", RenderHead);
			SKSEMenuFramework::AddSectionItem("Hand Tattoos", RenderHands);
			SKSEMenuFramework::AddSectionItem("Foot Tattoos", RenderFeet);
			logger::info("Registered SKSE Menu Framework pages");
		}
	}
}
