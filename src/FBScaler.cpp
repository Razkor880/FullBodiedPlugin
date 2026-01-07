#include "FBScaler.h"

#include "SKSE/SKSE.h"
#include "RE/Skyrim.h"

#include <algorithm>
#include <string>

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
			auto a = actor.get();
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
}
