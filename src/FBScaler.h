#pragma once
#include "RE/Skyrim.h"

#include <initializer_list>
#include <string_view>

namespace FB::Scaler
{
	// Scales a single node by name on the actor's 3D (thread-safe via SKSE task queue).
	// Returns immediately; work is executed on the game thread.
	void SetNodeScale(RE::ActorHandle actor, std::string_view nodeName, float scale, bool logOps);

	// Convenience: reset one or more nodes to scale=1.0f.
	void ResetNodes(RE::ActorHandle actor, std::initializer_list<std::string_view> nodeNames, bool logOps);

	// Convenience: scale the usual Skyrim head node.
	void SetHeadScale(RE::ActorHandle actor, float scale, bool logOps);
}
