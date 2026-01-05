// AnimationEvents.cpp
//
// Head shrink helper:
// - Runs node edits on the main thread via SKSE task interface.
// - Logs when 3D / head node is missing.
// - Updates world transforms so changes apply immediately.

#include "AnimationEvents.h"

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <spdlog/spdlog.h>
#include <cstdint>

namespace
{
    RE::NiAVObject* FindHeadNode(RE::NiAVObject* root)
    {
        if (!root) {
            return nullptr;
        }

        if (auto* n = root->GetObjectByName("NPC Head [Head]")) {
            return n;
        }
        if (auto* n = root->GetObjectByName("NPC Head")) {
            return n;
        }
        if (auto* n = root->GetObjectByName("Head")) {
            return n;
        }

        return nullptr;
    }
}

void ShrinkHead(RE::Actor* actor, float scale)
{
    if (!actor) {
        spdlog::warn("[FB] ShrinkHead: actor=null");
        return;
    }

    if (!(scale >= 0.0f) || scale > 100.0f) {
        spdlog::warn("[FB] ShrinkHead: invalid scale={} (actor='{}')", scale, actor->GetName());
        return;
    }

    const auto handle = actor->CreateRefHandle();

    auto* task = SKSE::GetTaskInterface();
    if (!task) {
        spdlog::warn("[FB] ShrinkHead: SKSE task interface not available");
        return;
    }

    task->AddTask([handle, scale]() {
        auto actorPtr = handle.get();
        if (!actorPtr) {
            spdlog::info("[FB] ShrinkHead: actor handle expired");
            return;
        }

        RE::NiAVObject* root = actorPtr->Get3D();
        if (!root) {
            spdlog::info("[FB] ShrinkHead: 3D not loaded for '{}' ({:08X})",
                actorPtr->GetName(),
                static_cast<std::uint32_t>(actorPtr->GetFormID()));
            return;
        }

        auto* headNode = FindHeadNode(root);
        if (!headNode) {
            spdlog::info("[FB] ShrinkHead: head node not found for '{}' ({:08X})",
                actorPtr->GetName(),
                static_cast<std::uint32_t>(actorPtr->GetFormID()));
            return;
        }

        const float oldScale = headNode->local.scale;

        spdlog::info("[FB] ShrinkHead: actor='{}' node='{}' oldScale={} newScale={}",
            actorPtr->GetName(),
            headNode->name.c_str(),
            oldScale,
            scale);

        headNode->local.scale = scale;

        // Your headers expect NiUpdateData*. nullptr is OK for a "force update" attempt.
        headNode->UpdateWorldData(nullptr);

        // Optional: also refresh the root to help propagate changes
        root->UpdateWorldData(nullptr);
        });
}
