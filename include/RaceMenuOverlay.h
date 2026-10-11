#pragma once
// C++ port of the overlay parts of papyrus/scripts/source/racemenu.psc:
//   OnOverlayTextureChange, OnOverlayColorChange, OnOverlayGlowColorChange,
//   InvalidateShaders, UpdateOverlay/UpdateOverlays, LoadDefaults' DEFAULT_OVERLAY.
// The RaceSex menu (Scaleform) plumbing is not ported; the ImGui UI calls these directly.

#include <format>
#include <memory>
#include <mutex>

#include "NiOverride.h"

namespace RaceMenuOverlay {
    inline constexpr auto DefaultOverlay = "Actors\\Character\\Overlays\\Default.dds"sv;

    // TINT_TYPE_* in racemenu.psc
    enum class Type : std::int32_t {
        kBody = 256,   // TINT_TYPE_BODYPAINT
        kHands = 257,  // TINT_TYPE_HANDPAINT
        kFeet = 258,   // TINT_TYPE_FEETPAINT
        kFace = 259,   // TINT_TYPE_FACEPAINT
    };

    // "Body [Ovl" + index + "]" etc.
    inline std::string NodeName(Type a_type, int a_index) {
        switch (a_type) {
            case Type::kBody: return std::format("Body [Ovl{}]", a_index);
            case Type::kHands: return std::format("Hands [Ovl{}]", a_index);
            case Type::kFeet: return std::format("Feet [Ovl{}]", a_index);
            case Type::kFace: return std::format("Face [Ovl{}]", a_index);
        }
        return {};
    }

    inline bool IsFemale(RE::Actor* a_actor) {
        auto base = a_actor ? a_actor->GetActorBase() : nullptr;
        return base && base->GetSex() == RE::SEX::kFemale;
    }

    // NetImmerse.HasNode(ref, node, firstPerson)
    inline bool HasNode(RE::Actor* a_actor, std::string_view a_node, bool a_firstPerson = false) {
        auto root = a_actor ? a_actor->Get3D(a_firstPerson) : nullptr;
        return root && root->GetObjectByName(RE::BSFixedString(a_node));
    }

    // InvalidateShaders()
    inline void InvalidateShaders(RE::Actor* a_actor) {
        if (!a_actor || !NiOverride::IsInstalled()) return;
        NiOverride::ApplyOverrides(a_actor);
        NiOverride::ApplyNodeOverrides(a_actor);
        NiOverride::ApplySkinOverrides(a_actor);
    }

    // OnOverlayTextureChange: a_texture may be a single path or "tex0|tex1|...|" (up to 9, "ignore" skips a slot)
    inline void SetTexture(RE::Actor* a_actor, Type a_type, int a_index, std::string_view a_texture) {
        if (!a_actor || !NiOverride::IsInstalled()) return;
        const auto node = NodeName(a_type, a_index);
        const bool isFemale = IsFemale(a_actor);

        switch (a_type) {
            case Type::kBody: NiOverride::RevertOverlay(a_actor, node, NiOverride::kMaskBody, NiOverride::kMaskBody); break;
            case Type::kHands: NiOverride::RevertOverlay(a_actor, node, NiOverride::kMaskHands, NiOverride::kMaskHands); break;
            case Type::kFeet: NiOverride::RevertOverlay(a_actor, node, NiOverride::kMaskFeet, NiOverride::kMaskFeet); break;
            case Type::kFace: NiOverride::RevertHeadOverlay(a_actor, node, NiOverride::kPartFace, NiOverride::kShaderFaceTint); break;
        }

        auto strIndex = a_texture.find('|');
        if (strIndex != std::string_view::npos) {
            std::size_t lastIndex = 0;
            for (int i = 0; strIndex != std::string_view::npos && i < 9; i++) {
                auto texturePath = a_texture.substr(lastIndex, strIndex - lastIndex);
                if (texturePath != "ignore") {
                    NiOverride::AddNodeOverrideString(a_actor, isFemale, node, NiOverride::kShaderTexture, i, texturePath, true);
                }
                lastIndex = strIndex + 1;
                strIndex = a_texture.find('|', lastIndex);
            }
        } else {
            NiOverride::AddNodeOverrideString(a_actor, isFemale, node, NiOverride::kShaderTexture, 0, a_texture, true);
        }
    }

    // OnOverlayColorChange: a_color is ARGB, alpha byte becomes ShaderAlpha (0-1)
    inline void SetColor(RE::Actor* a_actor, Type a_type, int a_index, std::uint32_t a_color) {
        if (!a_actor || !NiOverride::IsInstalled()) return;
        const auto node = NodeName(a_type, a_index);
        const bool isFemale = IsFemale(a_actor);
        const auto alpha = a_color >> 24;
        NiOverride::AddNodeOverrideInt(a_actor, isFemale, node, NiOverride::kShaderTintColor, NiOverride::NoIndex,
                                       static_cast<std::int32_t>(a_color), true);
        NiOverride::AddNodeOverrideFloat(a_actor, isFemale, node, NiOverride::kShaderAlpha, NiOverride::NoIndex,
                                         alpha / 255.0f, true);
    }

    // OnOverlayGlowColorChange: a_color is ARGB, alpha byte becomes EmissiveMultiple (alpha / 10)
    inline void SetGlow(RE::Actor* a_actor, Type a_type, int a_index, std::uint32_t a_color) {
        if (!a_actor || !NiOverride::IsInstalled()) return;
        const auto node = NodeName(a_type, a_index);
        const bool isFemale = IsFemale(a_actor);
        const auto alpha = a_color >> 24;
        NiOverride::AddNodeOverrideInt(a_actor, isFemale, node, NiOverride::kShaderEmissiveColor, NiOverride::NoIndex,
                                       static_cast<std::int32_t>(a_color), true);
        NiOverride::AddNodeOverrideFloat(a_actor, isFemale, node, NiOverride::kShaderEmissiveMultiple, NiOverride::NoIndex,
                                         alpha / 10.0f, true);
    }

    // Values UpdateOverlay() reads back for one overlay slot
    struct OverlayData {
        std::uint32_t color = 0;  // ARGB, alpha = ShaderAlpha * 255
        std::uint32_t glow = 0;   // ARGB, alpha = EmissiveMultiple * 10
        std::string texture;      // DefaultOverlay when empty
    };

    // UpdateOverlay() for a single slot. a_done runs once all values arrive (on the Papyrus thread).
    inline void ReadOverlay(RE::Actor* a_actor, Type a_type, int a_index, std::function<void(OverlayData)> a_done) {
        if (!a_actor || !NiOverride::IsInstalled()) return;

        struct State {
            std::mutex lock;
            int pending = 5;
            std::int32_t rgb = 0;
            std::int32_t glow = 0;
            float alpha = 0.0f;
            float multiple = 0.0f;
            std::string texture;
            std::function<void(OverlayData)> done;
        };
        auto state = std::make_shared<State>();
        state->done = std::move(a_done);

        auto finish = [state]() {
            std::lock_guard guard(state->lock);
            if (--state->pending > 0) return;
            OverlayData data;
            data.color = (static_cast<std::uint32_t>(state->rgb) & 0xFFFFFF) |
                         (static_cast<std::uint32_t>(static_cast<std::int32_t>(state->alpha * 255)) << 24);
            data.glow = (static_cast<std::uint32_t>(static_cast<std::int32_t>(state->multiple * 10.0f)) << 24) |
                        static_cast<std::uint32_t>(state->glow);
            data.texture = state->texture.empty() ? std::string(DefaultOverlay) : state->texture;
            if (state->done) state->done(std::move(data));
        };

        const auto node = NodeName(a_type, a_index);
        auto setInt = [state, finish](std::int32_t State::* a_field) {
            return [state, finish, a_field](std::int32_t v) { { std::lock_guard g(state->lock); (*state).*a_field = v; } finish(); };
        };
        auto setFloat = [state, finish](float State::* a_field) {
            return [state, finish, a_field](float v) { { std::lock_guard g(state->lock); (*state).*a_field = v; } finish(); };
        };
        auto setTexture = [state, finish](std::string v) { { std::lock_guard g(state->lock); state->texture = std::move(v); } finish(); };

        using namespace NiOverride;
        if (HasNode(a_actor, node)) {  // Actor has the node, get the immediate property
            GetNodePropertyInt(a_actor, false, node, kShaderTintColor, NoIndex, setInt(&State::rgb));
            GetNodePropertyInt(a_actor, false, node, kShaderEmissiveColor, NoIndex, setInt(&State::glow));
            GetNodePropertyFloat(a_actor, false, node, kShaderAlpha, NoIndex, setFloat(&State::alpha));
            GetNodePropertyString(a_actor, false, node, kShaderTexture, 0, setTexture);
            GetNodePropertyFloat(a_actor, false, node, kShaderEmissiveMultiple, NoIndex, setFloat(&State::multiple));
        } else {  // Doesn't have the node, get it from the override
            const bool isFemale = IsFemale(a_actor);
            GetNodeOverrideInt(a_actor, isFemale, node, kShaderTintColor, NoIndex, setInt(&State::rgb));
            GetNodeOverrideInt(a_actor, isFemale, node, kShaderEmissiveColor, NoIndex, setInt(&State::glow));
            GetNodeOverrideFloat(a_actor, isFemale, node, kShaderAlpha, NoIndex, setFloat(&State::alpha));
            GetNodeOverrideString(a_actor, isFemale, node, kShaderTexture, 0, setTexture);
            GetNodeOverrideFloat(a_actor, isFemale, node, kShaderEmissiveMultiple, NoIndex, setFloat(&State::multiple));
        }
    }

    // GetNum*Overlays() for a type
    inline void GetOverlayCount(Type a_type, NiOverride::IntCallback a_cb) {
        switch (a_type) {
            case Type::kBody: NiOverride::GetNumBodyOverlays(std::move(a_cb)); break;
            case Type::kHands: NiOverride::GetNumHandOverlays(std::move(a_cb)); break;
            case Type::kFeet: NiOverride::GetNumFeetOverlays(std::move(a_cb)); break;
            case Type::kFace: NiOverride::GetNumFaceOverlays(std::move(a_cb)); break;
        }
    }

    // ImGui float[3] (0-1) + alpha byte <-> ARGB int
    inline std::uint32_t PackColor(const float a_rgb[3], std::uint8_t a_alpha) {
        auto c = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return (static_cast<std::uint32_t>(a_alpha) << 24) | (c(a_rgb[0]) << 16) | (c(a_rgb[1]) << 8) | c(a_rgb[2]);
    }
    inline void UnpackColor(std::uint32_t a_color, float a_rgb[3]) {
        a_rgb[0] = ((a_color >> 16) & 0xFF) / 255.0f;
        a_rgb[1] = ((a_color >> 8) & 0xFF) / 255.0f;
        a_rgb[2] = (a_color & 0xFF) / 255.0f;
    }
}
