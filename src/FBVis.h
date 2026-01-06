#pragma once

#include "RE/Skyrim.h"

namespace FB::Vis
{
	// Toggle visibility on a non-node object (BSTriShape/BSGeometry/etc.) found by exact name.
	// Runs on the game thread via SKSE task queue.
	//
	// IMPORTANT:
	// This intentionally skips NiNodes (skeleton bones) to avoid hiding child subtrees
	// (e.g., hiding pelvis bone would hide legs).
	void SetObjectVisibleExact(RE::ActorHandle actor, std::string_view objectName, bool visible, bool logOps);

	// Convenience: set multiple exact names.
	void SetObjectsVisibleExact(RE::ActorHandle actor,
		std::initializer_list<std::string_view> objectNames,
		bool visible,
		bool logOps);

	// Debug helper: dump non-node object names under actor->Get3D() to the log.
	void DumpNonNodeObjectNames(RE::ActorHandle actor, bool logOps);
}
