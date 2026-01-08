#include "FBMorph.h"

#include "RE/F/FunctionArguments.h"
#include "RE/S/SkyrimVM.h"
#include "SKSE/SKSE.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace
{
    using Clock = std::chrono::steady_clock;

    std::mutex g_mutex;

    // Accumulated values by actorFormID -> (morphKey -> value)
    // morphKey here is the logical key coming from FBConfig (e.g. "PregnancyBelly").
    std::unordered_map<std::uint32_t, std::unordered_map<std::string, float>> g_values;

    struct StickyEntry
    {
        std::string logicalKey;     // e.g. "PregnancyBelly"
        std::string rmMorphName;    // actual RaceMenu morph name we call into (usually same as logicalKey)

        // Current displayed value (what we last sent via FBSetMorph)
        float value{ 0.0f };

        // Tween state: we smoothly animate from fromValue -> toValue over [startTime, endTime]
        float fromValue{ 0.0f };
        float toValue{ 0.0f };
        Clock::time_point startTime{};
        Clock::time_point endTime{};
        bool tweenActive{ false };

        // How often the worker re-applies the current value
        float intervalSeconds{ 0.05f };  // 20 Hz

        // Keep reapplying until this time (extended each AddDelta)
        Clock::time_point holdUntil{};

        std::atomic_bool running{ false };
    };

    // actorFormID -> (logicalKey -> sticky entry)
    std::unordered_map<std::uint32_t, std::unordered_map<std::string, std::shared_ptr<StickyEntry>>> g_sticky;

    static float Clamp(float v)
    {
        return std::clamp(v, FB::Morph::kMinValue, FB::Morph::kMaxValue);
    }

    // Map *logical* key from FBConfig -> actual RaceMenu morph name.
    // In the current design we just pass it through:
    //   FBMorph_PregnancyBelly(...) -> "PregnancyBelly"
    //   FBMorph_BreastsNewSH(...)   -> "BreastsNewSH"
    //
    // If you ever want aliases or nicer INI names, this is the place to map them.
    static const char* ResolveRaceMenuMorphName(std::string_view key)
    {
        // Example of explicit alias, if desired:
        // if (key == FB::Morph::kMorph_VorePreyBelly) {
        //     return "Vore Prey Belly";
        // }

        // Default: treat INI key as the actual morph name
        // (must match RaceMenu slider name exactly)
        return key.data();
    }

    static RE::BSScript::IVirtualMachine* GetVM()
    {
        auto* skyrimVM = RE::SkyrimVM::GetSingleton();
        return skyrimVM ? skyrimVM->impl.get() : nullptr;
    }

    //
    // Bridge helpers – call into FBMorphBridge.psc, which talks to NiOverride.
    //
    // Papyrus side:
    //   Function FBSetMorph(Actor akActor, String morphName, Float value) Global
    //   Function FBClearMorphs(Actor akActor) Global
    //

    static void Papyrus_FBSetMorph(RE::Actor* actor, const char* morphName, float value, bool logOps)
    {
        if (!actor || !morphName) {
            return;
        }

        auto* vm = GetVM();
        if (!vm) {
            if (logOps) {
                spdlog::warn("[FB] Morph: SkyrimVM/IVirtualMachine not available");
            }
            return;
        }

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result{};

        // FBMorphBridge.FBSetMorph(Actor akActor, String morphName, Float value)
        auto* args = RE::MakeFunctionArguments(
            static_cast<RE::Actor*>(actor),
            RE::BSFixedString(morphName),
            static_cast<float>(value));

        const bool ok = vm->DispatchStaticCall(
            RE::BSFixedString("FBMorphBridge"),
            RE::BSFixedString("FBSetMorph"),
            args,
            result);

        if (logOps) {
            spdlog::info("[FB] MorphBridgeCall: FBSetMorph={} morph='{}' value={}", ok, morphName, value);
        }
    }

    static void Papyrus_FBClearMorphs(RE::Actor* actor, bool logOps)
    {
        if (!actor) {
            return;
        }

        auto* vm = GetVM();
        if (!vm) {
            if (logOps) {
                spdlog::warn("[FB] Morph: SkyrimVM/IVirtualMachine not available");
            }
            return;
        }

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result{};

        // FBMorphBridge.FBClearMorphs(Actor akActor)
        auto* args = RE::MakeFunctionArguments(
            static_cast<RE::Actor*>(actor));

        const bool ok = vm->DispatchStaticCall(
            RE::BSFixedString("FBMorphBridge"),
            RE::BSFixedString("FBClearMorphs"),
            args,
            result);

        if (logOps) {
            spdlog::info("[FB] MorphBridgeCall: FBClearMorphs={} key='{}'", ok, FB::Morph::kMorphKey);
        }
    }

    //
    // Sticky worker – keeps reapplying the current morph value AND drives the tween.
    //

    static float EaseInOutQuad(float t)
    {
        // Classic ease-in-out quadratic
        if (t < 0.5f) {
            return 2.0f * t * t;
        }
        float u = -2.0f * t + 2.0f;
        return 1.0f - (u * u) / 2.0f;
    }

    static void EnsureStickyWorker(
        RE::ActorHandle actor,
        std::uint32_t formID,
        const std::shared_ptr<StickyEntry>& entry,
        bool logOps)
    {
        // Start worker once.
        bool expected = false;
        if (!entry->running.compare_exchange_strong(expected, true)) {
            return;
        }

        std::thread([actor, formID, entry, logOps]() {
            while (true) {
                // Sleep for the configured interval (20 Hz, min 10ms)
                const auto sleepMs =
                    static_cast<int>(std::max(10.0f, entry->intervalSeconds * 1000.0f));
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));

                float valueToApply = 0.0f;
                std::string morphNameCopy;

                {
                    std::lock_guard _{ g_mutex };

                    const auto now = Clock::now();

                    // Stop if the "hold" window expired
                    if (now > entry->holdUntil) {
                        break;
                    }

                    morphNameCopy = entry->rmMorphName;

                    if (entry->tweenActive) {
                        if (now >= entry->endTime) {
                            // Tween finished – snap to final
                            entry->value = entry->toValue;
                            entry->tweenActive = false;
                        }
                        else {
                            const auto total = entry->endTime - entry->startTime;
                            const auto elapsed = now - entry->startTime;

                            const float totalMs = static_cast<float>(
                                std::chrono::duration_cast<std::chrono::milliseconds>(total).count());
                            const float elapsedMs = static_cast<float>(
                                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

                            float t = (totalMs > 0.0f) ? (elapsedMs / totalMs) : 1.0f;
                            t = std::clamp(t, 0.0f, 1.0f);

                            const float eased = EaseInOutQuad(t);
                            const float v = entry->fromValue +
                                (entry->toValue - entry->fromValue) * eased;

                            entry->value = v;
                        }
                    }

                    valueToApply = entry->value;
                }

                // Re-apply current value on the game thread
                auto* task = SKSE::GetTaskInterface();
                if (!task) {
                    continue;
                }

                task->AddTask([actor, morphNameCopy, valueToApply]() {
                    auto aa = actor.get();
                    if (!aa) {
                        return;
                    }
                    // No logging here to avoid spam – this is just keeping the value alive / tweened.
                    Papyrus_FBSetMorph(aa.get(), morphNameCopy.c_str(), valueToApply, false);
                    });
            }

            // Mark not running and clean up entry if it’s still current
            entry->running.store(false);

            {
                std::lock_guard _{ g_mutex };
                auto itA = g_sticky.find(formID);
                if (itA != g_sticky.end()) {
                    auto itK = itA->second.find(entry->logicalKey);
                    if (itK != itA->second.end() && itK->second.get() == entry.get()) {
                        itA->second.erase(itK);
                        if (itA->second.empty()) {
                            g_sticky.erase(itA);
                        }
                    }
                }
            }

            if (logOps) {
                spdlog::info(
                    "[FB] Morph: Sticky end actorFormID={} morph='{}'",
                    formID,
                    entry->logicalKey);
            }
            }).detach();
    }
}  // namespace

namespace FB::Morph
{
    void FB::Morph::AddDelta(RE::ActorHandle actor, std::string_view morphKey, float delta, bool logOps)
    {
        auto a = actor.get();
        if (!a) {
            return;
        }

        const char* morphName = ResolveRaceMenuMorphName(morphKey);
        if (!morphName || morphName[0] == '\0') {
            if (logOps) {
                spdlog::warn("[FB] Morph: unknown MorphKey '{}'", std::string(morphKey));
            }
            return;
        }

        float newValue = 0.0f;
        const std::uint32_t formID = a->GetFormID();
        const std::string keyStr = std::string(morphKey);

        {
            std::lock_guard _{ g_mutex };

            const auto now = Clock::now();

            // Canonical logical values for this actor+morph
            auto& slot = g_values[formID][keyStr];
            const float prevValue = slot;         // value BEFORE this delta
            slot = Clamp(slot + delta);           // value AFTER this delta
            newValue = slot;

            // Get/create sticky tween entry
            auto& byKey = g_sticky[formID];
            auto& entry = byKey[keyStr];
            if (!entry) {
                entry = std::make_shared<StickyEntry>();
                entry->logicalKey = keyStr;
                entry->rmMorphName = morphName;
                entry->intervalSeconds = 0.05f;   // 20 Hz
                entry->value = prevValue;         // start from previous logical value, not 0
            }

            // Start (or restart) tween from prevValue -> newValue
            entry->fromValue = prevValue;
            entry->toValue = newValue;
            entry->startTime = now;
            entry->endTime = now + std::chrono::milliseconds(400);  // 0.4s tween
            entry->tweenActive = true;

            // Extend hold window so tween + a bit of idle time are covered
            const auto minHold = entry->endTime + std::chrono::milliseconds(850);
            if (entry->holdUntil < minHold) {
                entry->holdUntil = minHold;
            }
        }

        if (logOps) {
            spdlog::info(
                "[FB] Morph: AddDelta actor='{}' morph='{}' delta={} -> value={}",
                a->GetName(),
                morphName,
                delta,
                newValue);
        }

        // Ensure the sticky worker is running for this actor+morph
        std::shared_ptr<StickyEntry> entryCopy;
        {
            std::lock_guard _{ g_mutex };
            auto itA = g_sticky.find(formID);
            if (itA != g_sticky.end()) {
                auto itK = itA->second.find(keyStr);
                if (itK != itA->second.end()) {
                    entryCopy = itK->second;
                }
            }
        }
        if (entryCopy) {
            EnsureStickyWorker(actor, formID, entryCopy, logOps);
        }
    }


    void ResetAllForActor(RE::ActorHandle actor, bool logOps)
    {
        auto a = actor.get();
        if (!a) {
            return;
        }

        const std::uint32_t formID = a->GetFormID();

        {
            std::lock_guard _{ g_mutex };
            g_values.erase(formID);
            g_sticky.erase(formID);  // workers will naturally exit once holdUntil passes
        }

        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([actor, logOps]() {
                auto aa = actor.get();
                if (!aa) {
                    return;
                }
                Papyrus_FBClearMorphs(aa.get(), logOps);
                });
        }

        if (logOps) {
            spdlog::info(
                "[FB] Morph: ResetAllForActor actor='{}' key='{}'",
                a->GetName(),
                FB::Morph::kMorphKey);
        }
    }
}
