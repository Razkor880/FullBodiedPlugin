#pragma once

#include <string_view>

#include "RE/Skyrim.h"

namespace FB::Scaler
{
	// Backwards-compatible "exact name" setter.
	// Prefer SetNodeScaleByKey for INI-driven keys like "Head", "Pelvis", etc.
	void SetNodeScale(RE::ActorHandle actor, std::string_view nodeName, float scale, bool logOps);

	// New: resolve a bone node by a stable key ("Head", "Pelvis", "Spine1", "LThigh", ...)
	// and apply local scale.
	void SetNodeScaleByKey(RE::ActorHandle actor, std::string_view nodeKey, float scale, bool logOps);

	// Canonical vanilla skeleton names (for convenience/back-compat).
	inline constexpr std::string_view kNodePelvis    = "NPC Pelvis [Pelv]";
	inline constexpr std::string_view kNodeSpine0    = "NPC Spine [Spn0]";
	inline constexpr std::string_view kNodeSpine1    = "NPC Spine1 [Spn1]";
	inline constexpr std::string_view kNodeSpine2    = "NPC Spine2 [Spn2]";
	inline constexpr std::string_view kNodeSpine3    = "NPC Spine3 [Spn3]";
	inline constexpr std::string_view kNodeNeck      = "NPC Neck [Neck]";
	inline constexpr std::string_view kNodeHead      = "NPC Head [Head]";

	inline constexpr std::string_view kNodeLClavicle = "NPC L Clavicle [LClv]";
	inline constexpr std::string_view kNodeRClavicle = "NPC R Clavicle [RClv]";
	inline constexpr std::string_view kNodeLUpperArm = "NPC L UpperArm [LUar]";
	inline constexpr std::string_view kNodeRUpperArm = "NPC R UpperArm [RUar]";
	inline constexpr std::string_view kNodeLForearm  = "NPC L Forearm [LLar]";
	inline constexpr std::string_view kNodeRForearm  = "NPC R Forearm [RLar]";
	inline constexpr std::string_view kNodeLHand     = "NPC L Hand [LHnd]";
	inline constexpr std::string_view kNodeRHand     = "NPC R Hand [RHnd]";

	inline constexpr std::string_view kNodeLThigh    = "NPC L Thigh [LThg]";
	inline constexpr std::string_view kNodeRThigh    = "NPC R Thigh [RThg]";
	inline constexpr std::string_view kNodeLCalf     = "NPC L Calf [LClf]";
	inline constexpr std::string_view kNodeRCalf     = "NPC R Calf [RClf]";
	inline constexpr std::string_view kNodeLFoot     = "NPC L Foot [Lft ]";
	inline constexpr std::string_view kNodeRFoot     = "NPC R Foot [Rft ]";
	inline constexpr std::string_view kNodeLToe0     = "NPC L Toe0 [LToe]";
	inline constexpr std::string_view kNodeRToe0     = "NPC R Toe0 [RToe]";
}
