#include "tattoo_browser.h"
#include "tattoo_model.h"
#include "tattoo_thumbs.h"

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>
#include <imgui.h>  // types only: FLICK owns the ImGui context and every call goes through its interface
#include "FUCK_API.h"

#define SMOOTHCAM_API_COMMONLIB
#include "SmoothCamAPI.h"

// Tattoo Browser on FLICK.
//
// A panel docked to the left edge, the character beside it, the world still running. No game menu is used as a
// host: a hidden InventoryMenu still takes clicks (items got equipped through it). FLICK keeps all input while we
// are open and never passes any to the game; a drag outside the panel turns the character with our own turntable.
//
// Threads: FLICK calls Draw()/GetFlags() on the Present (render) thread. The camera, the character and anything
// else in the engine are touched from SKSE tasks on the game thread.

using namespace slavetats_ng::tattoo_model;

namespace slavetats_ng
{
	namespace tattoo_browser
	{
		namespace
		{
			constexpr const char*   kSettingsFile = "Data/SKSE/Plugins/SlavetatsUIPzx.ini";
			constexpr const char*   kOldSettingsFile = "Data/SKSE/Plugins/SlavetatsPzx.ini";
			constexpr int           kSlotsPerPage = 6;  // 2 x 3

			// ------------------------------ Settings -------------------------------------------------------------

			std::atomic<std::uint32_t> Hotkey = 0;
			// Camera framing when the browser opens: the character is centered in the screen area right of the panel
			bool  AutoFrame = true;      // work the sideways shift out from the panel width, FOV and camera distance
			float CameraShift = 0.0f;    // extra sideways offset on top of that (or the whole offset with AutoFrame off)
			float OpenZoom = 0.2f;       // third-person zoom to start at (0 = closest, 1 = farthest), < 0 = keep the current one
			float OpenHeight = -25.0f;   // camera height offset to start at, so the whole body is in shot
			bool                       PreviewMode = false;  // clicking a pattern shows it on the character without saving
			bool                       Available = false;

			void LoadSettings()
			{
				CSimpleIniA ini;
				ini.SetUnicode();
				// The plugin was called SlavetatsPzx before ui-v3-1: keep its settings until the new file is written
				std::error_code ec;
				const char*     file = std::filesystem::exists(kSettingsFile, ec) ? kSettingsFile : kOldSettingsFile;
				if (ini.LoadFile(file) >= 0) {
					Hotkey = static_cast<std::uint32_t>(ini.GetLongValue("Browser", "iHotkey", 0));
					AutoFrame = ini.GetBoolValue("Browser", "bAutoFrame", AutoFrame);
					CameraShift = static_cast<float>(ini.GetDoubleValue("Browser", "fCameraExtraShift", CameraShift));
					OpenZoom = static_cast<float>(ini.GetDoubleValue("Browser", "fOpenZoom", OpenZoom));
					OpenHeight = static_cast<float>(ini.GetDoubleValue("Browser", "fOpenHeight", OpenHeight));
					PreviewMode = ini.GetBoolValue("Browser", "bPreview", PreviewMode);
				}
				logger::info("Tattoo Browser: hotkey {:#x}, camera shift {}", Hotkey.load(), CameraShift);
			}

			void SaveSettings()
			{
				CSimpleIniA ini;
				ini.SetUnicode();
				ini.LoadFile(kSettingsFile);  // keep anything else in the file
				ini.SetLongValue("Browser", "iHotkey", static_cast<long>(Hotkey.load()),
					"; DirectInput scan code of the Tattoo Browser hotkey, 0 = none (set it in the menu: Setup > Tattoo Browser)", true);
				ini.Delete("Browser", "fCameraShift");  // before v4.0: a fixed offset, now worked out by bAutoFrame
				ini.SetBoolValue("Browser", "bAutoFrame", AutoFrame,
					"; Center the character in the screen area right of the browser (from the panel width, FOV and camera distance)");
				ini.SetDoubleValue("Browser", "fCameraExtraShift", CameraShift,
					"; Extra sideways camera offset on top of bAutoFrame (the whole offset when bAutoFrame = false). Negative = other side");
				ini.SetDoubleValue("Browser", "fOpenZoom", OpenZoom,
					"; Zoom the camera starts at when the browser opens: 0 = closest, 1 = farthest, -1 = keep the current zoom");
				ini.SetDoubleValue("Browser", "fOpenHeight", OpenHeight,
					"; Camera height offset the browser starts at (negative = lower), so the whole body is in shot");
				ini.SetBoolValue("Browser", "bPreview", PreviewMode,
					"; Clicking a pattern shows it on the character right away, without saving (Save/Replace keeps it)");
				if (ini.SaveFile(kSettingsFile) < 0)
					logger::warn("Tattoo Browser: could not write {}", kSettingsFile);
			}

			// ------------------------------ Theme ----------------------------------------------------------------

			constexpr ImVec4 Rgba(int r, int g, int b, int a = 255) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }

			constexpr ImVec4 kAccent = Rgba(226, 178, 86);
			constexpr ImVec4 kAccentHover = Rgba(238, 194, 108);
			constexpr ImVec4 kAccentSoft = Rgba(226, 178, 86, 46);
			constexpr ImVec4 kAccentInk = Rgba(28, 22, 12);
			constexpr ImVec4 kCard = Rgba(255, 255, 255, 10);
			constexpr ImVec4 kCardHover = Rgba(255, 255, 255, 24);
			constexpr ImVec4 kBorder = Rgba(255, 255, 255, 30);
			constexpr ImVec4 kText = Rgba(236, 236, 236);
			constexpr ImVec4 kMuted = Rgba(150, 150, 150);
			constexpr ImVec4 kPreviewBg = Rgba(0, 0, 0, 110);
			constexpr ImVec4 kCaption = Rgba(0, 0, 0, 160);
			constexpr ImVec4 kCheckerA = Rgba(178, 178, 182);
			constexpr ImVec4 kCheckerB = Rgba(204, 204, 208);
			constexpr ImVec4 kOk = Rgba(108, 196, 120);
			constexpr ImVec4 kWarn = Rgba(230, 160, 70);
			constexpr ImVec4 kDanger = Rgba(222, 92, 92);
			constexpr ImVec4 kDangerSoft = Rgba(222, 92, 92, 60);
			constexpr ImVec4 kPending = Rgba(240, 210, 90);

			constexpr float kRounding = 6.0f;
			constexpr float kCheckerCell = 8.0f;

			ImVec2 Add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }

			// ⚠ FLICK's ImGuiCol_ is TWO HIGHER than imgui 1.92.8's from ImGuiCol_WindowBg on: FUCK.dll is built on a
			// newer imgui that inserts two entries after ImGuiCol_TextDisabled (measured by Fitting Room against
			// FUCK.pdb). A raw index silently styles the wrong slot. ImGuiStyleVar_ is not affected.
			ImGuiCol FuckCol(ImGuiCol a_idx) { return a_idx >= ImGuiCol_WindowBg ? static_cast<ImGuiCol>(a_idx + 2) : a_idx; }
			void     PushCol(ImGuiCol a_idx, const ImVec4& a_col) { FUCK::PushStyleColor(FuckCol(a_idx), a_col); }

			// ------------------------------ Drawing helpers ------------------------------------------------------

			// Text at an absolute position without disturbing the layout cursor
			void TextAt(ImVec2 a_pos, const ImVec4& a_color, const string& a_text)
			{
				auto cursor = FUCK::GetCursorScreenPos();
				FUCK::SetCursorScreenPos(a_pos);
				PushCol(ImGuiCol_Text, a_color);
				FUCK::TextUnformatted(a_text.c_str());
				FUCK::PopStyleColor();
				FUCK::SetCursorScreenPos(cursor);
			}

			void Tooltip(const string& a_text)
			{
				if (FUCK::BeginTooltip()) {
					FUCK::TextUnformatted(a_text.c_str());
					FUCK::EndTooltip();
				}
			}

			string Ellipsize(const string& a_text, float a_width)
			{
				if (FUCK::CalcTextSize(a_text.c_str()).x <= a_width)
					return a_text;
				string s = a_text;
				while (!s.empty() && FUCK::CalcTextSize((s + "...").c_str()).x > a_width) {
					while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80)
						s.pop_back();
					if (!s.empty())
						s.pop_back();
				}
				return s + "...";
			}

			void DrawChecker(ImVec2 a_min, ImVec2 a_max)
			{
				FUCK::DrawRectFilled(a_min, a_max, kCheckerA, kRounding);
				int row = 0;
				for (float y = a_min.y; y < a_max.y; y += kCheckerCell, row++) {
					for (float x = a_min.x + ((row & 1) ? kCheckerCell : 0.0f); x < a_max.x; x += kCheckerCell * 2.0f) {
						bool corner = (x < a_min.x + kRounding || x + kCheckerCell > a_max.x - kRounding) &&
						              (y < a_min.y + kRounding || y + kCheckerCell > a_max.y - kRounding);
						if (!corner)
							FUCK::DrawRectFilled(ImVec2(x, y), ImVec2(std::min(x + kCheckerCell, a_max.x), std::min(y + kCheckerCell, a_max.y)), kCheckerB);
					}
				}
			}

			// DDS thumbnail fitted into the frame, or a dark frame with a_label while there is none
			void DrawPreview(ImVec2 a_min, ImVec2 a_max, const string& a_texture, const char* a_label)
			{
				float w = a_max.x - a_min.x;
				float h = a_max.y - a_min.y;

				// A FLICK image (PNG thumbnail), never one of our own D3D views: FLICK only draws images it loaded
				auto thumb = tattoo_thumbs::Get(a_texture);
				if (thumb.state == tattoo_thumbs::State::kReady && thumb.texture) {
					DrawChecker(a_min, a_max);
					float  scale = std::min(w / std::max(1.0f, thumb.width), h / std::max(1.0f, thumb.height));
					float  iw = thumb.width * scale;
					float  ih = thumb.height * scale;
					ImVec2 imin(a_min.x + (w - iw) * 0.5f, a_min.y + (h - ih) * 0.5f);
					FUCK::AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(thumb.texture)), imin, ImVec2(imin.x + iw, imin.y + ih));
					FUCK::DrawRect(a_min, a_max, kBorder, kRounding, 1.0f);
					return;
				}
				if (thumb.state == tattoo_thumbs::State::kLoading)
					a_label = "...";

				FUCK::DrawRectFilled(a_min, a_max, kPreviewBg, kRounding);
				FUCK::DrawRect(a_min, a_max, kBorder, kRounding, 1.0f);
				if (a_label && *a_label) {
					auto size = FUCK::CalcTextSize(a_label);
					if (size.x < w - 4.0f)
						TextAt(ImVec2(a_min.x + (w - size.x) * 0.5f, a_min.y + (h - size.y) * 0.5f), kMuted, a_label);
				}
			}

			bool StyledButton(const char* a_label, const ImVec4& a_bg, const ImVec4& a_hover, const ImVec4& a_text)
			{
				FUCK::PushStyleVar(ImGuiStyleVar_FrameRounding, kRounding);
				PushCol(ImGuiCol_Button, a_bg);
				PushCol(ImGuiCol_ButtonHovered, a_hover);
				PushCol(ImGuiCol_ButtonActive, a_hover);
				PushCol(ImGuiCol_Text, a_text);
				bool clicked = FUCK::Button(a_label);
				FUCK::PopStyleColor(4);
				FUCK::PopStyleVar();
				return clicked;
			}

			bool PrimaryButton(const char* a_label) { return StyledButton(a_label, kAccent, kAccentHover, kAccentInk); }
			bool DangerButton(const char* a_label) { return StyledButton(a_label, kDangerSoft, kDanger, kText); }
			bool GhostButton(const char* a_label) { return StyledButton(a_label, kCard, kCardHover, kText); }
			bool Chip(const char* a_label, bool a_active)
			{
				return StyledButton(a_label, a_active ? kAccent : kCard, a_active ? kAccentHover : kCardHover, a_active ? kAccentInk : kText);
			}

			void CenteredHint(const string& a_text)
			{
				auto avail = FUCK::GetContentRegionAvail();
				auto size = FUCK::CalcTextSize(a_text.c_str());
				auto pos = FUCK::GetCursorScreenPos();
				FUCK::SetCursorScreenPos(ImVec2(pos.x + std::max(0.0f, (avail.x - size.x) * 0.5f), pos.y + std::max(0.0f, (avail.y - size.y) * 0.5f)));
				FUCK::TextDisabled("%s", a_text.c_str());
			}

			// ------------------------------ Color field ----------------------------------------------------------

			// Swatches like the SlaveTats MCM color dialog: deep row, then light row
			constexpr int kPalette[] = {
				0xB71C1C, 0xC2185B, 0xB8860B, 0xA0522D, 0xE65100, 0x00838F, 0x1A237E, 0x4A148C, 0x6D3B5E, 0x558B2F, 0x1B5E20, 0x5D4037, 0x757575, 0x000000,
				0xF44336, 0xF06292, 0xFFC107, 0xD2A06D, 0xFF9800, 0x80DEEA, 0x3F51B5, 0x9575CD, 0xC48BA0, 0xCDDC39, 0x2ECC71, 0xD7CCC8, 0xBDBDBD, 0xFFFFFF
			};
			constexpr int kPaletteColumns = 14;

			ImVec4 ToVec4(const float a_c[3]) { return ImVec4(a_c[0], a_c[1], a_c[2], 1.0f); }

			// Swatch + label; clicking opens a compact palette with R/G/B sliders. Returns true when the color changed.
			bool ColorField(const char* a_label, float a_color[3], const char* a_tooltip)
			{
				bool edited = false;
				FUCK::PushID(a_label);
				float  frame = FUCK::GetFrameHeight();
				ImVec2 pos = FUCK::GetCursorScreenPos();
				ImVec2 size(frame * 1.6f, frame);
				if (FUCK::InvisibleButton("##swatch", size))
					FUCK::OpenPopup("picker");
				bool hovered = FUCK::IsItemHovered();
				FUCK::DrawRectFilled(pos, Add(pos, size), ToVec4(a_color), 4.0f);
				FUCK::DrawRect(pos, Add(pos, size), hovered ? kAccent : kBorder, 4.0f, 1.0f);
				if (hovered)
					Tooltip(fmt::format("{}\n#{:06X}", a_tooltip, ToRGB(a_color)));
				FUCK::SameLine(0.0f, 6.0f);
				FUCK::AlignTextToFramePadding();
				FUCK::TextUnformatted(a_label);

				if (FUCK::BeginPopup("picker")) {
					FUCK::TextDisabled("%s", a_label);
					const float swatch = 20.0f;
					const float gap = 3.0f;
					ImVec2      origin = FUCK::GetCursorScreenPos();
					int         current = ToRGB(a_color);
					for (int i = 0; i < static_cast<int>(std::size(kPalette)); i++) {
						int    col = i % kPaletteColumns;
						int    row = i / kPaletteColumns;
						ImVec2 p(origin.x + col * (swatch + gap), origin.y + row * (swatch + gap));
						FUCK::SetCursorScreenPos(p);
						FUCK::PushID(i);
						float c[3];
						ToFloat(kPalette[i], c);
						if (FUCK::InvisibleButton("##p", ImVec2(swatch, swatch))) {
							std::copy_n(c, 3, a_color);
							edited = true;
						}
						bool over = FUCK::IsItemHovered();
						FUCK::PopID();
						FUCK::DrawRectFilled(p, ImVec2(p.x + swatch, p.y + swatch), ToVec4(c), 3.0f);
						if (kPalette[i] == current || over)
							FUCK::DrawRect(p, ImVec2(p.x + swatch, p.y + swatch), kAccent, 3.0f, 2.0f);
					}
					float grid_w = kPaletteColumns * (swatch + gap) - gap;
					FUCK::SetCursorScreenPos(origin);
					FUCK::Dummy(ImVec2(grid_w, 2 * (swatch + gap)));

					// Fine tuning
					int rgb[3] = { (current >> 16) & 0xFF, (current >> 8) & 0xFF, current & 0xFF };
					const char* names[3] = { "R", "G", "B" };
					bool        changed = false;
					for (int i = 0; i < 3; i++) {
						FUCK::SetNextItemWidth(grid_w);
						changed |= FUCK::SliderInt(names[i], &rgb[i], 0, 255, "%d");
					}
					if (changed) {
						ToFloat((rgb[0] << 16) | (rgb[1] << 8) | rgb[2], a_color);
						edited = true;
					}
					FUCK::TextDisabled("#%06X", ToRGB(a_color));
					FUCK::EndPopup();
				}
				FUCK::PopID();
				return edited;
			}

			// ------------------------------ Window ---------------------------------------------------------------

			class BrowserWindow : public FUCK::IWindow
			{
			public:
				const char* Id() const override { return "TattooBrowser"; }
				const char* Title() const override { return "Tattoo Browser"; }
				bool        IsOpen() const override { return open_.load(std::memory_order_relaxed); }

				void SetOpen(bool a_open) override
				{
					if (open_.exchange(a_open, std::memory_order_relaxed) == a_open)
						return;
					if (a_open) {
						hotkeyWasDown_ = true;  // the press that opened us must not close us on the first frame
						fresh_ = true;
						previewKey_.clear();  // Draw isn't running while closed, so this can't race it
						pendingKey_.clear();
					}
					// Engine work on the game thread, whichever thread FLICK or we called from
					SKSE::GetTaskInterface()->AddTask([a_open] { a_open ? OnOpenedGame() : OnClosedGame(); });
				}

				FUCK::WindowFlags GetFlags() const override
				{
					using F = FUCK::WindowFlags;
					// Never kPassInputToGame: the drag outside the panel turns the character through our own
					// turntable, so the game never sees a click (no attacks, no walking, no hidden menu to hit).
					// No pause/blur flags: the world keeps running. kNoDecoration: a decorated FLICK window is kept
					// away from the screen edge; we draw our own header instead.
					return F::kNoMove | F::kNoResize | F::kNoDecoration | F::kCustomPosition | F::kCloseOnEsc | F::kCloseOnGameMenu |
					       F::kBlockVanity | F::kHideHUD;
				}

				// Left edge, full height, a bit under half the screen (scaled so 4K isn't a thin strip)
				ImVec2 GetDefaultSize() const override
				{
					ImVec2 display = FUCK::GetDisplaySize();
					float  s = std::max(1.0f, FUCK::GetResolutionScale());
					float  w = std::clamp(display.x * 0.466f, std::min(560.0f * s, display.x), std::min(1230.0f * s, display.x));
					return ImVec2(w, display.y > 96.0f ? display.y : 720.0f);
				}
				ImVec2 GetDefaultPos() const override { return ImVec2(0.0f, 0.0f); }

				void Draw() override
				{
					if (faulted_)
						return;
					try {
						DrawContents();
					} catch (const std::exception& e) {
						Fault(e.what());
					} catch (...) {
						Fault("unknown exception");
					}
				}

			private:
				// ⚠ An exception escaping into FLICK's Present hook ends the game without a crash log
				void Fault(const char* a_what)
				{
					faulted_ = true;
					logger::error("Tattoo Browser: drawing threw ({}), closing it", a_what ? a_what : "");
					SKSE::GetTaskInterface()->AddTask([this] { SetOpen(false); });
				}

				static void OnOpenedGame();
				static void OnClosedGame();

				void DrawContents();
				void DrawHeader();
				void DrawSlots(int a_area);
				bool DrawSlotTile(int a_area, int a_index, ImVec2 a_pos, ImVec2 a_size);
				void DrawEditor(int a_area, int a_index);
				void DrawGroups(int a_area, Slot& a_slot);
				void DrawPatternGrid(int a_area, Slot& a_slot, float a_height);
				bool DrawPatternTile(const TattooDef& a_tattoo, const string& a_section, bool a_selected, bool a_applied, bool a_show_section, ImVec2 a_pos, float a_tile, bool& a_hovered);
				void UpdatePreview();
				void StopPreview();

				std::atomic<bool> open_ = false;
				std::atomic<bool> overUI_ = true;
				bool              faulted_ = false;
				bool              hotkeyWasDown_ = false;
				bool              autoLoaded_ = false;
				int               area_ = 0;
				int               page_ = 0;
				int               editing_ = -1;  // slot open in the editor, -1 = overview
				std::array<char, 64> search_ = {};
				std::uint64_t     lastPump_ = 0;
				int               framesWithoutPump_ = 0;
				bool              fresh_ = false;        // first frame after opening: put the geometry back
				bool              mouseWasDown_ = false;
				bool              dragging_ = false;     // a turntable drag that started outside the panel
				// Preview (Setup page setting), Fitting Room style: the pattern under the mouse is shown on the
				// character; with nothing hovered, the slot's unsaved edits are; with neither, what is saved.
				string            hoverSection_;         // tile under the mouse this frame
				string            hoverName_;
				string            previewKey_;           // what is on the character now ("" = the records)
				string            pendingKey_;           // what should be, once it has held still for a moment
				double            pendingSince_ = 0.0;
			};

			BrowserWindow Window;

			// ------------------------------ Character turntable (game thread) ------------------------------------
			// No game menu under the browser. FLICK keeps the input while we are open (no walking, no attacks), and
			// a drag outside the panel is ours: the character turns in place while the third-person camera's
			// free-rotation yaw is counter-rotated by the same amount, so the shot stays still, like the
			// inventory's turntable. The camera starts in front of the character and shifted sideways so the
			// character stands beside the panel. Everything is restored on close.

			constexpr float kFrontYaw = 3.14159265f;  // free-rotation yaw that puts the camera in front of the character
			constexpr float kTurnPerPixel = 0.012f;
			constexpr float kPitchPerPixel = 0.006f;
			constexpr float kMaxPitch = 1.2f;         // radians up or down from level
			constexpr float kZoomPerNotch = 0.05f;
			constexpr float kPanPerPixel = 0.25f;     // game units the camera slides per pixel of middle drag
			constexpr float kMaxPan = 150.0f;

			// Render thread writes, game thread reads
			std::atomic<bool>  TurntableHeld = false;
			std::atomic<float> Turn = 0.0f;       // character heading change since opening (right drag)
			std::atomic<float> Orbit = 0.0f;      // camera yaw around the character (left drag, horizontal)
			std::atomic<float> Pitch = 0.0f;      // camera up/down (left drag, vertical)
			std::atomic<float> PanX = 0.0f;       // camera slide sideways (middle drag, horizontal)
			std::atomic<float> PanZ = 0.0f;       // camera slide up/down (middle drag, vertical)
			std::atomic<float> ZoomDelta = 0.0f;  // mouse wheel since opening
			std::atomic<float> PanelFraction = 0.466f;  // panel width / screen width, from the render thread

			// Back to the opening shot: camera in front, character centered beside the panel and facing the camera
			void Recenter()
			{
				Turn = 0.0f;
				Orbit = 0.0f;
				Pitch = 0.0f;
				PanX = 0.0f;
				PanZ = 0.0f;
				ZoomDelta = 0.0f;
			}

			// Sideways camera offset that puts the character in the middle of the area right of the panel: that
			// spot is panelWidth/2 right of the screen center, and at distance d the half screen is d*tan(fov/2) wide
			float FramingShift(RE::PlayerCharacter* a_player)
			{
				if (!AutoFrame)
					return CameraShift;
				auto camera = RE::PlayerCamera::GetSingleton();
				if (!camera || !camera->cameraRoot)
					return CameraShift;
				RE::NiPoint3 eye = camera->cameraRoot->world.translate;
				RE::NiPoint3 body = a_player->GetPosition();
				body.z += a_player->GetHeight() * 0.5f;
				float distance = eye.GetDistance(body);
				float halfWidth = distance * std::tan(camera->worldFOV * 0.5f * 3.14159265f / 180.0f);
				return PanelFraction.load(std::memory_order_relaxed) * halfWidth + CameraShift;
			}

			void AddTo(std::atomic<float>& a_value, float a_delta, float a_min = -1e9f, float a_max = 1e9f)
			{
				a_value.store(std::clamp(a_value.load(std::memory_order_relaxed) + a_delta, a_min, a_max), std::memory_order_relaxed);
			}

			struct SavedCamera
			{
				bool         valid = false;
				bool         wasFirstPerson = false;
				float        heading = 0.0f;
				bool         freeRotationEnabled = false;
				RE::NiPoint2 freeRotation{};
				float        offsetX = 0.0f;
				float        offsetZ = 0.0f;
				float        zoom = 0.0f;
			};
			SavedCamera Saved;

			RE::ThirdPersonState* ThirdPerson()
			{
				auto camera = RE::PlayerCamera::GetSingleton();
				if (!camera || !camera->IsInThirdPerson() || !camera->currentState)
					return nullptr;
				return static_cast<RE::ThirdPersonState*>(camera->currentState.get());
			}

			// Per frame while open: the engine may write these fields too, so they are re-asserted
			void ApplyTurntable()
			{
				if (!TurntableHeld)
					return;
				auto player = RE::PlayerCharacter::GetSingleton();
				auto state = ThirdPerson();  // null for a frame or two after ForceThirdPerson
				if (!player || !state)
					return;
				if (!Saved.valid) {
					Saved.valid = true;
					Saved.heading = player->GetAngleZ();
					Saved.freeRotationEnabled = state->freeRotationEnabled;
					Saved.freeRotation = state->freeRotation;
					Saved.offsetX = state->posOffsetExpected.x;
					Saved.offsetZ = state->posOffsetExpected.z;
					Saved.zoom = state->targetZoomOffset;
				}
				float turn = Turn.load(std::memory_order_relaxed);
				player->SetRotationZ(Saved.heading + turn);
				// Free rotation is relative to the character's heading: subtracting the turn keeps the camera still
				state->freeRotationEnabled = true;
				state->freeRotation.x = kFrontYaw - turn + Orbit.load(std::memory_order_relaxed);
				state->freeRotation.y = Pitch.load(std::memory_order_relaxed);
				state->posOffsetExpected.x = Saved.offsetX - FramingShift(player) + PanX.load(std::memory_order_relaxed);
				state->posOffsetExpected.z = Saved.offsetZ + OpenHeight + PanZ.load(std::memory_order_relaxed);
				float zoomStart = OpenZoom >= 0.0f ? OpenZoom : Saved.zoom;
				state->targetZoomOffset = std::clamp(zoomStart + ZoomDelta.load(std::memory_order_relaxed), 0.0f, 1.0f);
			}

			// ------------------------------ SmoothCam ------------------------------------------------------------
			// SmoothCam drives the third-person camera itself every frame, so our turntable and its re-asserted
			// offsets fought it. Its modder API lets another plugin borrow the camera: while we hold it SmoothCam
			// steps aside and the vanilla camera runs, which is what the turntable is written against.

			SmoothCamAPI::IVSmoothCam1*   SmoothCam = nullptr;
			SmoothCamAPI::InterfaceVersion SmoothCamVersion = SmoothCamAPI::InterfaceVersion::V1;
			bool                          SmoothCamHeld = false;  // game thread

			void TakeCameraFromSmoothCam()
			{
				if (!SmoothCam)
					return;
				using R = SmoothCamAPI::APIResult;
				auto result = SmoothCam->RequestCameraControl(SKSE::GetPluginHandle());
				SmoothCamHeld = result == R::OK || result == R::AlreadyGiven;
				if (!SmoothCamHeld)
					logger::info("Tattoo Browser: SmoothCam kept the camera (result {}), the turntable may fight it",
						static_cast<int>(result));
			}

			void GiveCameraBackToSmoothCam()
			{
				if (!SmoothCam || !SmoothCamHeld)
					return;
				auto handle = SKSE::GetPluginHandle();
				// Glide back to its usual spot behind the player instead of starting from where we left the camera
				if (SmoothCamVersion >= SmoothCamAPI::InterfaceVersion::V2)
					static_cast<SmoothCamAPI::IVSmoothCam2*>(SmoothCam)->SendToGoalPosition(handle, true, false, RE::PlayerCharacter::GetSingleton());
				SmoothCam->ReleaseCameraControl(handle);
				SmoothCamHeld = false;
			}

			void BrowserWindow::OnOpenedGame()
			{
				Saved = {};
				TakeCameraFromSmoothCam();  // before anything reads or writes the camera
				if (auto camera = RE::PlayerCamera::GetSingleton()) {
					Saved.wasFirstPerson = camera->IsInFirstPerson();
					camera->ForceThirdPerson();
				}
				Recenter();
				TurntableHeld = true;
				ApplyTurntable();
				FUCK::ForceCursor(true);  // no game menu is open, so nothing else shows a cursor
				logger::info("Tattoo Browser: opened");
			}

			void BrowserWindow::OnClosedGame()
			{
				TurntableHeld = false;
				EndPreview();  // whatever was only previewed goes back to what is saved
				if (Saved.valid) {
					if (auto player = RE::PlayerCharacter::GetSingleton())
						player->SetRotationZ(Saved.heading);
					if (auto state = ThirdPerson()) {
						state->freeRotationEnabled = Saved.freeRotationEnabled;
						state->freeRotation = Saved.freeRotation;
						state->posOffsetExpected.x = Saved.offsetX;
						state->posOffsetExpected.z = Saved.offsetZ;
						state->targetZoomOffset = Saved.zoom;
					}
				}
				if (Saved.wasFirstPerson) {
					if (auto camera = RE::PlayerCamera::GetSingleton())
						camera->ForceFirstPerson();
				}
				Saved = {};
				GiveCameraBackToSmoothCam();  // after our values are restored
				FUCK::ForceCursor(false);
				logger::info("Tattoo Browser: closed");
			}
			// ------------------------------ Drawing --------------------------------------------------------------

			void BrowserWindow::DrawContents()
			{
				DrainQueue();

				// Thumbnails should load outside FLICK's UI pass (the SKSE Menu Framework HUD pump in ui_menu.cpp);
				// if nothing else is pumping, do it here rather than never
				if (auto pumps = tattoo_thumbs::PumpCount(); pumps != lastPump_) {
					lastPump_ = pumps;
					framesWithoutPump_ = 0;
				} else if (++framesWithoutPump_ > 3) {
					tattoo_thumbs::Pump();
					lastPump_ = tattoo_thumbs::PumpCount();
				}

				if (auto display = FUCK::GetDisplaySize(); display.x > 0.0f)
					PanelFraction.store(GetDefaultSize().x / display.x, std::memory_order_relaxed);

				// FLICK restores the geometry it saved for this Id: put ours back on the first frame after opening
				if (fresh_ || FUCK::IsWindowAppearing()) {
					fresh_ = false;
					FUCK::SetWindowPos(GetDefaultPos());
					FUCK::SetWindowSize(GetDefaultSize());
				}
				// Hotkey closes (FLICK has the keyboard while we're open, so the game's input sink doesn't see it)
				if (auto key = Hotkey.load(); key != 0) {
					bool down = FUCK::IsInputDown(key);
					if (down && !hotkeyWasDown_ && !FUCK::IsAnyItemActive()) {
						hotkeyWasDown_ = true;
						SKSE::GetTaskInterface()->AddTask([this] { SetOpen(false); });
						return;
					}
					hotkeyWasDown_ = down;
				}

				if (!HasTarget) {
					if (!autoLoaded_ && Working == 0) {
						autoLoaded_ = true;
						LoadTarget(TargetMode::kPlayer);
					}
					CenteredHint(Status.empty() ? "Loading..." : Status);
				} else {
					DrawHeader();
					auto& slots = Slots[area_];
					if (editing_ >= static_cast<int>(slots.size()))
						editing_ = -1;

					FUCK::BeginChild("tb_body", ImVec2(0.0f, 0.0f), false, 0);
					if (editing_ >= 0)
						DrawEditor(area_, editing_);
					else
						DrawSlots(area_);
					FUCK::EndChild();

					UpdatePreview();
				}

				// Camera and character, only for gestures that START outside every UI window (panel, popups, tooltips):
				//   left drag:   free camera - horizontal orbits around the character, vertical tilts up/down
				//   right drag:  turns the character only
				//   middle drag: slides the camera left/right/up/down
				//   wheel:       zoom
				bool overUI = FUCK::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || FUCK::IsAnyItemActive();
				overUI_.store(overUI, std::memory_order_relaxed);
				bool left = FUCK::IsMouseDown(0);
				bool right = FUCK::IsMouseDown(1);
				bool middle = FUCK::IsMouseDown(2);
				bool down = left || right || middle;
				if (down && !mouseWasDown_)
					dragging_ = !overUI;
				if (!down)
					dragging_ = false;
				mouseWasDown_ = down;
				if (dragging_) {
					auto delta = FUCK::GetMouseDelta();
					if (middle) {
						AddTo(PanX, -delta.x * kPanPerPixel, -kMaxPan, kMaxPan);
						AddTo(PanZ, delta.y * kPanPerPixel, -kMaxPan, kMaxPan);
					} else if (right) {
						AddTo(Turn, delta.x * kTurnPerPixel);
					} else {
						AddTo(Orbit, delta.x * kTurnPerPixel);
						AddTo(Pitch, delta.y * kPitchPerPixel, -kMaxPitch, kMaxPitch);
					}
				}
				if (!overUI) {
					if (float wheel = FUCK::GetMouseWheel(); wheel != 0.0f)
						AddTo(ZoomDelta, -wheel * kZoomPerNotch, -1.0f, 1.0f);
				}
				SKSE::GetTaskInterface()->AddTask([] { ApplyTurntable(); });
			}

			void BrowserWindow::DrawHeader()
			{
				// Own title bar (kNoDecoration): title on the left, close on the right
				FUCK::AlignTextToFramePadding();
				FUCK::TextColored(kAccent, "%s", "TATTOOS");
				{
					const char* close = "Close";
					float       w = FUCK::CalcTextSize(close).x + 24.0f;
					FUCK::SameLine();
					auto cursor = FUCK::GetCursorScreenPos();
					FUCK::SetCursorScreenPos(ImVec2(cursor.x + std::max(0.0f, FUCK::GetContentRegionAvail().x - w), cursor.y));
					if (GhostButton(close))
						SKSE::GetTaskInterface()->AddTask([this] { SetOpen(false); });
				}
				FUCK::Separator();

				FUCK::AlignTextToFramePadding();
				FUCK::Text("Current Tattoos - %s", TargetName.c_str());
				FUCK::SameLine();
				FUCK::BeginDisabled(Working > 0);
				if (GhostButton("Refresh"))
					LoadTarget(TargetMode::kCurrent);
				FUCK::EndDisabled();
				FUCK::SameLine();
				if (GhostButton("Recenter"))
					Recenter();
				if (FUCK::IsItemHovered())
					Tooltip("Back to the starting shot: camera in front, character centered beside the panel.\nThe starting zoom and height are fOpenZoom / fOpenHeight in SlavetatsUIPzx.ini.");
				if (Working > 0) {
					FUCK::SameLine();
					FUCK::TextColored(kPending, "%s", "(working...)");
				}

				constexpr const char* kTabs[kAreaCount] = { "BODY", "FACE", "HANDS", "FEET" };
				for (int a = 0; a < kAreaCount; a++) {
					if (a > 0)
						FUCK::SameLine(0.0f, 6.0f);
					FUCK::PushID(a);
					if (Chip(kTabs[a], area_ == a) && area_ != a) {
						EndPreview();
						StopPreview();
						area_ = a;
						page_ = 0;
						editing_ = -1;
					}
					FUCK::PopID();
				}
				FUCK::TextDisabled("%s", "Outside the panel: left-drag camera, right-drag turn, middle-drag move, wheel zoom.");
				FUCK::Spacing();
			}

			bool BrowserWindow::DrawSlotTile(int a_area, int a_index, ImVec2 a_pos, ImVec2 a_size)
			{
				auto& slot = Slots[a_area][a_index];
				auto& applied = slot.applied;
				ImVec2 max = Add(a_pos, a_size);

				FUCK::SetCursorScreenPos(a_pos);
				FUCK::PushID(a_index);
				bool clicked = FUCK::InvisibleButton("##slot", a_size);
				bool hovered = FUCK::IsItemHovered();
				FUCK::PopID();

				bool external = applied.kind == SlotKind::kExternal;
				FUCK::DrawRectFilled(a_pos, max, hovered && !external ? kCardHover : kCard, kRounding);

				const float pad = 8.0f;
				float       line = FUCK::GetTextLineHeight();
				string      title = fmt::format("Slot {}", a_index + 1);

				if (applied.kind == SlotKind::kTattoo) {
					string texture;
					if (auto def = FindTattoo(a_area, applied.section, applied.name))
						texture = PreviewPath(*def);
					float cap_y = max.y - line - pad * 1.5f;
					DrawPreview(ImVec2(a_pos.x + pad, a_pos.y + pad), ImVec2(max.x - pad, cap_y - 4.0f), texture, "No preview");
					FUCK::DrawRectFilled(ImVec2(a_pos.x, cap_y), max, kCaption, kRounding);
					string caption = Ellipsize(fmt::format("{} - {}", title, applied.name), a_size.x - pad * 2.0f - 14.0f);
					TextAt(ImVec2(a_pos.x + pad, cap_y + pad * 0.75f), applied.locked ? kDanger : kText, caption);
					const ImVec4& dot = HasChanges(slot) ? kPending : (applied.locked ? kDanger : kOk);
					FUCK::DrawCircleFilled(ImVec2(max.x - pad - 4.0f, cap_y + (max.y - cap_y) * 0.5f), 4.0f, dot, 12);
				} else if (external) {
					DrawPreview(ImVec2(a_pos.x + pad, a_pos.y + pad + line + 4.0f), ImVec2(max.x - pad, max.y - pad), {}, "");
					TextAt(ImVec2(a_pos.x + pad, a_pos.y + pad), kMuted, title + " - External - Locked");
				} else {
					TextAt(ImVec2(a_pos.x + pad, a_pos.y + pad), kMuted, title);
					const string add = "+  Add";
					auto         size = FUCK::CalcTextSize(add.c_str());
					TextAt(ImVec2(a_pos.x + (a_size.x - size.x) * 0.5f, a_pos.y + (a_size.y - size.y) * 0.5f), hovered ? kAccent : kMuted, add);
					if (HasChanges(slot))
						FUCK::DrawCircleFilled(ImVec2(max.x - pad - 4.0f, a_pos.y + pad + line * 0.5f), 4.0f, kPending, 12);
				}
				FUCK::DrawRect(a_pos, max, hovered && !external ? kAccent : kBorder, kRounding, hovered && !external ? 1.5f : 1.0f);

				if (hovered) {
					if (applied.kind == SlotKind::kTattoo)
						Tooltip(fmt::format("{} - {}\n{}{}", title, applied.name, applied.section, applied.locked ? "\n(locked)" : ""));
					else if (external)
						Tooltip("This overlay slot is used by another mod.");
				}
				return clicked && !external;
			}

			void BrowserWindow::DrawSlots(int a_area)
			{
				auto& slots = Slots[a_area];
				if (!SlotsKnown[a_area]) {
					CenteredHint("Reading overlays...");
					return;
				}
				if (slots.empty()) {
					CenteredHint("No overlay slots (check RaceMenu/SKEE and iNumOverlays in skee64.ini).");
					return;
				}

				int pages = (static_cast<int>(slots.size()) + kSlotsPerPage - 1) / kSlotsPerPage;
				page_ = std::clamp(page_, 0, pages - 1);

				const float spacing = 8.0f;
				float       pager_h = FUCK::GetFrameHeight() + spacing;
				auto        avail = FUCK::GetContentRegionAvail();
				ImVec2      origin = FUCK::GetCursorScreenPos();
				ImVec2      tile((avail.x - spacing) * 0.5f, std::max(80.0f, (avail.y - pager_h - spacing * 2.0f) / 3.0f));

				int first = page_ * kSlotsPerPage;
				int last = std::min(first + kSlotsPerPage, static_cast<int>(slots.size()));
				for (int i = first; i < last; i++) {
					int    n = i - first;
					ImVec2 pos(origin.x + (n % 2) * (tile.x + spacing), origin.y + (n / 2) * (tile.y + spacing));
					if (DrawSlotTile(a_area, i, pos, tile)) {
						editing_ = i;
						if (slots[i].browse.empty())
							slots[i].browse = slots[i].section;
						search_[0] = '\0';
					}
				}
				// Claim the grid's area as one item, then the pager below it
				FUCK::SetCursorScreenPos(origin);
				FUCK::Dummy(ImVec2(avail.x, 3.0f * tile.y + 2.0f * spacing));

				FUCK::BeginDisabled(page_ <= 0);
				if (GhostButton("Prev"))
					page_--;
				FUCK::EndDisabled();
				FUCK::SameLine();
				FUCK::AlignTextToFramePadding();
				FUCK::Text("Page %d / %d", page_ + 1, pages);
				FUCK::SameLine();
				FUCK::BeginDisabled(page_ >= pages - 1);
				if (GhostButton("Next"))
					page_++;
				FUCK::EndDisabled();
			}

			void BrowserWindow::DrawGroups(int a_area, Slot& a_slot)
			{
				auto& sections = TattooCatalog.areas[a_area];

				FUCK::TextDisabled("%s", "GROUP");
				FUCK::SameLine();
				float search_w = std::min(240.0f, FUCK::GetContentRegionAvail().x * 0.5f);
				auto  cursor = FUCK::GetCursorScreenPos();
				FUCK::SetCursorScreenPos(ImVec2(cursor.x + FUCK::GetContentRegionAvail().x - search_w, cursor.y));
				FUCK::SetNextItemWidth(search_w);
				FUCK::InputText("##search", search_.data(), search_.size(), 0);
				if (search_[0] == '\0' && !FUCK::IsItemActive()) {
					auto rmin = FUCK::GetItemRectMin();
					TextAt(ImVec2(rmin.x + 6.0f, rmin.y + (FUCK::GetFrameHeight() - FUCK::GetTextLineHeight()) * 0.5f), kMuted, "Search all patterns...");
				}

				if (sections.empty()) {
					FUCK::TextDisabled("%s", "No tattoo packs for this area. Press \"Add/remove tattoos\" in the menu's Setup page.");
					return;
				}

				// Chips are laid out by hand so the row count is known before drawing: the strip is sized to whole
				// rows (up to 3, then it scrolls), so the last visible row is never cut in half
				const float gap = 6.0f;
				const int   kMaxRows = 3;
				float       frame = FUCK::GetFrameHeight();
				float       pad = FUCK::GetStyleVarVec(ImGuiStyleVar_FramePadding).x;

				struct ChipPos
				{
					const string* name;
					string        label;
					float         x;
					int           row;
				};
				auto layout = [&](float a_width, std::vector<ChipPos>& a_out) {
					a_out.clear();
					float x = 0.0f;
					int   row = 0;
					for (auto& [name, tattoos] : sections) {
						string label = fmt::format("{} ({})", name, tattoos.size());
						float  w = FUCK::CalcTextSize(label.c_str()).x + pad * 2.0f;
						if (x > 0.0f && x + w > a_width) {
							x = 0.0f;
							row++;
						}
						a_out.push_back({ &name, std::move(label), x, row });
						x += w + gap;
					}
					return row + 1;
				};

				std::vector<ChipPos> chips;
				float                width = FUCK::GetContentRegionAvail().x;
				int                  rows = layout(width, chips);
				if (rows > kMaxRows)  // it will scroll: lay out again without the scrollbar's width
					rows = layout(width - FUCK::GetStyleVar(ImGuiStyleVar_ScrollbarSize) - gap, chips);
				int   visible = std::min(rows, kMaxRows);
				float step = frame + gap;

				FUCK::BeginChild("tb_groups", ImVec2(0.0f, visible * frame + (visible - 1) * gap), false, 0);
				ImVec2 origin = FUCK::GetCursorScreenPos();
				for (int i = 0; i < static_cast<int>(chips.size()); i++) {
					auto& chip = chips[i];
					FUCK::SetCursorScreenPos(ImVec2(origin.x + chip.x, origin.y + chip.row * step));
					FUCK::PushID(i);
					if (Chip(chip.label.c_str(), IEquals(a_slot.browse, *chip.name))) {
						a_slot.browse = *chip.name;
						search_[0] = '\0';
					}
					FUCK::PopID();
				}
				// Claim the whole strip as one item so the child scrolls over every row
				FUCK::SetCursorScreenPos(origin);
				FUCK::Dummy(ImVec2(1.0f, rows * frame + (rows - 1) * gap));
				FUCK::EndChild();

				// Divider between the groups and the patterns
				FUCK::Spacing();
				FUCK::Separator();
			}
			bool BrowserWindow::DrawPatternTile(const TattooDef& a_tattoo, const string& a_section, bool a_selected, bool a_applied,
				bool a_show_section, ImVec2 a_pos, float a_tile, bool& a_hovered)
			{
				float  line = FUCK::GetTextLineHeight();
				ImVec2 size(a_tile, a_tile + line * (a_show_section ? 2.0f : 1.0f) + 10.0f);
				ImVec2 max = Add(a_pos, size);

				FUCK::SetCursorScreenPos(a_pos);
				bool clicked = FUCK::InvisibleButton("##tile", size);
				bool hovered = FUCK::IsItemHovered();
				a_hovered = hovered;

				FUCK::DrawRectFilled(a_pos, max, a_selected ? kAccentSoft : (hovered ? kCardHover : kCard), kRounding);
				const float inset = 5.0f;
				ImVec2      pmin(a_pos.x + inset, a_pos.y + inset);
				ImVec2      pmax(a_pos.x + a_tile - inset, a_pos.y + a_tile - inset);
				DrawPreview(pmin, pmax, PreviewPath(a_tattoo), "No preview");

				float  text_w = a_tile - inset * 2.0f;
				string name = Ellipsize(a_tattoo.name, text_w);
				TextAt(ImVec2(a_pos.x + (a_tile - FUCK::CalcTextSize(name.c_str()).x) * 0.5f, pmax.y + 4.0f), a_selected ? kAccent : kText, name);
				if (a_show_section) {
					string section = Ellipsize(a_section, text_w);
					TextAt(ImVec2(a_pos.x + (a_tile - FUCK::CalcTextSize(section.c_str()).x) * 0.5f, pmax.y + 4.0f + line), kMuted, section);
				}
				if (a_selected)
					FUCK::DrawRect(a_pos, max, kAccent, kRounding, 2.0f);
				if (a_applied)
					FUCK::DrawCircleFilled(ImVec2(pmax.x - 9.0f, pmin.y + 9.0f), 6.0f, kOk, 16);

				if (hovered) {
					string tip = a_tattoo.name + "\n" + a_section;
					if (!a_tattoo.credit.empty())
						tip += "\nTattoo by: " + a_tattoo.credit;
					if (a_applied)
						tip += "\n(applied in this slot)";
					tip += "\n" + a_tattoo.texture;
					Tooltip(tip);
				}
				return clicked;
			}

			void BrowserWindow::DrawPatternGrid(int a_area, Slot& a_slot, float a_height)
			{
				auto& sections = TattooCatalog.areas[a_area];
				bool  searching = search_[0] != '\0';

				std::vector<std::pair<const string*, const TattooDef*>> tiles;
				if (searching) {
					for (auto& [name, tattoos] : sections)
						for (auto& tattoo : tattoos)
							if (IContains(tattoo.name, search_.data()) || IContains(name, search_.data()))
								tiles.emplace_back(&name, &tattoo);
				} else if (auto it = sections.find(a_slot.browse); it != sections.end()) {
					for (auto& tattoo : it->second)
						tiles.emplace_back(&it->first, &tattoo);
				}

				PushCol(ImGuiCol_ChildBg, Rgba(0, 0, 0, 40));
				FUCK::BeginChild("tb_grid", ImVec2(0.0f, a_height), true, 0);
				if (tiles.empty()) {
					CenteredHint(searching ? "No pattern matches your search." : "Pick a group above to browse its patterns.");
				} else {
					const float spacing = 8.0f;
					float       avail = FUCK::GetContentRegionAvail().x;
					// Tiles grow to fill the row: as many ~120 px columns as fit, then stretched
					int    columns = std::max(1, static_cast<int>((avail + spacing) / (120.0f + spacing)));
					float  tile = (avail - spacing * (columns - 1)) / columns;
					float  line = FUCK::GetTextLineHeight();
					float  tile_h = tile + line * (searching ? 2.0f : 1.0f) + 10.0f;
					ImVec2 origin = FUCK::GetCursorScreenPos();
					for (int i = 0; i < static_cast<int>(tiles.size()); i++) {
						auto [section, tattoo] = tiles[i];
						ImVec2 pos(origin.x + (i % columns) * (tile + spacing), origin.y + (i / columns) * (tile_h + spacing));
						bool   selected = IEquals(a_slot.section, *section) && IEquals(a_slot.pattern, tattoo->name);
						bool   applied = a_slot.applied.kind == SlotKind::kTattoo && IEquals(a_slot.applied.section, *section) &&
						               IEquals(a_slot.applied.name, tattoo->name);
						FUCK::PushID(i);
						bool tile_hovered = false;
						if (DrawPatternTile(*tattoo, *section, selected, applied, searching, pos, tile, tile_hovered)) {
							SelectTattoo(a_slot, *section, tattoo);
							a_slot.browse = *section;
						}
						if (tile_hovered) {
							hoverSection_ = *section;
							hoverName_ = tattoo->name;
						}
						FUCK::PopID();
					}
					int rows = (static_cast<int>(tiles.size()) + columns - 1) / columns;
					FUCK::SetCursorScreenPos(origin);
					FUCK::Dummy(ImVec2(avail, rows * tile_h + (rows - 1) * spacing));
				}
				FUCK::EndChild();
				FUCK::PopStyleColor();
			}

			void BrowserWindow::DrawEditor(int a_area, int a_index)
			{
				auto& slot = Slots[a_area][a_index];
				auto& applied = slot.applied;

				FUCK::BeginDisabled(applied.locked);
				DrawGroups(a_area, slot);
				FUCK::Spacing();

				// Footer below the grid: selection | slot status, colors, actions (with Back on the right)
				float frame = FUCK::GetFrameHeight();
				float footer = frame * 3.0f + FUCK::GetTextLineHeight() * 2.0f + 40.0f;
				float grid_h = std::max(160.0f, FUCK::GetContentRegionAvail().y - footer);
				FUCK::TextDisabled("%s", "PATTERN");
				DrawPatternGrid(a_area, slot, grid_h);

				FUCK::Spacing();
				auto selected = Selected(a_area, slot);
				if (selected) {
					FUCK::Text("%s", selected->name.c_str());
					FUCK::SameLine();
					FUCK::TextDisabled("%s%s%s", slot.section.c_str(), selected->credit.empty() ? "" : "  -  by ", selected->credit.c_str());
				} else {
					FUCK::TextDisabled("%s", applied.kind == SlotKind::kTattoo ? "No pattern selected" : "Click a pattern to select it");
				}

				// What the slot holds now, on the same row
				FUCK::SameLine();
				FUCK::TextDisabled("%s", "|");
				FUCK::SameLine();
				FUCK::Text("Slot %d", a_index + 1);
				FUCK::SameLine();
				if (applied.kind == SlotKind::kTattoo) {
					FUCK::TextColored(applied.locked ? kDanger : kOk, "%s", applied.locked ? "Locked" : "Applied");
					FUCK::SameLine();
					FUCK::TextDisabled("%s / %s", applied.section.c_str(), applied.name.c_str());
				} else {
					FUCK::TextDisabled("%s", "Empty");
				}

				ColorField("Ink", slot.ink, "Pick the ink color for this tattoo.");
				FUCK::SameLine(0.0f, 24.0f);
				ColorField("Glow", slot.glow, "Pick a color for the tattoo to glow (black is no glow).");
				FUCK::SameLine(0.0f, 24.0f);
				FUCK::Checkbox("Glossy", &slot.glossy, false, false);

				FUCK::Spacing();
				bool changed = HasChanges(slot);
				FUCK::BeginDisabled(Working > 0);
				if (applied.kind != SlotKind::kTattoo) {
					FUCK::BeginDisabled(!selected);
					if (PrimaryButton("Save Tattoo"))
						ApplySlot(a_area, a_index);
					FUCK::EndDisabled();
				} else {
					FUCK::BeginDisabled(!selected || !changed);
					if (PrimaryButton("Replace"))
						ApplySlot(a_area, a_index);
					FUCK::EndDisabled();
					FUCK::SameLine();
					if (DangerButton("Remove"))
						RemoveSlot(a_area, a_index);
				}
				FUCK::SameLine();
				FUCK::BeginDisabled(!changed && !IsPreviewing());
				if (GhostButton("Undo")) {
					ResetPending(slot);
					StopPreview();
				}
				FUCK::EndDisabled();
				if (IsPreviewing()) {
					FUCK::SameLine();
					FUCK::AlignTextToFramePadding();
					FUCK::TextColored(kPending, "%s", "Previewing - not saved");
				}
				FUCK::EndDisabled();
				FUCK::EndDisabled();  // locked

				// Back to the slot overview, right-aligned on the action row (outside the disabled scopes: always usable)
				{
					const char* back = "Back";
					float       w = FUCK::CalcTextSize(back).x + FUCK::GetStyleVarVec(ImGuiStyleVar_FramePadding).x * 2.0f;
					FUCK::SameLine();
					auto cursor = FUCK::GetCursorScreenPos();
					float room = FUCK::GetContentRegionAvail().x;
					if (room >= w)
						FUCK::SetCursorScreenPos(ImVec2(cursor.x + room - w, cursor.y));
					else
						FUCK::Dummy(ImVec2(0.0f, 0.0f));  // ends the row, the button goes to the next line
					if (GhostButton(back)) {
						StopPreview();
						editing_ = -1;
						return;
					}
				}

				if (applied.locked)
					FUCK::TextDisabled("%s", "Locked: this tattoo can only be removed by the mod that applied it.");
				if (!slot.error.empty())
					FUCK::TextColored(kDanger, "%s", slot.error.c_str());
			}

			// ------------------------------ Preview --------------------------------------------------------------

			// Puts the records back on the character and forgets what was shown
			void BrowserWindow::StopPreview()
			{
				EndPreview();
				previewKey_.clear();
				pendingKey_.clear();
			}

			// Once per frame after the editor drew (so the hovered tile is known)
			void BrowserWindow::UpdatePreview()
			{
				string section = std::move(hoverSection_);
				string name = std::move(hoverName_);
				hoverSection_.clear();
				hoverName_.clear();

				if (!PreviewMode || editing_ < 0 || editing_ >= static_cast<int>(Slots[area_].size())) {
					if (!previewKey_.empty())
						StopPreview();
					return;
				}

				// What should be on the character: the hovered pattern, else the slot's unsaved edits, else the records
				auto&            slot = Slots[area_][editing_];
				const TattooDef* want = nullptr;
				if (!name.empty()) {
					want = FindTattoo(area_, section, name);
				} else if (HasChanges(slot)) {
					want = Selected(area_, slot);
					section = slot.section;
				}
				string key = want ? fmt::format("{}|{}|{}|{:06x}|{:06x}|{}", editing_, section, want->name, ToRGB(slot.ink), ToRGB(slot.glow), slot.glossy) : "";

				if (key == previewKey_) {
					pendingKey_ = key;
					return;
				}
				// Sweeping the mouse across the grid shouldn't redraw the character for every tile it passes
				double now = FUCK::GetTime();
				if (key != pendingKey_) {
					pendingKey_ = key;
					pendingSince_ = now;
					return;
				}
				if (now - pendingSince_ < 0.12)
					return;

				previewKey_ = key;
				if (want)
					PreviewSlot(area_, editing_, want);
				else
					EndPreview();
			}

			// ------------------------------ Hotkey (game thread) -------------------------------------------------

			// Opens the browser from gameplay. While it is open FLICK owns the keyboard, so closing is Draw's job.
			class InputSink : public RE::BSTEventSink<RE::InputEvent*>
			{
			public:
				RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
				{
					auto key = Hotkey.load();
					if (!a_event || key == 0 || Window.IsOpen())
						return RE::BSEventNotifyControl::kContinue;
					for (auto event = *a_event; event; event = event->next) {
						auto button = event->AsButtonEvent();
						if (!button || button->GetDevice() != RE::INPUT_DEVICE::kKeyboard || !button->IsDown() || button->GetIDCode() != key)
							continue;
						// Only from plain gameplay: not over the console, a game menu, or a text field
						auto ui = RE::UI::GetSingleton();
						if (ui && (ui->GameIsPaused() || ui->IsMenuOpen(RE::Console::MENU_NAME)))
							continue;
						Open();
						break;
					}
					return RE::BSEventNotifyControl::kContinue;
				}
			};

			InputSink Sink;
		}

		void Register()
		{
			LoadSettings();
			if (!FUCK::Connect("SlavetatsUIPzx")) {
				logger::info("Tattoo Browser: FLICK (FUCK.dll) not found or too old, the browser is disabled");
				return;
			}
			FUCK::RegisterWindow(&Window);
			Available = true;
			logger::info("Tattoo Browser: registered with FLICK");

			// SmoothCam answers the interface request (sent at kPostPostLoad) through this listener
			auto registered = SmoothCamAPI::RegisterInterfaceLoaderCallback(SKSE::GetMessagingInterface(),
				[](void* a_interface, SmoothCamAPI::InterfaceVersion a_version) {
					SmoothCam = static_cast<SmoothCamAPI::IVSmoothCam1*>(a_interface);
					SmoothCamVersion = a_version;
					logger::info("Tattoo Browser: got the SmoothCam interface (V{})", static_cast<int>(a_version) + 1);
				});
			if (!registered)
				logger::info("Tattoo Browser: SmoothCam not loaded");
		}

		void OnPostPostLoad()
		{
			if (Available)
				(void)SmoothCamAPI::RequestInterface(SKSE::GetMessagingInterface(), SmoothCamAPI::InterfaceVersion::V3);
		}

		void OnDataLoaded()
		{
			if (!Available)
				return;
			if (auto input = RE::BSInputDeviceManager::GetSingleton())
				input->AddEventSink(&Sink);
		}

		bool IsAvailable() { return Available; }

		void Open()
		{
			if (Available)
				Window.SetOpen(true);
		}

		void Close()
		{
			if (Available)
				Window.SetOpen(false);
		}

		bool GetPreviewEnabled() { return PreviewMode; }

		void SetPreviewEnabled(bool a_enabled)
		{
			PreviewMode = a_enabled;
			if (!a_enabled)
				EndPreview();
			SaveSettings();
		}

		std::uint32_t GetHotkey() { return Hotkey.load(); }

		void SetHotkey(std::uint32_t a_scanCode)
		{
			Hotkey = a_scanCode;
			SaveSettings();
		}

		std::string KeyName(std::uint32_t a_scanCode)
		{
			if (a_scanCode == 0)
				return "None";
			if (Available) {
				if (auto name = FUCK::GetKeyName(a_scanCode); name && *name)
					return name;
			}
			// Scan codes above 0x80 are the extended (E0-prefixed) keys: arrows, numpad enter, right ctrl...
			LONG lparam = static_cast<LONG>((a_scanCode & 0x7F) << 16) | ((a_scanCode & 0x80) ? (1 << 24) : 0);
			char name[64] = {};
			if (GetKeyNameTextA(lparam, name, sizeof(name)) > 0)
				return name;
			return fmt::format("Key {:#04x}", a_scanCode);
		}
	}
}
