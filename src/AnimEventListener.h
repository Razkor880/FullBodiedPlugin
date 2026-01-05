#pragma once

#include "RE/Skyrim.h"

// AnimEventListener is an event sink for animation graph events.
class AnimEventListener : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
{
public:
    static AnimEventListener* GetSingleton();

    // Registers this sink to the player's graphs (and optionally other actors if you call them elsewhere).
    void RegisterToPlayer();

    // BSTEventSink override
    RE::BSEventNotifyControl ProcessEvent(
        const RE::BSAnimationGraphEvent* a_event,
        RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_source) override;
};

// Legacy/global helper used by your FullBodiedPlugin.cpp (keeps your current architecture building)
void RegisterAnimationEventSink(RE::Actor* actor);
