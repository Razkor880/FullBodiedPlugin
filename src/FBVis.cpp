#include "FBVis.h"

#include "SKSE/SKSE.h"

#include <spdlog/spdlog.h>

#include <string>

namespace FB::Vis
{
	void SetObjectVisibleExact(RE::ActorHandle actor, std::string_view objectName, bool visible, bool logOps)
	{
		auto* task = SKSE::GetTaskInterface();
		if (!task) {
			return;
		}

		const std::string name{ objectName };

		task->AddTask([actor, name, visible, logOps]() {
			auto a = actor.get();
			if (!a) {
				return;
			}

			auto root = a->Get3D();
			if (!root) {
				return;
			}

			auto obj = root->GetObjectByName(name.c_str());
			if (!obj) {
				if (logOps) {
					spdlog::info("[FB] Vis: object '{}' not found for '{}'", name, a->GetName());
				}
				return;
			}

			// Avoid bone/subtree visibility pitfalls: if this resolves to a NiNode, warn and skip.
			if (obj->AsNode()) {
				if (logOps) {
					spdlog::warn("[FB] Vis: '{}' resolved to NiNode '{}' on '{}' (skipping to avoid hiding children). Use geometry/shape names instead.",
						name, obj->name.c_str(), a->GetName());
				}
				return;
			}

			if (logOps) {
				spdlog::info("[FB] Vis: actor='{}' object='{}' -> {}",
					a->GetName(),
					obj->name.c_str(),
					visible ? "VISIBLE" : "HIDDEN");
			}

			obj->SetAppCulled(!visible);
			obj->UpdateWorldBound();
			});
	}

	void SetObjectsVisibleExact(RE::ActorHandle actor,
		std::initializer_list<std::string_view> objectNames,
		bool visible,
		bool logOps)
	{
		for (auto n : objectNames) {
			SetObjectVisibleExact(actor, n, visible, logOps);
		}
	}

	void DumpNonNodeObjectNames(RE::ActorHandle actor, bool logOps)
	{
		// Temporarily no-op to maintain compatibility across CommonLib variants.
		// We can re-add a proper traversal once we confirm the exact NiNode child API in your environment.
		if (logOps) {
			spdlog::info("[FB] VisDump: disabled (NiNode child traversal differs across CommonLib builds).");
		}
		(void)actor;
	}
}
