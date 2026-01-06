#include "FBCommandRouter.h"

#include <chrono>
#include <thread>

#include "SKSE/SKSE.h"

#include "FBScaler.h"
#include "FBVis.h"

namespace FB::CommandRouter
{
	void ExecuteCommandNow(const Context& ctx, const Command& cmd)
	{
		switch (cmd.type) {
		case CommandType::kScale:
			if (cmd.target == TargetKind::kCaster) {
				FB::Scaler::SetNodeScaleByKey(ctx.caster, cmd.key, cmd.scale, ctx.logOps);
			} else {
				FB::Scaler::SetNodeScaleByKey(ctx.target, cmd.key, cmd.scale, ctx.logOps);
			}
			break;

		case CommandType::kVis:
			if (cmd.target == TargetKind::kCaster) {
				FB::Vis::SetVisibleByKey(ctx.caster, cmd.key, cmd.visible, ctx.logOps);
			} else {
				FB::Vis::SetVisibleByKey(ctx.target, cmd.key, cmd.visible, ctx.logOps);
			}
			break;

		default:
			break;
		}
	}

	void ScheduleCommands(
		const Context& ctx,
		const std::vector<Command>& commands,
		std::uint32_t casterFormID,
		std::uint64_t token,
		GetTokenFn getTokenFn)
	{
		for (const auto& cmd : commands) {
			std::thread([ctx, casterFormID, token, getTokenFn, cmd]() mutable {
				if (cmd.timeSeconds > 0.0f) {
					std::this_thread::sleep_for(std::chrono::duration<float>(cmd.timeSeconds));
				}

				// If a newer token exists, abort (canceled/reset).
				if (getTokenFn && getTokenFn(casterFormID) != token) {
					return;
				}

				// Graph / NiNode work must run on the game thread.
				if (auto* taskIF = SKSE::GetTaskInterface(); taskIF) {
					taskIF->AddTask([ctx, cmd]() mutable {
						ExecuteCommandNow(ctx, cmd);
					});
				} else {
					// Fallback: attempt direct call (may be unsafe in some contexts).
					ExecuteCommandNow(ctx, cmd);
				}
			}).detach();
		}
	}
}
