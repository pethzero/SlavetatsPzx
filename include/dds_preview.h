#pragma once

namespace slavetats_ng
{
	namespace dds_preview
	{
		enum class State
		{
			kNone,     // no texture path
			kLoading,  // queued or being decoded
			kReady,
			kFailed  // missing file or unsupported format
		};

		struct Preview
		{
			void*         texture = nullptr;  // ImTextureID (ID3D11ShaderResourceView*), valid this frame
			State         state = State::kNone;
			std::uint32_t width = 0;
			std::uint32_t height = 0;
		};

		// Thumbnail for a DDS texture, path relative to Data\Textures (loose files and BSAs).
		// The first call queues the file; it is decoded in the background and shows up a few frames later.
		// UI thread only.
		Preview Get(const std::string& a_texture);

		// Uploads decoded textures and frees unused ones. Call once per frame from the UI thread, before Get().
		void Pump();
	}
}
