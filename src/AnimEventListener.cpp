#include "AnimEventListener.h"
#include "RE/Skyrim.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <unordered_set>

namespace
{
    // =========================================================
    // Payload Interpreter configuration
    // =========================================================
    // IMPORTANT:
    // PIE does NOT reliably emit an AnimEvent tag named "PIE".
    // The ONLY reliable proof that PIE fired is that the graph
    // variable set by its payload has changed.
    static constexpr std::string_view kShrinkBool = "FB_HeadShrink";

    static constexpr std::string_view kPairedTags[] = {
        "PairStart",
        "PairEnd",
        "NPCpairedStop",
        "NPCKillMoveStart",
        "NPCKillMoveEnd",
        "PairedStop",
        "PairStop",
        "PairFail"
    };

    bool IsPairedTag(std::string_view tag)
    {
        for (auto& t : kPairedTags) {
            if (tag == t) {
                return true;
            }
        }
        return false;
    }

    // =========================================================
    // Registration bookkeeping
    // =========================================================
    std::unordered_set<RE::FormID> g_registeredActors;

    auto g_lastHighScan = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    static constexpr auto kHighScanCooldown = std::chrono::milliseconds(1500);

    // =========================================================
    // Graph variable access (THIS is the PI truth source)
    // =========================================================
    bool GetGraphBool(RE::Actor* actor, std::string_view name, bool& out)
    {
        if (!actor) {
            return false;
        }

        RE::BSTSmartPointer<RE::BSAnimationGraphManager> mgr;
        if (!actor->GetAnimationGraphManager(mgr) || !mgr) {
            return false;
        }

        for (auto& graph : mgr->graphs) {
            if (!graph) {
                continue;
            }

            bool value = false;
            if (graph->GetGraphVariableBool(name.data(), value)) {
                out = value;
                return true;
            }
        }

        return false;
    }

    void LogPIEState(RE::Actor* actor, std::string_view context)
    {
        bool value = false;
        const bool ok = GetGraphBool(actor, kShrinkBool, value);

        spdlog::info(
            "[PIE-Check] ctx='{}' actor={:08X} '{}' {} ok={} value={}",
            context,
            actor->GetFormID(),
            actor->GetName(),
            kShrinkBool.data(),
            ok,
            value
        );
    }

    // =========================================================
    // Actor registration helpers
    // =========================================================
    template <class T>
    concept HasGet = requires(T t) { t.get(); };

    template <class T>
    concept PtrLike = std::is_pointer_v<std::remove_reference_t<T>>;

    template <class T>
    RE::Actor* ResolveActorPtr(T&& v)
    {
        using U = std::remove_reference_t<T>;

        if constexpr (std::is_same_v<U, RE::Actor*> || std::is_same_v<U, const RE::Actor*>) {
            return const_cast<RE::Actor*>(v);
        }
        else if constexpr (PtrLike<U>) {
            if (!v) {
                return nullptr;
            }
            return const_cast<std::remove_const_t<std::remove_pointer_t<U>>*>(v)
                ->template As<RE::Actor>();
        }
        else if constexpr (HasGet<U>) {
            return ResolveActorPtr(v.get());
        }
        else {
            return nullptr;
        }
    }

    void RegisterToActorGraphs(AnimEventListener* sink, RE::Actor* actor, std::string_view reason)
    {
        if (!sink || !actor) {
            return;
        }

        const auto fid = actor->GetFormID();
        if (fid == 0 || !g_registeredActors.insert(fid).second) {
            return;
        }

        RE::BSTSmartPointer<RE::BSAnimationGraphManager> mgr;
        const bool hasMgr = actor->GetAnimationGraphManager(mgr) && mgr;

        spdlog::info(
            "[RegisterActor] reason='{}' actor={:08X} '{}' isPlayer={} hasGraphMgr={} 3DLoaded={}",
            reason,
            fid,
            actor->GetName(),
            actor->IsPlayerRef(),
            hasMgr,
            actor->Is3DLoaded()
        );

        if (!hasMgr) {
            return;
        }

        for (auto& graph : mgr->graphs) {
            if (graph) {
                graph->AddEventSink<RE::BSAnimationGraphEvent>(sink);
            }
        }

        LogPIEState(actor, "RegisterActor");
    }

    void RegisterToAllHighActors(AnimEventListener* sink, std::string_view reason)
    {
        if (!sink) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - g_lastHighScan < kHighScanCooldown) {
            return;
        }
        g_lastHighScan = now;

        auto* lists = RE::ProcessLists::GetSingleton();
        if (!lists) {
            return;
        }

        for (auto&& elem : lists->highActorHandles) {
            if (auto* a = ResolveActorPtr(elem)) {
                RegisterToActorGraphs(sink, a, reason);
            }
        }
    }

    std::unordered_set<RE::FormID> g_oneShot;
}

// =============================================================
// External hook (called by your plugin entry)
// =============================================================
void RegisterAnimationEventSink(RE::Actor* actor)
{
    if (!actor) {
        return;
    }

    RegisterToActorGraphs(
        AnimEventListener::GetSingleton(),
        actor,
        "external/RegisterAnimationEventSink"
    );
}

// =============================================================
// AnimEventListener implementation
// =============================================================
AnimEventListener* AnimEventListener::GetSingleton()
{
    static AnimEventListener singleton;
    return &singleton;
}

void AnimEventListener::RegisterToPlayer()
{
    if (auto* player = RE::PlayerCharacter::GetSingleton()) {
        RegisterToActorGraphs(this, player, "startup/player");
        RegisterToAllHighActors(this, "startup/highActors");
    }
}

RE::BSEventNotifyControl AnimEventListener::ProcessEvent(
    const RE::BSAnimationGraphEvent* a_event,
    RE::BSTEventSource<RE::BSAnimationGraphEvent>*)
{
    if (!a_event || a_event->tag.empty() || !a_event->holder) {
        return RE::BSEventNotifyControl::kContinue;
    }

    auto* actor = const_cast<RE::Actor*>(a_event->holder->As<RE::Actor>());
    if (!actor) {
        return RE::BSEventNotifyControl::kContinue;
    }

    const std::string_view tag{ a_event->tag.c_str(), a_event->tag.size() };

    if (IsPairedTag(tag) || tag == "KillMoveStart") {
        spdlog::info(
            "[AnimEvt] tag='{}' actor={:08X} '{}' isPlayer={} 3DLoaded={}",
            a_event->tag.c_str(),
            actor->GetFormID(),
            actor->GetName(),
            actor->IsPlayerRef(),
            actor->Is3DLoaded()
        );
    }

    if (IsPairedTag(tag)) {
        RegisterToAllHighActors(this, tag);

        if (tag == "PairEnd" && g_oneShot.insert(actor->GetFormID()).second) {
            LogPIEState(actor, "PairEnd");
        }

        if (tag == "FB_HeadShrink" && g_oneShot.insert(actor->GetFormID()).second) {
            LogPIEState(actor, "FB_HeadShrink");
        }
    }

    return RE::BSEventNotifyControl::kContinue;
}
