#pragma once

#include "RE/Skyrim.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace FB::Morph
{
	// Key used for all NiOverride operations from this plugin
	inline constexpr std::string_view kMorphKey = "FullBodiedPlugin";

	// Logical morph keys supported by FBConfig
	// FBConfig validates these; FBMorph maps them to real RaceMenu morph names.
	inline constexpr std::string_view kMorph_VorePreyBelly = "Vore Prey Belly";

	// Clamp range for final slider values
	inline constexpr float kMinValue = 0.0f;
	inline constexpr float kMaxValue = 100.0f;

	// Add delta to this plugin's current value for (actor, morphKey)
	void AddDelta(
		RE::ActorHandle actor,
		std::string_view morphKey,
		float delta,
		bool logOps);

	// Clear all morphs applied by this plugin (by key) for actor
	void ResetAllForActor(
		RE::ActorHandle actor,
		bool logOps);
}
