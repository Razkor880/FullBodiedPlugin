// FullBodiedPlugin.cpp

#include <memory>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include "AnimEventListener.h"
#include "AnimationEvents.h"

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

namespace
{
	void SetupLogging()
	{
		auto path = SKSE::log::log_directory();
		if (!path) {
			// If this is null, SKSE couldn't resolve Documents\My Games\...\SKSE
			// (usually profile / Documents redirection / permissions)
			return;
		}

		*path /= "FullBodiedPlugin.log";

		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto logger = std::make_shared<spdlog::logger>("global", sink);

		spdlog::set_default_logger(std::move(logger));
		spdlog::set_level(spdlog::level::info);
		spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
		spdlog::flush_on(spdlog::level::info);

		spdlog::info("Logging initialized: {}", path->string());
	}

	void RegisterSinksToPlayer()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			spdlog::warn("PlayerCharacter singleton not available yet.");
			return;
		}

		// Optional (but nice): parse INI once up front so the first event doesn't "pay" for it.
		// This is the function we added in AnimationEvents.h / AnimationEvents.cpp.
		LoadHeadScaleConfig();

		// 1) Head scale sink (FB_HeadScaleS### + config-driven timelines)
		RegisterAnimationEventSink(player);

		// 2) Your debug/logger sink (prints tags you care about)
		AnimEventListener::GetSingleton()->RegisterToPlayer();

		spdlog::info("Registered animation sinks to player.");
	}
}

// CommonLibSSE-NG export macro (expands to exported SKSEPlugin_Load entry point)
SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
	SKSE::Init(skse);
	SetupLogging();

	spdlog::info("FullBodiedPlugin loaded");

	// Don't register to the player here: during SKSEPluginLoad the player/graphs
	// are often not ready yet. Use MessagingInterface events instead.
	if (auto* messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener([](SKSE::MessagingInterface::Message* msg) {
			if (!msg) {
				return;
			}

			switch (msg->type) {
			case SKSE::MessagingInterface::kDataLoaded:
			case SKSE::MessagingInterface::kNewGame:
			case SKSE::MessagingInterface::kPostLoadGame:
				RegisterSinksToPlayer();
				break;

			default:
				break;
			}
			});
	}
	else {
		spdlog::warn("Messaging interface not available.");
	}

	return true;
}
