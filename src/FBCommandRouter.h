#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "RE/Skyrim.h"

namespace FB::CommandRouter
{
	enum class TargetKind : std::uint8_t
	{
		kCaster = 0,
		kTarget = 1
	};

	enum class CommandType : std::uint8_t
	{
		kScale = 0,
		kVis = 1
	};

	// NOTE:
	// - For kScale: key = node key (e.g. "Head", "Pelvis", "Spine1", "LThigh", ...)
	// - For kVis:   key = vis key (either exact object name, or a group key defined via FB::Vis::SetGroups)
	struct Command
	{
		CommandType type{ CommandType::kScale };
		TargetKind  target{ TargetKind::kCaster };
		float       timeSeconds{ 0.0f };

		std::string key;     // nodeKey or visKey
		float       scale{ 1.0f };
		bool        visible{ true };
	};

	struct Context
	{
		RE::ActorHandle caster;
		RE::ActorHandle target;
		bool            logOps{ false };
	};

	using GetTokenFn = std::uint64_t(*)(std::uint32_t casterFormID);

	// Schedules commands with per-command delays.
	// - Token checks are performed on the worker thread before dispatching to the game thread.
	void ScheduleCommands(
		const Context& ctx,
		const std::vector<Command>& commands,
		std::uint32_t casterFormID,
		std::uint64_t token,
		GetTokenFn getTokenFn);

	// Executes immediately (CALL ON GAME THREAD when possible).
	void ExecuteCommandNow(const Context& ctx, const Command& cmd);
}
