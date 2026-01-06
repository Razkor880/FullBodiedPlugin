#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "RE/Skyrim.h"

namespace FB::Vis
{
	// Configure "group keys" -> list of exact object names.
	// Example: groups["LThigh"] = { "3BA_LThighShape", "SomeArmor_LThigh" }
	void SetGroups(std::unordered_map<std::string, std::vector<std::string>> groups);

	// Apply visibility using a key:
	// 1) If key exists in groups, apply to every object in that group.
	// 2) Otherwise treat key as an exact object name and apply to that object.
	void SetVisibleByKey(RE::ActorHandle actor, std::string_view key, bool visible, bool logOps);

	// Apply to an exact object name under the actor's 3D.
	// Returns true if a matching object was found.
	bool SetObjectVisibleExact(RE::ActorHandle actor, std::string_view objectName, bool visible, bool logOps);
}
