#include "jcontainers_wrapper.h"
#include "nioverride_wrapper.h"
#include "constants.h"
#include "cache.h"
#include "tattoo.h"
#include "overlays.h"
#include "ui_menu.h"
#include "dds_preview.h"

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

			// ------------------------------ Theme ----------------------------------------------------------------

			using ImGuiMCP::ImU32;
			using ImGuiMCP::ImVec2;
			using ImGuiMCP::ImVec4;
			namespace draw = ImGuiMCP::ImDrawListManager;

			// ImGui packs colors as ABGR
			constexpr ImU32 Rgba(int r, int g, int b, int a = 255)
			{
				return (static_cast<ImU32>(a) << 24) | (static_cast<ImU32>(b) << 16) | (static_cast<ImU32>(g) << 8) | static_cast<ImU32>(r);
			}

			ImVec4 ToVec4(ImU32 c)
			{
				return { (c & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, ((c >> 16) & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f };
			}

			constexpr ImU32 kAccent = Rgba(226, 178, 86);
			constexpr ImU32 kAccentHover = Rgba(238, 194, 108);
			constexpr ImU32 kAccentSoft = Rgba(226, 178, 86, 46);
			constexpr ImU32 kAccentInk = Rgba(28, 22, 12);  // text on accent
			constexpr ImU32 kCard = Rgba(255, 255, 255, 10);
			constexpr ImU32 kCardHover = Rgba(255, 255, 255, 24);
			constexpr ImU32 kBorder = Rgba(255, 255, 255, 30);
			constexpr ImU32 kText = Rgba(236, 236, 236);
			constexpr ImU32 kMuted = Rgba(150, 150, 150);
			constexpr ImU32 kPreviewBg = Rgba(0, 0, 0, 110);
			constexpr ImU32 kHatch = Rgba(255, 255, 255, 12);
			constexpr ImU32 kOk = Rgba(108, 196, 120);
			constexpr ImU32 kWarn = Rgba(230, 160, 70);
			constexpr ImU32 kDanger = Rgba(222, 92, 92);
			constexpr ImU32 kDangerHover = Rgba(236, 112, 112);
			constexpr ImU32 kPending = Rgba(240, 210, 90);

			constexpr float kRounding = 6.0f;
			constexpr float kTile = 112.0f;       // pattern thumbnail edge
			constexpr float kSlotCardH = 58.0f;   // slot list row height
			constexpr ImGuiMCP::ImDrawFlags kAllCorners = ImGuiMCP::ImDrawFlags_RoundCornersAll;

			// ------------------------------ UI state (pages) -----------------------------------------------------

			std::array<int, kAreaCount>                     SelectedSlot = { -1, -1, -1, -1 };
			std::array<std::array<char, 64>, kAreaCount>    Search = {};

			// ------------------------------ Small helpers --------------------------------------------------------

			bool IContains(const string& a_text, const char* a_needle)
			{
				if (!a_needle || !*a_needle)
					return true;
				auto it = std::search(a_text.begin(), a_text.end(), a_needle, a_needle + std::strlen(a_needle),
					[](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
				return it != a_text.end();
			}

			// Shortens a_text with "..." until it fits a_width (keeps UTF-8 sequences whole)
			string Ellipsize(const string& a_text, float a_width)
			{
				if (ImGuiMCP::CalcTextSize(a_text.c_str()).x <= a_width)
					return a_text;
				string s = a_text;
				while (!s.empty() && ImGuiMCP::CalcTextSize((s + "...").c_str()).x > a_width) {
					while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80)
						s.pop_back();
					if (!s.empty())
						s.pop_back();
				}
				return s + "...";
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

			// Pending edits differ from what is applied
			bool HasChanges(const Slot& a_slot)
			{
				auto& applied = a_slot.applied;
				if (applied.kind != SlotKind::kTattoo)
					return !a_slot.pattern.empty();
				return !IEquals(applied.section, a_slot.section) || !IEquals(applied.name, a_slot.pattern) ||
				       applied.color != ToRGB(a_slot.ink) || applied.glow != ToRGB(a_slot.glow) || applied.glossy != a_slot.glossy;
			}

			// ------------------------------ Previews -------------------------------------------------------------

			// Path SlaveTats puts on the overlay, relative to Data\Textures
			string PreviewPath(const TattooDef& a_tattoo)
			{
				return a_tattoo.texture.empty() ? string() : "Actors\\Character\\slavetats\\" + a_tattoo.texture;
			}

			// Light neutral-gray checkerboard behind loaded thumbnails (an image editor's transparency grid):
			// tattoo textures are mostly black ink on transparency, so the backdrop must stay light for it to show
			constexpr ImU32 kCheckerA = Rgba(178, 178, 182);
			constexpr ImU32 kCheckerB = Rgba(204, 204, 208);
			constexpr float kCheckerCell = 8.0f;

			void DrawChecker(ImGuiMCP::ImDrawList* a_dl, ImVec2 a_min, ImVec2 a_max)
			{
				draw::AddRectFilled(a_dl, a_min, a_max, kCheckerA, kRounding, kAllCorners);
				// Keep the light cells off the rounded corners
				ImVec2 cmin{ a_min.x + 1.0f, a_min.y + 1.0f };
				ImVec2 cmax{ a_max.x - 1.0f, a_max.y - 1.0f };
				draw::PushClipRect(a_dl, cmin, cmax, true);
				int row = 0;
				for (float y = a_min.y; y < a_max.y; y += kCheckerCell, row++) {
					for (float x = a_min.x + ((row & 1) ? kCheckerCell : 0.0f); x < a_max.x; x += kCheckerCell * 2.0f) {
						bool corner = (x < a_min.x + kRounding || x + kCheckerCell > a_max.x - kRounding) &&
						              (y < a_min.y + kRounding || y + kCheckerCell > a_max.y - kRounding);
						if (!corner)
							draw::AddRectFilled(a_dl, { x, y }, { x + kCheckerCell, y + kCheckerCell }, kCheckerB, 0.0f, 0);
					}
				}
				draw::PopClipRect(a_dl);
			}

			// Draws the DDS thumbnail (fitted, keeping its aspect), or a hatched frame with a_label while there is no image
			void DrawPreview(ImGuiMCP::ImDrawList* a_dl, ImVec2 a_min, ImVec2 a_max, const string& a_texture, const char* a_label)
			{
				float w = a_max.x - a_min.x;
				float h = a_max.y - a_min.y;

				auto preview = dds_preview::Get(a_texture);
				if (preview.state == dds_preview::State::kReady && preview.texture) {
					DrawChecker(a_dl, a_min, a_max);
					float scale = std::min(w / std::max(1u, preview.width), h / std::max(1u, preview.height));
					float iw = preview.width * scale;
					float ih = preview.height * scale;
					ImVec2 imin{ a_min.x + (w - iw) * 0.5f, a_min.y + (h - ih) * 0.5f };
					draw::AddImage(a_dl, preview.texture, imin, { imin.x + iw, imin.y + ih }, { 0.0f, 0.0f }, { 1.0f, 1.0f }, Rgba(255, 255, 255));
					draw::AddRect(a_dl, a_min, a_max, kBorder, kRounding, kAllCorners, 1.0f);
					return;
				}
				if (preview.state == dds_preview::State::kLoading)
					a_label = "...";
				else if (preview.state == dds_preview::State::kFailed && std::strcmp(a_label, "DDS") == 0)
					a_label = "N/A";

				draw::AddRectFilled(a_dl, a_min, a_max, kPreviewBg, kRounding, kAllCorners);
				draw::PushClipRect(a_dl, a_min, a_max, true);
				for (float d = -h; d < w; d += 10.0f)
					draw::AddLine(a_dl, { a_min.x + d, a_max.y }, { a_min.x + d + h, a_min.y }, kHatch, 1.0f);
				draw::PopClipRect(a_dl);
				draw::AddRect(a_dl, a_min, a_max, kBorder, kRounding, kAllCorners, 1.0f);

				if (a_label && *a_label) {
					auto size = ImGuiMCP::CalcTextSize(a_label);
					if (size.x < w - 4.0f)
						draw::AddText(a_dl, { a_min.x + (w - size.x) * 0.5f, a_min.y + (h - size.y) * 0.5f }, kMuted, a_label);
				}
			}
			// ------------------------------ Widgets --------------------------------------------------------------

			// Rounded pill button for groups
			bool Chip(const char* a_label, bool a_active)
			{
				ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_FrameRounding, 12.0f);
				ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_FramePadding, ImVec2{ 10.0f, 4.0f });
				ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button, a_active ? kAccent : kCard);
				ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonHovered, a_active ? kAccentHover : kCardHover);
				ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonActive, kAccentHover);
				ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, a_active ? kAccentInk : kText);
				bool clicked = ImGuiMCP::Button(a_label);
				ImGuiMCP::PopStyleColor(4);
				ImGuiMCP::PopStyleVar(2);
				return clicked;
			}

			bool ColoredButton(const char* a_label, ImU32 a_bg, ImU32 a_hover, ImU32 a_text, float a_width = 0.0f)
			{
				ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_FrameRounding, kRounding);
				ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_FramePadding, ImVec2{ 14.0f, 6.0f });
				ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button, a_bg);
				ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonHovered, a_hover);
				ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ButtonActive, a_hover);
				ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, a_text);
				bool clicked = ImGuiMCP::Button(a_label, { a_width, 0.0f });
				ImGuiMCP::PopStyleColor(4);
				ImGuiMCP::PopStyleVar(2);
				return clicked;
			}

			bool PrimaryButton(const char* a_label) { return ColoredButton(a_label, kAccent, kAccentHover, kAccentInk, 130.0f); }
			bool DangerButton(const char* a_label) { return ColoredButton(a_label, Rgba(222, 92, 92, 60), kDangerHover, kText); }
			bool GhostButton(const char* a_label) { return ColoredButton(a_label, kCard, kCardHover, kText); }

			// Swatches like the SlaveTats MCM color dialog: deep row, then light row
			constexpr int kPalette[] = {
				0xB71C1C, 0xC2185B, 0xB8860B, 0xA0522D, 0xE65100, 0x00838F, 0x1A237E, 0x4A148C, 0x6D3B5E, 0x558B2F, 0x1B5E20, 0x5D4037, 0x757575, 0x000000,
				0xF44336, 0xF06292, 0xFFC107, 0xD2A06D, 0xFF9800, 0x80DEEA, 0x3F51B5, 0x9575CD, 0xC48BA0, 0xCDDC39, 0x2ECC71, 0xD7CCC8, 0xBDBDBD, 0xFFFFFF
			};
			constexpr int kPaletteColumns = 14;

			// Swatch + label; clicking opens a compact popup (palette + small picker) instead of ImGui's
			// font-sized one, which gets huge with Skyrim's menu font
			void ColorField(const char* a_label, float a_color[3], const char* a_tooltip)
			{
				ImGuiMCP::PushID(a_label);
				const auto flags = ImGuiMCP::ImGuiColorEditFlags_NoTooltip | ImGuiMCP::ImGuiColorEditFlags_NoAlpha;
				float      frame = ImGuiMCP::GetFrameHeight();

				if (ImGuiMCP::ColorButton("##swatch", { a_color[0], a_color[1], a_color[2], 1.0f }, flags, { frame * 1.6f, frame }))
					ImGuiMCP::OpenPopup("picker");
				if (ImGuiMCP::IsItemHovered())
					ImGuiMCP::SetTooltip("%s\n#%06X", a_tooltip, ToRGB(a_color));
				ImGuiMCP::SameLine(0.0f, 6.0f);
				ImGuiMCP::AlignTextToFramePadding();
				ImGuiMCP::TextUnformatted(a_label);

				if (ImGuiMCP::BeginPopup("picker")) {
					ImGuiMCP::TextDisabled("%s", a_label);

					const float swatch = 18.0f;
					const float gap = 3.0f;
					ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ItemSpacing, ImVec2{ gap, gap });
					int current = ToRGB(a_color);
					for (int i = 0; i < static_cast<int>(std::size(kPalette)); i++) {
						if (i % kPaletteColumns != 0)
							ImGuiMCP::SameLine();
						float c[3];
						ToFloat(kPalette[i], c);
						ImGuiMCP::PushID(i);
						if (ImGuiMCP::ColorButton("##p", { c[0], c[1], c[2], 1.0f }, flags, { swatch, swatch }))
							std::copy_n(c, 3, a_color);
						if (kPalette[i] == current) {
							auto dl = ImGuiMCP::GetWindowDrawList();
							draw::AddRect(dl, ImGuiMCP::GetItemRectMin(), ImGuiMCP::GetItemRectMax(), kAccent, 2.0f, kAllCorners, 2.0f);
						}
						ImGuiMCP::PopID();
					}
					ImGuiMCP::PopStyleVar();

					ImGuiMCP::Spacing();
					// Fixed width: ImGui would size the picker from the (large) menu font
					ImGuiMCP::SetNextItemWidth(std::min(240.0f, kPaletteColumns * (swatch + gap) - gap));
					ImGuiMCP::ColorPicker3("##picker", a_color,
						ImGuiMCP::ImGuiColorEditFlags_NoSidePreview | ImGuiMCP::ImGuiColorEditFlags_NoLabel | ImGuiMCP::ImGuiColorEditFlags_NoAlpha |
							ImGuiMCP::ImGuiColorEditFlags_PickerHueBar | ImGuiMCP::ImGuiColorEditFlags_DisplayHex);
					ImGuiMCP::EndPopup();
				}
				ImGuiMCP::PopID();
			}

			// Muted caps label above a block of controls
			void SectionLabel(const char* a_text)
			{
				ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, kMuted);
				ImGuiMCP::TextUnformatted(a_text);
				ImGuiMCP::PopStyleColor();
			}

			void CenteredHint(const char* a_text)
			{
				auto avail = ImGuiMCP::GetContentRegionAvail();
				auto size = ImGuiMCP::CalcTextSize(a_text);
				auto pos = ImGuiMCP::GetCursorScreenPos();
				ImGuiMCP::SetCursorScreenPos({ pos.x + std::max(0.0f, (avail.x - size.x) * 0.5f), pos.y + std::max(0.0f, (avail.y - size.y) * 0.5f) });
				ImGuiMCP::TextDisabled("%s", a_text);
			}

			// ------------------------------ Slot list (left) -----------------------------------------------------

			bool SlotCard(int a_area, int a_index, bool a_selected)
			{
				auto& slot = Slots[a_area][a_index];
				auto  dl = ImGuiMCP::GetWindowDrawList();
				auto  pos = ImGuiMCP::GetCursorScreenPos();
				float width = ImGuiMCP::GetContentRegionAvail().x;

				ImGuiMCP::PushID(a_index);
				bool clicked = ImGuiMCP::InvisibleButton("card", { width, kSlotCardH });
				bool hovered = ImGuiMCP::IsItemHovered();
				ImGuiMCP::PopID();

				ImVec2 max{ pos.x + width, pos.y + kSlotCardH };
				draw::AddRectFilled(dl, pos, max, a_selected ? kAccentSoft : (hovered ? kCardHover : kCard), kRounding, kAllCorners);
				if (a_selected)
					draw::AddRectFilled(dl, pos, { pos.x + 3.0f, max.y }, kAccent, kRounding, ImGuiMCP::ImDrawFlags_RoundCornersLeft);

				// What the slot holds
				auto&       applied = slot.applied;
				string      texture;
				const char* thumb_label = "+";
				string      subtitle = "Empty";
				ImU32       subtitle_color = kMuted;
				ImU32       dot = 0;
				if (applied.kind == SlotKind::kExternal) {
					thumb_label = "EXT";
					subtitle = "Used by another mod";
					subtitle_color = kWarn;
					dot = kWarn;
				} else if (applied.kind == SlotKind::kTattoo) {
					if (auto def = FindTattoo(a_area, applied.section, applied.name))
						texture = PreviewPath(*def);
					thumb_label = "DDS";
					subtitle = applied.name;
					subtitle_color = kText;
					dot = applied.locked ? kDanger : kOk;
				}
				if (applied.kind != SlotKind::kExternal && HasChanges(slot)) {
					subtitle = slot.pattern.empty() ? "Unsaved: remove" : "Unsaved: " + slot.pattern;
					subtitle_color = kPending;
					dot = kPending;
				}

				const float pad = 7.0f;
				const float thumb = kSlotCardH - pad * 2.0f;
				ImVec2      tmin{ pos.x + pad + 4.0f, pos.y + pad };
				DrawPreview(dl, tmin, { tmin.x + thumb, tmin.y + thumb }, texture, thumb_label);

				float  tx = tmin.x + thumb + 10.0f;
				float  line = ImGuiMCP::GetTextLineHeight();
				float  top = pos.y + (kSlotCardH - line * 2.0f - 3.0f) * 0.5f;
				string title = fmt::format("Slot {}", a_index + 1);
				draw::AddText(dl, { tx, top }, kText, title.c_str());
				string sub = Ellipsize(subtitle, max.x - tx - 24.0f);
				draw::AddText(dl, { tx, top + line + 3.0f }, subtitle_color, sub.c_str());
				if (dot)
					draw::AddCircleFilled(dl, { max.x - 13.0f, pos.y + kSlotCardH * 0.5f }, 4.0f, dot, 12);

				if (hovered && applied.kind == SlotKind::kTattoo) {
					ImGuiMCP::SetTooltip("%s / %s%s", applied.section.c_str(), applied.name.c_str(), applied.locked ? "\n(locked)" : "");
				}
				return clicked;
			}

			// ------------------------------ Pattern grid ---------------------------------------------------------

			bool TattooTile(const TattooDef& a_tattoo, const string& a_section, bool a_selected, bool a_applied, bool a_show_section)
			{
				auto  dl = ImGuiMCP::GetWindowDrawList();
				auto  pos = ImGuiMCP::GetCursorScreenPos();
				float line = ImGuiMCP::GetTextLineHeight();
				ImVec2 size{ kTile, kTile + line * (a_show_section ? 2.0f : 1.0f) + 10.0f };

				bool clicked = ImGuiMCP::InvisibleButton("tile", size);
				bool hovered = ImGuiMCP::IsItemHovered();

				ImVec2 max{ pos.x + size.x, pos.y + size.y };
				draw::AddRectFilled(dl, pos, max, a_selected ? kAccentSoft : (hovered ? kCardHover : kCard), kRounding, kAllCorners);

				const float inset = 5.0f;
				ImVec2      pmin{ pos.x + inset, pos.y + inset };
				ImVec2      pmax{ pos.x + kTile - inset, pos.y + kTile - inset };
				DrawPreview(dl, pmin, pmax, PreviewPath(a_tattoo), "No preview");

				float  text_w = kTile - inset * 2.0f;
				string name = Ellipsize(a_tattoo.name, text_w);
				float  name_w = ImGuiMCP::CalcTextSize(name.c_str()).x;
				draw::AddText(dl, { pos.x + (kTile - name_w) * 0.5f, pmax.y + 4.0f }, a_selected ? kAccent : kText, name.c_str());
				if (a_show_section) {
					string section = Ellipsize(a_section, text_w);
					float  section_w = ImGuiMCP::CalcTextSize(section.c_str()).x;
					draw::AddText(dl, { pos.x + (kTile - section_w) * 0.5f, pmax.y + 4.0f + line }, kMuted, section.c_str());
				}

				if (a_selected)
					draw::AddRect(dl, pos, max, kAccent, kRounding, kAllCorners, 2.0f);
				if (a_applied) {
					ImVec2 c{ pmax.x - 9.0f, pmin.y + 9.0f };
					draw::AddCircleFilled(dl, c, 6.0f, kOk, 16);
					draw::AddCircle(dl, c, 6.0f, Rgba(0, 0, 0, 160), 16, 1.5f);
				}

				if (hovered) {
					ImGuiMCP::SetTooltip("%s\n%s%s%s%s\n%s", a_tattoo.name.c_str(), a_section.c_str(),
						a_tattoo.credit.empty() ? "" : "\nTattoo by: ", a_tattoo.credit.c_str(),
						a_applied ? "\n(applied in this slot)" : "", a_tattoo.texture.c_str());
				}
				return clicked;
			}

			void RenderGroups(int a_area, Slot& a_slot)
			{
				auto& sections = TattooCatalog.areas[a_area];

				SectionLabel("GROUP");
				ImGuiMCP::SameLine();
				float search_w = std::min(220.0f, ImGuiMCP::GetContentRegionAvail().x * 0.5f);
				ImGuiMCP::SetCursorPosX(ImGuiMCP::GetCursorPosX() + ImGuiMCP::GetContentRegionAvail().x - search_w);
				ImGuiMCP::SetNextItemWidth(search_w);
				auto& search = Search[a_area];
				ImGuiMCP::InputTextWithHint("##search", "Search all patterns...", search.data(), search.size(), ImGuiMCP::ImGuiInputTextFlags_AutoSelectAll);

				if (sections.empty()) {
					ImGuiMCP::TextDisabled("No tattoo packs for this area. Press \"Add/remove tattoos\" on the Setup page.");
					return;
				}

				// Chips wrap onto as many rows as needed, the strip scrolls after ~3 rows
				float row_h = ImGuiMCP::GetFrameHeight() + 6.0f;
				ImGuiMCP::BeginChild("groups", { 0.0f, row_h * 3.0f + 4.0f });
				float right = ImGuiMCP::GetCursorPosX() + ImGuiMCP::GetContentRegionAvail().x;
				int   i = 0;
				for (auto& [name, tattoos] : sections) {
					string label = fmt::format("{} ({})", name, tattoos.size());
					float  chip_w = ImGuiMCP::CalcTextSize(label.c_str()).x + 20.0f;
					if (i > 0) {
						ImGuiMCP::SameLine(0.0f, 6.0f);
						if (ImGuiMCP::GetCursorPosX() + chip_w > right)
							ImGuiMCP::NewLine();
					}
					ImGuiMCP::PushID(i++);
					if (Chip(label.c_str(), IEquals(a_slot.browse, name))) {
						a_slot.browse = name;
						search[0] = '\0';
					}
					ImGuiMCP::PopID();
				}
				ImGuiMCP::EndChild();
			}

			void RenderPatternGrid(int a_area, Slot& a_slot, float a_height)
			{
				auto& sections = TattooCatalog.areas[a_area];
				auto& search = Search[a_area];
				bool  searching = search[0] != '\0';

				// Tiles to show: the browsed group, or every match when searching
				std::vector<std::pair<const string*, const TattooDef*>> tiles;
				if (searching) {
					for (auto& [name, tattoos] : sections)
						for (auto& tattoo : tattoos)
							if (IContains(tattoo.name, search.data()) || IContains(name, search.data()))
								tiles.emplace_back(&name, &tattoo);
				} else if (auto it = sections.find(a_slot.browse); it != sections.end()) {
					for (auto& tattoo : it->second)
						tiles.emplace_back(&it->first, &tattoo);
				}

				ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_ChildBg, Rgba(0, 0, 0, 40));
				ImGuiMCP::BeginChild("grid", { 0.0f, a_height }, ImGuiMCP::ImGuiChildFlags_Border);
				if (tiles.empty()) {
					CenteredHint(searching ? "No pattern matches your search." : "Pick a group above to browse its patterns.");
				} else {
					const float spacing = 8.0f;
					float       avail = ImGuiMCP::GetContentRegionAvail().x;
					int         columns = std::max(1, static_cast<int>((avail + spacing) / (kTile + spacing)));
					ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ItemSpacing, ImVec2{ spacing, spacing });
					for (int i = 0; i < static_cast<int>(tiles.size()); i++) {
						auto [section, tattoo] = tiles[i];
						if (i % columns != 0)
							ImGuiMCP::SameLine();
						bool selected = IEquals(a_slot.section, *section) && IEquals(a_slot.pattern, tattoo->name);
						bool applied = a_slot.applied.kind == SlotKind::kTattoo && IEquals(a_slot.applied.section, *section) &&
						               IEquals(a_slot.applied.name, tattoo->name);
						ImGuiMCP::PushID(i);
						if (TattooTile(*tattoo, *section, selected, applied, searching)) {
							SelectTattoo(a_slot, *section, tattoo);
							a_slot.browse = *section;
						}
						ImGuiMCP::PopID();
					}
					ImGuiMCP::PopStyleVar();
				}
				ImGuiMCP::EndChild();
				ImGuiMCP::PopStyleColor();
			}

			// ------------------------------ Slot editor (right) --------------------------------------------------

			void RenderEditor(int a_area, int a_index)
			{
				auto& slot = Slots[a_area][a_index];
				auto& applied = slot.applied;

				// Header: slot + what is on it
				ImGuiMCP::Text("Slot %d", a_index + 1);
				ImGuiMCP::SameLine();
				if (applied.kind == SlotKind::kExternal) {
					ImGuiMCP::TextColored(ToVec4(kWarn), "%s", "External");
				} else if (applied.kind == SlotKind::kTattoo) {
					ImGuiMCP::TextColored(ToVec4(applied.locked ? kDanger : kOk), "%s", applied.locked ? "Locked" : "Applied");
					ImGuiMCP::SameLine();
					bool installed = TattooCatalog.areas[a_area].contains(applied.section);
					ImGuiMCP::TextDisabled("%s / %s%s", applied.section.c_str(), applied.name.c_str(), installed ? "" : "  (pack not installed)");
				} else {
					ImGuiMCP::TextDisabled("Empty");
				}
				ImGuiMCP::Separator();
				ImGuiMCP::Spacing();

				if (applied.kind == SlotKind::kExternal) {
					CenteredHint("This overlay slot is used by another mod and can't be edited here.");
					return;
				}

				ImGuiMCP::BeginDisabled(applied.locked);

				RenderGroups(a_area, slot);
				ImGuiMCP::Spacing();

				// The footer (selection, colors, buttons, error) keeps a fixed height below the grid
				float frame = ImGuiMCP::GetFrameHeight();
				float footer = frame * 3.0f + ImGuiMCP::GetTextLineHeight() * 2.0f + 40.0f;
				float grid_h = std::max(kTile + 40.0f, ImGuiMCP::GetContentRegionAvail().y - footer);
				SectionLabel("PATTERN");
				RenderPatternGrid(a_area, slot, grid_h);

				ImGuiMCP::Spacing();
				auto selected = Selected(a_area, slot);
				if (selected) {
					ImGuiMCP::Text("%s", selected->name.c_str());
					ImGuiMCP::SameLine();
					ImGuiMCP::TextDisabled("%s%s%s", slot.section.c_str(), selected->credit.empty() ? "" : "  -  by ", selected->credit.c_str());
				} else {
					ImGuiMCP::TextDisabled("%s", applied.kind == SlotKind::kTattoo ? "No pattern selected" : "Click a pattern to select it");
				}

				// Colors
				ColorField("Ink", slot.ink, "Pick the ink color for this tattoo.");
				ImGuiMCP::SameLine(0.0f, 24.0f);
				ColorField("Glow", slot.glow, "Pick a color for the tattoo to glow (black is no glow).");
				ImGuiMCP::SameLine(0.0f, 24.0f);
				ImGuiMCP::Checkbox("Glossy", &slot.glossy);
				if (ImGuiMCP::IsItemHovered())
					ImGuiMCP::SetTooltip("Checking this makes the tattoo slightly glossy, like skin.");

				// Actions
				ImGuiMCP::Spacing();
				bool changed = HasChanges(slot);
				ImGuiMCP::BeginDisabled(Working > 0);
				if (applied.kind != SlotKind::kTattoo) {
					ImGuiMCP::BeginDisabled(!selected);
					if (PrimaryButton("Save Tattoo"))
						ApplySlot(a_area, a_index);
					ImGuiMCP::EndDisabled();
				} else {
					ImGuiMCP::BeginDisabled(!selected || !changed);
					if (PrimaryButton("Replace"))
						ApplySlot(a_area, a_index);
					ImGuiMCP::EndDisabled();
					ImGuiMCP::SameLine();
					if (DangerButton("Remove"))
						RemoveSlot(a_area, a_index);
				}
				ImGuiMCP::SameLine();
				ImGuiMCP::BeginDisabled(!changed);
				if (GhostButton("Undo"))
					ResetPending(slot);
				ImGuiMCP::EndDisabled();
				if (Working > 0) {
					ImGuiMCP::SameLine();
					ImGuiMCP::TextDisabled("Working...");
				}
				ImGuiMCP::EndDisabled();

				ImGuiMCP::EndDisabled();  // locked

				if (applied.locked)
					ImGuiMCP::TextDisabled("Locked: this tattoo can only be removed by the mod that applied it.");
				if (!slot.error.empty())
					ImGuiMCP::TextColored(ToVec4(kDanger), "%s", slot.error.c_str());
			}

			// ------------------------------ Area page ------------------------------------------------------------

			void RenderArea(int a_area)
			{
				DrainQueue();
				dds_preview::Pump();

				if (!HasTarget) {
					CenteredHint("Go to Setup and select a target.");
					return;
				}

				ImGuiMCP::Text("%s Tattoos", kPageNames[a_area]);
				ImGuiMCP::SameLine();
				ImGuiMCP::TextDisabled("on %s", TargetName.c_str());
				if (Working > 0) {
					ImGuiMCP::SameLine();
					ImGuiMCP::TextColored(ToVec4(kPending), "%s", "(working...)");
				}

				if (!SlotsKnown[a_area]) {
					CenteredHint("Reading overlays...");
					return;
				}
				auto& slots = Slots[a_area];
				if (slots.empty()) {
					ImGuiMCP::TextDisabled("No %s overlay slots (check RaceMenu/SKEE is installed and iNumOverlays in skee64.ini).", kAreas[a_area]);
					return;
				}

				auto& selected = SelectedSlot[a_area];
				if (selected >= static_cast<int>(slots.size()))
					selected = -1;

				float avail = ImGuiMCP::GetContentRegionAvail().x;
				float list_w = std::clamp(avail * 0.28f, 200.0f, 280.0f);

				ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ChildRounding, 8.0f);

				// Left: collapsed slot list
				ImGuiMCP::BeginChild("slots", { list_w, 0.0f }, ImGuiMCP::ImGuiChildFlags_Border);
				ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ItemSpacing, ImVec2{ 6.0f, 6.0f });
				for (int i = 0; i < static_cast<int>(slots.size()); i++) {
					if (SlotCard(a_area, i, i == selected)) {
						selected = i;
						if (slots[i].browse.empty())
							slots[i].browse = slots[i].section;
					}
				}
				ImGuiMCP::PopStyleVar();
				ImGuiMCP::EndChild();

				ImGuiMCP::SameLine();

				// Right: editor for the chosen slot
				ImGuiMCP::BeginChild("editor", { 0.0f, 0.0f }, ImGuiMCP::ImGuiChildFlags_Border);
				if (selected < 0)
					CenteredHint("Select a slot on the left to edit it.");
				else
					RenderEditor(a_area, selected);
				ImGuiMCP::EndChild();

				ImGuiMCP::PopStyleVar();
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
