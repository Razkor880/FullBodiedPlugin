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
        kMorph
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
        std::string morphName{};   // IMPORTANT: own the string (no dangling string_view)
        float       delta{ 0.0f }; // delta-only (add/subtract)
    };
}

namespace FB::ActorManager
{
    void StartTimeline(
        RE::ActorHandle caster,
        RE::ActorHandle target,
        std::uint32_t casterFormID,
        std::vector<FB::TimedCommand> commands,
        bool logOps);

    // Cancel pending work, reset touched scales, and optionally clear morph keys.
    void CancelAndReset(
        RE::ActorHandle caster,
        std::uint32_t casterFormID,
        bool logOps,
        bool resetMorphCaster,
        bool resetMorphTarget);
}
