#pragma once

#include "RE/Skyrim.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace FB
{
    enum class TargetKind
    {
        kCaster,
        kTarget
    };

    enum class CommandKind
    {
        kScale,
        kMorph,
        kHide
    };

    enum class TweenCurve
    {
        kLinear
    };

    // Hide mode is separated (not nested) so callsites can use FB::HideMode::kAll / kSlot cleanly.
    enum class HideMode
    {
        kAll,
        kSlot
    };

    struct TimedCommand
    {
        CommandKind kind{ CommandKind::kScale };
        TargetKind  target{ TargetKind::kCaster };
        float       timeSeconds{ 0.0f };

        // Scale payload (valid when kind==kScale)
        std::string_view nodeKey{};
        float            scale{ 1.0f };

        // Morph payload (valid when kind==kMorph)
        std::string morphName{};
        float       delta{ 0.0f };

        // Tween payload (optional; used only when kind==kMorph)
        float tweenSeconds{ 0.0f };
        TweenCurve tweenCurve{ TweenCurve::kLinear };

        // Hide payload (valid when kind==kHide)
        HideMode      hideMode{ HideMode::kAll };
        std::uint16_t hideSlot{ 0 };  // valid when hideMode==Slot
        bool          hide{ false };
    };



    namespace ActorManager
    {
        // Start a deterministic timeline for a caster/target pair.
        // Commands include their own TargetKind (caster/target) and timeSeconds.
        void StartTimeline(
            RE::ActorHandle caster,
            RE::ActorHandle target,
            std::uint32_t casterFormID,
            std::vector<FB::TimedCommand> commands,
            bool logOps);

        // Cancel current work for casterFormID/token lineage and optionally reset morphs.
        void CancelAndReset(
            RE::ActorHandle caster,
            std::uint32_t casterFormID,
            bool logOps,
            bool resetMorphCaster,
            bool resetMorphTarget);

        // Deterministic tick entry point. Called by your PlayerCharacter::Update hook/pump.
        void Update(float dtSeconds);
    }
}
