#pragma once

#include "ActorManager.h"  // FB::TimedCommand

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace FB::Config
{
	// AnimationEvents owns the "public API" mapping from NodeKey -> canonical node string_view.
	// FBConfig needs it to validate and translate NodeKeys while parsing.
	using NodeKeyResolver = std::optional<std::string_view>(*)(std::string_view);

	struct DebugConfig
	{
		bool strictIni{ true };
		bool logOps{ true };
		bool logIni{ true };
		bool logTargetResolve{ false };
		bool logTimelineStart{ true };
		bool resetMorphsOnPairEnd{ true };
		bool resetMorphsOnPairedStop{ true };

	};

	struct ConfigData
	{
		bool enableTimelines{ true };

		// Scale/visibility/etc reset behavior (existing)
		bool resetOnPairEnd{ true };
		bool resetOnPairedStop{ true };
		bool resetScalesOnPairEnd{ true };
		bool resetScalesOnPairedStop{ true };

		// NEW: Morph reset behavior (RaceMenu/NiOverride key cleanup)
		bool resetMorphsOnPairEnd{ true };
		bool resetMorphsOnPairedStop{ true };  // optional but recommended for parity

		

		DebugConfig dbg{};

		// StartEventTag -> TimelineName
		std::unordered_map<std::string, std::string> eventToTimeline;

		// TimelineName -> commands
		std::unordered_map<std::string, std::vector<FB::TimedCommand>> timelines;
	};

	// Get cached config (lazy-load on first call). Resolver must be provided at least once.
	const ConfigData& Get(NodeKeyResolver resolver);

	// Force reload from disk using resolver.
	void Reload(NodeKeyResolver resolver);
}
