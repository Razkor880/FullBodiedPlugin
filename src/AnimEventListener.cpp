#include "AnimEventListener.h"

#include "RE/Skyrim.h"
#include <spdlog/spdlog.h>

AnimEventListener* AnimEventListener::GetSingleton()
{
    static AnimEventListener singleton;
    return &singleton;
}

void AnimEventListener::RegisterToPlayer()
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        spdlog::warn("AnimEventListener::RegisterToPlayer: player not available");
        return;
    }

    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    if (!player->GetAnimationGraphManager(manager) || !manager) {
        spdlog::warn("AnimEventListener::RegisterToPlayer: no animation graph manager");
        return;
    }

    for (auto& graph : manager->graphs) {
        if (!graph) {
            continue;
        }

        graph->AddEventSink<RE::BSAnimationGraphEvent>(this);
    }

    spdlog::info("AnimEventListener registered to player graphs");
}

RE::BSEventNotifyControl AnimEventListener::ProcessEvent(
    const RE::BSAnimationGraphEvent* a_event,
    RE::BSTEventSource<RE::BSAnimationGraphEvent>*)
{
    if (!a_event || a_event->tag.empty()) {
        return RE::BSEventNotifyControl::kContinue;
    }

    const std::string_view tag{ a_event->tag.c_str(), a_event->tag.size() };

    // your filter/logging here...
    // spdlog::info("Anim event: {}", tag);

    return RE::BSEventNotifyControl::kContinue;
}
