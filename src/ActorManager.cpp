#include "ActorManager.h"

#include "FBScaler.h"
#include "FBMorph.h"      // <-- add this

#include <spdlog/spdlog.h>

#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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

        const auto token = BumpToken(casterFormID);
        SetLastTarget(casterFormID, target);

        for (const auto& cmd : commands) {
            std::thread([caster, target, casterFormID, token, cmd, logOps]() mutable {
                if (cmd.timeSeconds > 0.0f) {
                    std::this_thread::sleep_for(std::chrono::duration<float>(cmd.timeSeconds));
                }

                if (!IsTokenCurrent(casterFormID, token)) {
                    return;
                }

                const auto actorHandle = (cmd.target == FB::TargetKind::kCaster) ? caster : target;
                if (!actorHandle) {
                    return;
                }

                if (cmd.kind == FB::CommandKind::kScale) {
                    FB::Scaler::SetNodeScale(actorHandle, cmd.nodeKey, cmd.scale, logOps);
                    MarkTouchedScale(casterFormID, cmd.target, cmd.nodeKey);
                }
                else if (cmd.kind == FB::CommandKind::kMorph) {
                    FB::Morph::AddDelta(actorHandle, cmd.morphName, cmd.delta, logOps);
                    MarkTouchedMorph(casterFormID, cmd.target);
                }
                }).detach();
        }
    }

    void FB::ActorManager::Update(float /*dtSeconds*/)
    {
        // Phase 4/5: intentionally empty (no behavior change yet)
    }

    void CancelAndReset(
        RE::ActorHandle caster,
        std::uint32_t casterFormID,
        bool logOps,
        bool resetMorphCaster,
        bool resetMorphTarget)
    {
        

        auto snap = TakeSnapshot(casterFormID);
        (void)BumpToken(casterFormID);
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
