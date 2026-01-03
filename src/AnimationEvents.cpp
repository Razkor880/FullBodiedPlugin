#include "AnimationEvents.h"
#include "RE/Skyrim.h"
#include "REL/Relocation.h"
#include <string_view>

class AnimationEventSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent> {
public:
    virtual RE::BSEventNotifyControl ProcessEvent(
        const RE::BSAnimationGraphEvent* a_event,
        RE::BSTEventSource<RE::BSAnimationGraphEvent>* /*a_eventSource*/
    ) override
    {
        if (!a_event || !a_event->holder)
            return RE::BSEventNotifyControl::kContinue;

        RE::Actor* actor = const_cast<RE::Actor*>(a_event->holder->As<RE::Actor>());
        if (!actor)
            return RE::BSEventNotifyControl::kContinue;

        std::string tag = a_event->tag.c_str();

        if (tag == "FB_HeadShrink") {
            ShrinkHead(actor);
        }

        return RE::BSEventNotifyControl::kContinue;
    }
};

static AnimationEventSink g_animationEventSink;


void RegisterAnimationEventSink(RE::Actor* actor)
{
    if (!actor) {
        return;
    }

    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    if (!actor->GetAnimationGraphManager(manager) || !manager) {  // correct signature
        return;
    }

    for (auto& graph : manager->graphs) {
        if (!graph) {
            continue;
        }

        graph->AddEventSink<RE::BSAnimationGraphEvent>(&g_animationEventSink); // no offsets
    }
}



void ShrinkHead(RE::Actor* actor) {
    if (!actor)
        return;

    auto root = actor->Get3D();
    if (!root)
        return;

    auto headNode = root->GetObjectByName("NPC Head [Head]");
    if (!headNode)
        return;

    headNode->local.scale = 0.25f;
    headNode->UpdateWorldData(nullptr);
}
