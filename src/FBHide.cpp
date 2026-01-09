#include "FBHide.h"

#include <spdlog/spdlog.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    struct ShapeKey
    {
        std::string shapeName;
        std::string parentName;  // optional / best-effort

        bool operator==(const ShapeKey& o) const noexcept
        {
            return shapeName == o.shapeName && parentName == o.parentName;
        }
    };

    struct ShapeKeyHash
    {
        std::size_t operator()(const ShapeKey& k) const noexcept
        {
            std::size_t h1 = std::hash<std::string>{}(k.shapeName);
            std::size_t h2 = std::hash<std::string>{}(k.parentName);
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ull + (h1 << 6) + (h1 >> 2));
        }
    };

    struct ActorHideState
    {
        // baselineHidden = whether NiAVObject::Flag::kHidden was set when first touched
        std::unordered_map<ShapeKey, bool, ShapeKeyHash> baselineHidden;
    };

    std::mutex g_mutex;
    std::unordered_map<std::uint32_t, ActorHideState> g_state;  // keyed by actor FormID

    struct ShapeVisit
    {
        RE::NiAVObject* obj{ nullptr };  // ephemeral, only valid during pass
        ShapeKey key;
    };

    static bool IsRenderableGeometry(RE::NiAVObject* obj)
    {
        if (!obj) {
            return false;
        }

        // Render geometry only:
        // - BSTriShape is common
        // - NiGeometry covers more
        if (obj->As<RE::BSTriShape>()) {
            return true;
        }
        if (obj->As<RE::NiGeometry>()) {
            return true;
        }

        return false;
    }

    static void TraverseNode(RE::NiAVObject* obj, std::vector<ShapeVisit>& out)
    {
        if (!obj) {
            return;
        }

        if (IsRenderableGeometry(obj)) {
            ShapeVisit v;
            v.obj = obj;
            v.key.shapeName = obj->name.c_str();

            if (obj->parent && obj->parent->name.c_str()) {
                v.key.parentName = obj->parent->name.c_str();
            }

            out.push_back(std::move(v));
            return;
        }

        if (auto node = obj->As<RE::NiNode>()) {
            const auto& children = node->children;
            for (const auto& ch : children) {
                if (ch) {
                    TraverseNode(ch.get(), out);
                }
            }
        }
    }

    static std::vector<ShapeVisit> CollectShapes(RE::Actor* a)
    {
        std::vector<ShapeVisit> shapes;
        if (!a) {
            return shapes;
        }

        auto* root = a->Get3D();
        if (!root) {
            return shapes;
        }

        TraverseNode(root, shapes);
        return shapes;
    }

    static bool GetHiddenFlag(RE::NiAVObject* obj)
    {
        auto& flags = obj->GetFlags();
        return flags.any(RE::NiAVObject::Flag::kHidden);
    }

    static void SetHiddenFlag(RE::NiAVObject* obj, bool hidden)
    {
        auto& flags = obj->GetFlags();
        if (hidden) {
            flags.set(RE::NiAVObject::Flag::kHidden);
        }
        else {
            flags.reset(RE::NiAVObject::Flag::kHidden);
        }
    }
}

namespace FB::Hide
{
    void ApplyHide(RE::ActorHandle actor, bool hide, bool logOps)
    {
        if (!actor) {
            return;
        }

        auto a = actor.get();
        if (!a) {
            return;
        }

        const auto actorFormID = a->GetFormID();

        // Collect shapes (ephemeral pointers only)
        auto shapes = CollectShapes(a.get());
        if (shapes.empty()) {
            if (logOps) {
                spdlog::info("[FBHide] ApplyHide: actor='{}' has no 3D/shapes (noop)", a->GetName());
            }
            return;
        }

        std::size_t touched = 0;

        {
            std::lock_guard _{ g_mutex };
            auto& st = g_state[actorFormID];

            for (auto& s : shapes) {
                if (!s.obj) {
                    continue;
                }

                // Capture baseline once per shape key
                if (!st.baselineHidden.contains(s.key)) {
                    st.baselineHidden.emplace(s.key, GetHiddenFlag(s.obj));
                }

                if (hide) {
                    SetHiddenFlag(s.obj, true);
                }
                else {
                    // best-effort restore baseline (do not clear state)
                    const bool baseline = st.baselineHidden[s.key];
                    SetHiddenFlag(s.obj, baseline);
                }

                ++touched;
            }
        }

        if (logOps) {
            spdlog::info("[FBHide] ApplyHide: actor='{}' hide={} shapes={}",
                a->GetName(), hide, touched);
        }
    }

    void ResetActor(RE::ActorHandle actor, bool logOps)
    {
        std::uint32_t actorFormID = 0;
        std::string actorName = "<null>";

        RE::Actor* aPtr = nullptr;
        if (actor) {
            auto a = actor.get();
            if (a) {
                aPtr = a.get();
                actorFormID = aPtr->GetFormID();
                actorName = aPtr->GetName();
            }
        }

        if (actorFormID == 0) {
            return;
        }

        // Snapshot baseline keys/state, then remove from map so we always clear state
        ActorHideState snapshot;
        {
            std::lock_guard _{ g_mutex };
            auto it = g_state.find(actorFormID);
            if (it == g_state.end()) {
                return;
            }
            snapshot = std::move(it->second);
            g_state.erase(it);
        }

        if (!aPtr) {
            // Can't traverse; state already cleared
            if (logOps) {
                spdlog::info("[FBHide] ResetActor: actor=<missing> (state cleared, restore skipped)");
            }
            return;
        }

        auto shapes = CollectShapes(aPtr);
        if (shapes.empty()) {
            // 3D missing; state already cleared, rely on default visible on reload
            if (logOps) {
                spdlog::info("[FBHide] ResetActor: actor='{}' has no 3D (state cleared, restore skipped)", actorName);
            }
            return;
        }

        std::size_t restored = 0;

        // Best-effort restore: match by (shape name, parent name)
        for (auto& s : shapes) {
            if (!s.obj) {
                continue;
            }

            auto it = snapshot.baselineHidden.find(s.key);
            if (it == snapshot.baselineHidden.end()) {
                continue;
            }

            SetHiddenFlag(s.obj, it->second);
            ++restored;
        }

        if (logOps) {
            spdlog::info("[FBHide] ResetActor: actor='{}' restoredShapes={}", actorName, restored);
        }
    }

#ifndef NDEBUG
    void ResetAll(bool logOps)
    {
        std::lock_guard _{ g_mutex };
        g_state.clear();
        if (logOps) {
            spdlog::info("[FBHide] ResetAll: cleared all hide state");
        }
    }
#endif
}
