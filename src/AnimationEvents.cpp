// AnimationEvents.cpp
//
// FullBodiedPlugin - FB INI timeline parser (Scale system, expanding)
//
// INI file name: FullBodiedIni.ini
// Preferred: Data\FullBodiedIni.ini
// Fallback:  Data\SKSE\Plugins\FullBodiedIni.ini
//
// Event mapping:
//   [EventToTimeline]   (also supports legacy [EventMap])
//   <StartEventTag> = <TimelineName>
//
// Example:
//   FBEvent = paired_huga.hkx
//
// Timeline sections (2-part):
//   [FB:paired_huga.hkx|Caster]
//   0.000000 FBScale_Head(1.0)
//
//   [FB:paired_huga.hkx|Target]
//   0.000000 2_FBScale_Head(1.0)
//
// Token format (canonical):
//   FBScale_<NodeKey>(<floatMultiplier>)
//   2_FBScale_<NodeKey>(<floatMultiplier>)
//
// Future extension:
//   FBVisible_<NodeKey>(true/false) etc (parser structured for this)
//
// Reset reinforcement:
//   PairEnd and NPCPairedStop cancel pending tasks and reset caster + target (touched nodes only).

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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
	// =========================
	// Constants
	// =========================
	static constexpr std::string_view kPairEndEvent = "PairEnd";
	static constexpr std::string_view kPairedStopEvent = "NPCPairedStop";

	// Target search radius (tune as needed; 200–300 tends to work for paired idles)
	static constexpr float kTargetSearchRadius = 250.0f;

	// Debounce to avoid duplicate starts from duplicate sink registration / duplicate graphs
	static constexpr float kStartDebounceSeconds = 0.20f;

	// Target line prefix standard
	static constexpr std::string_view kTargetPrefix = "2_";

	// =========================
	// Small string utils
	// =========================
	static inline void TrimInPlace(std::string& s)
	{
		auto notSpace = [](unsigned char c) { return !std::isspace(c); };
		s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
		s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
	}

	static inline void StripInlineComment(std::string& s)
	{
		auto p1 = s.find(';');
		auto p2 = s.find('#');
		auto p = std::min(p1 == std::string::npos ? s.size() : p1,
			p2 == std::string::npos ? s.size() : p2);
		if (p != std::string::npos && p < s.size()) {
			s.resize(p);
		}
	}

	


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

	static std::optional<float> ParseFloat(std::string s)
	{
		TrimInPlace(s);
		if (s.empty()) {
			return std::nullopt;
		}
		char* end = nullptr;
		const float f = std::strtof(s.c_str(), &end);
		if (!end || end == s.c_str()) {
			return std::nullopt;
		}
		return f;
	}

	static bool ParseBool(const std::string& v, bool fallback)
	{
		std::string s = v;
		TrimInPlace(s);
		if (s.empty()) {
			return fallback;
		}
		if (IEquals(s, "1") || IEquals(s, "true") || IEquals(s, "yes") || IEquals(s, "on")) {
			return true;
		}
		if (IEquals(s, "0") || IEquals(s, "false") || IEquals(s, "no") || IEquals(s, "off")) {
			return false;
		}
		return fallback;
	}

	static std::vector<std::string> Split(const std::string& s, char delim)
	{
		std::vector<std::string> out;
		std::string cur;
		for (char c : s) {
			if (c == delim) {
				out.push_back(cur);
				cur.clear();
			}
			else {
				cur.push_back(c);
			}
		}
		out.push_back(cur);
		return out;
	}

	// =========================
	// Commands / Config model
	// =========================
	using FB::TargetKind;
	using FB::TimedCommand;

	// =========================
	// Node mapping (stays here)
	// =========================
	static std::optional<std::string_view> ResolveNodeKey(std::string_view key)
	{
		// Author-facing NodeKey -> skeleton NiNode name mapping.
		// Keep these keys stable; they are part of the INI “public API”.

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

		// 1) FIRST: check whether this tag is mapped at all
		auto itMap = cfg.eventToTimeline.find(std::string(startEventTag));
		if (itMap == cfg.eventToTimeline.end()) {
			return;  // not a start tag we care about
		}

		const std::string_view timelineName{ itMap->second };

		// 2) THEN: debounce only real starts
		const std::uint32_t casterFormID = caster->GetFormID();
		if (ShouldDebounceStart(casterFormID)) {
			if (cfg.dbg.logOps) {
				spdlog::info("[FB] Debounce: ignoring start '{}' on '{}'",
					std::string(startEventTag), caster->GetName());
			}
			return;
		}

		// 3) Now find the timeline commands
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

			if ((cfg.resetOnPairEnd && tag == kPairEndEvent) ||
				(cfg.resetOnPairedStop && tag == kPairedStopEvent)) {

				if (cfg.dbg.logOps) {
					spdlog::info("[FB] '{}' on '{}' -> cancel + reset",
						std::string(tag), caster->GetName());
				}

				CancelAndReset(caster, tag);
				return RE::BSEventNotifyControl::kContinue;
			}

			// Start timelines based on EventToTimeline mapping
			// Start timelines based on EventToTimeline mapping (quick prefilter)
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
void RegisterAnimationEventSink(RE::Actor* actor)
{
	if (!actor) {
		return;
	}

	RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
	if (!actor->GetAnimationGraphManager(manager) || !manager) {
		spdlog::warn("RegisterAnimationEventSink: no animation graph manager");
		return;
	}

	for (auto& graph : manager->graphs) {
		if (!graph) {
			continue;
		}
		graph->AddEventSink(&g_animationEventSink);
	}

	spdlog::info("Registered animation sinks to actor={}", actor->GetName());
}

void LoadFBConfig()
{
	FB::Config::Reload(&ResolveNodeKey);
}

void LoadHeadScaleConfig()
{
	// Backward compatible wrapper
	LoadFBConfig();
}

void HeadScale(RE::Actor* actor, float scale)
{
	if (!actor) {
		return;
	}
	const auto& cfg = FB::Config::Get(&ResolveNodeKey);
	FB::Scaler::SetNodeScale(actor->CreateRefHandle(), FB::Scaler::kNodeHead, scale, cfg.dbg.logOps);
}
