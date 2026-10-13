#pragma once

// Tattoo thumbnails for the FLICK browser.
//
// FLICK only draws images it loaded itself, by file path (FUCK::Image); it doesn't take our own D3D11 views. So
// a worker thread decodes the DDS (DirectXTex: every BC format, BC7 included), shrinks it to a thumbnail and
// writes a PNG cache file, and Pump() turns finished PNGs into FLICK images.

namespace slavetats_ng
{
	namespace tattoo_thumbs
	{
		enum class State
		{
			kNone,     // no texture path
			kLoading,  // queued, decoding, or waiting for Pump()
			kReady,
			kFailed  // missing file or undecodable
		};

		struct Thumb
		{
			void*  texture = nullptr;  // FLICK image handle (ImTextureID), valid while kReady
			State  state = State::kNone;
			float  width = 0.0f;
			float  height = 0.0f;
		};

		// Path relative to Data\Textures. The first call queues it. Safe from a FLICK Draw: no device or FLICK
		// image work happens here.
		Thumb Get(const std::string& a_texture);

		// Turns at most a couple of finished PNGs into FLICK images per call. Call once per frame, preferably
		// OUTSIDE FLICK's own UI pass (loading images inside it made Fitting Room's editor vanish on those frames).
		void Pump();

		// Increments on every Pump(), so a caller can tell whether something else is pumping.
		std::uint64_t PumpCount();
	}
}
