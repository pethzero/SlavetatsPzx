#include "tattoo_thumbs.h"
#include "dds_preview.h"

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>
#include <DirectXTex.h>
#include <imgui.h>  // types only, for FUCK_API.h
#include "FUCK_API.h"

// DDS -> PNG thumbnail cache for the FLICK browser. See tattoo_thumbs.h for why it goes through a file.

namespace slavetats_ng
{
	namespace tattoo_thumbs
	{
		namespace
		{
			constexpr std::size_t kThumbPx = 256;      // longest edge of a cached thumbnail
			constexpr int         kImagesPerPump = 2;  // FLICK image loads per frame
			constexpr const char* kCacheDir = "Data/SKSE/Plugins/SlavetatsUIPzx/Thumbs";
			constexpr const char* kCacheVersion = "v1";  // bump to invalidate every cached PNG

			std::string Key(const std::string& a_texture)
			{
				std::string key = a_texture;
				std::ranges::replace(key, '/', '\\');
				std::ranges::transform(key, key.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
				return key;
			}

			std::filesystem::path PngPath(const std::string& a_key)
			{
				// FNV-1a: stable across sessions, so a thumbnail is decoded once ever
				std::uint64_t hash = 0xcbf29ce484222325ull;
				for (unsigned char c : a_key) {
					hash ^= c;
					hash *= 0x100000001b3ull;
				}
				return std::filesystem::absolute(kCacheDir) / fmt::format("{}-{:016x}.png", kCacheVersion, hash);
			}

			// ------------------------------ Worker ---------------------------------------------------------------

			struct Request
			{
				std::string key;
				std::string texture;
			};

			struct Result
			{
				std::string           key;
				bool                  ok = false;
				std::filesystem::path png;
			};

			std::mutex              WorkLock;
			std::condition_variable WorkReady;
			std::vector<Request>    Requests;  // newest last; the worker takes the newest (what is on screen)
			std::vector<Result>     Done;
			bool                    WorkerStarted = false;

			// Decode, shrink, save. Returns an error text, empty on success.
			std::string BuildPng(const std::string& a_texture, const std::filesystem::path& a_png)
			{
				std::vector<std::uint8_t> file;
				if (!dds_preview::ReadTexture(a_texture, file))
					return "file not found";

				DirectX::TexMetadata  meta{};
				DirectX::ScratchImage dds;
				if (FAILED(DirectX::LoadFromDDSMemory(file.data(), file.size(), DirectX::DDS_FLAGS_NONE, &meta, dds)))
					return "not a readable DDS";

				// Smallest mip that is still at least thumbnail sized: less to decompress
				std::size_t mip = 0;
				while (mip + 1 < meta.mipLevels && std::max(meta.width >> (mip + 1), meta.height >> (mip + 1)) >= kThumbPx)
					mip++;
				const DirectX::Image* top = dds.GetImage(mip, 0, 0);
				if (!top)
					return "no image data";

				// Decompress and Convert are different calls: Convert refuses block-compressed input
				DirectX::ScratchImage rgba;
				HRESULT               hr = DirectX::IsCompressed(top->format) ?
				                               DirectX::Decompress(*top, DXGI_FORMAT_R8G8B8A8_UNORM, rgba) :
				                               DirectX::Convert(*top, DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, rgba);
				if (FAILED(hr))
					return fmt::format("decode failed ({:#x}, format {})", static_cast<std::uint32_t>(hr), static_cast<int>(top->format));

				const DirectX::Image* src = rgba.GetImage(0, 0, 0);
				DirectX::ScratchImage scaled;
				if (std::max(src->width, src->height) > kThumbPx) {
					double      scale = static_cast<double>(kThumbPx) / std::max(src->width, src->height);
					std::size_t w = std::max<std::size_t>(1, static_cast<std::size_t>(src->width * scale));
					std::size_t h = std::max<std::size_t>(1, static_cast<std::size_t>(src->height * scale));
					// Non-WIC filter with alpha handled separately, so the transparent surround doesn't bleed in
					hr = DirectX::Resize(*src, w, h,
						DirectX::TEX_FILTER_TRIANGLE | DirectX::TEX_FILTER_FORCE_NON_WIC | DirectX::TEX_FILTER_SEPARATE_ALPHA, scaled);
					if (FAILED(hr))
						return fmt::format("resize failed ({:#x})", static_cast<std::uint32_t>(hr));
					src = scaled.GetImage(0, 0, 0);
				}

				std::error_code ec;
				std::filesystem::create_directories(a_png.parent_path(), ec);
				auto tmp = a_png;
				tmp += L".tmp";
				hr = DirectX::SaveToWICFile(*src, DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), tmp.c_str());
				if (FAILED(hr))
					return fmt::format("PNG write failed ({:#x})", static_cast<std::uint32_t>(hr));
				std::filesystem::rename(tmp, a_png, ec);  // never leave a half-written PNG under the real name
				return ec ? "PNG rename failed" : "";
			}

			void Worker()
			{
				// WIC (the PNG encoder) needs COM on this thread
				CoInitializeEx(nullptr, COINIT_MULTITHREADED);
				for (;;) {
					Request request;
					{
						std::unique_lock lock(WorkLock);
						WorkReady.wait(lock, [] { return !Requests.empty(); });
						request = std::move(Requests.back());
						Requests.pop_back();
					}

					Result result{ request.key, false, PngPath(request.key) };
					std::error_code ec;
					if (std::filesystem::exists(result.png, ec)) {
						result.ok = true;  // decoded in an earlier session
					} else {
						try {
							auto error = BuildPng(request.texture, result.png);
							result.ok = error.empty();
							if (!result.ok)
								logger::debug("[thumbs] {}: {}", request.texture, error);
						} catch (const std::exception& e) {
							logger::debug("[thumbs] {}: {}", request.texture, e.what());
						}
					}
					std::lock_guard lock(WorkLock);
					Done.push_back(std::move(result));
				}
			}

			// ------------------------------ Render thread --------------------------------------------------------

			struct Entry
			{
				State        state = State::kLoading;
				FUCK::Image* image = nullptr;
			};

			// ⚠ Never destroyed: FUCK::Image's destructor calls into FUCK.dll, which may already be gone at exit
			auto& Cache = *new std::unordered_map<std::string, Entry>();

			std::atomic<std::uint64_t> Pumps = 0;
		}

		Thumb Get(const std::string& a_texture)
		{
			if (a_texture.empty())
				return {};

			auto key = Key(a_texture);
			auto [it, inserted] = Cache.try_emplace(key);
			if (inserted) {
				std::lock_guard lock(WorkLock);
				Requests.push_back({ key, a_texture });
				if (!WorkerStarted) {
					WorkerStarted = true;
					std::thread(Worker).detach();
				}
				WorkReady.notify_one();
			}

			auto& entry = it->second;
			Thumb thumb;
			thumb.state = entry.state;
			if (entry.state == State::kReady && entry.image) {
				thumb.texture = static_cast<void*>(*entry.image);
				thumb.width = entry.image->GetWidth();
				thumb.height = entry.image->GetHeight();
			}
			return thumb;
		}

		std::uint64_t PumpCount() { return Pumps.load(); }

		void Pump()
		{
			Pumps++;
			if (!FUCK::GetInterface())
				return;

			std::vector<Result> ready;
			{
				std::lock_guard lock(WorkLock);
				int count = std::min(static_cast<int>(Done.size()), kImagesPerPump);
				for (int i = 0; i < count; i++)
					ready.push_back(std::move(Done[i]));
				Done.erase(Done.begin(), Done.begin() + count);
			}

			for (auto& result : ready) {
				auto it = Cache.find(result.key);
				if (it == Cache.end())
					continue;
				auto& entry = it->second;
				if (result.ok) {
					auto image = new FUCK::Image(result.png.string().c_str());
					if (image->IsLoaded()) {
						entry.image = image;
						entry.state = State::kReady;
						continue;
					}
					logger::debug("[thumbs] FLICK could not load {}", result.png.string());
					// Leaked on purpose like every FUCK::Image here; a failed one holds nothing
				}
				entry.state = State::kFailed;
			}
		}
	}
}
