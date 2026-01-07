#include "ActorManager.h"

#include "FBScaler.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
	struct ActorRuntimeState
	{
		std::uint64_t token{ 0 };
		RE::ActorHandle lastTarget{};

		std::unordered_set<std::string_view> casterTouched;
		std::unordered_set<std::string_view> targetTouched;
	};

	std::mutex g_stateMutex;
	std::unordered_map<std::uint32_t, ActorRuntimeState> g_state;  // caster FormID -> runtime state

	static std::uint64_t BumpToken(std::uint32_t casterFormID)
	{
		std::lock_guard _{ g_stateMutex };
		auto& st = g_state[casterFormID];
		++st.token;

		// New run / cancel: clear touched sets (we only reset nodes touched in the current run)
		st.casterTouched.clear();
		st.targetTouched.clear();
		return st.token;
	}

	static bool IsTokenCurrent(std::uint32_t casterFormID, std::uint64_t token)
	{
		std::lock_guard _{ g_stateMutex };
		auto it = g_state.find(casterFormID);
		return it != g_state.end() && it->second.token == token;
	}

	static void SetLastTarget(std::uint32_t casterFormID, RE::ActorHandle target)
	{
		std::lock_guard _{ g_stateMutex };
		g_state[casterFormID].lastTarget = target;
	}

	static RE::ActorHandle GetLastTarget(std::uint32_t casterFormID)
	{
		std::lock_guard _{ g_stateMutex };
		auto it = g_state.find(casterFormID);
		return it == g_state.end() ? RE::ActorHandle{} : it->second.lastTarget;
	}

	static void MarkTouched(std::uint32_t casterFormID, FB::TargetKind who, std::string_view nodeKey)
	{
		std::lock_guard _{ g_stateMutex };
		auto& st = g_state[casterFormID];
		if (who == FB::TargetKind::kCaster) {
			st.casterTouched.insert(nodeKey);
		}
		else {
			st.targetTouched.insert(nodeKey);
		}
	}

	static std::pair<std::unordered_set<std::string_view>, std::unordered_set<std::string_view>>
		TakeTouchedSets(std::uint32_t casterFormID)
	{
		std::lock_guard _{ g_stateMutex };
		auto& st = g_state[casterFormID];

		auto caster = std::move(st.casterTouched);
		auto target = std::move(st.targetTouched);

		st.casterTouched.clear();
		st.targetTouched.clear();

		return { std::move(caster), std::move(target) };
	}
}

namespace FB::ActorManager
{
	void StartTimeline(
		RE::ActorHandle caster,
		RE::ActorHandle target,
		std::uint32_t casterFormID,
		std::vector<FB::TimedCommand> commands,
		bool logOps)
	{
		if (!caster) {
			return;
		}

		const auto token = BumpToken(casterFormID);
		SetLastTarget(casterFormID, target);

		// Detach one worker per command (preserving previous behavior)
		for (const auto& cmd : commands) {
			std::thread([caster, target, casterFormID, token, cmd, logOps]() {
				if (cmd.timeSeconds > 0.0f) {
					std::this_thread::sleep_for(std::chrono::duration<float>(cmd.timeSeconds));
				}

				if (!IsTokenCurrent(casterFormID, token)) {
					return;
				}

				const auto actorHandle = (cmd.target == FB::TargetKind::kCaster) ? caster : target;
				if (!actorHandle) {
					return;
				}

				FB::Scaler::SetNodeScale(actorHandle, cmd.nodeKey, cmd.scale, logOps);
				MarkTouched(casterFormID, cmd.target, cmd.nodeKey);
				}).detach();
		}
	}

	void CancelAndReset(
		RE::ActorHandle caster,
		std::uint32_t casterFormID,
		bool logOps)
	{
		// Cancel pending work
		(void)BumpToken(casterFormID);

		// Snapshot last target + touched nodes
		const auto lastTarget = GetLastTarget(casterFormID);
		auto [casterTouched, targetTouched] = TakeTouchedSets(casterFormID);

		// Reset only nodes we touched (scale back to 1.0)
		if (caster) {
			for (auto node : casterTouched) {
				FB::Scaler::SetNodeScale(caster, node, 1.0f, logOps);
			}
		}

		if (lastTarget) {
			for (auto node : targetTouched) {
				FB::Scaler::SetNodeScale(lastTarget, node, 1.0f, logOps);
			}
		}

		if (logOps) {
			auto c = caster.get();
			spdlog::info("[FB] Reset: caster='{}' casterNodes={} targetNodes={}",
				c ? c->GetName() : "<null>",
				casterTouched.size(),
				targetTouched.size());
		}
	}
}
