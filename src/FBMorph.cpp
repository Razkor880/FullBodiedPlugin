#include "FBMorph.h"

#include "RE/F/FunctionArguments.h"
#include "RE/S/SkyrimVM.h"

#include "SKSE/SKSE.h"

#include <spdlog/spdlog.h>


namespace
{
	std::mutex g_mutex;
	std::unordered_map<std::uint32_t, std::unordered_map<std::string, float>> g_values;

	static float Clamp(float v)
	{
		return std::clamp(v, FB::Morph::kMinValue, FB::Morph::kMaxValue);
	}

	static const char* ResolveRaceMenuMorphName(std::string_view key)
	{
		if (key == FB::Morph::kMorph_VorePreyBelly) {
			return "Vore Prey Belly";
		}
		return nullptr;
	}

	static RE::BSScript::IVirtualMachine* GetVM()
	{
		auto* skyrimVM = RE::SkyrimVM::GetSingleton();
		return skyrimVM ? skyrimVM->impl.get() : nullptr;
	}

	static void Papyrus_SetBodyMorph(RE::Actor* actor, const char* morphName, float value, bool logOps)
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

		// NiOverride.SetBodyMorph(Actor akActor, string morphName, string key, float value)
		auto* args = RE::MakeFunctionArguments(
			static_cast<RE::Actor*>(actor),
			RE::BSFixedString(morphName),
			RE::BSFixedString(FB::Morph::kMorphKey.data()),
			static_cast<float>(value));

		auto* args2 = RE::MakeFunctionArguments(static_cast<RE::Actor*>(actor));

		bool ok1 = vm->DispatchStaticCall(
			RE::BSFixedString("NiOverride"),
			RE::BSFixedString("SetBodyMorph"),
			args,
			result);

		bool ok2 = vm->DispatchStaticCall(
			RE::BSFixedString("NiOverride"),
			RE::BSFixedString("UpdateModelWeight"),
			args2,
			result);

		if (logOps) {
			spdlog::info("[FB] MorphCall: SetBodyMorph={} UpdateModelWeight={} morph='{}' value={}",
				ok1, ok2, morphName, value);
		}


		if (logOps) {
			spdlog::info("[FB] Morph: actor='{}' morph='{}' value={}", actor->GetName(), morphName, value);
		}
	}

	static void Papyrus_ClearBodyMorphKeys(RE::Actor* actor, bool logOps)
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

		// NiOverride.ClearBodyMorphKeys(Actor akActor, string key)
		auto* args = RE::MakeFunctionArguments(
			static_cast<RE::Actor*>(actor),
			RE::BSFixedString(FB::Morph::kMorphKey.data()));

		auto* args2 = RE::MakeFunctionArguments(static_cast<RE::Actor*>(actor));

		bool ok1 = vm->DispatchStaticCall(
			RE::BSFixedString("NiOverride"),
			RE::BSFixedString("SetBodyMorph"),
			args,
			result);

		bool ok2 = vm->DispatchStaticCall(
			RE::BSFixedString("NiOverride"),
			RE::BSFixedString("UpdateModelWeight"),
			args2,
			result);

		if (logOps) {
			spdlog::info("[FB] MorphCall: ClearBodyMorphKeys={} UpdateModelWeight={}", ok1, ok2);
		}


		if (logOps) {
			spdlog::info("[FB] Morph: actor='{}' cleared key='{}'", actor->GetName(), FB::Morph::kMorphKey);
		}
	}
}

namespace FB::Morph
{
	void AddDelta(RE::ActorHandle actor, std::string_view morphKey, float delta, bool logOps)
	{
		auto a = actor.get();
		if (!a) {
			return;
		}

		const char* morphName = ResolveRaceMenuMorphName(morphKey);
		if (!morphName) {
			if (logOps) {
				spdlog::warn("[FB] Morph: unknown MorphKey '{}'", std::string(morphKey));
			}
			return;
		}

		if (logOps) {
			spdlog::info("[FB] Morph: AddDelta actor='{}' morph='{}' delta={}",
				a->GetName(), morphName, delta);
		}

		float newValue = 0.0f;
		const std::uint32_t formID = a->GetFormID();

		{
			std::lock_guard _{ g_mutex };
			auto& slot = g_values[formID][std::string(morphKey)];
			slot = Clamp(slot + delta);
			newValue = slot;
		}

		auto* task = SKSE::GetTaskInterface();
		if (!task) {
			return;
		}

		task->AddTask([actor, morphName, newValue, logOps]() {
			auto aa = actor.get();
			if (!aa) {
				return;
			}
			Papyrus_SetBodyMorph(aa.get(), morphName, newValue, logOps);
			});
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
		}

		auto* task = SKSE::GetTaskInterface();
		if (!task) {
			return;
		}

		task->AddTask([actor, logOps]() {
			auto aa = actor.get();
			if (!aa) {
				return;
			}
			Papyrus_ClearBodyMorphKeys(aa.get(), logOps);
			});
	}
}
