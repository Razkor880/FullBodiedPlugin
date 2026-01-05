// AnimationEvents.cpp
//
// Minimal paired-safe trigger for pa_HugA / paired_huga.hkx:
// - Use the paired event "PairStart" as the trigger (it is commonly forwarded in paired contexts).
// - When PairStart fires, shrink the actor's head node to 0.25.
// - No float variables, no payload parsing, no BDI, no polling.

#include "AnimationEvents.h"

#include "SKSE/SKSE.h"

#include <spdlog/spdlog.h>
#include <string_view>

namespace
{
    static constexpr std::string_view kTriggerTag = "KillMoveStart";
    static constexpr float kHeadScale = 0.25f;

    class AnimationEventSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(
            const RE::BSAnimationGraphEvent* a_event,
            RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
        {
            if (!a_event || !a_event->holder || a_event->tag.empty()) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* actor = const_cast<RE::Actor*>(a_event->holder->As<RE::Actor>());
            if (!actor) {
                return RE::BSEventNotifyControl::kContinue;
            }

            const std::string_view tag{ a_event->tag.c_str(), a_event->tag.size() };

            // Minimal debug logging: prove the paired event is arriving.
            if (tag == kTriggerTag) {
                spdlog::info("[FB] {} on {} -> ShrinkHead(scale={})",
                    a_event->tag.c_str(), actor->GetName(), kHeadScale);

                ShrinkHead(actor, kHeadScale);
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };

    AnimationEventSink g_animationEventSink;
}

void RegisterAnimationEventSink(RE::Actor* actor)
{
    if (!actor) {
        return;
    }

    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    if (!actor->GetAnimationGraphManager(manager) || !manager) {
        spdlog::warn("RegisterAnimationEventSink: no animation graph manager");
        return;
    }

    for (auto& graph : manager->graphs) {
        if (!graph) {
            continue;
        }
        graph->AddEventSink<RE::BSAnimationGraphEvent>(&g_animationEventSink);
    }

    spdlog::info("Registered animation sinks to actor={}", actor->GetName());
}

void ShrinkHead(RE::Actor* actor, float scale)
{
    if (!actor) {
        return;
    }

    // Defer node edits to SKSE task queue (avoids touching scene graph mid-update)
    const auto handle = actor->CreateRefHandle();
    if (auto* task = SKSE::GetTaskInterface()) {
        task->AddTask([handle, scale]() {
            auto actorPtr = handle.get();
            if (!actorPtr) {
                return;
            }

            auto root = actorPtr->Get3D();
            if (!root) {
                return;
            }

            // Most reliable vanilla head node name
            auto headNode = root->GetObjectByName("NPC Head [Head]");
            if (!headNode) {
                spdlog::info("[FB] ShrinkHead: head node not found for '{}'", actorPtr->GetName());
                return;
            }

            spdlog::info("[FB] ShrinkHead: actor='{}' node='{}' oldScale={} newScale={}",
                actorPtr->GetName(), headNode->name.c_str(), headNode->local.scale, scale);

            headNode->local.scale = scale;
            });
    }
}
