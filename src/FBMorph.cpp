#include "FBMorph.h"

#include "RE/F/FunctionArguments.h"
#include "RE/I/IObjectHandlePolicy.h"
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
    static std::string ResolveRaceMenuMorphName(std::string_view key)
    {
        // Default: the key is already the intended RaceMenu morph name.
        // Alias mapping belongs in FBConfig.cpp (ResolveMorphAlias).
        return std::string(key);
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

    static void Papyrus_ActorSetExpressionPhoneme(RE::Actor* actor, int phonemeIndex, float value01, bool logOps)
    {
        if (!actor) {
            return;
        }

        auto* vm = GetVM();
        if (!vm) {
            if (logOps) {
                spdlog::warn("[FB] Morph: SkyrimVM/IVirtualMachine not available (phoneme)");
            }
            return;
        }

        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy) {
            if (logOps) {
                spdlog::warn("[FB] Morph: ObjectHandlePolicy not available (phoneme)");
            }
            return;
        }

        const auto handle = policy->GetHandleForObject(actor->GetFormType(), actor);
        if (!handle || handle == policy->EmptyHandle()) {
            if (logOps) {
                spdlog::warn("[FB] Morph: invalid VM handle for actor (phoneme)");
            }
            return;
        }

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result{};

        // Actor.SetExpressionPhoneme(int index, float value)
        auto* args = RE::MakeFunctionArguments(
            static_cast<std::int32_t>(phonemeIndex),
            static_cast<float>(value01));

        const bool ok = vm->DispatchMethodCall(
            handle,
            RE::BSFixedString("Actor"),
            RE::BSFixedString("SetExpressionPhoneme"),
            args,
            result);

        if (logOps) {
            spdlog::info("[FB] ActorCall: SetExpressionPhoneme={} idx={} value={}", ok, phonemeIndex, value01);
        }
    }

    static void Papyrus_ActorSetExpressionOverride(RE::Actor* actor, int moodId, int strength, bool logOps)
    {
        if (!actor) {
            return;
        }

        auto* vm = GetVM();
        if (!vm) {
            if (logOps) spdlog::warn("[FB] Morph: SkyrimVM not available (expression)");
            return;
        }

        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy) {
            if (logOps) spdlog::warn("[FB] Morph: ObjectHandlePolicy not available (expression)");
            return;
        }

        const auto handle = policy->GetHandleForObject(actor->GetFormType(), actor);
        if (!handle || handle == policy->EmptyHandle()) {
            if (logOps) spdlog::warn("[FB] Morph: invalid VM handle for actor (expression)");
            return;
        }

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result{};
        auto* args = RE::MakeFunctionArguments(
            static_cast<std::int32_t>(moodId),
            static_cast<std::int32_t>(strength));

        const bool ok = vm->DispatchMethodCall(
            handle,
            RE::BSFixedString("Actor"),
            RE::BSFixedString("SetExpressionOverride"),
            args,
            result);

        if (logOps) {
            spdlog::info("[FB] ActorCall: SetExpressionOverride={} mood={} strength={}", ok, moodId, strength);
        }
    }

    static void Papyrus_ActorSetExpressionModifier(RE::Actor* actor, int modifierIndex, float value, bool logOps)
    {
        if (!actor) return;

        auto* vm = GetVM();
        if (!vm) return;

        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy) return;

        const auto handle = policy->GetHandleForObject(actor->GetFormType(), actor);
        if (!handle || handle == policy->EmptyHandle()) return;

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result{};

        auto* args = RE::MakeFunctionArguments(
            static_cast<std::int32_t>(modifierIndex),
            static_cast<float>(value));

        vm->DispatchMethodCall(
            handle,
            RE::BSFixedString("Actor"),
            RE::BSFixedString("SetExpressionModifier"),
            args,
            result);
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

    static std::optional<std::int32_t> TryGetPhonemeIndex(std::string_view name)
    {
        using sv = std::string_view;
        // Exact match only (case-sensitive)
        if (name == sv{ "Aah" })    return 0;
        if (name == sv{ "BigAah" }) return 1;
        if (name == sv{ "BMP" })    return 2;
        if (name == sv{ "ChJSh" })  return 3;
        if (name == sv{ "DST" })    return 4;
        if (name == sv{ "Eee" })    return 5;
        if (name == sv{ "Eh" })     return 6;
        if (name == sv{ "FV" })     return 7;
        if (name == sv{ "I" })      return 8;
        if (name == sv{ "K" })      return 9;
        if (name == sv{ "N" })      return 10;
        if (name == sv{ "Oh" })     return 11;
        if (name == sv{ "OohQ" })   return 12;
        if (name == sv{ "R" })      return 13;
        if (name == sv{ "Th" })     return 14;
        if (name == sv{ "W" })      return 15;
        return std::nullopt;
    }

    static float Normalize01(float v)
    {
        // Allow authoring either 0..1 or 0..100
        if (v > 1.0f) {
            v /= 100.0f;
        }
        return std::clamp(v, 0.0f, 1.0f);
    }



    static std::optional<std::int32_t> TryGetMoodId(std::string_view name)
    {
        using sv = std::string_view;
        if (name == sv{ "Neutral" })   return 7;
        if (name == sv{ "Anger" })     return 8;
        if (name == sv{ "Fear" })      return 9;
        if (name == sv{ "Happy" })     return 10;
        if (name == sv{ "Sad" })       return 11;
        if (name == sv{ "Surprise" })  return 12;
        if (name == sv{ "Puzzled" })   return 13;
        if (name == sv{ "Disgusted" }) return 14;
        return std::nullopt;
    }

    static std::int32_t NormalizeStrength100(float v)
    {
        // Allow 0..1 or 0..100
        if (v <= 1.0f) {
            v *= 100.0f;
        }
        auto i = static_cast<std::int32_t>(std::lround(v));
        return std::clamp(i, 0, 100);
    }

    static std::optional<std::int32_t> TryGetModifierIndex(std::string_view name)
    {
        using sv = std::string_view;

        // Eyes / look
        if (name == sv{ "BlinkL" } || name == sv{ "BlinkLeft" })   return 0;
        if (name == sv{ "BlinkR" } || name == sv{ "BlinkRight" })  return 1;
        if (name == sv{ "LookDown" })  return 8;
        if (name == sv{ "LookLeft" })  return 9;
        if (name == sv{ "LookRight" }) return 10;
        if (name == sv{ "LookUp" })    return 11;

        if (name == sv{ "SquintL" } || name == sv{ "SquintLeft" })  return 12;
        if (name == sv{ "SquintR" } || name == sv{ "SquintRight" }) return 13;

        // Brows
        if (name == sv{ "BrowDownL" } || name == sv{ "BrowDownLeft" })   return 2;
        if (name == sv{ "BrowDownR" } || name == sv{ "BrowDownRight" })  return 3;
        if (name == sv{ "BrowInL" } || name == sv{ "BrowInLeft" })     return 4;
        if (name == sv{ "BrowInR" } || name == sv{ "BrowInRight" })    return 5;
        if (name == sv{ "BrowUpL" } || name == sv{ "BrowUpLeft" })     return 6;
        if (name == sv{ "BrowUpR" } || name == sv{ "BrowUpRight" })    return 7;

        // Optional head modifiers (if you want them later)
        // if (name == sv{"HeadPitch"}) return 14;
        // if (name == sv{"HeadRoll"})  return 15;
        // if (name == sv{"HeadYaw"})   return 16;

        return std::nullopt;
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

                const float valueCopy = valueToApply;          // or whatever your float is called

                task->AddTask([actor, morphNameCopy, valueCopy]() {
                    auto aa = actor.get();
                    if (!aa) {
                        return;
                    }

                    // 1) Phonemes
                    if (auto idx = TryGetPhonemeIndex(morphNameCopy); idx) {
                        Papyrus_ActorSetExpressionPhoneme(aa.get(), *idx, Normalize01(valueCopy), false);
                        return;
                    }

                    // 2) Eye/Brow/Look modifiers (blink/look up/down/etc)
                    if (auto midx = TryGetModifierIndex(morphNameCopy); midx) {
                        // Convention: treat like 0..100 unless authored as 0..1
                        float v = valueCopy;
                        if (v > 1.0f) v /= 100.0f;
                        v = std::clamp(v, 0.0f, 1.0f);

                        Papyrus_ActorSetExpressionModifier(aa.get(), *midx, v, false);
                        return;
                    }

                    // 3) Mood expressions (Happy/Sad/etc)
                    if (auto mood = TryGetMoodId(morphNameCopy); mood) {
                        Papyrus_ActorSetExpressionOverride(aa.get(), *mood, NormalizeStrength100(valueCopy), false);
                        return;
                    }

                    // 4) Default: keep the original working bridge behavior
                    Papyrus_FBSetMorph(aa.get(), morphNameCopy.c_str(), valueCopy, false);
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
    void AddDelta(RE::ActorHandle actor, std::string_view morphKey, float delta, bool logOps)
    {
        auto a = actor.get();
        if (!a) {
            return;
        }

        const std::string morphName = ResolveRaceMenuMorphName(morphKey);
        if (morphName.empty()) {
            if (logOps) {
                spdlog::warn("[FB] Morph: failed to resolve morph key '{}'", std::string(morphKey));
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
                
                entry->intervalSeconds = 0.05f;   // 20 Hz
                entry->value = prevValue;         // start from previous logical value, not 0
            }
            entry->rmMorphName = morphName;
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
