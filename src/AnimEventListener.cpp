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


RE::BSEventNotifyControl AnimEventListener::ProcessEvent(const RE::BSAnimationGraphEvent* a_event,
                                                         RE::BSTEventSource<RE::BSAnimationGraphEvent>*) {
    if (a_event && !a_event->tag.empty()) {
        SKSE::log::info("Anim event: {}", a_event->tag);
    }
    return RE::BSEventNotifyControl::kContinue;
}
