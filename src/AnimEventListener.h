#pragma once
#include "RE/Skyrim.h"

class AnimEventListener : public RE::BSTEventSink<RE::BSAnimationGraphEvent> {
public:
    static AnimEventListener* GetSingleton();

    void RegisterToPlayer();

    RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event,
                                          RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_source) override;

private:
    bool _registered = false;
};
