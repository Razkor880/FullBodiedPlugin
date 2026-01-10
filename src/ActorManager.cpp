#include "ActorManager.h"

#include "FBScaler.h"
#include "FBMorph.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <cassert>

namespace
{
    struct ActorRuntimeState
    {
        std::uint64_t token{ 0 };
        RE::ActorHandle lastTarget{};

        std::unordered_set<std::string_view> casterTouchedScale;
        std::unordered_set<std::string_view> targetTouchedScale;

        bool casterTouchedMorph{ false };
        bool targetTouchedMorph{ false };
    };

    std::mutex g_stateMutex;
    std::unordered_map<std::uint32_t, ActorRuntimeState> g_state;

    static std::uint64_t BumpToken(std::uint32_t casterFormID)
    {
        std::lock_guard _{ g_stateMutex };
        auto& st = g_state[casterFormID];
        ++st.token;

        st.casterTouchedScale.clear();
        st.targetTouchedScale.clear();
        st.casterTouchedMorph = false;
        st.targetTouchedMorph = false;

        return st.token;
    }

    static bool IsTokenCurrent(std::uint32_t casterFormID, std::uint64_t token)
    {
        std::lock_guard _{ g_stateMutex };
        auto it = g_state.find(casterFormID);
        return it != g_state.end() && it->second.token == token;
    }

    static void SetLastTarget(std::uint32_t casterFormID, RE::ActorHandle target)
    {
        std::lock_guard _{ g_stateMutex };
        g_state[casterFormID].lastTarget = target;
    }

    static RE::ActorHandle GetLastTarget(std::uint32_t casterFormID)
    {
        std::lock_guard _{ g_stateMutex };
        auto it = g_state.find(casterFormID);
        return it == g_state.end() ? RE::ActorHandle{} : it->second.lastTarget;
    }

    static void MarkTouchedScale(std::uint32_t casterFormID, FB::TargetKind who, std::string_view nodeKey)
    {
        std::lock_guard _{ g_stateMutex };
        auto& st = g_state[casterFormID];
        if (who == FB::TargetKind::kCaster) {
            st.casterTouchedScale.insert(nodeKey);
        }
        else {
            st.targetTouchedScale.insert(nodeKey);
        }
    }

    static void MarkTouchedMorph(std::uint32_t casterFormID, FB::TargetKind who)
    {
        std::lock_guard _{ g_stateMutex };
        auto& st = g_state[casterFormID];
        if (who == FB::TargetKind::kCaster) {
            st.casterTouchedMorph = true;
        }
        else {
            st.targetTouchedMorph = true;
        }
    }

    struct ResetSnapshot
    {
        RE::ActorHandle lastTarget{};
        std::unordered_set<std::string_view> casterScale;
        std::unordered_set<std::string_view> targetScale;
        bool casterMorph{ false };
        bool targetMorph{ false };
    };

    static ResetSnapshot TakeSnapshot(std::uint32_t casterFormID)
    {
        std::lock_guard _{ g_stateMutex };
        auto& st = g_state[casterFormID];

        ResetSnapshot out;
        out.lastTarget = st.lastTarget;
        out.casterScale = std::move(st.casterTouchedScale);
        out.targetScale = std::move(st.targetTouchedScale);
        out.casterMorph = st.casterTouchedMorph;
        out.targetMorph = st.targetTouchedMorph;

        st.casterTouchedScale.clear();
        st.targetTouchedScale.clear();
        st.casterTouchedMorph = false;
        st.targetTouchedMorph = false;

        return out;
    }

    // ------------------------------------------------------------
    // TODO(TweenRefactor): Phase 6/7 - deterministic timeline runtime
    // ------------------------------------------------------------
    struct ActiveTimeline
    {
        RE::ActorHandle caster;
        RE::ActorHandle target;
        std::uint32_t casterFormID{ 0 };
        std::uint64_t token{ 0 };

        bool logOps{ false };

        float elapsedSeconds{ 0.0f };
        std::size_t nextIndex{ 0 };
        std::vector<FB::TimedCommand> commands;
    };

    // Keyed by casterFormID (matches existing token + reset ownership model)
    static std::unordered_map<std::uint32_t, ActiveTimeline> g_activeTimelines;

    // ------------------------------------------------------------
    // TODO(TweenRefactor): Phase 8 - active morph tweens
    // One tween per (actor, morph key); scheduling replaces existing.
    // ------------------------------------------------------------
    struct TweenKey
    {
        std::uint32_t actorFormID{ 0 };
        std::string morphName;

        bool operator==(const TweenKey& o) const noexcept
        {
            return actorFormID == o.actorFormID && morphName == o.morphName;
        }
    };

    struct TweenKeyHash
    {
        std::size_t operator()(const TweenKey& k) const noexcept
        {
            std::size_t h1 = std::hash<std::uint32_t>{}(k.actorFormID);
            std::size_t h2 = std::hash<std::string>{}(k.morphName);
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ull + (h1 << 6) + (h1 >> 2));
        }
    };

    struct ActiveTween
    {
        RE::ActorHandle actor;
        std::uint32_t actorFormID{ 0 };

        // Ownership for reset/token validity
        std::uint32_t casterFormID{ 0 };
        std::uint64_t token{ 0 };
        FB::TargetKind who{ FB::TargetKind::kCaster };

        std::string morphName;
        float totalDelta{ 0.0f };
        float appliedSoFar{ 0.0f };

        float durationSeconds{ 0.0f };
        float elapsedSeconds{ 0.0f };

        bool touchedMarked{ false };
    };

    static std::unordered_map<TweenKey, ActiveTween, TweenKeyHash> g_activeTweens;

    static RE::ActorHandle ResolveActor(const ActiveTimeline& tl, FB::TargetKind who)
    {
        return (who == FB::TargetKind::kCaster) ? tl.caster : tl.target;
    }

    static void ExecuteScale(std::uint32_t casterFormID, RE::ActorHandle actor, const FB::TimedCommand& cmd, bool logOps)
    {
        if (!actor) {
            return;
        }
        FB::Scaler::SetNodeScale(actor, cmd.nodeKey, cmd.scale, logOps);
        MarkTouchedScale(casterFormID, cmd.target, cmd.nodeKey);
    }

    static void ExecuteMorphInstant(std::uint32_t casterFormID, RE::ActorHandle actor, const FB::TimedCommand& cmd, bool logOps)
    {
        if (!actor) {
            return;
        }
        FB::Morph::AddDelta(actor, cmd.morphName, cmd.delta, logOps);
        MarkTouchedMorph(casterFormID, cmd.target);
    }

    static void ScheduleMorphTween(const ActiveTimeline& tl, const FB::TimedCommand& cmd)
    {
        RE::ActorHandle actor = ResolveActor(tl, cmd.target);
        if (!actor) {
            return;
        }

        auto a = actor.get();  // NiPointer<RE::Actor>
        if (!a) {
            return;
        }

        ActiveTween tw;
        tw.actor = actor;
        tw.actorFormID = a->GetFormID();

        tw.casterFormID = tl.casterFormID;
        tw.token = tl.token;
        tw.who = cmd.target;

        tw.morphName = cmd.morphName;
        tw.totalDelta = cmd.delta;
        tw.appliedSoFar = 0.0f;

        tw.durationSeconds = cmd.tweenSeconds;
        tw.elapsedSeconds = 0.0f;

        TweenKey key{ tw.actorFormID, tw.morphName };

        // Replacement rule: one tween per (actor, morph key)
        g_activeTweens[key] = std::move(tw);
    }

    static void ClearTweensForCaster(std::uint32_t casterFormID)
    {
        for (auto it = g_activeTweens.begin(); it != g_activeTweens.end(); ) {
            if (it->second.casterFormID == casterFormID) {
                it = g_activeTweens.erase(it);
            }
            else {
                ++it;
            }
        }
    }
}

namespace FB::ActorManager
{
    void StartTimeline(
        RE::ActorHandle caster,
        RE::ActorHandle target,
        std::uint32_t casterFormID,
        std::vector<FB::TimedCommand> commands,
        bool logOps)
    {
        if (!caster) {
            return;
        }

        // TODO(TweenRefactor): Phase 7 - remove thread-per-command timing; register deterministic runtime state
        const auto token = BumpToken(casterFormID);
        SetLastTarget(casterFormID, target);

        ActiveTimeline tl;
        tl.caster = caster;
        tl.target = target;
        tl.casterFormID = casterFormID;
        tl.token = token;
        tl.logOps = logOps;
        tl.elapsedSeconds = 0.0f;
        tl.nextIndex = 0;
        tl.commands = std::move(commands);

        g_activeTimelines[casterFormID] = std::move(tl);
    }

    void Update(float dtSeconds)
    {
        // TODO(TweenRefactor): Phase 6/7/8 - deterministic tick entry point (called from PlayerCharacter::Update hook)

        // Required safety: skip dt <= 0
        if (dtSeconds <= 0.0f) {
            return;
        }

        // Required safety: clamp pathological dt spikes (loading/hitch/pause)
        constexpr float kMaxDtSeconds = 0.25f;
        if (dtSeconds > kMaxDtSeconds) {
            dtSeconds = kMaxDtSeconds;
        }

        // 1) Advance deterministic timelines
        for (auto it = g_activeTimelines.begin(); it != g_activeTimelines.end(); ) {
            ActiveTimeline& tl = it->second;

            // Token validity must be checked before executing any work
            if (!IsTokenCurrent(tl.casterFormID, tl.token)) {
                it = g_activeTimelines.erase(it);
                continue;
            }

            tl.elapsedSeconds += dtSeconds;

            // Execute all due commands in correct order
            while (tl.nextIndex < tl.commands.size()) {
                const auto& cmd = tl.commands[tl.nextIndex];
                if (cmd.timeSeconds > tl.elapsedSeconds) {
                    break;
                }

                // Resolve target actor for this command
                const auto actorHandle = ResolveActor(tl, cmd.target);
                if (cmd.kind == FB::CommandKind::kScale) {
                    ExecuteScale(tl.casterFormID, actorHandle, cmd, tl.logOps);
                }
                else if (cmd.kind == FB::CommandKind::kMorph) {

                    // TODO(TweenRefactor Phase 9): runtime currently supports LINEAR tween curves only.
                    // Parser is expected to enforce this, but we defensively guard here.
#ifndef NDEBUG
                    assert(cmd.tweenCurve == FB::TweenCurve::kLinear);
#endif

                    if (cmd.tweenCurve != FB::TweenCurve::kLinear) {
                        // Should never happen unless parser rules are bypassed or future changes forget to update runtime.
                        if (tl.logOps) {
                            spdlog::warn(
                                "[FB] Non-linear tween curve reached runtime (forcing linear). morph='{}'",
                                cmd.morphName);
                        }
                        // No behavior change: we continue using linear progression.
                    }

                    // Phase 8: if tweenSeconds > 0, schedule a tween instead of instant apply
                    if (cmd.tweenSeconds > 0.0f) {
                        ScheduleMorphTween(tl, cmd);
                    }
                    else {
                        ExecuteMorphInstant(tl.casterFormID, actorHandle, cmd, tl.logOps);
                    }
                }



                ++tl.nextIndex;
            }

            // Done
            if (tl.nextIndex >= tl.commands.size()) {
                it = g_activeTimelines.erase(it);
                continue;
            }

            ++it;
        }

        // 2) Advance active tweens (after timeline scheduling)
        for (auto it = g_activeTweens.begin(); it != g_activeTweens.end(); ) {
            ActiveTween& tw = it->second;

            // Token validity must be checked before applying any morph delta
            if (!IsTokenCurrent(tw.casterFormID, tw.token)) {
                it = g_activeTweens.erase(it);
                continue;
            }

            if (!tw.actor) {
                it = g_activeTweens.erase(it);
                continue;
            }

            auto a = tw.actor.get();  // NiPointer<RE::Actor>
            if (!a) {
                it = g_activeTweens.erase(it);
                continue;
            }


            // Protect against bad durations
            if (tw.durationSeconds <= 0.0f) {
                it = g_activeTweens.erase(it);
                continue;
            }

            tw.elapsedSeconds += dtSeconds;

            const float alpha = std::clamp(tw.elapsedSeconds / tw.durationSeconds, 0.0f, 1.0f);
            const float targetApplied = tw.totalDelta * alpha;
            const float stepDelta = targetApplied - tw.appliedSoFar;

            if (stepDelta != 0.0f) {
                FB::Morph::AddDelta(tw.actor, tw.morphName, stepDelta, false);

                // Mark touched morph only once we actually apply something
                if (!tw.touchedMarked) {
                    MarkTouchedMorph(tw.casterFormID, tw.who);
                    tw.touchedMarked = true;
                }

                tw.appliedSoFar = targetApplied;
            }

            if (alpha >= 1.0f) {
                it = g_activeTweens.erase(it);
                continue;
            }

            ++it;
        }
    }

    void CancelAndReset(
        RE::ActorHandle caster,
        std::uint32_t casterFormID,
        bool logOps,
        bool resetMorphCaster,
        bool resetMorphTarget)
    {
        // Invalidate all pending work
        (void)BumpToken(casterFormID);

        // TODO(TweenRefactor): Phase 7/8 - clear deterministic timelines and tweens for this caster
        g_activeTimelines.erase(casterFormID);
        ClearTweensForCaster(casterFormID);

        auto snap = TakeSnapshot(casterFormID);

        if (caster) {
            for (auto node : snap.casterScale) {
                FB::Scaler::SetNodeScale(caster, node, 1.0f, logOps);
            }

            if (resetMorphCaster) {
                FB::Morph::ResetAllForActor(caster, logOps);
            }
        }

        if (snap.lastTarget) {
            for (auto node : snap.targetScale) {
                FB::Scaler::SetNodeScale(snap.lastTarget, node, 1.0f, logOps);
            }

            if (resetMorphTarget) {
                FB::Morph::ResetAllForActor(snap.lastTarget, logOps);
            }
        }

        if (logOps) {
            auto c = caster.get();
            spdlog::info("[FB] Reset: caster='{}' casterNodes={} targetNodes={} resetMorphCaster={} resetMorphTarget={}",
                c ? c->GetName() : "<null>",
                snap.casterScale.size(),
                snap.targetScale.size(),
                resetMorphCaster,
                resetMorphTarget);
        }
    }
}
