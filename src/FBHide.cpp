//#include "FBHide.h"

#include <RE/N/NiNode.h>
#include <RE/N/NiRTTI.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
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

			void Clear()
			{
				baselineHiddenByObj.clear();
				touched.clear();
			}
		};

		std::mutex g_mutex;
		std::unordered_map<std::uint32_t, ActorHideState> g_stateByActorID;

		static bool IsRenderableGeometry(RE::NiAVObject* a_obj)
		{
			if (!a_obj) {
				return false;
			}

			if (netimmerse_cast<RE::BSTriShape*>(a_obj)) {
				return true;
			}
			if (netimmerse_cast<RE::NiGeometry*>(a_obj)) {
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
				// IMPORTANT: with ENABLE_SKYRIM_VR=1, NiNode does not expose `children`.
				// Use GetChildren() which CommonLib provides across builds.
				for (auto& ch : node->GetChildren()) {
					if (ch) {
						TraverseNode(ch.get(), a_out);
					}
				}
			}
		}

		static void SetHiddenFlag(RE::NiAVObject* a_obj, bool a_hidden)
		{
			if (!a_obj) {
				return;
			}

			// GetFlags() is the CommonLib-safe accessor across builds (incl. VR).
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
			if (!a_obj) {
				return false;
			}

			return a_obj->GetFlags().all(RE::NiAVObject::Flag::kHidden);
		}


		static std::uint32_t GetActorID(const RE::ActorHandle& a_handle)
		{
			const auto ap = a_handle.get();   // NiPointer<Actor>
			auto* actor = ap.get();           // Actor*
			if (!actor) {
				return 0;
			}
			return actor->GetFormID();
		}

		static RE::NiAVObject* GetRoot3D(const RE::ActorHandle& a_handle)
		{
			const auto ap = a_handle.get();
			auto* actor = ap.get();
			if (!actor) {
				return nullptr;
			}
			return actor->Get3D();
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

			// Capture baseline once per object per actor.
			if (state.baselineHiddenByObj.find(obj) == state.baselineHiddenByObj.end()) {
				const bool baseline = GetHiddenFlag(obj);
				state.baselineHiddenByObj.emplace(obj, baseline);
				state.touched.push_back(obj);
			}

			// hide=true: force hidden; hide=false: restore baseline
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
			for (auto* obj : state.touched) {
				auto jt = state.baselineHiddenByObj.find(obj);
				if (jt != state.baselineHiddenByObj.end()) {
					SetHiddenFlag(const_cast<RE::NiAVObject*>(obj), jt->second);
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
