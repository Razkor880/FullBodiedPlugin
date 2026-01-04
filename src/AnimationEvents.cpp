#include "AnimationEvents.h"

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <spdlog/spdlog.h>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

namespace
{
    // --- NEW: helper to read anim var float ---
    static bool GetAnimVarFloat(RE::Actor* actor, const char* name, float& out)
    {
        if (!actor) {
            return false;
        }
        return actor->GetGraphVariableFloat(name, out);
    }

    // --- NEW: per-actor latch to make the float pulse fire once ---
    static std::unordered_map<RE::FormID, bool> g_varLatch;

    // Name of the variable as set in your HKX annotations
    static constexpr const char* kVar_HeadShrink = "FB_HeadShrinkValue";

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

            // Optional: log only FB_ tags to avoid insane spam
            if (tag.rfind("FB_", 0) == 0) {
                spdlog::info("[AnimSink] actor={} tag='{}'", actor->GetName(), a_event->tag.c_str());
            }

            // Catch anything containing HeadShrink (helps confirm annotation is arriving at all)
            if (tag.find("HeadShrink") != std::string_view::npos) {
                spdlog::info("[DBG] Saw tag containing 'HeadShrink': actor={} tag='{}'",
                    actor->GetName(), a_event->tag.c_str());
            }

            // Known-good trigger: KillMoveStart
            if (tag == "PairEnd") {
                float v = 0.0f;
                const bool hasV = GetAnimVarFloat(actor, "FB_HeadShrinkValue", v);

                actor->SetGraphVariableFloat("FB_HeadShrinkValue", 0.25f);

                float check = 0.0f;
                bool ok = actor->GetGraphVariableFloat("FB_HeadShrinkValue", check);
                spdlog::info("[KM] after SetGraphVariableFloat ok={} value={}", ok ? 1 : 0, check);



                spdlog::info("[KM] KillMoveStart on {}  hasVar={} FB_HeadShrinkValue={}",
                    actor->GetName(), hasV ? 1 : 0, v);

                // Probe likely alt name
                float v2 = 0.0f;
                const bool hasV2 = GetAnimVarFloat(actor, "FB_HeadShrink", v2);
                spdlog::info("[KM] probe FB_HeadShrink hasVar={} value={}", hasV2 ? 1 : 0, v2);

                float scale = 0.0f;
                if (hasV && v > 0.0f) {
                    scale = v;
                }
                else if (hasV2 && v2 > 0.0f) {
                    scale = v2;
                }

                if (scale > 0.0f) {
                    if (scale < 0.05f) scale = 0.05f;
                    if (scale > 3.00f) scale = 3.00f;

                    spdlog::info("[KM] CALL ShrinkHead(actor='{}', scale={})", actor->GetName(), scale);
                    ShrinkHead(actor, scale);
                }
            }

            return RE::BSEventNotifyControl::kContinue;
        }





    };

    AnimationEventSink g_animationEventSink;

    // One-shot guard so we don't spam re-apply (optional but recommended)
    std::mutex g_onceMtx;
    std::unordered_set<RE::FormID> g_shrunk;

    bool MarkOnce(RE::Actor* actor)
    {
        std::scoped_lock lock(g_onceMtx);
        return g_shrunk.insert(actor->GetFormID()).second;
    }
}

void RegisterAnimationEventSink(RE::Actor* actor)
{
    if (!actor) {
        return;
    }

    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    if (!actor->GetAnimationGraphManager(manager) || !manager) {
        return;
    }

    for (auto& graph : manager->graphs) {
        if (!graph) {
            continue;
        }

        graph->AddEventSink<RE::BSAnimationGraphEvent>(&g_animationEventSink);
    }
}


void ShrinkHead(RE::Actor* actor, float scale)

{
    if (!actor) {
        return;
    }

    // One-shot per actor (comment out if you want it repeatable)
    if (!MarkOnce(actor)) {
        return;
    }

    // Safer: defer node edits to a task (avoids touching scene graph mid-update)
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

            auto headNode = root->GetObjectByName("NPC Head [Head]");
            if (!headNode) {
                return;
            }

            headNode->local.scale = scale;
            });
    }
}
