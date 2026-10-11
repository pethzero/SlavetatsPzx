#include "UI.h"

void UI::Register() {
    if (!SKSEMenuFramework::IsInstalled()) {
        return;
    }
    SKSEMenuFramework::SetSection(MOD_NAME);
    SKSEMenuFramework::AddSectionItem("Setup", SlaveTattoo::RenderSetup);
    SKSEMenuFramework::AddSectionItem("Body Tattoos", SlaveTattoo::RenderBody);
    SKSEMenuFramework::AddSectionItem("Head Tattoos", SlaveTattoo::RenderHead);
    SKSEMenuFramework::AddSectionItem("Hand Tattoos", SlaveTattoo::RenderHands);
    SKSEMenuFramework::AddSectionItem("Foot Tattoos", SlaveTattoo::RenderFeet);
}

void UI::SlaveTattoo::SelectPlayer() {
    auto player = RE::PlayerCharacter::GetSingleton();
    TargetName = player && player->GetName() && *player->GetName() ? player->GetName() : "Player";
    HasTarget = true;
    Searched = false;
}

void UI::SlaveTattoo::RunSearch() {
    // Dummy data until the real tattoo folder scan exists.
    Groups[kBody] = {
        {"General", {"Pleasure (right breast)", "Princess (right breast)", "Tramp (left breast)", "Lady (right breast)"}},
        {"Slave Marks", {"Property Of", "Slave Brand", "Owned"}},
        {"Tattoo", {"Yakuza_Asura", "Yakuza_Dragon", "Tribal Back"}},
    };
    Groups[kHead] = {
        {"General", {"Cheek Mark", "Forehead Rune"}},
        {"Warpaint", {"Nord Stripes", "Forsworn Mask"}},
    };
    Groups[kHands] = {
        {"General", {"Knuckle Runes", "Wrist Band"}},
        {"Henna", {"Henna Left", "Henna Right"}},
    };
    Groups[kFeet] = {
        {"General", {"Ankle Chain", "Toe Ring Mark"}},
    };
    for (auto& area : Slots) {
        area.fill(Slot{});
    }
    Searched = true;
}

void __stdcall UI::SlaveTattoo::RenderSetup() {
    ImGuiMCP::SeparatorText("Target");
    if (ImGuiMCP::Button("Select Player")) {
        SelectPlayer();
    }
    ImGuiMCP::SameLine();
    if (HasTarget) {
        ImGuiMCP::Text("Target: %s", TargetName.c_str());
    } else {
        ImGuiMCP::TextDisabled("No target selected");
    }

    ImGuiMCP::SeparatorText("Tattoos");
    ImGuiMCP::BeginDisabled(!HasTarget);
    if (ImGuiMCP::Button("Run Search")) {
        RunSearch();
    }
    ImGuiMCP::EndDisabled();
    ImGuiMCP::SameLine();
    if (Searched) {
        size_t groups = 0, tattoos = 0;
        for (auto& area : Groups) {
            groups += area.size();
            for (auto& group : area) tattoos += group.tattoos.size();
        }
        ImGuiMCP::Text("Found %zu tattoos in %zu groups", tattoos, groups);
    } else {
        ImGuiMCP::TextDisabled(HasTarget ? "Press Run Search to scan tattoos" : "Select a target first");
    }
}

void __stdcall UI::SlaveTattoo::RenderBody() { RenderArea(kBody); }
void __stdcall UI::SlaveTattoo::RenderHead() { RenderArea(kHead); }
void __stdcall UI::SlaveTattoo::RenderHands() { RenderArea(kHands); }
void __stdcall UI::SlaveTattoo::RenderFeet() { RenderArea(kFeet); }

void UI::SlaveTattoo::RenderArea(Area area) {
    if (!HasTarget || !Searched) {
        ImGuiMCP::TextDisabled("Go to Setup: Select Player, then Run Search.");
        return;
    }
    ImGuiMCP::Text("%s tattoos on %s", AreaNames[area], TargetName.c_str());
    for (int i = 0; i < SlotCount; i++) {
        RenderSlot(area, i);
    }
}

void UI::SlaveTattoo::RenderSlot(Area area, int index) {
    auto& slot = Slots[area][index];
    auto& groups = Groups[area];

    ImGuiMCP::PushID(index);
    ImGuiMCP::SeparatorText(std::format("Slot {}", index + 1).c_str());

    if (ImGuiMCP::BeginTable("slot", 2, ImGuiMCP::ImGuiTableFlags_BordersInnerV | ImGuiMCP::ImGuiTableFlags_SizingStretchProp)) {
        ImGuiMCP::TableNextRow();

        // Left: pattern selection
        ImGuiMCP::TableNextColumn();
        const char* groupPreview = slot.group >= 0 ? groups[slot.group].name.c_str() : "[No Tattoo]";
        if (ImGuiMCP::BeginCombo("Group", groupPreview)) {
            if (ImGuiMCP::Selectable("[No Tattoo]", slot.group < 0)) {
                slot.group = -1;
                slot.tattoo = -1;
            }
            for (int g = 0; g < (int)groups.size(); g++) {
                if (ImGuiMCP::Selectable(groups[g].name.c_str(), slot.group == g) && slot.group != g) {
                    slot.group = g;
                    slot.tattoo = -1;
                }
            }
            ImGuiMCP::EndCombo();
        }

        ImGuiMCP::BeginDisabled(slot.group < 0);
        const char* tattooPreview = slot.group >= 0 && slot.tattoo >= 0 ? groups[slot.group].tattoos[slot.tattoo].c_str() : "[No Tattoo]";
        if (ImGuiMCP::BeginCombo("Tattoo", tattooPreview)) {
            auto& tattoos = groups[slot.group].tattoos;
            for (int t = 0; t < (int)tattoos.size(); t++) {
                if (ImGuiMCP::Selectable(tattoos[t].c_str(), slot.tattoo == t)) {
                    slot.tattoo = t;
                }
            }
            ImGuiMCP::EndCombo();
        }
        ImGuiMCP::EndDisabled();

        // Right: colors
        ImGuiMCP::TableNextColumn();
        ImGuiMCP::ColorEdit3("Ink", slot.ink, ImGuiMCP::ImGuiColorEditFlags_NoInputs);
        ImGuiMCP::SameLine();
        ImGuiMCP::ColorEdit3("Glow", slot.glow, ImGuiMCP::ImGuiColorEditFlags_NoInputs);
        ImGuiMCP::SameLine();
        ImGuiMCP::Checkbox("Glossy", &slot.glossy);

        ImGuiMCP::EndTable();
    }

    // Actions
    bool hasSelection = slot.group >= 0 && slot.tattoo >= 0;
    if (!slot.IsApplied()) {
        ImGuiMCP::BeginDisabled(!hasSelection);
        if (ImGuiMCP::Button("Save Tattoo")) {
            slot.appliedGroup = slot.group;
            slot.appliedTattoo = slot.tattoo;
        }
        ImGuiMCP::EndDisabled();
    } else {
        bool changed = slot.group != slot.appliedGroup || slot.tattoo != slot.appliedTattoo;
        ImGuiMCP::BeginDisabled(!hasSelection || !changed);
        if (ImGuiMCP::Button("Replace")) {
            slot.appliedGroup = slot.group;
            slot.appliedTattoo = slot.tattoo;
        }
        ImGuiMCP::EndDisabled();
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Remove")) {
            slot = Slot{};
        }
        if (slot.IsApplied()) {
            ImGuiMCP::SameLine();
            ImGuiMCP::TextDisabled("Applied: %s / %s", groups[slot.appliedGroup].name.c_str(),
                                   groups[slot.appliedGroup].tattoos[slot.appliedTattoo].c_str());
        }
    }

    ImGuiMCP::PopID();
}
