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

		// Camera while the browser is open (Setup > Camera), like Show Player In Inventory's: offsets in game units
		// added to the normal third-person camera. X sideways (negative moves the camera left, so the character
		// stands right of the panel), Y forward/back, Z up/down.
		struct CameraSettings
		{
			float offsetX = -72.5f;
			float offsetY = 80.0f;
			float offsetZ = -13.5f;
			float zoom = 0.2f;  // 0 = closest, 1 = farthest, < 0 = keep the current zoom
			float fov = 55.0f;  // field of view while browsing, 0 = keep the game's
			float rotation = -20.0f;  // Rotation Z (yaw), degrees around the character: 0 = in front, +/-180 = behind
			float pitch = 10.0f;    // Rotation X (pitch), degrees the camera tilts: positive looks down from above
		};

		CameraSettings GetCameraSettings();
		void           SetCameraSettings(const CameraSettings& a_settings);  // live: an open browser follows at once
		CameraSettings DefaultCameraSettings();
		void           SaveSettings();  // writes everything to Data/SKSE/Plugins/SlavetatsUIPzx.ini
	}
}