#include "FBScaler.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "RE/Skyrim.h"
#include "spdlog/spdlog.h"

namespace
{
	template <class T, class = void>
	struct has_get : std::false_type {};

	template <class T>
	struct has_get<T, std::void_t<decltype(std::declval<T>().get())>> : std::true_type {};

	template <class T>
	constexpr bool has_get_v = has_get<T>::value;

	template <class T>
	RE::TESObjectREFR* UnwrapRefr(const T& v)
	{
		if constexpr (std::is_pointer_v<T>) {
			return v;
		} else if constexpr (has_get_v<T>) {
			return UnwrapRefr(v.get());
		} else {
			return nullptr;
		}
	}

	static RE::Actor* ResolveActor(RE::ActorHandle h)
	{
		auto* refr = UnwrapRefr(h);
		return refr ? refr->As<RE::Actor>() : nullptr;
	}

	static RE::NiAVObject* GetRoot3D(RE::Actor* actor)
	{
		if (!actor) {
			return nullptr;
		}
		return actor->Get3D();
	}

	static bool ieq_char(char a, char b)
	{
		return static_cast<unsigned char>(std::tolower(a)) == static_cast<unsigned char>(std::tolower(b));
	}

	static bool icontains(std::string_view haystack, std::string_view needle)
	{
		if (needle.empty()) {
			return true;
		}
		if (haystack.size() < needle.size()) {
			return false;
		}
		for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
			bool ok = true;
			for (std::size_t j = 0; j < needle.size(); ++j) {
				if (!ieq_char(haystack[i + j], needle[j])) {
					ok = false;
					break;
				}
			}
			if (ok) {
				return true;
			}
		}
		return false;
	}

	static void CollectAllNames(RE::NiAVObject* obj, std::vector<std::string>& out, std::size_t limit = 2000)
	{
		if (!obj || out.size() >= limit) {
			return;
		}

		out.emplace_back(obj->name.c_str());

		if (auto* node = obj->AsNode(); node) {
			for (auto& child : node->children) {
				if (child) {
					CollectAllNames(child.get(), out, limit);
					if (out.size() >= limit) {
						return;
					}
				}
			}
		}
	}

	static RE::NiAVObject* FindByExactName(RE::NiAVObject* obj, const std::string& name)
	{
		if (!obj) {
			return nullptr;
		}

		if (obj->name == name.c_str()) {
			return obj;
		}

		if (auto* node = obj->AsNode(); node) {
			for (auto& child : node->children) {
				if (!child) {
					continue;
				}
				if (auto* found = FindByExactName(child.get(), name)) {
					return found;
				}
			}
		}

		return nullptr;
	}

	static RE::NiAVObject* FindFirstNameContaining(RE::NiAVObject* obj, std::string_view needle)
	{
		if (!obj) {
			return nullptr;
		}

		std::string_view nm{ obj->name.c_str() };
		if (icontains(nm, needle)) {
			return obj;
		}

		if (auto* node = obj->AsNode(); node) {
			for (auto& child : node->children) {
				if (!child) {
					continue;
				}
				if (auto* found = FindFirstNameContaining(child.get(), needle)) {
					return found;
				}
			}
		}

		return nullptr;
	}

	static std::optional<std::string_view> BracketCodeForKey(std::string_view key)
	{
		// Common vanilla bracket codes.
		// Extend this list as you add keys.
		if (key == "Pelvis") return "Pelv";
		if (key == "Spine" || key == "Spine0") return "Spn0";
		if (key == "Spine1") return "Spn1";
		if (key == "Spine2") return "Spn2";
		if (key == "Spine3") return "Spn3";
		if (key == "Neck") return "Neck";
		if (key == "Head") return "Head";

		if (key == "LClavicle") return "LClv";
		if (key == "RClavicle") return "RClv";
		if (key == "LUpperArm") return "LUar";
		if (key == "RUpperArm") return "RUar";
		if (key == "LForearm") return "LLar";
		if (key == "RForearm") return "RLar";
		if (key == "LHand") return "LHnd";
		if (key == "RHand") return "RHnd";

		if (key == "LThigh") return "LThg";
		if (key == "RThigh") return "RThg";
		if (key == "LCalf") return "LClf";
		if (key == "RCalf") return "RClf";
		if (key == "LFoot") return "Lft ";
		if (key == "RFoot") return "Rft ";
		if (key == "LToe0") return "LToe";
		if (key == "RToe0") return "RToe";

		return std::nullopt;
	}

	static std::vector<std::string_view> CandidatesForKey(std::string_view key)
	{
		using namespace FB::Scaler;

		if (key == "Pelvis") return { kNodePelvis };
		if (key == "Spine" || key == "Spine0") return { kNodeSpine0 };
		if (key == "Spine1") return { kNodeSpine1 };
		if (key == "Spine2") return { kNodeSpine2 };
		if (key == "Spine3") return { kNodeSpine3 };
		if (key == "Neck") return { kNodeNeck };
		if (key == "Head") return { kNodeHead };

		if (key == "LClavicle") return { kNodeLClavicle };
		if (key == "RClavicle") return { kNodeRClavicle };
		if (key == "LUpperArm") return { kNodeLUpperArm };
		if (key == "RUpperArm") return { kNodeRUpperArm };
		if (key == "LForearm") return { kNodeLForearm };
		if (key == "RForearm") return { kNodeRForearm };
		if (key == "LHand") return { kNodeLHand };
		if (key == "RHand") return { kNodeRHand };

		if (key == "LThigh") return { kNodeLThigh };
		if (key == "RThigh") return { kNodeRThigh };
		if (key == "LCalf") return { kNodeLCalf };
		if (key == "RCalf") return { kNodeRCalf };
		if (key == "LFoot") return { kNodeLFoot };
		if (key == "RFoot") return { kNodeRFoot };
		if (key == "LToe0") return { kNodeLToe0 };
		if (key == "RToe0") return { kNodeRToe0 };

		// If a user passes a full node name, try it directly.
		return { key };
	}

	static RE::NiAVObject* ResolveNodeByKey(RE::NiAVObject* root, std::string_view key)
	{
		if (!root) {
			return nullptr;
		}

		// 1) Try canonical candidates (exact).
		for (auto cand : CandidatesForKey(key)) {
			std::string candStr(cand);
			if (auto* found = FindByExactName(root, candStr)) {
				return found;
			}
		}

		// 2) Try bracket code fallback (e.g. "[LThg]").
		if (auto code = BracketCodeForKey(key)) {
			std::string pattern = "[";
			pattern += std::string(*code);
			pattern += "]";
			if (auto* found = FindFirstNameContaining(root, pattern)) {
				return found;
			}
		}

		// 3) Try loose contains(key) (case-insensitive).
		if (auto* found = FindFirstNameContaining(root, key)) {
			return found;
		}

		return nullptr;
	}

	static void LogClosestMatches(RE::NiAVObject* root, std::string_view key)
	{
		if (!root) {
			return;
		}

		std::vector<std::string> names;
		names.reserve(256);
		CollectAllNames(root, names);

		// Show a small sample to help users fix keys.
		int shown = 0;
		for (const auto& nm : names) {
			if (icontains(nm, key) && shown < 12) {
				spdlog::info("[FB] NodeScale: suggestion match for key='{}' -> '{}'", key, nm);
				++shown;
			}
		}

		if (shown == 0) {
			// Try bracket code suggestions.
			if (auto code = BracketCodeForKey(key)) {
				std::string pattern = "[";
				pattern += std::string(*code);
				pattern += "]";
				for (const auto& nm : names) {
					if (icontains(nm, pattern) && shown < 12) {
						spdlog::info("[FB] NodeScale: suggestion match for key='{}' -> '{}'", key, nm);
						++shown;
					}
				}
			}
		}
	}
}

namespace FB::Scaler
{
	void SetNodeScale(RE::ActorHandle actorHandle, std::string_view nodeName, float scale, bool logOps)
	{
		auto* actor = ResolveActor(actorHandle);
		if (!actor) {
			return;
		}

		auto* root = GetRoot3D(actor);
		if (!root) {
			return;
		}

		std::string nameStr(nodeName);
		auto* obj = FindByExactName(root, nameStr);
		if (!obj) {
			if (logOps) {
				spdlog::info("[FB] NodeScale: node '{}' not found for '{}'", nameStr, actor->GetName());
			}
			return;
		}

		const float oldScale = obj->local.scale;
		obj->local.scale = scale;

		// Ensure world data updates quickly.
		obj->Update(0.0f);

		if (logOps) {
			spdlog::info("[FB] NodeScale: actor='{}' node='{}' oldScale={} newScale={}",
				actor->GetName(),
				obj->name.c_str(),
				oldScale,
				scale);
		}
	}

	void SetNodeScaleByKey(RE::ActorHandle actorHandle, std::string_view nodeKey, float scale, bool logOps)
	{
		auto* actor = ResolveActor(actorHandle);
		if (!actor) {
			return;
		}

		auto* root = GetRoot3D(actor);
		if (!root) {
			return;
		}

		auto* obj = ResolveNodeByKey(root, nodeKey);
		if (!obj) {
			if (logOps) {
				spdlog::info("[FB] NodeScale: key '{}' not resolved for '{}'", nodeKey, actor->GetName());
				LogClosestMatches(root, nodeKey);
			}
			return;
		}

		const float oldScale = obj->local.scale;
		obj->local.scale = scale;
		obj->Update(0.0f);

		if (logOps) {
			spdlog::info("[FB] NodeScale: actor='{}' key='{}' node='{}' oldScale={} newScale={}",
				actor->GetName(),
				nodeKey,
				obj->name.c_str(),
				oldScale,
				scale);
		}
	}
}
