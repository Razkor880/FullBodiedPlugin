#include "FBScaler.h"

#include "SKSE/SKSE.h"
#include "RE/Skyrim.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
	// Per-actor baseline tracking: nodeName -> baselineScale
	using NodeBaselineMap = std::unordered_map<std::string, float>;

	std::mutex g_baselineMutex;
	std::unordered_map<std::uint32_t, NodeBaselineMap> g_actorBaselines;

	static void RecordBaselineIfNeeded(std::uint32_t actorFormID, const std::string& nodeName, float baseline)
	{
		std::lock_guard _{ g_baselineMutex };
		auto& perActor = g_actorBaselines[actorFormID];
		if (perActor.find(nodeName) == perActor.end()) {
			perActor.emplace(nodeName, baseline);
		}
	}

	static std::vector<std::pair<std::string, float>> TakeAndClearBaselines(std::uint32_t actorFormID)
	{
		std::lock_guard _{ g_baselineMutex };
		std::vector<std::pair<std::string, float>> out;

		auto it = g_actorBaselines.find(actorFormID);
		if (it == g_actorBaselines.end()) {
			return out;
		}

		out.reserve(it->second.size());
		for (auto& [name, baseline] : it->second) {
			out.emplace_back(name, baseline);
		}

		g_actorBaselines.erase(it);
		return out;
	}

	static std::vector<std::pair<std::uint32_t, std::vector<std::pair<std::string, float>>>> TakeAndClearAllBaselines()
	{
		std::lock_guard _{ g_baselineMutex };
		std::vector<std::pair<std::uint32_t, std::vector<std::pair<std::string, float>>>> out;

		out.reserve(g_actorBaselines.size());
		for (auto& [actorID, map] : g_actorBaselines) {
			std::vector<std::pair<std::string, float>> nodes;
			nodes.reserve(map.size());
			for (auto& [name, baseline] : map) {
				nodes.emplace_back(name, baseline);
			}
			out.emplace_back(actorID, std::move(nodes));
		}

		g_actorBaselines.clear();
		return out;
	}
}

namespace FB::Scaler
{
	void SetNodeScale(RE::ActorHandle actor, std::string_view nodeName, float scale, bool logOps)
	{
		// Clamp here so every caller benefits and we keep behavior consistent.
		scale = std::clamp(scale, 0.0f, 5.0f);

		auto* task = SKSE::GetTaskInterface();
		if (!task) {
			return;
		}

		// Copy nodeName into owned string because we hop threads.
		const std::string node{ nodeName };

		task->AddTask([actor, node, scale, logOps]() {
			auto a = actor.get();  // NiPointer<RE::Actor>
			if (!a) {
				return;
			}

			auto root = a->Get3D();
			if (!root) {
				return;
			}

			auto obj = root->GetObjectByName(node.c_str());
			if (!obj) {
				if (logOps) {
					spdlog::info("[FB] NodeScale: node '{}' not found for '{}'", node, a->GetName());
				}
				return;
			}

			// Step 1: record baseline on first touch (per actor + node)
			RecordBaselineIfNeeded(a->GetFormID(), node, obj->local.scale);

			if (logOps) {
				spdlog::info("[FB] NodeScale: actor='{}' node='{}' oldScale={} newScale={}",
					a->GetName(),
					obj->name.c_str(),
					obj->local.scale,
					scale);
			}

			obj->local.scale = scale;
			});
	}

	void ResetNodes(RE::ActorHandle actor, std::initializer_list<std::string_view> nodeNames, bool logOps)
	{
		for (auto n : nodeNames) {
			SetNodeScale(actor, n, 1.0f, logOps);
		}
	}

	void ResetActor(RE::ActorHandle actor, bool logOps)
	{
		auto* task = SKSE::GetTaskInterface();
		if (!task) {
			return;
		}

		task->AddTask([actor, logOps]() {
			auto a = actor.get();  // NiPointer<RE::Actor>
			if (!a) {
				return;
			}

			auto nodes = TakeAndClearBaselines(a->GetFormID());
			if (nodes.empty()) {
				if (nodes.empty() && logOps) {
					spdlog::info("[FB] NodeReset: actor='{}' no tracked baselines", a->GetName());
				}

				return;
			}

			auto root = a->Get3D();
			if (!root) {
				// Actor exists but no 3D; we already cleared baselines to avoid leaks.
				return;
			}

			std::size_t restored = 0;

			for (auto& [nodeName, baseline] : nodes) {
				auto obj = root->GetObjectByName(nodeName.c_str());
				if (!obj) {
					continue;
				}

				if (logOps) {
					spdlog::info("[FB] NodeReset: actor='{}' node='{}' oldScale={} baseline={}",
						a->GetName(),
						obj->name.c_str(),
						obj->local.scale,
						baseline);
				}

				obj->local.scale = baseline;
				++restored;
			}

			if (logOps) {
				spdlog::info("[FB] NodeReset: actor='{}' restoredNodes={}", a->GetName(), restored);
			}
			});
	}

	void ResetAll(bool logOps)
	{
		auto* task = SKSE::GetTaskInterface();
		if (!task) {
			return;
		}

		auto all = TakeAndClearAllBaselines();
		if (all.empty()) {
			return;
		}

		task->AddTask([all = std::move(all), logOps]() mutable {
			auto* processLists = RE::ProcessLists::GetSingleton();
			if (!processLists) {
				return;
			}

			// Build a quick lookup: actorID -> node list
			std::unordered_map<std::uint32_t, std::vector<std::pair<std::string, float>>> map;
			map.reserve(all.size());
			for (auto& entry : all) {
				map.emplace(entry.first, std::move(entry.second));
			}

			processLists->ForEachHighActor([&](RE::Actor& act) {
				auto id = act.GetFormID();
				auto it = map.find(id);
				if (it == map.end()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}

				auto root = act.Get3D();
				if (!root) {
					return RE::BSContainer::ForEachResult::kContinue;
				}

				std::size_t restored = 0;
				for (auto& [nodeName, baseline] : it->second) {
					auto obj = root->GetObjectByName(nodeName.c_str());
					if (!obj) {
						continue;
					}
					obj->local.scale = baseline;
					++restored;
				}

				if (logOps) {
					spdlog::info("[FB] ResetAll: actor='{}' restoredNodes={}", act.GetName(), restored);
				}

				return RE::BSContainer::ForEachResult::kContinue;
				});
			});
	}
}
