#include "FBVis.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "RE/Skyrim.h"
#include "spdlog/spdlog.h"

namespace
{
	std::unordered_map<std::string, std::vector<std::string>> g_groups;
	static std::shared_mutex g_groupsLock;

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
}

namespace FB::Vis
{
	void SetGroups(std::unordered_map<std::string, std::vector<std::string>> groups)
	{
		std::unique_lock lk(g_groupsLock);
		g_groups = std::move(groups);
	}

	bool SetObjectVisibleExact(RE::ActorHandle actorHandle, std::string_view objectName, bool visible, bool logOps)
	{
		auto* actor = ResolveActor(actorHandle);
		if (!actor) {
			return false;
		}

		auto* root = actor->Get3D();
		if (!root) {
			return false;
		}

		std::string nameStr(objectName);
		auto* obj = FindByExactName(root, nameStr);
		if (!obj) {
			if (logOps) {
				spdlog::info("[FB] Vis: object '{}' not found for '{}'", nameStr, actor->GetName());

				// Helpful suggestions: list a few objects that contain the token.
				std::vector<std::string> names;
				names.reserve(256);
				CollectAllNames(root, names);

				int shown = 0;
				for (const auto& nm : names) {
					if (icontains(nm, objectName) && shown < 12) {
						spdlog::info("[FB] Vis: suggestion match for key='{}' -> '{}'", objectName, nm);
						++shown;
					}
				}

				if (shown == 0) {
					spdlog::info("[FB] Vis: note: keys like 'LThigh' are usually *bone keys*; for FBVis you typically want mesh object names (or define a VisGroup mapping).");
				}
			}
			return false;
		}

		// Cull/uncull the object.
		// NOTE: AppCulled is the most common "hard hide" toggle in Skyrim.
		obj->SetAppCulled(!visible);
		obj->Update(0.0f);

		if (logOps) {
			spdlog::info("[FB] Vis: actor='{}' object='{}' visible={}", actor->GetName(), obj->name.c_str(), visible);
		}

		return true;
	}

	void SetVisibleByKey(RE::ActorHandle actor, std::string_view key, bool visible, bool logOps)
	{
		// Copy group list outside the lock (keep lock held briefly).
		std::vector<std::string> group;
		{
			std::shared_lock lk(g_groupsLock);
			auto it = g_groups.find(std::string(key));
			if (it != g_groups.end()) {
				group = it->second;
			}
		}

		if (!group.empty()) {
			for (const auto& objName : group) {
				SetObjectVisibleExact(actor, objName, visible, logOps);
			}
			return;
		}

		// No group mapping -> treat as exact object name.
		SetObjectVisibleExact(actor, key, visible, logOps);
	}
}
