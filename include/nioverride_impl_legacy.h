#pragma once

namespace slavetats_ng
{
	namespace skee_wrapper
	{
		namespace nioverride_impl
		{
			namespace nioverride_legacy
			{
				using slavetats_ng::skee_wrapper::nioverride_impl::NiOverride_functions;

				// ⚠ Every game string handed to RaceMenu here is pinned: its reference count is set to the pool's maximum,
				// which the pool treats as permanent, so the string is never freed. With RaceMenu 0.4.16 (these address
				// tables) node names were freed while still in use: the log read them back as garbage ("Cleared vl14]",
				// several nodes showing the same text) and the game crashed later, on saving or rendering. Where the extra
				// release comes from isn't known (one extra reference per call did not stop it), so the strings are made
				// unfreeable instead. The cost is a few hundred short strings kept for the session.
				// Do NOT declare the parameters as RE::BSFixedString: its copy constructor makes the compiler pass it by
				// address, and RaceMenu then reads that address as the text (every slot showed as External).
				inline const char* Pin(const char* a_string)
				{
					if (!a_string || !*a_string)
						return a_string;
					alignas(RE::BSFixedString) std::byte storage[sizeof(RE::BSFixedString)];  // never destroyed
					auto        held = new (storage) RE::BSFixedString(a_string);              // interns it, one reference
					const char* data = held->c_str();
					auto        entry = reinterpret_cast<RE::BSStringPool::Entry*>(const_cast<char*>(data)) - 1;
					std::atomic_ref<std::uint16_t> flags{ entry->_flags };
					std::uint16_t                  expected = flags.load();
					while ((expected & RE::BSStringPool::Entry::kRefCountMask) != RE::BSStringPool::Entry::kRefCountMask &&
						   !flags.compare_exchange_weak(expected, static_cast<std::uint16_t>(expected | RE::BSStringPool::Entry::kRefCountMask))) {
					}
					return data;
				}

				// Diagnostic: the node names came back as garbage after these calls even with Pin(). Watch snapshots the
				// text before a call and logs (up to 50 times) when the same pointer reads differently afterwards, naming
				// the RaceMenu function: that tells which call damages the string.
				// Recent strings handed to RaceMenu, re-read before every call and after ApplyNodeOverrides: damage done
				// between calls (or by a call that takes no string) shows up too. Game thread only.
				struct Seen
				{
					const char* ptr = nullptr;
					std::string text;
				};
				inline std::array<Seen, 64> Recent{};
				inline std::size_t          RecentNext = 0;
				inline int                  Reports = 0;

				inline void Report(const char* a_what, const char* a_ptr, const std::string& a_before)
				{
					if (Reports++ >= 50)
						return;
					logger::error("legacy RaceMenu {}: string at {} changed: '{}' -> '{}'", a_what, static_cast<const void*>(a_ptr), a_before,
						std::string(a_ptr, strnlen(a_ptr, 64)));
				}

				inline void CheckRecent(const char* a_when)
				{
					for (auto& seen : Recent) {
						if (seen.ptr && std::strncmp(seen.ptr, seen.text.c_str(), seen.text.size() + 1) != 0) {
							Report(a_when, seen.ptr, seen.text);
							seen.ptr = nullptr;
						}
					}
				}

				inline void Remember(const char* a_ptr)
				{
					for (auto& seen : Recent) {
						if (seen.ptr == a_ptr)
							return;
					}
					Recent[RecentNext] = { a_ptr, a_ptr };
					RecentNext = (RecentNext + 1) % Recent.size();
				}

				// Diagnostic: node names came back as garbage after these calls even with Pin(). Watch snapshots the text
				// before a call and reports (up to 50 times in all) when the same pointer reads differently afterwards,
				// naming the RaceMenu function: that tells which call damages the string.
				struct Watch
				{
					const char* ptr;
					const char* what;
					std::string before;

					Watch(const char* a_ptr, const char* a_what) :
						ptr(a_ptr), what(a_what), before(a_ptr ? a_ptr : "")
					{
						CheckRecent("between calls (seen before this one)");
					}

					~Watch()
					{
						if (!ptr)
							return;
						if (std::strncmp(ptr, before.c_str(), before.size() + 1) != 0)
							Report(what, ptr, before);
						else
							Remember(ptr);
					}
				};

				typedef void (*_AddNodeOverrideFloat_t)(RE::StaticFunctionTag*, RE::TESObjectREFR* a_ref, bool a_female, const char* a_node, int a_key, int a_index, float a_value, bool);
				typedef void (*_AddNodeOverrideInt_t)(RE::StaticFunctionTag*, RE::TESObjectREFR* a_ref, bool a_female, const char* a_node, int a_key, int a_index, int a_value, bool);
				typedef void (*_AddNodeOverrideString_t)(RE::StaticFunctionTag*, RE::TESObjectREFR* a_ref, bool a_female, const char* a_node, int a_key, int a_index, const char* a_value, bool);
				typedef void (*_AddOverlays_t)(RE::StaticFunctionTag*, RE::TESObjectREFR* a_ref);
				typedef void (*_ApplyNodeOverrides_t)(RE::StaticFunctionTag*, RE::Actor* a_actor);
				typedef float (*_GetNodeOverrideFloat_t)(RE::StaticFunctionTag*, RE::TESObjectREFR* a_ref, bool a_female, const char* a_node, int a_key, int a_index);
				typedef int (*_GetNodeOverrideInt_t)(RE::StaticFunctionTag*, RE::TESObjectREFR* a_ref, bool a_female, const char* a_node, int a_key, int a_index);
				typedef RE::BSFixedString (*_GetNodeOverrideString_t)(RE::StaticFunctionTag*, RE::TESObjectREFR* a_ref, bool a_female, const char* a_node, int a_key, int a_index);
				typedef bool (*_HasNodeOverride_t)(RE::StaticFunctionTag*, RE::TESObjectREFR* a_ref, bool a_female, const char* a_node, int a_key, int a_index);
				typedef bool (*_HasOverlays_t)(RE::StaticFunctionTag*, RE::TESObjectREFR* a_ref);
				typedef void (*_RemoveNodeOverride_t)(RE::StaticFunctionTag*, RE::TESObjectREFR* a_ref, bool a_female, const char* a_node, int a_key, int a_index);
				typedef void (*_RemoveOverlays_t)(RE::StaticFunctionTag*, RE::TESObjectREFR* a_ref);

				class RaceMenuAddressTables : public clib_util::singleton::ISingleton<RaceMenuAddressTables>, public NiOverride_functions
				{
				public:
					RaceMenuAddressTables();
					void init();

				private:
					RE::StaticFunctionTag    _static_function_tag;
					_AddNodeOverrideFloat_t  _AddNodeOverrideFloat = nullptr;
					_AddNodeOverrideInt_t    _AddNodeOverrideInt = nullptr;
					_AddNodeOverrideString_t _AddNodeOverrideString = nullptr;
					_AddOverlays_t           _AddOverlays = nullptr;
					_ApplyNodeOverrides_t    _ApplyNodeOverrides = nullptr;
					_GetNodeOverrideFloat_t  _GetNodeOverrideFloat = nullptr;
					_GetNodeOverrideInt_t    _GetNodeOverrideInt = nullptr;
					_GetNodeOverrideString_t _GetNodeOverrideString = nullptr;
					_HasNodeOverride_t       _HasNodeOverride = nullptr;
					_HasOverlays_t           _HasOverlays = nullptr;
					_RemoveNodeOverride_t    _RemoveNodeOverride = nullptr;
					_RemoveOverlays_t        _RemoveOverlays = nullptr;

				public:
					virtual void AddNodeOverrideFloat(RE::TESObjectREFR* a_ref, bool a_female, const char* a_node, int a_key, int a_index, float a_value, bool a_persist) override
					{
						auto  node = Pin(a_node);
						Watch watch_node{ node, "AddNodeOverrideFloat(node)" };
						_AddNodeOverrideFloat(&_static_function_tag, a_ref, a_female, node, a_key, a_index, a_value, a_persist);
					}
					virtual void AddNodeOverrideInt(RE::TESObjectREFR* a_ref, bool a_female, const char* a_node, int a_key, int a_index, int a_value, bool a_persist) override
					{
						auto  node = Pin(a_node);
						Watch watch_node{ node, "AddNodeOverrideInt(node)" };
						_AddNodeOverrideInt(&_static_function_tag, a_ref, a_female, node, a_key, a_index, a_value, a_persist);
					}

					virtual void AddNodeOverrideString(RE::TESObjectREFR* a_ref, bool a_female, const char* a_node, int a_key, int a_index, const char* a_value, bool a_persist) override
					{
						auto  node = Pin(a_node);
						Watch watch_node{ node, "AddNodeOverrideString(node)" };
						auto  value = Pin(a_value);
						Watch watch_value{ value, "AddNodeOverrideString(value)" };
						_AddNodeOverrideString(&_static_function_tag, a_ref, a_female, node, a_key, a_index, value, a_persist);
					}

					virtual void AddOverlays(RE::TESObjectREFR* a_ref) override
					{
						_AddOverlays(&_static_function_tag, a_ref);
					}

					virtual void ApplyNodeOverrides(RE::Actor* a_actor) override
					{
						CheckRecent("between calls (seen before ApplyNodeOverrides)");
						_ApplyNodeOverrides(&_static_function_tag, a_actor);
						CheckRecent("during ApplyNodeOverrides");
					}

					virtual float GetNodeOverrideFloat(RE::TESObjectREFR* a_ref, bool a_female, const char* a_node, int a_key, int a_index) override
					{
						auto  node = Pin(a_node);
						Watch watch_node{ node, "GetNodeOverrideFloat(node)" };
						return _GetNodeOverrideFloat(&_static_function_tag, a_ref, a_female, node, a_key, a_index);
					}

					virtual int GetNodeOverrideInt(RE::TESObjectREFR* a_ref, bool a_female, const char* a_node, int a_key, int a_index) override
					{
						auto  node = Pin(a_node);
						Watch watch_node{ node, "GetNodeOverrideInt(node)" };
						return _GetNodeOverrideInt(&_static_function_tag, a_ref, a_female, node, a_key, a_index);
					}

					virtual RE::BSFixedString GetNodeOverrideString(RE::TESObjectREFR* a_ref, bool a_female, const char* a_node, int a_key, int a_index) override
					{
						auto  node = Pin(a_node);
						Watch watch_node{ node, "GetNodeOverrideString(node)" };
						return _GetNodeOverrideString(&_static_function_tag, a_ref, a_female, node, a_key, a_index);
					}

					virtual bool HasNodeOverride(RE::TESObjectREFR* a_ref, bool a_female, const char* a_node, int a_key, int a_index) override
					{
						auto  node = Pin(a_node);
						Watch watch_node{ node, "HasNodeOverride(node)" };
						return _HasNodeOverride(&_static_function_tag, a_ref, a_female, node, a_key, a_index);
					}

					virtual bool HasOverlays(RE::TESObjectREFR* a_ref) override
					{
						return _HasOverlays(&_static_function_tag, a_ref);
					}

					// ⚠⚠ NOT CALLED. Caught by Watch (2026-10-15): with the skee64.dll UBE 2.0 ships ("Skyrim 1.5.97, UBE 2.0
					// U.0.7" in skee_addresses.json) every call wrote into the node name it was given ('Body [Ovl8]' ->
					// '', 'Body [Ovl16]' -> 'vl16]'), 50 of 50 reports, no other function ever did. It behaves as if the
					// address were a function taking the string by reference: it zeroes the caller's text and releases
					// a garbage pointer, which damaged the game's string pool - the source of the garbled "Cleared" lines,
					// the crashes on saving / loading a cell / rendering, and the invisible character.
					// Removing an override is only tidying: SlaveTats always sets the slot to the blank texture with alpha 0
					// first (clear_overlay_part1), so the slot is already empty. The overrides simply stay, overwritten
					// the next time the slot is used.
					virtual void RemoveNodeOverride([[maybe_unused]] RE::TESObjectREFR* a_ref, [[maybe_unused]] bool a_female, const char* a_node,
						[[maybe_unused]] int a_key, [[maybe_unused]] int a_index) override
					{
						static std::atomic_bool told = false;
						if (!told.exchange(true))
							logger::info("legacy RaceMenu: RemoveNodeOverride is skipped (its address damages the caller's string), first: {}", a_node ? a_node : "");
					}

					virtual void RemoveOverlays(RE::TESObjectREFR* a_ref) override
					{
						_RemoveOverlays(&_static_function_tag, a_ref);
					}
				};

			}

		}
	}
}
