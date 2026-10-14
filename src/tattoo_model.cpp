#include "jcontainers_wrapper.h"
#include "nioverride_wrapper.h"
#include "constants.h"
#include "cache.h"
#include "tattoo.h"
#include "overlays.h"
#include "tattoo_model.h"

using namespace slavetats_ng::jcwrapper;

// Tattoo data and SlaveTats actions shared by the in-game UIs, the ImGui counterpart of SlaveTatsMCMMenu.psc.
// All SlaveTats/JContainers/NiOverride work runs on the game thread (SKSE task), the results are handed back
// to the UI thread, which only touches its own copies.

namespace slavetats_ng
{
	namespace tattoo_model
	{
		constexpr const char* kPool = "SlaveTatsNG-UI";
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

		std::atomic<bool> Previewing = false;  // an overlay on the target differs from the records
		std::set<std::pair<int, int>> LookShown;  // (area, slot) the last PreviewLook drew a tattoo in

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
						tattoos.push_back({ GetStr(tattoo, "name"), GetStr(tattoo, "credit"), GetStr(tattoo, "texture"),
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
				a_slot.browse = a_slot.applied.section;
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
			if (tattoo)
				Previewing = false;  // the synchronize below redraws every slot from the records
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
			if (!slot.applied.locked)
				Previewing = false;  // the synchronize below redraws every slot from the records
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

		// ------------------------------ Looks (presets) -------------------------------------------------------

		std::vector<LookTattoo> CurrentLook()
		{
			std::vector<LookTattoo> look;
			for (int a = 0; a < kAreaCount; a++) {
				for (int i = 0; i < static_cast<int>(Slots[a].size()); i++) {
					auto& applied = Slots[a][i].applied;
					if (applied.kind == SlotKind::kTattoo && !applied.locked)
						look.push_back({ a, i, applied.section, applied.name, applied.color, applied.glow, applied.glossy });
				}
			}
			return look;
		}

		void ApplyLook(std::vector<LookTattoo> a_look, std::function<void(int, std::vector<string>)> a_done)
		{
			Previewing = false;  // the synchronize below redraws every slot from the records
			Status.clear();
			RunOnGameThread([id = TargetID, look = std::move(a_look), done = std::move(a_done)]() mutable -> std::function<void()> {
				auto actor = GetActor(id);
				if (!actor)
					return [] { Status = "The target is not available anymore."; };

				// Slots other mods draw their own overlays in: never ours to fill
				std::array<std::set<int>, kAreaCount> external;
				for (int a = 0; a < kAreaCount; a++) {
					int list = JValue::addToPool(JArray::object(), kPool);
					external_slots(actor, kAreas[a], list);
					for (int i = 0, n = JArray::count(list); i < n; i++)
						external[a].insert(JArray::getInt(list, i));
				}
				JValue::cleanPool(kPool);

				// Off with the current look (locked tattoos refuse and stay where they are)
				for (int a = 0; a < kAreaCount; a++) {
					RE::BSFixedString area = kAreas[a];
					for (int i = 0, n = SLOTS(area); i < n; i++) {
						int entry = get_applied_tattoo_in_slot(actor, area, i);
						if (entry != 0 && JMap::getInt(entry, "locked") == 0)
							remove_tattoo_from_slot(actor, area, i, false, true);
					}
				}

				// On with the new one. A tattoo can require another one of the same look, so the ones SlaveTats
				// refuses get a second try once the rest are on.
				int                             applied = 0;
				std::vector<string>             failed;
				std::vector<const LookTattoo*>  pending;
				for (auto& t : look) {
					if (t.area >= 0 && t.area < kAreaCount)
						pending.push_back(&t);
				}
				for (int pass = 0; pass < 2 && !pending.empty(); pass++) {
					std::vector<const LookTattoo*> refused;
					for (auto t : pending) {
						RE::BSFixedString area = kAreas[t->area];
						string            label = fmt::format("{}: {} / {}", kAreas[t->area], t->section, t->name);

						int tattoo = FindInCache(t->area, t->section, t->name);
						if (tattoo == 0) {
							failed.push_back(label + " - not installed");
							continue;
						}
						int slot = t->slot;
						if (slot < 0 || slot >= SLOTS(area) || external[t->area].contains(slot) || get_applied_tattoo_in_slot(actor, area, slot) != 0)
							slot = _available_slot(actor, area);
						if (slot < 0) {
							failed.push_back(label + " - no free slot");
							continue;
						}

						int copy = JValue::addToPool(JValue::shallowCopy(tattoo), kPool);
						JMap::removeKey(copy, "slot");
						int entry = add_and_get_tattoo(actor, copy, slot, false, true);
						JValue::cleanPool(kPool);
						if (entry == 0) {
							if (pass == 0)
								refused.push_back(t);
							else
								failed.push_back(label + " - refused (missing texture, or excluded/required by another tattoo)");
							continue;
						}
						JMap::setInt(entry, "color", t->color);
						JMap::setInt(entry, "glow", t->glow);
						if (IsGlossy(entry) != t->glossy)
							SetGlossy(entry, t->glossy);
						applied++;
					}
					pending = std::move(refused);
				}

				JFormDB::setInt(actor, ".SlaveTats.updated", 1);
				synchronize_tattoos(actor, true);

				std::array<std::vector<SlotState>, kAreaCount> states;
				for (int a = 0; a < kAreaCount; a++)
					states[a] = ReadSlots(actor, a);
				return [applied, failed = std::move(failed), states = std::move(states), done = std::move(done)]() mutable {
					for (int a = 0; a < kAreaCount; a++)
						SetSlots(a, std::move(states[a]));
					if (done)
						done(applied, std::move(failed));
				};
			});
		}

		// ------------------------------ Preview --------------------------------------------------------------

		bool IsPreviewing() { return Previewing.load(); }

		void PreviewSlot(int a_area, int a_index, const TattooDef* a_tattoo)
		{
			if (a_area < 0 || a_area >= kAreaCount || a_index < 0 || a_index >= static_cast<int>(Slots[a_area].size()))
				return;
			auto& slot = Slots[a_area][a_index];
			if (slot.applied.kind == SlotKind::kExternal || slot.applied.locked)
				return;
			auto tattoo = a_tattoo ? a_tattoo : Selected(a_area, slot);
			if (!tattoo || tattoo->texture.empty())
				return;

			string path = string(PREFIX().c_str()) + tattoo->texture;
			int    color = ToRGB(slot.ink);
			int    glow = ToRGB(slot.glow);
			bool   gloss = slot.glossy;
			Previewing = true;

			// Straight to the overlay, the way synchronize_tattoos draws an applied entry, but no record is made
			SKSE::GetTaskInterface()->AddTask([id = TargetID, a_area, a_index, path, color, glow, gloss] {
				auto actor = GetActor(id);
				if (!actor)
					return;
				auto base = actor->GetActorBase();
				bool female = base && base->GetSex() == RE::SEX::kFemale;
				apply_overlay(actor, female, kAreas[a_area], a_index, path.c_str(), color, glow, gloss, "", 1.0f);
			});
		}

		void PreviewLook(const std::vector<LookTattoo>& a_look)
		{
			for (int a = 0; a < kAreaCount; a++) {
				if (!SlotsKnown[a])
					return;
			}

			// What each slot shows: path empty = cleared
			struct Paint
			{
				int    area;
				int    slot;
				string path;
				int    color = 0;
				int    glow = 0;
				bool   gloss = false;
			};
			std::vector<Paint>            paint;
			std::set<std::pair<int, int>> shown;
			string                        prefix = PREFIX().c_str();

			for (int a = 0; a < kAreaCount; a++) {
				auto& slots = Slots[a];
				int   n = static_cast<int>(slots.size());
				// Same placement as ApplyLook: every slot that isn't locked or another mod's is free once the
				// current look is off; a tattoo goes back to its own slot, else to the first free one
				std::vector<bool> usable(n);
				for (int i = 0; i < n; i++)
					usable[i] = slots[i].applied.kind != SlotKind::kExternal && !slots[i].applied.locked;
				std::vector<const LookTattoo*> at(n, nullptr);
				std::vector<const LookTattoo*> later;
				for (auto& t : a_look) {
					if (t.area != a)
						continue;
					auto def = FindTattoo(a, t.section, t.name);
					if (!def || def->texture.empty())
						continue;  // not installed: Load skips it too
					if (t.slot >= 0 && t.slot < n && usable[t.slot] && !at[t.slot])
						at[t.slot] = &t;
					else
						later.push_back(&t);
				}
				for (auto t : later) {
					for (int i = 0; i < n; i++) {
						if (usable[i] && !at[i]) {
							at[i] = t;
							break;
						}
					}
				}
				// Clearing is an overlay update too: only where something shows (saved, or the last preview's)
				for (int i = 0; i < n; i++) {
					if (!usable[i])
						continue;
					if (auto t = at[i]) {
						paint.push_back({ a, i, prefix + FindTattoo(a, t->section, t->name)->texture, t->color, t->glow, t->glossy });
						shown.insert({ a, i });
					} else if (slots[i].applied.kind == SlotKind::kTattoo || LookShown.contains({ a, i })) {
						paint.push_back({ a, i, {} });
					}
				}
			}
			LookShown = std::move(shown);

			Previewing = true;
			SKSE::GetTaskInterface()->AddTask([id = TargetID, paint = std::move(paint)] {
				auto actor = GetActor(id);
				if (!actor)
					return;
				auto base = actor->GetActorBase();
				bool female = base && base->GetSex() == RE::SEX::kFemale;
				for (auto& p : paint) {
					if (p.path.empty())
						clear_overlay(actor, female, kAreas[p.area], p.slot);
					else
						apply_overlay(actor, female, kAreas[p.area], p.slot, p.path.c_str(), p.color, p.glow, p.gloss, "", 1.0f);
				}
			});
		}

		void EndPreview()
		{
			if (!Previewing.exchange(false))
				return;
			// Forced synchronize: applied slots are drawn again from the records, empty ones are cleared
			SKSE::GetTaskInterface()->AddTask([id = TargetID] {
				if (auto actor = GetActor(id)) {
					JFormDB::setInt(actor, ".SlaveTats.updated", 1);
					synchronize_tattoos(actor, true);
				}
			});
		}

		// ------------------------------ Shared helpers -------------------------------------------------------

		bool IContains(const string& a_text, const char* a_needle)
		{
			if (!a_needle || !*a_needle)
				return true;
			auto it = std::search(a_text.begin(), a_text.end(), a_needle, a_needle + std::strlen(a_needle),
				[](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
			return it != a_text.end();
		}

		string PreviewPath(const TattooDef& a_tattoo)
		{
			return a_tattoo.texture.empty() ? string() : "Actors\\Character\\slavetats\\" + a_tattoo.texture;
		}

		const TattooDef* FindTattoo(int a_area, const string& a_section, const string& a_name)
		{
			auto& sections = TattooCatalog.areas[a_area];
			auto  it = sections.find(a_section);
			if (it == sections.end())
				return nullptr;
			for (auto& tattoo : it->second) {
				if (IEquals(tattoo.name, a_name))
					return &tattoo;
			}
			return nullptr;
		}

		bool HasChanges(const Slot& a_slot)
		{
			auto& applied = a_slot.applied;
			if (applied.kind != SlotKind::kTattoo)
				return !a_slot.pattern.empty();
			return !IEquals(applied.section, a_slot.section) || !IEquals(applied.name, a_slot.pattern) ||
			       applied.color != ToRGB(a_slot.ink) || applied.glow != ToRGB(a_slot.glow) || applied.glossy != a_slot.glossy;
		}
	}
}