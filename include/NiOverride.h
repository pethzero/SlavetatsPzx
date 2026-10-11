#pragma once
// C++ port of papyrus/scripts/source/nioverride.psc (overlay/node-override subset used by the UI).
// NiOverride functions are natives registered by RaceMenu's skee plugin, so each call is
// dispatched to the Papyrus VM as a static call. Calls are queued, not run immediately.
// Getter callbacks run on the Papyrus thread, not the UI thread.

#include <functional>
#include <string>

namespace NiOverride {
    inline constexpr int ScriptVersion = 7;

    // Valid keys (see nioverride.psc header)
    enum Key : std::int32_t {
        kShaderEmissiveColor = 0,     // int
        kShaderEmissiveMultiple = 1,  // float
        kShaderGlossiness = 2,        // float
        kShaderSpecularStrength = 3,  // float
        kShaderLightingEffect1 = 4,   // float
        kShaderLightingEffect2 = 5,   // float
        kShaderTextureSet = 6,        // TextureSet
        kShaderTintColor = 7,         // int
        kShaderAlpha = 8,             // float
        kShaderTexture = 9,           // string (index 0-8)
        kControllerStartStop = 20,    // float
        kControllerStartTime = 21,    // float
        kControllerStopTime = 22,     // float
        kControllerFrequency = 23,    // float
        kControllerPhase = 24,        // float
    };

    // -1 = not a controller/texture-indexed property
    inline constexpr std::int32_t NoIndex = -1;

    // RevertOverlay masks
    inline constexpr std::int32_t kMaskBody = 4;
    inline constexpr std::int32_t kMaskHands = 8;
    inline constexpr std::int32_t kMaskFeet = 128;

    // RevertHeadOverlay
    inline constexpr std::int32_t kPartFace = 1;
    inline constexpr std::int32_t kShaderFaceTint = 4;

    // Equivalent of SKSE.GetPluginVersion("skee") >= 1
    inline bool IsInstalled() {
        return GetModuleHandleW(L"skee64.dll") || GetModuleHandleW(L"skeevr.dll");
    }

    namespace detail {
        inline constexpr auto ClassName = "NiOverride"sv;

        class Callback : public RE::BSScript::IStackCallbackFunctor {
        public:
            explicit Callback(std::function<void(const RE::BSScript::Variable&)> a_fn) : fn(std::move(a_fn)) {}
            void operator()(RE::BSScript::Variable a_result) override {
                if (fn) fn(a_result);
            }
            void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            std::function<void(const RE::BSScript::Variable&)> fn;
        };

        using ResultFn = std::function<void(const RE::BSScript::Variable&)>;

        // Dispatch with a result callback (getters)
        template <class... Args>
        bool Get(std::string_view a_fn, ResultFn a_result, Args&&... a_args) {
            auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) return false;
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
            if (a_result) callback.reset(new Callback(std::move(a_result)));
            auto args = RE::MakeFunctionArguments(std::forward<Args>(a_args)...);
            return vm->DispatchStaticCall(RE::BSFixedString(ClassName), RE::BSFixedString(a_fn), args, callback);
        }

        // Fire-and-forget dispatch (setters)
        template <class... Args>
        bool Call(std::string_view a_fn, Args&&... a_args) {
            return Get(a_fn, ResultFn{}, std::forward<Args>(a_args)...);
        }

        inline RE::BSFixedString Str(std::string_view a_str) { return RE::BSFixedString(a_str); }
    }

    using IntCallback = std::function<void(std::int32_t)>;
    using FloatCallback = std::function<void(float)>;
    using BoolCallback = std::function<void(bool)>;
    using StringCallback = std::function<void(std::string)>;

    namespace detail {
        inline auto Wrap(IntCallback a_cb) {
            return [cb = std::move(a_cb)](const RE::BSScript::Variable& v) { cb(v.IsInt() ? v.GetSInt() : 0); };
        }
        inline auto Wrap(FloatCallback a_cb) {
            return [cb = std::move(a_cb)](const RE::BSScript::Variable& v) { cb(v.IsFloat() ? v.GetFloat() : 0.0f); };
        }
        inline auto Wrap(BoolCallback a_cb) {
            return [cb = std::move(a_cb)](const RE::BSScript::Variable& v) { cb(v.IsBool() && v.GetBool()); };
        }
        inline auto Wrap(StringCallback a_cb) {
            return [cb = std::move(a_cb)](const RE::BSScript::Variable& v) {
                cb(v.IsString() ? std::string(v.GetString()) : std::string());
            };
        }
    }

    // ---------------- Node overrides ----------------
    // ADVANCED USE ONLY, THESE DO NOT SELF CLEANUP IF THE NODE IS NOT FOUND

    inline void HasNodeOverride(RE::Actor* a_ref, bool a_isFemale, std::string_view a_node, std::int32_t a_key,
                                std::int32_t a_index, BoolCallback a_cb) {
        detail::Get("HasNodeOverride", detail::Wrap(std::move(a_cb)), a_ref, a_isFemale, detail::Str(a_node), a_key, a_index);
    }

    inline void AddNodeOverrideFloat(RE::Actor* a_ref, bool a_isFemale, std::string_view a_node, std::int32_t a_key,
                                     std::int32_t a_index, float a_value, bool a_persist) {
        detail::Call("AddNodeOverrideFloat", a_ref, a_isFemale, detail::Str(a_node), a_key, a_index, a_value, a_persist);
    }
    inline void AddNodeOverrideInt(RE::Actor* a_ref, bool a_isFemale, std::string_view a_node, std::int32_t a_key,
                                   std::int32_t a_index, std::int32_t a_value, bool a_persist) {
        detail::Call("AddNodeOverrideInt", a_ref, a_isFemale, detail::Str(a_node), a_key, a_index, a_value, a_persist);
    }
    inline void AddNodeOverrideBool(RE::Actor* a_ref, bool a_isFemale, std::string_view a_node, std::int32_t a_key,
                                    std::int32_t a_index, bool a_value, bool a_persist) {
        detail::Call("AddNodeOverrideBool", a_ref, a_isFemale, detail::Str(a_node), a_key, a_index, a_value, a_persist);
    }
    inline void AddNodeOverrideString(RE::Actor* a_ref, bool a_isFemale, std::string_view a_node, std::int32_t a_key,
                                      std::int32_t a_index, std::string_view a_value, bool a_persist) {
        detail::Call("AddNodeOverrideString", a_ref, a_isFemale, detail::Str(a_node), a_key, a_index, detail::Str(a_value),
                     a_persist);
    }

    // Return the stored override, returns default (nil) values if the override D.N.E
    inline void GetNodeOverrideFloat(RE::Actor* a_ref, bool a_isFemale, std::string_view a_node, std::int32_t a_key,
                                     std::int32_t a_index, FloatCallback a_cb) {
        detail::Get("GetNodeOverrideFloat", detail::Wrap(std::move(a_cb)), a_ref, a_isFemale, detail::Str(a_node), a_key, a_index);
    }
    inline void GetNodeOverrideInt(RE::Actor* a_ref, bool a_isFemale, std::string_view a_node, std::int32_t a_key,
                                   std::int32_t a_index, IntCallback a_cb) {
        detail::Get("GetNodeOverrideInt", detail::Wrap(std::move(a_cb)), a_ref, a_isFemale, detail::Str(a_node), a_key, a_index);
    }
    inline void GetNodeOverrideBool(RE::Actor* a_ref, bool a_isFemale, std::string_view a_node, std::int32_t a_key,
                                    std::int32_t a_index, BoolCallback a_cb) {
        detail::Get("GetNodeOverrideBool", detail::Wrap(std::move(a_cb)), a_ref, a_isFemale, detail::Str(a_node), a_key, a_index);
    }
    inline void GetNodeOverrideString(RE::Actor* a_ref, bool a_isFemale, std::string_view a_node, std::int32_t a_key,
                                      std::int32_t a_index, StringCallback a_cb) {
        detail::Get("GetNodeOverrideString", detail::Wrap(std::move(a_cb)), a_ref, a_isFemale, detail::Str(a_node), a_key, a_index);
    }

    // Gets the property straight from the node (Handy if you need the current value if an override D.N.E yet)
    inline void GetNodePropertyFloat(RE::Actor* a_ref, bool a_firstPerson, std::string_view a_node, std::int32_t a_key,
                                     std::int32_t a_index, FloatCallback a_cb) {
        detail::Get("GetNodePropertyFloat", detail::Wrap(std::move(a_cb)), a_ref, a_firstPerson, detail::Str(a_node), a_key, a_index);
    }
    inline void GetNodePropertyInt(RE::Actor* a_ref, bool a_firstPerson, std::string_view a_node, std::int32_t a_key,
                                   std::int32_t a_index, IntCallback a_cb) {
        detail::Get("GetNodePropertyInt", detail::Wrap(std::move(a_cb)), a_ref, a_firstPerson, detail::Str(a_node), a_key, a_index);
    }
    inline void GetNodePropertyBool(RE::Actor* a_ref, bool a_firstPerson, std::string_view a_node, std::int32_t a_key,
                                    std::int32_t a_index, BoolCallback a_cb) {
        detail::Get("GetNodePropertyBool", detail::Wrap(std::move(a_cb)), a_ref, a_firstPerson, detail::Str(a_node), a_key, a_index);
    }
    inline void GetNodePropertyString(RE::Actor* a_ref, bool a_firstPerson, std::string_view a_node, std::int32_t a_key,
                                      std::int32_t a_index, StringCallback a_cb) {
        detail::Get("GetNodePropertyString", detail::Wrap(std::move(a_cb)), a_ref, a_firstPerson, detail::Str(a_node), a_key, a_index);
    }

    // Applies all overrides visually to the actor, this shouldn't be necessary under normal circumstances
    inline void ApplyOverrides(RE::Actor* a_ref) { detail::Call("ApplyOverrides", a_ref); }
    inline void ApplyNodeOverrides(RE::Actor* a_ref) { detail::Call("ApplyNodeOverrides", a_ref); }
    inline void ApplySkinOverrides(RE::Actor* a_ref) { detail::Call("ApplySkinOverrides", a_ref); }

    // Remove functions do not revert the modified state, only remove it from the save
    inline void RemoveAllNodeNameOverrides(RE::Actor* a_ref, bool a_isFemale, std::string_view a_node) {
        detail::Call("RemoveAllNodeNameOverrides", a_ref, a_isFemale, detail::Str(a_node));
    }
    inline void RemoveNodeOverride(RE::Actor* a_ref, bool a_isFemale, std::string_view a_node, std::int32_t a_key,
                                   std::int32_t a_index) {
        detail::Call("RemoveNodeOverride", a_ref, a_isFemale, detail::Str(a_node), a_key, a_index);
    }

    // ---------------- Overlay data ----------------
    inline void GetNumBodyOverlays(IntCallback a_cb) { detail::Get("GetNumBodyOverlays", detail::Wrap(std::move(a_cb))); }
    inline void GetNumHandOverlays(IntCallback a_cb) { detail::Get("GetNumHandOverlays", detail::Wrap(std::move(a_cb))); }
    inline void GetNumFeetOverlays(IntCallback a_cb) { detail::Get("GetNumFeetOverlays", detail::Wrap(std::move(a_cb))); }
    inline void GetNumFaceOverlays(IntCallback a_cb) { detail::Get("GetNumFaceOverlays", detail::Wrap(std::move(a_cb))); }

    // Adds all enabled overlays to an Actor (Cannot add to player, always exists for player)
    inline void AddOverlays(RE::Actor* a_ref) { detail::Call("AddOverlays", a_ref); }

    // Returns whether this actor has overlays enabled (Always true for player)
    inline void HasOverlays(RE::Actor* a_ref, BoolCallback a_cb) {
        detail::Get("HasOverlays", detail::Wrap(std::move(a_cb)), a_ref);
    }

    // Removes overlays from an actor (Cannot remove from player)
    inline void RemoveOverlays(RE::Actor* a_ref) { detail::Call("RemoveOverlays", a_ref); }

    // Restores the original non-diffuse skin textures to skin overlays
    inline void RevertOverlays(RE::Actor* a_ref) { detail::Call("RevertOverlays", a_ref); }

    // Restores the original non-diffuse skin textures to particular overlay (kMaskBody/kMaskHands/kMaskFeet)
    inline void RevertOverlay(RE::Actor* a_ref, std::string_view a_node, std::int32_t a_armorMask, std::int32_t a_addonMask) {
        detail::Call("RevertOverlay", a_ref, detail::Str(a_node), a_armorMask, a_addonMask);
    }

    // Restores the original non-diffuse skin textures to all head overlays
    inline void RevertHeadOverlays(RE::Actor* a_ref) { detail::Call("RevertHeadOverlays", a_ref); }

    // Restores the original non-diffuse skin textures to particular head overlay (kPartFace, kShaderFaceTint)
    inline void RevertHeadOverlay(RE::Actor* a_ref, std::string_view a_node, std::int32_t a_partType, std::int32_t a_shaderType) {
        detail::Call("RevertHeadOverlay", a_ref, detail::Str(a_node), a_partType, a_shaderType);
    }

    // Call prior to frequent changes in dyes to prevent massive lag, release when finished
    inline void EnableTintTextureCache() { detail::Call("EnableTintTextureCache"); }
    inline void ReleaseTintTextureCache() { detail::Call("ReleaseTintTextureCache"); }
}
