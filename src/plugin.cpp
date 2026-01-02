#include <spdlog/sinks/basic_file_sink.h>

#include "AnimEventListener.h"
#include "SKSE/SKSE.h"

namespace {
    void SetupLogging() {
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
}

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    SetupLogging();

    spdlog::info("FullBodiedPlugin loaded");

    if (auto* messaging = SKSE::GetMessagingInterface()) {
        messaging->RegisterListener([](SKSE::MessagingInterface::Message* msg) {
            if (!msg) {
                return;
            }

            switch (msg->type) {
                case SKSE::MessagingInterface::kDataLoaded:
                case SKSE::MessagingInterface::kNewGame:
                case SKSE::MessagingInterface::kPostLoadGame:
                    AnimEventListener::GetSingleton()->RegisterToPlayer();
                    break;
                default:
                    break;
            }
        });
    }

    return true;
}
