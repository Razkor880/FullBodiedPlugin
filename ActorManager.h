#pragma once

#include "RE/Skyrim.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace FB
{
	enum class TargetKind
	{
		kCaster,
		kTarget
	};

	// Parsed command. AnimationEvents is responsible for parsing + mapping nodeKey.
	struct TimedCommand
	{
		TargetKind target{ TargetKind::kCaster };
		float timeSeconds{ 0.0f };
		std::string_view nodeKey{};  // canonical node name (e.g. FB::Scaler::kNodeHead)
		float scale{ 1.0f };
	};
}

namespace FB::ActorManager
{
	// Starts a new timeline for a caster. Cancels any previous pending work for this caster.
	// Commands are copied/moved in to ensure lifetime across detached worker threads.
	void StartTimeline(
		RE::ActorHandle caster,
		RE::ActorHandle target,
		std::uint32_t casterFormID,
		std::vector<FB::TimedCommand> commands,
		bool logOps);

	// Cancels pending work and resets only touched nodes for caster + last target.
	void CancelAndReset(
		RE::ActorHandle caster,
		std::uint32_t casterFormID,
		bool logOps);
}
