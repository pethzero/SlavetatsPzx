#pragma once
#include "SKSEMenuFramework.h"
#include "Configuration.h"
namespace UI {
    void Register();
    namespace SlaveTattoo {
        // Dummy UI only: no real tattoo backend yet.
        enum Area : int { kBody = 0, kHead, kHands, kFeet, kAreaCount };
        inline constexpr int SlotCount = 6;
        inline constexpr const char* AreaNames[kAreaCount] = {"Body", "Head", "Hands", "Feet"};

        struct TattooGroup {
            std::string name;
            std::vector<std::string> tattoos;
        };

        struct Slot {
            int group = -1;           // selected in combo (pending)
            int tattoo = -1;
            int appliedGroup = -1;    // what is "saved" on the actor
            int appliedTattoo = -1;
            float ink[3] = {0.0f, 0.0f, 0.0f};
            float glow[3] = {0.0f, 0.0f, 0.0f};
            bool glossy = false;
            bool IsApplied() const { return appliedTattoo >= 0; }
        };

        inline std::string TargetName;
        inline bool HasTarget = false;
        inline bool Searched = false;
        inline std::array<std::vector<TattooGroup>, kAreaCount> Groups;
        inline std::array<std::array<Slot, SlotCount>, kAreaCount> Slots;

        void SelectPlayer();
        void RunSearch();
        void RenderArea(Area area);
        void RenderSlot(Area area, int index);

        void __stdcall RenderSetup();
        void __stdcall RenderBody();
        void __stdcall RenderHead();
        void __stdcall RenderHands();
        void __stdcall RenderFeet();
    }
};
