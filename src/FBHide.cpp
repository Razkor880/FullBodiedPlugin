#include "FBHide.h"

#include <RE/B/BSGeometry.h>
#include <RE/B/BSDismemberSkinInstance.h>
#include <RE/N/NiNode.h>
#include <RE/N/NiRTTI.h>

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>

namespace FB::Hide
{
    namespace
    {
        struct ActorHideState
        {
            // Baseline: whether object was hidden before we touched it
            std::unordered_map<const RE::NiAVObject*, bool> baselineHiddenByObj;
            // Touched list for reset iteration
            std::vector<const RE::NiAVObject*> touched;

            // Slots we toggled via dismember (best-effort restore)
            std::unordered_set<std::uint16_t> touchedSlots;

            // Avoid log spam when an actor has no dismember partitions
            std::unordered_set<std::uint16_t> loggedNoDismemberSlots;

            void Clear()
            {
                baselineHiddenByObj.clear();
                touched.clear();
                touchedSlots.clear();
                loggedNoDismemberSlots.clear();
            }
        };

        std::mutex g_mutex;
        std::unordered_map<std::uint32_t, ActorHideState> g_stateByActorID;

        static std::uint32_t GetActorID(const RE::ActorHandle& a_handle)
        {
            const auto ap = a_handle.get();
            auto* actor = ap.get();
            return actor ? actor->GetFormID() : 0;
        }

        static RE::NiAVObject* GetRoot3D(const RE::ActorHandle& a_handle)
        {
            const auto ap = a_handle.get();
            auto* actor = ap.get();
            return actor ? actor->Get3D() : nullptr;
        }

        static bool IsRenderableGeometry(RE::NiAVObject* a_obj)
        {
            if (!a_obj) {
                return false;
            }
            // BSGeometry covers most skinned renderables; BSTriShape is also common
            if (netimmerse_cast<RE::BSGeometry*>(a_obj)) {
                return true;
            }
            if (netimmerse_cast<RE::BSTriShape*>(a_obj)) {
                return true;
            }
            return false;
        }

        static void TraverseNode(RE::NiAVObject* a_obj, std::vector<RE::NiAVObject*>& a_out)
        {
            if (!a_obj) {
                return;
            }

            if (IsRenderableGeometry(a_obj)) {
                a_out.push_back(a_obj);
            }

            if (auto* node = netimmerse_cast<RE::NiNode*>(a_obj)) {
                auto& kids = node->GetChildren();
                const auto n = kids.size();
                for (std::size_t i = 0; i < n; ++i) {
                    auto& child = kids[i];
                    if (child) {
                        TraverseNode(child.get(), a_out);
                    }
                }
            }
        }

        static void TraverseGeometry(RE::NiAVObject* a_obj, std::vector<RE::BSGeometry*>& a_out)
        {
            if (!a_obj) {
                return;
            }

            if (auto* geo = netimmerse_cast<RE::BSGeometry*>(a_obj)) {
                a_out.push_back(geo);
            }
            else if (auto* tri = netimmerse_cast<RE::BSTriShape*>(a_obj)) {
                // In many builds BSTriShape is also a BSGeometry; if not, this keeps us safe.
                if (auto* asGeo = netimmerse_cast<RE::BSGeometry*>(tri)) {
                    a_out.push_back(asGeo);
                }
            }

            if (auto* node = netimmerse_cast<RE::NiNode*>(a_obj)) {
                auto& kids = node->GetChildren();
                const auto n = kids.size();
                for (std::size_t i = 0; i < n; ++i) {
                    auto& child = kids[i];
                    if (child) {
                        TraverseGeometry(child.get(), a_out);
                    }
                }
            }
        }

        static void SetHiddenFlag(RE::NiAVObject* a_obj, bool a_hidden)
        {
            if (!a_obj) {
                return;
            }

            auto& flags = a_obj->GetFlags();
            if (a_hidden) {
                flags.set(RE::NiAVObject::Flag::kHidden);
            }
            else {
                flags.reset(RE::NiAVObject::Flag::kHidden);
            }
        }

        static bool GetHiddenFlag(RE::NiAVObject* a_obj)
        {
            return a_obj ? a_obj->GetFlags().all(RE::NiAVObject::Flag::kHidden) : false;
        }
    }

    void ApplyHide(RE::ActorHandle a_actor, bool a_hide, bool logOps)
    {
        const auto actorID = GetActorID(a_actor);
        if (actorID == 0) {
            return;
        }

        auto* root = GetRoot3D(a_actor);
        if (!root) {
            return;
        }

        std::vector<RE::NiAVObject*> geoms;
        geoms.reserve(256);
        TraverseNode(root, geoms);

        std::scoped_lock lk(g_mutex);
        auto& state = g_stateByActorID[actorID];

        for (auto* obj : geoms) {
            if (!obj) {
                continue;
            }

            if (!state.baselineHiddenByObj.contains(obj)) {
                const bool baseline = GetHiddenFlag(obj);
                state.baselineHiddenByObj.emplace(obj, baseline);
                state.touched.push_back(obj);
            }

            if (a_hide) {
                SetHiddenFlag(obj, true);
            }
            else {
                const bool baseline = state.baselineHiddenByObj[obj];
                SetHiddenFlag(obj, baseline);
            }
        }

        if (logOps) {
            spdlog::info("[FBHide] ApplyHide: actor {:08X} hide={} touchedNow={}", actorID, a_hide, geoms.size());
        }
    }

    void ApplyHideSlot(RE::ActorHandle a_actor, std::uint16_t slotNumber, bool hide, bool logOps)
    {
        if (logOps) {
            const auto actorID = GetActorID(a_actor);
            spdlog::info("[FBHide] ApplyHideSlot: actor {:08X} slot={} hide={} (stub/no-op)", actorID, slotNumber, hide);
        }
    }


    void ResetActor(RE::ActorHandle a_actor, bool logOps)
    {
        const auto actorID = GetActorID(a_actor);
        if (actorID == 0) {
            return;
        }

        auto* root = GetRoot3D(a_actor);

        std::scoped_lock lk(g_mutex);
        auto it = g_stateByActorID.find(actorID);
        if (it == g_stateByActorID.end()) {
            return;
        }

        auto& state = it->second;

        if (root) {
            // Restore kHidden baselines
            for (auto* obj : state.touched) {
                auto jt = state.baselineHiddenByObj.find(obj);
                if (jt != state.baselineHiddenByObj.end()) {
                    SetHiddenFlag(const_cast<RE::NiAVObject*>(obj), jt->second);
                }
            }

            // Best-effort restore of dismember slots: enable them again
            if (!state.touchedSlots.empty()) {
                std::vector<RE::BSGeometry*> geoms;
                geoms.reserve(256);
                TraverseGeometry(root, geoms);

                for (auto* geo : geoms) {
                    if (!geo) {
                        continue;
                    }

                    auto* skin = geo->GetGeometryRuntimeData().skinInstance.get();
                    if (!skin) {
                        continue;
                    }

                    auto* dismember = netimmerse_cast<RE::BSDismemberSkinInstance*>(skin);
                    if (!dismember) {
                        continue;
                    }

                    for (auto slot : state.touchedSlots) {
                        dismember->UpdateDismemberPartion(slot, true);
                    }
                }
            }
        }
        else {
            if (logOps) {
                spdlog::info("[FBHide] ResetActor: actor {:08X} missing 3D; restore skipped, state cleared", actorID);
            }
        }

        state.Clear();
        g_stateByActorID.erase(it);

        if (logOps) {
            spdlog::info("[FBHide] ResetActor: actor {:08X} done", actorID);
        }
    }

#ifndef NDEBUG
    void ResetAll(bool logOps)
    {
        std::scoped_lock lk(g_mutex);
        g_stateByActorID.clear();

        if (logOps) {
            spdlog::info("[FBHide] ResetAll: cleared all hide state");
        }
    }
#endif
}
