// AnimationEvents.cpp
//
// FullBodiedPlugin
//
// Responsibilities (post-refactor):
// - Receive BSAnimationGraphEvent tags on an actor
// - Filter stop events (PairEnd / NPCPairedStop) and call CancelAndReset
// - Filter FBEvent (and other start tags mapped in config), resolve a likely target,
//   and start the timeline via ActorManager using commands parsed by FBConfig
//
// Parsing responsibilities live exclusively in FBConfig.

#include "AnimationEvents.h"

#include "ActorManager.h"
#include "FBConfig.h"
#include "FBScaler.h"

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace
{
	// =========================
	// Constants
	// =========================
	static constexpr std::string_view kPairEndEvent = "PairEnd";
	static constexpr std::string_view kPairedStopEvent = "NPCPairedStop";

	// Target search radius (tune as needed; 200-300 tends to work for paired idles)
	static constexpr float kTargetSearchRadius = 250.0f;

	// Debounce to avoid duplicate starts from duplicate sink registration / duplicate graphs
	static constexpr float kStartDebounceSeconds = 0.20f;

	// =========================
	// Small string utils
	// =========================
	static inline bool IEquals(const std::string& a, const char* b)
	{
		size_t i = 0;
		for (; i < a.size() && b[i] != '\0'; ++i) {
			char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
			char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
			if (ca != cb) {
				return false;
			}
		}
		return i == a.size() && b[i] == '\0';
	}

	// =========================
	// Node mapping (stays here)
	// =========================
	static std::optional<std::string_view> ResolveNodeKey(std::string_view key)
	{
		// Author-facing NodeKey -> skeleton NiNode name mapping.
		// Keep these keys stable; they are part of the INI "public API".

		// Head / Neck
		if (key == "Head") return FB::Scaler::kNodeHead;
		if (key == "Neck") return FB::Scaler::kNodeNeck;

		// Spine
		if (key == "Spine0") return FB::Scaler::kNodeSpine0;
		if (key == "Spine1") return FB::Scaler::kNodeSpine1;
		if (key == "Spine2") return FB::Scaler::kNodeSpine2;
		if (key == "Spine3") return FB::Scaler::kNodeSpine3;

		// Pelvis
		if (key == "Pelvis") return FB::Scaler::kNodePelvis;

		// Arms
		if (key == "LClavicle") return FB::Scaler::kNodeLClavicle;
		if (key == "RClavicle") return FB::Scaler::kNodeRClavicle;
		if (key == "LUpperArm") return FB::Scaler::kNodeLUpperArm;
		if (key == "RUpperArm") return FB::Scaler::kNodeRUpperArm;
		if (key == "LForearm") return FB::Scaler::kNodeLForearm;
		if (key == "RForearm") return FB::Scaler::kNodeRForearm;
		if (key == "LHand") return FB::Scaler::kNodeLHand;
		if (key == "RHand") return FB::Scaler::kNodeRHand;

		// Legs
		if (key == "LThigh") return FB::Scaler::kNodeLThigh;
		if (key == "RThigh") return FB::Scaler::kNodeRThigh;
		if (key == "LCalf") return FB::Scaler::kNodeLCalf;
		if (key == "RCalf") return FB::Scaler::kNodeRCalf;
		if (key == "LFoot") return FB::Scaler::kNodeLFoot;
		if (key == "RFoot") return FB::Scaler::kNodeRFoot;
		if (key == "LToe0") return FB::Scaler::kNodeLToe0;
		if (key == "RToe0") return FB::Scaler::kNodeRToe0;

		// Legacy convenience keys
		if (key == "Spine") return FB::Scaler::kNodeSpine0;

		return std::nullopt;
	}

	// =========================
	// Debounce (event-level)
	// =========================
	static std::mutex g_debounceMutex;
	static std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> g_lastStart;

	static bool ShouldDebounceStart(std::uint32_t casterFormID)
	{
		std::lock_guard _{ g_debounceMutex };

		const auto now = std::chrono::steady_clock::now();
		auto& last = g_lastStart[casterFormID];

		if (last.time_since_epoch().count() != 0) {
			const float dt = std::chrono::duration<float>(now - last).count();
			if (dt < kStartDebounceSeconds) {
				return true;
			}
		}

		last = now;
		return false;
	}

	// =========================
	// Target resolution
	// =========================
	static RE::ActorHandle FindLikelyPairedTarget(RE::Actor* caster, bool log)
	{
		if (!caster) {
			return {};
		}

		auto casterPos = caster->GetPosition();

		RE::Actor* best = nullptr;
		float bestDist2 = kTargetSearchRadius * kTargetSearchRadius;

		auto* processLists = RE::ProcessLists::GetSingleton();
		if (!processLists) {
			return {};
		}

		auto* casterCell = caster->GetParentCell();

		processLists->ForEachHighActor([&](RE::Actor& a) {
			RE::Actor* ap = std::addressof(a);

			if (!ap || ap == caster) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			if (ap->IsDead()) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			if (!ap->Is3DLoaded()) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			if (casterCell && ap->GetParentCell() != casterCell) {
				return RE::BSContainer::ForEachResult::kContinue;
			}

			auto d = ap->GetPosition() - casterPos;
			const float dist2 = (d.x * d.x) + (d.y * d.y) + (d.z * d.z);
			if (dist2 < bestDist2) {
				bestDist2 = dist2;
				best = ap;
			}

			return RE::BSContainer::ForEachResult::kContinue;
			});

		if (best) {
			if (log) {
				spdlog::info("[FB] TargetResolve: caster='{}' -> target='{}' dist={}",
					caster->GetName(), best->GetName(), std::sqrt(bestDist2));
			}
			return best->CreateRefHandle();
		}

		if (log) {
			spdlog::info("[FB] TargetResolve: caster='{}' -> no target found", caster->GetName());
		}
		return {};
	}

	// =========================
	// Timeline start + dispatch
	// =========================
	static void StartTimelineForCaster(RE::Actor* caster, std::string_view startEventTag)
	{
		if (!caster) {
			return;
		}

		const auto& cfg = FB::Config::Get(&ResolveNodeKey);

		if (!cfg.enableTimelines) {
			return;
		}

		// 1) Only proceed if this tag is mapped
		auto itMap = cfg.eventToTimeline.find(std::string(startEventTag));
		if (itMap == cfg.eventToTimeline.end()) {
			return;
		}

		const std::string_view timelineName{ itMap->second };

		// 2) Debounce only real starts
		const std::uint32_t casterFormID = caster->GetFormID();
		if (ShouldDebounceStart(casterFormID)) {
			if (cfg.dbg.logOps) {
				spdlog::info("[FB] Debounce: ignoring start '{}' on '{}'",
					std::string(startEventTag), caster->GetName());
			}
			return;
		}

		// 3) Retrieve timeline commands
		auto itTL = cfg.timelines.find(std::string(timelineName));
		if (itTL == cfg.timelines.end() || itTL->second.empty()) {
			if (cfg.dbg.logOps) {
				spdlog::info("[FB] Timeline '{}' has no commands", std::string(timelineName));
			}
			return;
		}

		auto targetHandle = FindLikelyPairedTarget(caster, cfg.dbg.logTargetResolve);

		if (cfg.dbg.logOps && cfg.dbg.logTimelineStart) {
			auto t = targetHandle.get();
			spdlog::info("[FB] StartTimeline: tag='{}' timeline='{}' caster='{}' target='{}' cmds={}",
				std::string(startEventTag),
				std::string(timelineName),
				caster->GetName(),
				t ? t->GetName() : "<none>",
				itTL->second.size());
		}

		FB::ActorManager::StartTimeline(
			caster->CreateRefHandle(),
			targetHandle,
			casterFormID,
			itTL->second,
			cfg.dbg.logOps);
	}

	static void CancelAndReset(RE::Actor* caster, std::string_view tag)
	{
		if (!caster) {
			return;
		}

		const auto& cfg = FB::Config::Get(&ResolveNodeKey);
		const std::uint32_t casterFormID = caster->GetFormID();

		const bool isPairEnd = (tag == kPairEndEvent);
		const bool isPairedStop = (tag == kPairedStopEvent);

		const bool doMorphReset =
			(isPairEnd && cfg.resetMorphsOnPairEnd) ||
			(isPairedStop && cfg.resetMorphsOnPairedStop);

		FB::ActorManager::CancelAndReset(
			caster->CreateRefHandle(),
			casterFormID,
			cfg.dbg.logOps,
			/*resetMorphsForCaster=*/doMorphReset,
			/*resetMorphsForTarget=*/doMorphReset);
	}

	// =========================
	// Event sink
	// =========================
	class AnimationEventSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(
			const RE::BSAnimationGraphEvent* a_event,
			RE::BSTEventSource<RE::BSAnimationGraphEvent>*)
			override
		{
			if (!a_event || !a_event->holder || a_event->tag.empty()) {
				return RE::BSEventNotifyControl::kContinue;
			}

			auto* caster = const_cast<RE::Actor*>(a_event->holder->As<RE::Actor>());
			if (!caster) {
				return RE::BSEventNotifyControl::kContinue;
			}

			const std::string_view tag{ a_event->tag.c_str(), a_event->tag.size() };
			const auto& cfg = FB::Config::Get(&ResolveNodeKey);

			// Stop events -> cancel + reset
			if ((cfg.resetOnPairEnd && tag == kPairEndEvent) ||
				(cfg.resetOnPairedStop && tag == kPairedStopEvent)) {

				if (cfg.dbg.logOps) {
					spdlog::info("[FB] '{}' on '{}' -> cancel + reset",
						std::string(tag), caster->GetName());
				}

				CancelAndReset(caster, tag);
				return RE::BSEventNotifyControl::kContinue;
			}

			// Start events based on EventToTimeline mapping
			if (cfg.eventToTimeline.contains(std::string(tag))) {
				StartTimelineForCaster(caster, tag);
			}

			return RE::BSEventNotifyControl::kContinue;
		}
	};

	AnimationEventSink g_animationEventSink;
}  // namespace

// =========================
// Public API
// =========================
bool RegisterAnimationEventSink(RE::Actor* actor)
{
	if (!actor) return false;

	RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
	if (!actor->GetAnimationGraphManager(manager) || !manager) {
		spdlog::warn("RegisterAnimationEventSink: no animation graph manager");
		return false;
	}

	bool attached = false;
	for (auto& graph : manager->graphs) {
		if (!graph) continue;
		graph->AddEventSink(&g_animationEventSink);
		attached = true;
	}

	if (attached) {
		spdlog::info("Registered animation sinks to actor={}", actor->GetName());
	}
	else {
		spdlog::warn("RegisterAnimationEventSink: no graphs for actor={}", actor->GetName());
	}

	return attached;
}


void LoadFBConfig()
{
	FB::Config::Reload(&ResolveNodeKey);
}


void HeadScale(RE::Actor* actor, float scale)
{
	if (!actor) {
		return;
	}

	const auto& cfg = FB::Config::Get(&ResolveNodeKey);
	FB::Scaler::SetNodeScale(actor->CreateRefHandle(), FB::Scaler::kNodeHead, scale, cfg.dbg.logOps);
}
