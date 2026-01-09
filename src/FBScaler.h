#pragma once

#include "RE/Skyrim.h"

#include <initializer_list>
#include <string_view>

namespace FB::Scaler
{
	// -------------------------
	// Core primitive
	// -------------------------
	// Scales a single node by name on the actor's 3D (thread-safe via SKSE task queue).
	// Returns immediately; work is executed on the game thread.
	//
	// NEW: records per-actor baseline scale the first time this system touches a given node.
	void SetNodeScale(RE::ActorHandle actor, std::string_view nodeName, float scale, bool logOps);

	// Convenience: reset one or more nodes to scale=1.0f (legacy helper).
	void ResetNodes(RE::ActorHandle actor, std::initializer_list<std::string_view> nodeNames, bool logOps);

	// -------------------------
	// Reset support (baseline-based)
	// -------------------------
	// Resets ONLY nodes previously touched by this system for the given actor,
	// restoring them to the stored baseline (the scale observed before our first modification).
	// Clears the tracked baseline state for that actor.
	void ResetActor(RE::ActorHandle actor, bool logOps);

	// Debug-only convenience. Not wired anywhere by default.
	void ResetAll(bool logOps);

	// -------------------------
	// Canonical node names
	// -------------------------
	inline constexpr std::string_view kNodeHead = "NPC Head [Head]";
	inline constexpr std::string_view kNodeNeck = "NPC Neck [Neck]";

	inline constexpr std::string_view kNodeSpine0 = "NPC Spine [Spn0]";
	inline constexpr std::string_view kNodeSpine1 = "NPC Spine1 [Spn1]";
	inline constexpr std::string_view kNodeSpine2 = "NPC Spine2 [Spn2]";
	// Note: Spine3 may not exist on all skeletons; safe to call anyway.
	inline constexpr std::string_view kNodeSpine3 = "NPC Spine3 [Spn3]";

	inline constexpr std::string_view kNodePelvis = "NPC Pelvis [Pelv]";

	inline constexpr std::string_view kNodeLClavicle = "NPC L Clavicle [LClv]";
	inline constexpr std::string_view kNodeRClavicle = "NPC R Clavicle [RClv]";
	inline constexpr std::string_view kNodeLUpperArm = "NPC L UpperArm [LUar]";
	inline constexpr std::string_view kNodeRUpperArm = "NPC R UpperArm [RUar]";
	inline constexpr std::string_view kNodeLForearm = "NPC L Forearm [LLar]";
	inline constexpr std::string_view kNodeRForearm = "NPC R Forearm [RLar]";
	inline constexpr std::string_view kNodeLHand = "NPC L Hand [LHnd]";
	inline constexpr std::string_view kNodeRHand = "NPC R Hand [RHnd]";

	inline constexpr std::string_view kNodeLThigh = "NPC L Thigh [LThg]";
	inline constexpr std::string_view kNodeRThigh = "NPC R Thigh [RThg]";
	inline constexpr std::string_view kNodeLCalf = "NPC L Calf [LClf]";
	inline constexpr std::string_view kNodeRCalf = "NPC R Calf [RClf]";
	inline constexpr std::string_view kNodeLFoot = "NPC L Foot [Lft ]";
	inline constexpr std::string_view kNodeRFoot = "NPC R Foot [Rft ]";
	inline constexpr std::string_view kNodeLToe0 = "NPC L Toe0 [LToe]";
	inline constexpr std::string_view kNodeRToe0 = "NPC R Toe0 [RToe]";

	// -------------------------
	// Convenience wrappers
	// -------------------------
	inline void SetHeadScale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeHead, scale, logOps); }
	inline void SetNeckScale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeNeck, scale, logOps); }

	inline void SetSpine0Scale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeSpine0, scale, logOps); }
	inline void SetSpine1Scale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeSpine1, scale, logOps); }
	inline void SetSpine2Scale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeSpine2, scale, logOps); }
	inline void SetSpine3Scale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeSpine3, scale, logOps); }

	inline void SetPelvisScale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodePelvis, scale, logOps); }

	inline void SetLeftClavicleScale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeLClavicle, scale, logOps); }
	inline void SetRightClavicleScale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeRClavicle, scale, logOps); }
	inline void SetLeftUpperArmScale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeLUpperArm, scale, logOps); }
	inline void SetRightUpperArmScale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeRUpperArm, scale, logOps); }
	inline void SetLeftForearmScale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeLForearm, scale, logOps); }
	inline void SetRightForearmScale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeRForearm, scale, logOps); }
	inline void SetLeftHandScale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeLHand, scale, logOps); }
	inline void SetRightHandScale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeRHand, scale, logOps); }

	inline void SetLeftThighScale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeLThigh, scale, logOps); }
	inline void SetRightThighScale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeRThigh, scale, logOps); }
	inline void SetLeftCalfScale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeLCalf, scale, logOps); }
	inline void SetRightCalfScale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeRCalf, scale, logOps); }
	inline void SetLeftFootScale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeLFoot, scale, logOps); }
	inline void SetRightFootScale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeRFoot, scale, logOps); }
	inline void SetLeftToe0Scale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeLToe0, scale, logOps); }
	inline void SetRightToe0Scale(RE::ActorHandle actor, float scale, bool logOps) { SetNodeScale(actor, kNodeRToe0, scale, logOps); }
}
