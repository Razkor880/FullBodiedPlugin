#include "AnimEventListener.h"

#include "SKSE/SKSE.h"

AnimEventListener* AnimEventListener::GetSingleton() {
    static AnimEventListener singleton;
    return &singleton;
}

void AnimEventListener::RegisterToPlayer() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        SKSE::log::warn("Player not ready");
        return;
    }

    RE::BSTSmartPointer<RE::BSAnimationGraphManager> mgr;
    if (!player->GetAnimationGraphManager(mgr) || !mgr) {
        SKSE::log::warn("No animation graph manager yet");
        return;
    }

    for (auto& graph : mgr->graphs) {
        if (graph) {
            graph->RemoveEventSink(this);  // safe even if not present
            graph->AddEventSink(this);
        }
    }

    SKSE::log::info("Registered animation listener to player graphs");
}


static bool ShouldLogTag(std::string_view tag) {
    // examples — replace with your real tags once you discover them
    return tag == "PairStart" || tag == "PairEnd" || tag.starts_with("FNIS_") || tag.starts_with("OAR_") ||
           tag.find("Hug") != std::string_view::npos;
}

RE::BSEventNotifyControl AnimEventListener::ProcessEvent(const RE::BSAnimationGraphEvent* a_event,
                                                         RE::BSTEventSource<RE::BSAnimationGraphEvent>*) {
    if (!a_event || a_event->tag.empty()) {
        return RE::BSEventNotifyControl::kContinue;
    }

    const std::string_view tag{a_event->tag.c_str(), a_event->tag.size()};

    if (ShouldLogTag(tag)) {
        SKSE::log::info("Anim event: {}", tag);
    }

    return RE::BSEventNotifyControl::kContinue;
}