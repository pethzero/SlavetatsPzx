#pragma once

// Tattoo Browser: a FLICK window docked to the left edge, the character beside it, the world still running.
// Dragging outside the window turns the character (our own turntable; the game never gets the click).
// FLICK (FUCK.dll) is a soft dependency resolved at runtime.

namespace slavetats_ng
{
	namespace tattoo_browser
	{
		// kPostLoad: connect to FLICK and register the window. Does nothing (IsAvailable() stays false) without FLICK.
		void Register();

		// kPostPostLoad: ask SmoothCam (if installed) for its API, so the browser can borrow the camera from it.
		void OnPostPostLoad();

		// kDataLoaded: start listening for the hotkey.
		void OnDataLoaded();

		bool IsAvailable();

		// Thread-safe; the engine work (camera, character, cursor) is queued to the game thread.
		void Open();
		void Close();

		// DirectInput scan code, 0 = none. SetHotkey saves it to Data/SKSE/Plugins/SlavetatsUIPzx.ini.
		std::uint32_t GetHotkey();
		void          SetHotkey(std::uint32_t a_scanCode);
		std::string   KeyName(std::uint32_t a_scanCode);

		// Preview: hovering a pattern in the browser shows it on the character without saving. Saved to the ini.
		bool GetPreviewEnabled();
		void SetPreviewEnabled(bool a_enabled);
	}
}
