#pragma once

#include "RE/Skyrim.h"

// Register the animation graph event sink to a specific actor's graphs.
// (Implemented in AnimationEvents.cpp)
void RegisterAnimationEventSink(RE::Actor* actor);

// Public API
namespace FB
{
	// Loads/refreshes config from Data\FullBodiedIni.ini
	void ReloadConfig();

	// Convenience: registers the sink(s) to the player (calls RegisterAnimationEventSink internally)
	void RegisterAnimationEventSinkToPlayer();
}
