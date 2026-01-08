#include "FBMorph.h"

#include "RE/F/FunctionArguments.h"
#include "RE/S/SkyrimVM.h"
#include "SKSE/SKSE.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace
{
	using Clock = std::chrono::steady_clock;

	std::mutex g_mutex;

	// Accumulated values by actorFormID -> (morphName -> value)
	// morphName here is the actual morph name passed to the bridge/Papyrus.
	std::unordered_map<std::uint32_t, std::unordered_map<std::string, float>> g_values;

	struct StickyEntry
	{
		std::string morphName;         // actual RM/NiOverride morph name
		float value{ 0.0f };

		float intervalSeconds{ 0.10f }; // default 10 Hz
		Clock::time_point holdUntil{};  // keep reapplying until this time

		std::atomic_bool running{ false };
	};

	// actorFormID -> (morphName -> sticky entry)
	std::unordered_map<std::uint32_t, std::unordered_map<std::string, std::shared_ptr<StickyEntry>>> g_sticky;

	static float ClampValue(float v)
	{
		return std::clamp(v, FB::Morph::kMinValue, FB::Morph::kMaxValue);
	}

	// Compatibility: if caller passes the old canonical key, map it to the RM morph name.
	// Otherwise treat the incoming string as already being the RM morph name.
	static std::string ResolveToRaceMenuMorphName(std::string_view keyOrName)
	{
		if (keyOrName == FB::Morph::kMorph_VorePreyBelly) {
			return "Vore Prey Belly";
		}
		return std::string(keyOrName);
	}

	static RE::BSScript::IVirtualMachine* GetVM()
	{
		auto* skyrimVM = RE::SkyrimVM::GetSingleton();
		return skyrimVM ? skyrimVM->impl.get() : nullptr;
	}

	//
	// Papyrus bridge helpers
	// FBMorphBridge.psc:
	//
	// Scriptname FBMorphBridge Hidden
	// String Property FBKeyName = "FullBodiedPlugin" Auto
	//
	// Function FBSetMorph(Actor akActor, String morphName, float value) global
	//   NiOverride.SetBodyMorph(akActor, morphName, FBKeyName, value)
	//   NiOverride.UpdateModelWeight(akActor)
	// EndFunction
	//
	// Function FBClearMorphs(Actor akActor) global
	//   NiOverride.ClearBodyMorphKeys(akActor, FBKeyName)
	//   NiOverride.UpdateModelWeight(akActor)
	// EndFunction
	//

	static void Papyrus_FBSetMorph(RE::Actor* actor, const char* morphName, float value, bool logOps)
	{
		if (!actor || !morphName || morphName[0] == '\0') {
			return;
		}

		auto* vm = GetVM();
		if (!vm) {
			if (logOps) {
				spdlog::warn("[FB] MorphBridge: SkyrimVM/IVirtualMachine not available");
			}
			return;
		}

		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result{};

		// FBMorphBridge.FBSetMorph(Actor akActor, string morphName, float value)
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
			spdlog::info(
				"[FB] MorphBridgeCall: FBSetMorph={} morph='{}' value={}",
				ok,
				morphName,
				value);
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
				spdlog::warn("[FB] MorphBridge: SkyrimVM/IVirtualMachine not available");
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
			spdlog::info("[FB] MorphBridgeCall: FBClearMorphs={}", ok);
		}
	}

	static void EnsureStickyWorker(
		RE::ActorHandle actor,
		std::uint32_t formID,
		const std::string& morphName,
		const std::shared_ptr<StickyEntry>& entry,
		bool logOps)
	{
		bool expected = false;
		if (!entry->running.compare_exchange_strong(expected, true)) {
			return;  // already running
		}

		std::thread([actor, formID, morphName, entry, logOps]() {
			for (;;) {
				const auto sleepMs = static_cast<int>(std::max(10.0f, entry->intervalSeconds * 1000.0f));
				std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));

				if (Clock::now() > entry->holdUntil) {
					break;
				}

				auto* task = SKSE::GetTaskInterface();
				if (!task) {
					continue;
				}

				const float v = entry->value;
				task->AddTask([actor, morphName, v]() {
					auto aa = actor.get();
					if (!aa) {
						return;
					}
					// Re-apply via Papyrus bridge (no spam logging)
					Papyrus_FBSetMorph(aa.get(), morphName.c_str(), v, false);
				});
			}

			entry->running.store(false);

			// Cleanup if still current
			{
				std::lock_guard _{ g_mutex };
				auto itA = g_sticky.find(formID);
				if (itA != g_sticky.end()) {
					auto itK = itA->second.find(morphName);
					if (itK != itA->second.end() && itK->second.get() == entry.get()) {
						itA->second.erase(itK);
						if (itA->second.empty()) {
							g_sticky.erase(itA);
						}
					}
				}
			}

			if (logOps) {
				spdlog::info("[FB] Morph: Sticky end actorFormID={} morph='{}'", formID, morphName);
			}
		}).detach();
	}
}

namespace FB::Morph
{
	void AddDelta(RE::ActorHandle actor, std::string_view morphKeyOrName, float delta, bool logOps)
	{
		auto a = actor.get();
		if (!a) {
			return;
		}

		const std::string morphName = ResolveToRaceMenuMorphName(morphKeyOrName);
		if (morphName.empty()) {
			if (logOps) {
				spdlog::warn("[FB] Morph: empty morph name");
			}
			return;
		}

		float newValue = 0.0f;
		const std::uint32_t formID = a->GetFormID();

		{
			std::lock_guard _{ g_mutex };
			auto& slot = g_values[formID][morphName];
			slot = ClampValue(slot + delta);
			newValue = slot;

			auto& byKey = g_sticky[formID];
			auto& entry = byKey[morphName];
			if (!entry) {
				entry = std::make_shared<StickyEntry>();
				entry->morphName = morphName;
				entry->intervalSeconds = 0.10f;  // 10 Hz
			}

			entry->value = newValue;
			entry->holdUntil = Clock::now() + std::chrono::milliseconds(1250);
		}

		if (logOps) {
			spdlog::info(
				"[FB] Morph: AddDelta actor='{}' morph='{}' delta={} -> value={}",
				a->GetName(),
				morphName,
				delta,
				newValue);
		}

		// One immediate apply on game thread via bridge
		if (auto* task = SKSE::GetTaskInterface()) {
			task->AddTask([actor, morphName, newValue, logOps]() {
				auto aa = actor.get();
				if (!aa) {
					return;
				}
				Papyrus_FBSetMorph(aa.get(), morphName.c_str(), newValue, logOps);
			});
		}

		// Ensure the sticky worker is running for this actor+morph
		std::shared_ptr<StickyEntry> entryCopy;
		{
			std::lock_guard _{ g_mutex };
			entryCopy = g_sticky[formID][morphName];
		}
		if (entryCopy) {
			EnsureStickyWorker(actor, formID, morphName, entryCopy, logOps);
		}
	}

	void ResetAllForActor(RE::ActorHandle actor, bool logOps)
	{
		auto a = actor.get();
		if (!a) {
			return;
		}

		const std::uint32_t formID = a->GetFormID();

		// Stop stickies ASAP
		{
			std::lock_guard _{ g_mutex };

			auto it = g_sticky.find(formID);
			if (it != g_sticky.end()) {
				for (auto& [_, entry] : it->second) {
					if (entry) {
						entry->holdUntil = Clock::now();
					}
				}
			}

			g_values.erase(formID);
			g_sticky.erase(formID);
		}

		// Clear all morphs for our key via bridge
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
