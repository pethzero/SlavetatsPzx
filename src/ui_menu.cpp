#include "jcontainers_wrapper.h"
#include "nioverride_wrapper.h"
#include "constants.h"
#include "cache.h"
#include "tattoo.h"
#include "overlays.h"
#include "ui_menu.h"
#include "dds_preview.h"
#include "tattoo_model.h"
#include "tattoo_browser.h"
#include "tattoo_thumbs.h"
#include "tattoo_presets.h"

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
			using namespace tattoo_model;

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

			// ------------------------------ Previews -------------------------------------------------------------

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

			// ------------------------------ Presets page -------------------------------------------------------
			// Same presets as the browser's PRESET tab (tattoo_presets): my 12 + the shared ones on the left, the
			// selected one's tattoos and actions on the right.

			int                  PresetSlot = -1;   // one of my 12 selected, or
			int                  SharedIndex = -1;  // one of the shared ones
			std::array<char, 64> PresetName = {};   // name used by Save / Rename
			int                  CopySlot = 0;      // where Copy puts a shared preset
			bool                 ConfirmDelete = false;
			int                  LastPresetFrame = -100;

			bool PresetCard(const tattoo_presets::PresetFile& a_file, const string& a_title, bool a_selected, int a_id)
			{
				auto  dl = ImGuiMCP::GetWindowDrawList();
				auto  pos = ImGuiMCP::GetCursorScreenPos();
				float width = ImGuiMCP::GetContentRegionAvail().x;

				ImGuiMCP::PushID(a_id);
				bool clicked = ImGuiMCP::InvisibleButton("preset", { width, kSlotCardH });
				bool hovered = ImGuiMCP::IsItemHovered();
				ImGuiMCP::PopID();

				ImVec2 max{ pos.x + width, pos.y + kSlotCardH };
				draw::AddRectFilled(dl, pos, max, a_selected ? kAccentSoft : (hovered ? kCardHover : kCard), kRounding, kAllCorners);
				if (a_selected)
					draw::AddRectFilled(dl, pos, { pos.x + 3.0f, max.y }, kAccent, kRounding, ImGuiMCP::ImDrawFlags_RoundCornersLeft);

				string texture;
				string subtitle = "Empty";
				ImU32  subtitle_color = kMuted;
				if (a_file.exists && a_file.error.empty()) {
					if (auto first = tattoo_presets::FirstInstalled(a_file.preset))
						texture = PreviewPath(*first);
					int count = static_cast<int>(a_file.preset.tattoos.size());
					int missing = tattoo_presets::MissingCount(a_file.preset);
					subtitle = missing ? fmt::format("{} tattoos - {} missing", count, missing) : fmt::format("{} tattoo{}", count, count == 1 ? "" : "s");
					subtitle_color = missing ? kWarn : kText;
				} else if (a_file.exists) {
					subtitle = "Can't read this file";
					subtitle_color = kDanger;
				}

				const float pad = 7.0f;
				const float thumb = kSlotCardH - pad * 2.0f;
				ImVec2      tmin{ pos.x + pad + 4.0f, pos.y + pad };
				DrawPreview(dl, tmin, { tmin.x + thumb, tmin.y + thumb }, texture, a_file.exists ? "" : "+");

				float  tx = tmin.x + thumb + 10.0f;
				float  line = ImGuiMCP::GetTextLineHeight();
				float  top = pos.y + (kSlotCardH - line * 2.0f - 3.0f) * 0.5f;
				string title = Ellipsize(a_title, max.x - tx - 10.0f);
				draw::AddText(dl, { tx, top }, a_file.exists ? kText : kMuted, title.c_str());
				string sub = Ellipsize(subtitle, max.x - tx - 10.0f);
				draw::AddText(dl, { tx, top + line + 3.0f }, subtitle_color, sub.c_str());

				if (hovered && a_file.exists)
					ImGuiMCP::SetTooltip("%s%s%s", a_file.file.c_str(), a_file.error.empty() ? "" : "\n", a_file.error.c_str());
				return clicked;
			}

			void SelectPreset(int a_slot, int a_shared)
			{
				PresetSlot = a_slot;
				SharedIndex = a_shared;
				ConfirmDelete = false;
				string name;
				if (a_slot >= 0) {
					auto& file = tattoo_presets::Mine[a_slot];
					name = file.exists && file.error.empty() ? file.preset.name : fmt::format("Preset {}", a_slot + 1);
				}
				strncpy_s(PresetName.data(), PresetName.size(), name.c_str(), _TRUNCATE);
			}

			void RenderPresetTattoos(const tattoo_presets::Preset& a_preset)
			{
				if (a_preset.tattoos.empty())
					ImGuiMCP::TextDisabled("%s", "No tattoos: loading it takes every tattoo off.");
				float line = ImGuiMCP::GetTextLineHeight();
				for (auto& t : a_preset.tattoos) {
					auto   dl = ImGuiMCP::GetWindowDrawList();
					auto   pos = ImGuiMCP::GetCursorScreenPos();
					float  c[3];
					ToFloat(t.color, c);
					ImU32  ink = Rgba(static_cast<int>(c[0] * 255.0f), static_cast<int>(c[1] * 255.0f), static_cast<int>(c[2] * 255.0f));
					draw::AddRectFilled(dl, { pos.x, pos.y + 2.0f }, { pos.x + line, pos.y + line - 2.0f }, ink, 3.0f, kAllCorners);
					draw::AddRect(dl, { pos.x, pos.y + 2.0f }, { pos.x + line, pos.y + line - 2.0f }, kBorder, 3.0f, kAllCorners, 1.0f);
					ImGuiMCP::Dummy({ line, line });
					ImGuiMCP::SameLine();
					ImGuiMCP::TextDisabled("%s %d", kAreas[t.area], t.slot + 1);
					ImGuiMCP::SameLine();
					ImGuiMCP::Text("%s", t.name.c_str());
					ImGuiMCP::SameLine();
					ImGuiMCP::TextDisabled("%s%s", t.section.c_str(), t.glossy ? "  -  glossy" : "");
					if (TattooCatalog.loaded && !FindTattoo(t.area, t.section, t.name)) {
						ImGuiMCP::SameLine();
						ImGuiMCP::TextColored(ToVec4(kDanger), "%s", "not installed");
					}
				}
			}

			void RenderPresetActions()
			{
				using namespace tattoo_presets;
				bool any = true;  // a button is on the action row
				ImGuiMCP::BeginDisabled(Working > 0);
				if (PresetSlot >= 0) {
					auto& file = Mine[PresetSlot];
					bool  usable = file.exists && file.error.empty();
					ImGuiMCP::SetNextItemWidth(240.0f);
					ImGuiMCP::InputText("Name", PresetName.data(), PresetName.size());
					if (PrimaryButton("Save Current Look"))
						SaveCurrent(PresetSlot, PresetName.data());
					if (ImGuiMCP::IsItemHovered())
						ImGuiMCP::SetTooltip("Saves the %d tattoos %s wears now under this name%s.", static_cast<int>(CurrentLook().size()), TargetName.c_str(),
							file.exists ? ", replacing what this slot holds" : "");
					if (usable) {
						ImGuiMCP::SameLine();
						if (GhostButton("Load"))
							Load(file.preset);
						ImGuiMCP::SameLine();
						ImGuiMCP::BeginDisabled(PresetName[0] == '\0');
						if (GhostButton("Rename"))
							Rename(PresetSlot, PresetName.data());
						ImGuiMCP::EndDisabled();
					}
					if (file.exists) {
						ImGuiMCP::SameLine();
						if (DangerButton(ConfirmDelete ? "Click again to delete" : "Delete")) {
							if (ConfirmDelete) {
								Delete(PresetSlot);
								SelectPreset(PresetSlot, -1);
							} else {
								ConfirmDelete = true;
							}
						}
					}
				} else if (SharedIndex >= 0 && Shared[SharedIndex].error.empty()) {
					if (PrimaryButton("Load"))
						Load(Shared[SharedIndex].preset);
					ImGuiMCP::SameLine();
					auto   slot_name = [](int i) {
						auto& file = Mine[i];
						return file.exists ? fmt::format("{}  {} (replaces it)", i + 1, file.error.empty() ? file.preset.name : file.file) : fmt::format("{}  Empty", i + 1);
					};
					CopySlot = std::clamp(CopySlot, 0, kSlotCount - 1);
					string current = slot_name(CopySlot);
					ImGuiMCP::SetNextItemWidth(220.0f);
					if (ImGuiMCP::BeginCombo("##copyslot", current.c_str())) {
						for (int i = 0; i < kSlotCount; i++) {
							string label = slot_name(i) + fmt::format("##c{}", i);
							if (ImGuiMCP::Selectable(label.c_str(), CopySlot == i))
								CopySlot = i;
						}
						ImGuiMCP::EndCombo();
					}
					ImGuiMCP::SameLine();
					if (GhostButton("Copy to My Presets")) {
						if (CopyToSlot(Shared[SharedIndex].preset, CopySlot))
							SelectPreset(CopySlot, -1);
					}
				} else {
					any = false;
				}
				if (CanUndoLoad()) {
					if (any)
						ImGuiMCP::SameLine();
					if (GhostButton("Undo Load"))
						UndoLoad();
					if (ImGuiMCP::IsItemHovered())
						ImGuiMCP::SetTooltip("Put back the tattoos that were on before the last Load.");
				}
				ImGuiMCP::EndDisabled();

				if (!Message.empty())
					ImGuiMCP::TextColored(ToVec4(MessageIsError ? kWarn : kOk), "%s", Message.c_str());
			}

			void RenderPresets()
			{
				using namespace tattoo_presets;
				DrainQueue();
				dds_preview::Pump();

				if (!HasTarget) {
					CenteredHint("Go to Setup and select a target.");
					return;
				}

				// The page was just opened (not drawn last frame): read the files again, shared ones may be new
				int frame = ImGuiMCP::GetFrameCount();
				if (frame - LastPresetFrame > 2)
					Rescan();
				LastPresetFrame = frame;
				if (PresetSlot >= kSlotCount)
					PresetSlot = -1;
				if (SharedIndex >= static_cast<int>(Shared.size()))
					SharedIndex = -1;

				ImGuiMCP::Text("%s", "Presets");
				ImGuiMCP::SameLine();
				ImGuiMCP::TextDisabled("on %s", TargetName.c_str());
				if (Working > 0) {
					ImGuiMCP::SameLine();
					ImGuiMCP::TextColored(ToVec4(kPending), "%s", "(working...)");
				}
				ImGuiMCP::SameLine();
				if (ImGuiMCP::SmallButton("Refresh"))
					Rescan();
				if (ImGuiMCP::IsItemHovered())
					ImGuiMCP::SetTooltip("Read the preset files again.\nMine: %s\nShared (from other people): %s", Folder().c_str(), SharedFolder().c_str());

				float avail = ImGuiMCP::GetContentRegionAvail().x;
				float list_w = std::clamp(avail * 0.30f, 220.0f, 300.0f);
				ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ChildRounding, 8.0f);

				// Left: my 12, then the shared ones
				ImGuiMCP::BeginChild("presets", { list_w, 0.0f }, ImGuiMCP::ImGuiChildFlags_Border);
				ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ItemSpacing, ImVec2{ 6.0f, 6.0f });
				SectionLabel("MY PRESETS");
				for (int i = 0; i < kSlotCount; i++) {
					auto&  file = Mine[i];
					string title = file.exists ? fmt::format("{}  {}", i + 1, file.error.empty() ? file.preset.name : file.file) : fmt::format("{}  Empty", i + 1);
					if (PresetCard(file, title, PresetSlot == i, i))
						SelectPreset(i, -1);
				}
				ImGuiMCP::Spacing();
				SectionLabel("SHARED");
				if (Shared.empty())
					ImGuiMCP::TextDisabled("%s", "None. Put .json presets from others in Presets/Shared, then Refresh.");
				for (int i = 0; i < static_cast<int>(Shared.size()); i++) {
					auto& file = Shared[i];
					if (PresetCard(file, file.error.empty() ? file.preset.name : file.file, SharedIndex == i, 100 + i))
						SelectPreset(-1, i);
				}
				ImGuiMCP::PopStyleVar();
				ImGuiMCP::EndChild();

				ImGuiMCP::SameLine();

				// Right: what the selected one holds, and its actions
				ImGuiMCP::BeginChild("preset", { 0.0f, 0.0f }, ImGuiMCP::ImGuiChildFlags_Border);
				const PresetFile* selected = PresetSlot >= 0 ? &Mine[PresetSlot] : (SharedIndex >= 0 ? &Shared[SharedIndex] : nullptr);
				if (!selected) {
					CenteredHint("Select a preset on the left. Load puts the whole look on; Save Current Look stores it.");
				} else {
					if (!selected->exists) {
						ImGuiMCP::Text("Slot %d", PresetSlot + 1);
						ImGuiMCP::SameLine();
						ImGuiMCP::TextDisabled("%s", "Empty");
					} else if (!selected->error.empty()) {
						ImGuiMCP::TextColored(ToVec4(kDanger), "%s: %s", selected->file.c_str(), selected->error.c_str());
					} else {
						ImGuiMCP::Text("%s", selected->preset.name.c_str());
						ImGuiMCP::SameLine();
						ImGuiMCP::TextDisabled("%s%s%s%s", selected->preset.character.empty() ? "" : "worn by ", selected->preset.character.c_str(),
							selected->preset.saved.empty() ? "" : "  -  ", selected->preset.saved.c_str());
					}
					ImGuiMCP::Separator();

					// The list takes what the footer (name, buttons, message) leaves
					float frame_h = ImGuiMCP::GetFrameHeight();
					float footer = frame_h * 2.0f + ImGuiMCP::GetTextLineHeight() * 2.0f + 36.0f;
					ImGuiMCP::BeginChild("preset_tattoos", { 0.0f, std::max(80.0f, ImGuiMCP::GetContentRegionAvail().y - footer) });
					if (!selected->exists)
						ImGuiMCP::TextDisabled("Save Current Look stores the %d tattoos %s wears now. Locked tattoos and other mods' overlays are left out.",
							static_cast<int>(CurrentLook().size()), TargetName.c_str());
					else if (selected->error.empty())
						RenderPresetTattoos(selected->preset);
					ImGuiMCP::EndChild();

					ImGuiMCP::Spacing();
					RenderPresetActions();
				}
				ImGuiMCP::EndChild();

				ImGuiMCP::PopStyleVar();
			}

			// ------------------------------ Tattoo Browser (Setup section) ---------------------------------------
			// The browser itself is a FLICK window (tattoo_browser.cpp); this page opens it and sets its hotkey.

			std::atomic<bool> CapturingHotkey = false;
			constexpr std::uint32_t kEscape = 0x01;

			// Menu Framework sees keys while its menu is open, which is when "Set hotkey" waits for one
			bool __stdcall OnInput(RE::InputEvent* a_event)
			{
				if (!CapturingHotkey)
					return false;
				for (auto event = a_event; event; event = event->next) {
					auto button = event->AsButtonEvent();
					if (!button || button->GetDevice() != RE::INPUT_DEVICE::kKeyboard || !button->IsDown())
						continue;
					CapturingHotkey = false;
					if (button->GetIDCode() != kEscape)
						tattoo_browser::SetHotkey(button->GetIDCode());
					break;
				}
				return false;  // never swallow game input
			}

			void RenderBrowserSetup()
			{
				ImGuiMCP::SeparatorText("Tattoo Browser");
				bool available = tattoo_browser::IsAvailable();
				ImGuiMCP::BeginDisabled(!available);
				if (PrimaryButton("Open Tattoo Browser")) {
					if (auto main = SKSEMenuFramework::GetMainWindow())
						main->IsOpen = false;
					tattoo_browser::Open();
				}
				ImGuiMCP::EndDisabled();
				if (ImGuiMCP::IsItemHovered())
					ImGuiMCP::SetTooltip("Closes this menu and opens the browser beside the character.\nDrag outside the browser to turn the character.");

				ImGuiMCP::SameLine(0.0f, 18.0f);
				ImGuiMCP::AlignTextToFramePadding();
				ImGuiMCP::Text("Hotkey:");
				ImGuiMCP::SameLine();
				auto hotkey = tattoo_browser::GetHotkey();
				if (CapturingHotkey) {
					ImGuiMCP::TextColored(ToVec4(kPending), "%s", "Press a key... (Esc to cancel)");
				} else {
					ImGuiMCP::TextColored(ToVec4(kAccent), "%s", tattoo_browser::KeyName(hotkey).c_str());
					ImGuiMCP::SameLine();
					if (GhostButton("Set hotkey"))
						CapturingHotkey = true;
					if (hotkey != 0) {
						ImGuiMCP::SameLine();
						if (GhostButton("Clear"))
							tattoo_browser::SetHotkey(0);
					}
				}
				ImGuiMCP::TextDisabled("The hotkey opens and closes the browser during gameplay.");

				bool preview = tattoo_browser::GetPreviewEnabled();
				if (ImGuiMCP::Checkbox("Preview", &preview))
					tattoo_browser::SetPreviewEnabled(preview);
				if (ImGuiMCP::IsItemHovered())
					ImGuiMCP::SetTooltip("In the browser, hovering a pattern shows it on the character right away, like trying on clothes.\nNothing is saved until you press Save/Replace; Undo, leaving the slot or closing the browser takes it back off.");
				ImGuiMCP::SameLine();
				ImGuiMCP::TextDisabled("%s", "Show patterns on the character while browsing, without saving");
				// ---- Camera (the browser's framing) ----
				ImGuiMCP::SeparatorText("Camera");
				auto cam = tattoo_browser::GetCameraSettings();
				bool changed = false;


				const float slider_w = 260.0f;
				auto slider = [&](const char* a_label, float* a_value, float a_min, float a_max, const char* a_format, const char* a_tip) {
					ImGuiMCP::SetNextItemWidth(slider_w);
					changed |= ImGuiMCP::SliderFloat(a_label, a_value, a_min, a_max, a_format);
					if (ImGuiMCP::IsItemHovered())
						ImGuiMCP::SetTooltip("%s", a_tip);
				};
				slider("Offset X", &cam.offsetX, -150.0f, 150.0f, "%.1f", "Sideways. Negative moves the camera left, so the character stands right of the browser.");
				slider("Offset Y", &cam.offsetY, -150.0f, 150.0f, "%.1f", "Forward / back.");
				slider("Offset Z", &cam.offsetZ, -150.0f, 150.0f, "%.1f", "Up / down. Lower it to get the feet in shot.");
				slider("Zoom", &cam.zoom, 0.0f, 1.0f, "%.2f", "Distance the camera starts at: 0 = closest, 1 = farthest.");
				slider("FOV", &cam.fov, 0.0f, 120.0f, cam.fov > 0.0f ? "%.1f" : "Game default", "Field of view while browsing. 0 = keep the game's.");
				slider("Rotation Z", &cam.rotation, -180.0f, 180.0f, "%.0f deg", "Where the camera sits around the character: 0 = in front, 90 / -90 = the sides, 180 = behind.\nLeft-drag outside the browser still orbits on top of it.");
				slider("Rotation X", &cam.pitch, -60.0f, 60.0f, "%.0f deg", "Tilts the camera: positive looks down from above, negative looks up from below, 0 = level.\nLeft-drag up/down outside the browser still tilts on top of it.");
				if (changed)
					tattoo_browser::SetCameraSettings(cam);  // an open browser follows at once; Recenter uses them too

				if (GhostButton("Save Settings"))
					tattoo_browser::SaveSettings();
				if (ImGuiMCP::IsItemHovered())
					ImGuiMCP::SetTooltip("Write the camera settings (and the rest) to Data/SKSE/Plugins/SlavetatsUIPzx.ini.");
				ImGuiMCP::SameLine();
				if (GhostButton("Reset Defaults"))
					tattoo_browser::SetCameraSettings(tattoo_browser::DefaultCameraSettings());
				if (ImGuiMCP::IsItemHovered())
					ImGuiMCP::SetTooltip("Put the camera settings back to their defaults (press Save Settings to keep them).");
				if (!available)
					ImGuiMCP::TextColored(ToVec4(kDanger), "%s", "The Tattoo Browser needs FLICK (FUCK.dll), which isn't installed or is too old.");
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

				RenderBrowserSetup();

				if (Working > 0) {
					ImGuiMCP::TextDisabled("Working...");
				}
				if (!Status.empty()) {
					ImGuiMCP::TextColored({ 1.0f, 0.6f, 0.3f, 1.0f }, "%s", Status.c_str());
				}
			}

			// FLICK image loads for the Tattoo Browser, run from Menu Framework's present pass rather than inside
			// FLICK's own UI pass. Draws nothing.
			void __stdcall PumpThumbs() { tattoo_thumbs::Pump(); }

			void __stdcall RenderBody() { RenderArea(0); }
			void __stdcall RenderHead() { RenderArea(1); }
			void __stdcall RenderHands() { RenderArea(2); }
			void __stdcall RenderFeet() { RenderArea(3); }
			void __stdcall RenderPresetsPage() { RenderPresets(); }
		}

		void Register()
		{
			if (!SKSEMenuFramework::IsInstalled()) {
				logger::info("SKSE Menu Framework is not installed, the in-game menu is disabled");
				return;
			}
			SKSEMenuFramework::SetSection("SlavetatsUIPzx");
			SKSEMenuFramework::AddSectionItem("Setup", RenderSetup);
			SKSEMenuFramework::AddSectionItem("Body Tattoos", RenderBody);
			SKSEMenuFramework::AddSectionItem("Head Tattoos", RenderHead);
			SKSEMenuFramework::AddSectionItem("Hand Tattoos", RenderHands);
			SKSEMenuFramework::AddSectionItem("Foot Tattoos", RenderFeet);
			SKSEMenuFramework::AddSectionItem("Presets", RenderPresetsPage);

			SKSEMenuFramework::AddInputEvent(OnInput);  // hotkey capture on the Setup page
			SKSEMenuFramework::AddHudElement(PumpThumbs);
			logger::info("Registered SKSE Menu Framework pages");
		}
	}
}