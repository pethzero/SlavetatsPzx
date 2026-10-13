#include "dds_preview.h"

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <d3d11.h>

// DDS thumbnails for the in-game menu.
// A worker thread reads the file through the game's resource system (so BSA textures work too), picks the
// mip level closest to kTargetSize and converts uncompressed formats to RGBA8. The UI thread then creates the
// D3D11 texture on the game's device and hands its shader resource view to ImGui.

namespace slavetats_ng
{
	namespace dds_preview
	{
		namespace
		{
			constexpr std::uint32_t kTargetSize = 256;   // preferred thumbnail edge
			constexpr std::size_t   kMaxLoaded = 96;     // textures kept on the GPU
			constexpr int           kUploadsPerFrame = 3;

			// ------------------------------ DDS format -----------------------------------------------------------

			constexpr std::uint32_t FourCC(char a, char b, char c, char d)
			{
				return static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 8) | (static_cast<std::uint32_t>(c) << 16) |
				       (static_cast<std::uint32_t>(d) << 24);
			}

			constexpr std::uint32_t kMagic = FourCC('D', 'D', 'S', ' ');
			constexpr std::uint32_t kPfAlphaPixels = 0x1;
			constexpr std::uint32_t kPfAlpha = 0x2;
			constexpr std::uint32_t kPfFourCC = 0x4;
			constexpr std::uint32_t kPfRGB = 0x40;
			constexpr std::uint32_t kPfLuminance = 0x20000;

			struct PixelFormat
			{
				std::uint32_t size, flags, fourCC, bits, r, g, b, a;
			};

			struct Header
			{
				std::uint32_t size, flags, height, width, pitch, depth, mipCount;
				std::uint32_t reserved1[11];
				PixelFormat   pf;
				std::uint32_t caps, caps2, caps3, caps4, reserved2;
			};
			static_assert(sizeof(Header) == 124);

			struct HeaderDX10
			{
				std::uint32_t format, dimension, misc, arraySize, misc2;
			};

			// How a level is laid out in the file, and what it becomes on the GPU
			enum class Layout
			{
				kBlock8,   // BC1, BC4
				kBlock16,  // BC2, BC3, BC5, BC6H, BC7
				kRGBA32,
				kBGRA32,
				kBGRX32,
				kRGBX32,
				kBGR24,
				kRGB24,
				kL8,
				kA8,
				kL8A8
			};

			bool IsBlock(Layout a_layout) { return a_layout == Layout::kBlock8 || a_layout == Layout::kBlock16; }

			std::uint32_t BytesPerPixel(Layout a_layout)
			{
				switch (a_layout) {
				case Layout::kRGBA32:
				case Layout::kBGRA32:
				case Layout::kBGRX32:
				case Layout::kRGBX32:
					return 4;
				case Layout::kBGR24:
				case Layout::kRGB24:
					return 3;
				case Layout::kL8A8:
					return 2;
				default:
					return 1;
				}
			}

			std::size_t LevelSize(Layout a_layout, std::uint32_t a_w, std::uint32_t a_h)
			{
				if (IsBlock(a_layout)) {
					std::size_t blocks = static_cast<std::size_t>(std::max(1u, (a_w + 3) / 4)) * std::max(1u, (a_h + 3) / 4);
					return blocks * (a_layout == Layout::kBlock8 ? 8 : 16);
				}
				return static_cast<std::size_t>(a_w) * a_h * BytesPerPixel(a_layout);
			}

			// Works out the layout and GPU format; false = unsupported
			bool Classify(const Header& a_hdr, const HeaderDX10* a_dx10, Layout& a_layout, DXGI_FORMAT& a_format)
			{
				const auto& pf = a_hdr.pf;
				if (a_dx10) {
					auto fmt = static_cast<DXGI_FORMAT>(a_dx10->format);
					switch (fmt) {
					case DXGI_FORMAT_BC1_UNORM:
					case DXGI_FORMAT_BC1_UNORM_SRGB:
					case DXGI_FORMAT_BC4_UNORM:
					case DXGI_FORMAT_BC4_SNORM:
						a_layout = Layout::kBlock8;
						a_format = fmt;
						return true;
					case DXGI_FORMAT_BC2_UNORM:
					case DXGI_FORMAT_BC2_UNORM_SRGB:
					case DXGI_FORMAT_BC3_UNORM:
					case DXGI_FORMAT_BC3_UNORM_SRGB:
					case DXGI_FORMAT_BC5_UNORM:
					case DXGI_FORMAT_BC5_SNORM:
					case DXGI_FORMAT_BC6H_UF16:
					case DXGI_FORMAT_BC6H_SF16:
					case DXGI_FORMAT_BC7_UNORM:
					case DXGI_FORMAT_BC7_UNORM_SRGB:
						a_layout = Layout::kBlock16;
						a_format = fmt;
						return true;
					case DXGI_FORMAT_R8G8B8A8_UNORM:
					case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
						a_layout = Layout::kRGBA32;
						break;
					case DXGI_FORMAT_B8G8R8A8_UNORM:
					case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
						a_layout = Layout::kBGRA32;
						break;
					case DXGI_FORMAT_B8G8R8X8_UNORM:
					case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
						a_layout = Layout::kBGRX32;
						break;
					case DXGI_FORMAT_R8_UNORM:
						a_layout = Layout::kL8;
						break;
					case DXGI_FORMAT_A8_UNORM:
						a_layout = Layout::kA8;
						break;
					default:
						return false;
					}
					a_format = DXGI_FORMAT_R8G8B8A8_UNORM;  // uncompressed data is converted
					return true;
				}

				if (pf.flags & kPfFourCC) {
					switch (pf.fourCC) {
					case FourCC('D', 'X', 'T', '1'):
						a_layout = Layout::kBlock8;
						a_format = DXGI_FORMAT_BC1_UNORM;
						return true;
					case FourCC('D', 'X', 'T', '2'):
					case FourCC('D', 'X', 'T', '3'):
						a_layout = Layout::kBlock16;
						a_format = DXGI_FORMAT_BC2_UNORM;
						return true;
					case FourCC('D', 'X', 'T', '4'):
					case FourCC('D', 'X', 'T', '5'):
						a_layout = Layout::kBlock16;
						a_format = DXGI_FORMAT_BC3_UNORM;
						return true;
					case FourCC('A', 'T', 'I', '1'):
					case FourCC('B', 'C', '4', 'U'):
						a_layout = Layout::kBlock8;
						a_format = DXGI_FORMAT_BC4_UNORM;
						return true;
					case FourCC('A', 'T', 'I', '2'):
					case FourCC('B', 'C', '5', 'U'):
						a_layout = Layout::kBlock16;
						a_format = DXGI_FORMAT_BC5_UNORM;
						return true;
					default:
						return false;
					}
				}

				a_format = DXGI_FORMAT_R8G8B8A8_UNORM;
				if ((pf.flags & kPfRGB) && pf.bits == 32) {
					bool alpha = (pf.flags & kPfAlphaPixels) && pf.a == 0xFF000000;
					if (pf.r == 0x00FF0000 && pf.g == 0x0000FF00 && pf.b == 0x000000FF) {
						a_layout = alpha ? Layout::kBGRA32 : Layout::kBGRX32;
						return true;
					}
					if (pf.r == 0x000000FF && pf.g == 0x0000FF00 && pf.b == 0x00FF0000) {
						a_layout = alpha ? Layout::kRGBA32 : Layout::kRGBX32;
						return true;
					}
					return false;
				}
				if ((pf.flags & kPfRGB) && pf.bits == 24) {
					a_layout = pf.r == 0x00FF0000 ? Layout::kBGR24 : Layout::kRGB24;
					return true;
				}
				if ((pf.flags & kPfLuminance) && pf.bits == 8) {
					a_layout = Layout::kL8;
					return true;
				}
				if ((pf.flags & kPfLuminance) && (pf.flags & kPfAlphaPixels) && pf.bits == 16) {
					a_layout = Layout::kL8A8;
					return true;
				}
				if ((pf.flags & kPfAlpha) && pf.bits == 8) {
					a_layout = Layout::kA8;
					return true;
				}
				return false;
			}

			// One source pixel as RGBA8
			void ToRGBA(Layout a_layout, const std::uint8_t* a_src, std::uint8_t* a_dst)
			{
				switch (a_layout) {
				case Layout::kRGBA32:
					std::memcpy(a_dst, a_src, 4);
					break;
				case Layout::kRGBX32:
					std::memcpy(a_dst, a_src, 3);
					a_dst[3] = 255;
					break;
				case Layout::kBGRA32:
					a_dst[0] = a_src[2], a_dst[1] = a_src[1], a_dst[2] = a_src[0], a_dst[3] = a_src[3];
					break;
				case Layout::kBGRX32:
				case Layout::kBGR24:
					a_dst[0] = a_src[2], a_dst[1] = a_src[1], a_dst[2] = a_src[0], a_dst[3] = 255;
					break;
				case Layout::kRGB24:
					a_dst[0] = a_src[0], a_dst[1] = a_src[1], a_dst[2] = a_src[2], a_dst[3] = 255;
					break;
				case Layout::kL8:
					a_dst[0] = a_dst[1] = a_dst[2] = a_src[0], a_dst[3] = 255;
					break;
				case Layout::kA8:
					a_dst[0] = a_dst[1] = a_dst[2] = 255, a_dst[3] = a_src[0];
					break;
				case Layout::kL8A8:
					a_dst[0] = a_dst[1] = a_dst[2] = a_src[0], a_dst[3] = a_src[1];
					break;
				default:
					break;
				}
			}

			// ------------------------------ Worker ---------------------------------------------------------------

			struct Decoded
			{
				std::string               key;
				bool                      ok = false;
				DXGI_FORMAT               format = DXGI_FORMAT_UNKNOWN;
				std::uint32_t             width = 0;
				std::uint32_t             height = 0;
				std::uint32_t             pitch = 0;
				std::vector<std::uint8_t> pixels;
				std::string               error;
			};

			bool ReadFileImpl(const std::string& a_texture, std::vector<std::uint8_t>& a_out)
			{
				// Game resource system first (loose files and BSAs, through MO2's VFS), plain file as fallback
				std::string path = "Textures\\" + a_texture;
				{
					RE::BSResourceNiBinaryStream stream(path);
					if (stream.good() && stream.stream && stream.stream->totalSize > 0) {
						a_out.resize(stream.stream->totalSize);
						if (stream.read(a_out.data(), static_cast<std::uint32_t>(a_out.size())))
							return true;
					}
				}
				std::ifstream file("Data\\" + path, std::ios::binary);
				if (!file)
					return false;
				a_out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
				return !a_out.empty();
			}

			Decoded Decode(const std::string& a_key, const std::string& a_texture)
			{
				Decoded result;
				result.key = a_key;

				std::vector<std::uint8_t> file;
				if (!ReadFileImpl(a_texture, file)) {
					result.error = "file not found";
					return result;
				}
				if (file.size() < 4 + sizeof(Header) || *reinterpret_cast<const std::uint32_t*>(file.data()) != kMagic) {
					result.error = "not a DDS file";
					return result;
				}

				Header hdr;
				std::memcpy(&hdr, file.data() + 4, sizeof(Header));
				std::size_t offset = 4 + sizeof(Header);

				HeaderDX10        dx10{};
				const HeaderDX10* dx10_ptr = nullptr;
				if ((hdr.pf.flags & kPfFourCC) && hdr.pf.fourCC == FourCC('D', 'X', '1', '0')) {
					if (file.size() < offset + sizeof(HeaderDX10)) {
						result.error = "truncated DX10 header";
						return result;
					}
					std::memcpy(&dx10, file.data() + offset, sizeof(HeaderDX10));
					dx10_ptr = &dx10;
					offset += sizeof(HeaderDX10);
				}

				Layout      layout;
				DXGI_FORMAT format;
				if (!Classify(hdr, dx10_ptr, layout, format)) {
					result.error = fmt::format("unsupported format (fourCC {:08X}, dxgi {})", hdr.pf.fourCC, dx10.format);
					return result;
				}

				// Skip down the mip chain to the level closest to kTargetSize
				std::uint32_t w = std::max(1u, hdr.width);
				std::uint32_t h = std::max(1u, hdr.height);
				std::uint32_t levels = std::max(1u, hdr.mipCount);
				for (std::uint32_t level = 1; level < levels && std::max(w, h) > kTargetSize; level++) {
					std::uint32_t nw = std::max(1u, w / 2);
					std::uint32_t nh = std::max(1u, h / 2);
					if (IsBlock(layout) && (nw % 4 != 0 || nh % 4 != 0))
						break;  // D3D11 wants block-compressed sizes in whole blocks
					offset += LevelSize(layout, w, h);
					w = nw;
					h = nh;
				}

				std::size_t size = LevelSize(layout, w, h);
				if (file.size() < offset + size) {
					result.error = "truncated pixel data";
					return result;
				}
				const std::uint8_t* src = file.data() + offset;

				if (IsBlock(layout)) {
					if (w % 4 != 0 || h % 4 != 0) {
						result.error = "block-compressed size is not a multiple of 4";
						return result;
					}
					result.pixels.assign(src, src + size);
					result.pitch = std::max(1u, (w + 3) / 4) * (layout == Layout::kBlock8 ? 8 : 16);
				} else {
					// Convert to RGBA8 and point-sample down if there were no mips to skip to
					std::uint32_t step = std::max(1u, (std::max(w, h) + kTargetSize - 1) / kTargetSize);
					std::uint32_t ow = std::max(1u, w / step);
					std::uint32_t oh = std::max(1u, h / step);
					std::uint32_t bpp = BytesPerPixel(layout);
					result.pixels.resize(static_cast<std::size_t>(ow) * oh * 4);
					for (std::uint32_t y = 0; y < oh; y++)
						for (std::uint32_t x = 0; x < ow; x++)
							ToRGBA(layout, src + (static_cast<std::size_t>(y) * step * w + static_cast<std::size_t>(x) * step) * bpp,
								result.pixels.data() + (static_cast<std::size_t>(y) * ow + x) * 4);
					w = ow;
					h = oh;
					result.pitch = w * 4;
				}

				result.ok = true;
				result.format = format;
				result.width = w;
				result.height = h;
				return result;
			}

			struct Request
			{
				std::string key;
				std::string texture;
			};

			std::mutex              WorkLock;
			std::condition_variable WorkReady;
			std::vector<Request>    Requests;  // newest last: the worker takes the most recent first (what is on screen)
			std::vector<Decoded>    Done;
			bool                    WorkerStarted = false;

			void Worker()
			{
				for (;;) {
					Request request;
					{
						std::unique_lock lock(WorkLock);
						WorkReady.wait(lock, [] { return !Requests.empty(); });
						request = std::move(Requests.back());
						Requests.pop_back();
					}
					Decoded decoded;
					try {
						decoded = Decode(request.key, request.texture);
					} catch (const std::exception& e) {
						decoded.key = request.key;
						decoded.error = e.what();
					}
					if (!decoded.ok)
						logger::debug("[dds_preview] {}: {}", request.texture, decoded.error);
					std::lock_guard lock(WorkLock);
					Done.push_back(std::move(decoded));
				}
			}

			// ------------------------------ UI thread ------------------------------------------------------------

			struct Entry
			{
				State                     state = State::kLoading;
				ID3D11ShaderResourceView* srv = nullptr;
				std::uint32_t             width = 0;
				std::uint32_t             height = 0;
				std::uint64_t             lastUsed = 0;
			};

			std::unordered_map<std::string, Entry> Cache;
			std::uint64_t                          Frame = 0;

			std::string Key(const std::string& a_texture)
			{
				std::string key = a_texture;
				std::ranges::replace(key, '/', '\\');
				std::ranges::transform(key, key.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
				return key;
			}

			ID3D11Device* Device()
			{
				auto renderer = RE::BSGraphics::Renderer::GetSingleton();
				return renderer ? renderer->data.forwarder : nullptr;
			}

			ID3D11ShaderResourceView* Upload(ID3D11Device* a_device, const Decoded& a_img)
			{
				D3D11_TEXTURE2D_DESC desc{};
				desc.Width = a_img.width;
				desc.Height = a_img.height;
				desc.MipLevels = 1;
				desc.ArraySize = 1;
				desc.Format = a_img.format;
				desc.SampleDesc.Count = 1;
				desc.Usage = D3D11_USAGE_IMMUTABLE;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

				D3D11_SUBRESOURCE_DATA data{};
				data.pSysMem = a_img.pixels.data();
				data.SysMemPitch = a_img.pitch;

				ID3D11Texture2D* texture = nullptr;
				if (FAILED(a_device->CreateTexture2D(&desc, &data, &texture)) || !texture)
					return nullptr;

				ID3D11ShaderResourceView* srv = nullptr;
				auto hr = a_device->CreateShaderResourceView(texture, nullptr, &srv);
				texture->Release();  // the view keeps the texture alive
				return SUCCEEDED(hr) ? srv : nullptr;
			}

			// Drops the least recently drawn textures (never ones drawn in the last two frames, ImGui may still use them)
			void Evict()
			{
				std::size_t loaded = 0;
				for (auto& [key, entry] : Cache)
					if (entry.srv)
						loaded++;

				while (loaded > kMaxLoaded) {
					auto oldest = Cache.end();
					for (auto it = Cache.begin(); it != Cache.end(); ++it)
						if (it->second.srv && it->second.lastUsed + 2 < Frame && (oldest == Cache.end() || it->second.lastUsed < oldest->second.lastUsed))
							oldest = it;
					if (oldest == Cache.end())
						break;
					oldest->second.srv->Release();
					Cache.erase(oldest);  // a later Get() queues it again
					loaded--;
				}
			}
		}

		void Pump()
		{
			Frame++;

			auto device = Device();
			if (!device)
				return;

			std::vector<Decoded> ready;
			{
				std::lock_guard lock(WorkLock);
				int count = std::min(static_cast<int>(Done.size()), kUploadsPerFrame);
				for (int i = 0; i < count; i++)
					ready.push_back(std::move(Done[i]));
				Done.erase(Done.begin(), Done.begin() + count);
			}

			for (auto& img : ready) {
				auto it = Cache.find(img.key);
				if (it == Cache.end())
					continue;  // evicted while loading
				auto& entry = it->second;
				entry.srv = img.ok ? Upload(device, img) : nullptr;
				entry.state = entry.srv ? State::kReady : State::kFailed;
				entry.width = img.width;
				entry.height = img.height;
				if (img.ok && !entry.srv)
					logger::debug("[dds_preview] {}: D3D11 texture creation failed", img.key);
			}

			Evict();
		}

		bool ReadTexture(const std::string& a_texture, std::vector<std::uint8_t>& a_out)
		{
			return ReadFileImpl(a_texture, a_out);
		}

		Preview Get(const std::string& a_texture)
		{
			if (a_texture.empty())
				return {};

			auto key = Key(a_texture);
			auto [it, inserted] = Cache.try_emplace(key);
			auto& entry = it->second;
			entry.lastUsed = Frame;

			if (inserted) {
				std::lock_guard lock(WorkLock);
				Requests.push_back({ key, a_texture });
				if (!WorkerStarted) {
					WorkerStarted = true;
					std::thread(Worker).detach();
				}
				WorkReady.notify_one();
			}

			return { entry.srv, entry.state, entry.width, entry.height };
		}
	}
}
