// AnimationEvents.cpp
//
// Consolidated version (caster + target head scale timelines driven by INI)
//
// Behavior/Graph emits ONLY: FB_HeadScale  (start timeline immediately)
//
// INI layout (must look like this):
//
//   [CasterHeadScale]
//   ; timeSeconds   HeadScaleS###
//   0.000000 HeadScaleS100
//   12.000000 HeadScaleS100
//
//   [TargetHeadScale]
//   ; timeSeconds   2_HeadScaleS###
//   0.000000 2_HeadScaleS100
//   5.000000 2_HeadScaleS025
//   12.000000 2_HeadScaleS100
//
// Target resolution: best-effort "nearest 3D-loaded actor in same cell within radius"
// Reset reinforcement: cancels pending steps + resets both caster + remembered target on PairEnd
// (also supports optional NPCPairedStop).
//
// Notes:
// - Worker threads sleep; actual scene graph edits are queued via SKSE task interface.
// - Per-caster token cancels old scheduled actions.
// - Debounce prevents double-scheduling if FB_HeadScale fires twice rapidly (common with multiple graphs).

#include "AnimationEvents.h"

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
	// =========================
	// Constants / naming
	// =========================
	static constexpr std::string_view kStartEvent = "FB_HeadScale";

	// Reset reinforcement tags:
	static constexpr std::string_view kPairEndEvent = "PairEnd";
	// Many graphs use this for interruption; enable via INI if present in your setup.
	static constexpr std::string_view kPairedStopEvent = "NPCPairedStop";

	// Skeleton node name to scale
	static constexpr const char* kHeadNodeName = "NPC Head [Head]";
	static constexpr float       kResetScale = 1.0f;

	// Command prefixes (INI)
	static constexpr std::string_view kCmdHeadScalePrefix = "HeadScaleS";        // caster
	static constexpr std::string_view kCmdTargetPrefix = "2_";                  // target prefix
	static constexpr std::string_view kCmdTargetHeadScalePrefix = "2_HeadScaleS"; // target

	// Target search radius (paired idles are very close; tune as needed)
	static constexpr float kTargetSearchRadius = 250.0f;

	// Debounce window for duplicate FB_HeadScale starts (seconds)
	static constexpr float kStartDebounceSeconds = 0.20f;

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

	// =========================
	// Scenegraph operation
	// =========================
	static void ApplyHeadScaleHandle(RE::ActorHandle handle, float scale)
	{
		// If handle is empty or actor unloaded, this becomes a no-op.
		if (auto* task = SKSE::GetTaskInterface()) {
			task->AddTask([handle, scale]() {
				auto actorPtr = handle.get();
				if (!actorPtr) {
					return;
				}
				auto root = actorPtr->Get3D();
				if (!root) {
					return;
				}

				auto headNode = root->GetObjectByName(kHeadNodeName);
				if (!headNode) {
					spdlog::info("[FB] HeadScale: head node not found for '{}'", actorPtr->GetName());
					return;
				}

				spdlog::info("[FB] HeadScale: actor='{}' node='{}' oldScale={} newScale={}",
					actorPtr->GetName(),
					headNode->name.c_str(),
					headNode->local.scale,
					scale);

				headNode->local.scale = scale;
				});
		}
	}

	// =========================
	// Commands
	// =========================
	enum class TargetKind
	{
		kCaster,
		kTarget
	};

	enum class CommandType
	{
		kHeadScale
	};

	struct Command
	{
		TargetKind  target{ TargetKind::kCaster };
		CommandType type{ CommandType::kHeadScale };
		float       timeSeconds{ 0.0f };
		float       scale{ 1.0f };
	};

	static std::optional<float> TryParseScaleSuffix(std::string_view cmd, std::string_view prefix)
	{
		if (cmd.rfind(prefix, 0) != 0) {
			return std::nullopt;
		}
		auto suffix = cmd.substr(prefix.size());
		if (suffix.size() != 3) {
			return std::nullopt;
		}
		for (char c : suffix) {
			if (c < '0' || c > '9') {
				return std::nullopt;
			}
		}

		const int percent =
			(static_cast<int>(suffix[0] - '0') * 100) +
			(static_cast<int>(suffix[1] - '0') * 10) +
			(static_cast<int>(suffix[2] - '0') * 1);

		const int clamped = std::clamp(percent, 0, 100);
		return static_cast<float>(clamped) / 100.0f;
	}

	// Parses either:
	// - HeadScaleS###       -> caster
	// - 2_HeadScaleS###     -> target
	static std::optional<Command> ParseCommandLine(float t, std::string_view cmdTok)
	{
		// Target command
		if (cmdTok.rfind(kCmdTargetPrefix, 0) == 0) {
			if (auto s = TryParseScaleSuffix(cmdTok, kCmdTargetHeadScalePrefix)) {
				Command c;
				c.target = TargetKind::kTarget;
				c.type = CommandType::kHeadScale;
				c.timeSeconds = t;
				c.scale = *s;
				return c;
			}
			return std::nullopt;
		}

		// Caster command
		if (auto s = TryParseScaleSuffix(cmdTok, kCmdHeadScalePrefix)) {
			Command c;
			c.target = TargetKind::kCaster;
			c.type = CommandType::kHeadScale;
			c.timeSeconds = t;
			c.scale = *s;
			return c;
		}

		return std::nullopt;
	}

	// =========================
	// Config
	// =========================
	struct Config
	{
		bool enableTimelines{ true };
		bool resetOnPairEnd{ true };
		bool resetOnPairedStop{ true };  // recommended

		// StartEvent -> TimelineName
		std::unordered_map<std::string, std::string> eventToTimeline;

		// TimelineName -> list of commands
		std::unordered_map<std::string, std::vector<Command>> timelines;
	};

	std::mutex g_cfgMutex;
	Config     g_cfg;
	bool       g_cfgLoaded = false;

	static std::filesystem::path GetConfigPath()
	{
		return std::filesystem::path("Data") / "SKSE" / "Plugins" / "FullBodiedPlugin.ini";
	}

	static void SortAndClamp(std::vector<Command>& cmds)
	{
		for (auto& c : cmds) {
			if (c.timeSeconds < 0.0f) {
				c.timeSeconds = 0.0f;
			}
			c.scale = std::clamp(c.scale, 0.0f, 2.0f);
		}
		std::sort(cmds.begin(), cmds.end(), [](const auto& a, const auto& b) { return a.timeSeconds < b.timeSeconds; });
	}

	static void LoadConfigLocked()
	{
		Config newCfg;

		const auto path = GetConfigPath();
		std::ifstream in(path);
		if (!in.good()) {
			spdlog::warn("[FB] Config not found: {} (using defaults)", path.string());
			newCfg.eventToTimeline[std::string(kStartEvent)] = "DEFAULT";
			g_cfg = std::move(newCfg);
			g_cfgLoaded = true;
			return;
		}

		std::string currentSection;
		std::string line;

		while (std::getline(in, line)) {
			StripInlineComment(line);
			TrimInPlace(line);
			if (line.empty()) {
				continue;
			}

			if (line.size() >= 3 && line.front() == '[' && line.back() == ']') {
				currentSection = line.substr(1, line.size() - 2);
				TrimInPlace(currentSection);
				continue;
			}

			// [General]
			if (IEquals(currentSection, "General")) {
				auto eq = line.find('=');
				if (eq == std::string::npos) {
					continue;
				}
				std::string key = line.substr(0, eq);
				std::string val = line.substr(eq + 1);
				TrimInPlace(key);
				TrimInPlace(val);

				if (IEquals(key, "bEnableHeadScaleTimelines")) {
					newCfg.enableTimelines = ParseBool(val, newCfg.enableTimelines);
				}
				else if (IEquals(key, "bResetOnPairEnd")) {
					newCfg.resetOnPairEnd = ParseBool(val, newCfg.resetOnPairEnd);
				}
				else if (IEquals(key, "bResetOnPairedStop")) {
					newCfg.resetOnPairedStop = ParseBool(val, newCfg.resetOnPairedStop);
				}
				continue;
			}

			// [EventToTimeline]
			if (IEquals(currentSection, "EventToTimeline")) {
				auto eq = line.find('=');
				if (eq == std::string::npos) {
					continue;
				}
				std::string key = line.substr(0, eq);
				std::string val = line.substr(eq + 1);
				TrimInPlace(key);
				TrimInPlace(val);
				if (!key.empty() && !val.empty()) {
					newCfg.eventToTimeline[key] = val;
				}
				continue;
			}

			// [CasterHeadScale] / [TargetHeadScale]
			const bool isCasterSection = IEquals(currentSection, "CasterHeadScale");
			const bool isTargetSection = IEquals(currentSection, "TargetHeadScale");
			if (isCasterSection || isTargetSection) {
				std::istringstream iss(line);
				std::string timeTok;
				std::string cmdTok;

				if (!(iss >> timeTok >> cmdTok)) {
					continue;
				}

				auto t = ParseFloat(timeTok);
				if (!t) {
					continue;
				}

				// Enforce your intended INI style:
				// - CasterHeadScale should NOT start with "2_"
				// - TargetHeadScale should start with "2_"
				if (isCasterSection && cmdTok.rfind("2_", 0) == 0) {
					spdlog::warn("[FB] INI: '{}' contains target-prefixed cmd '{}' (ignored)", currentSection, cmdTok);
					continue;
				}
				if (isTargetSection && cmdTok.rfind("2_", 0) != 0) {
					spdlog::warn("[FB] INI: '{}' contains non-target cmd '{}' (ignored)", currentSection, cmdTok);
					continue;
				}

				if (auto cmd = ParseCommandLine(*t, cmdTok)) {
					newCfg.timelines["DEFAULT"].push_back(*cmd);
				}
				continue;
			}

			// Unknown sections ignored
		}

		// Defaults if not provided
		if (newCfg.eventToTimeline.find(std::string(kStartEvent)) == newCfg.eventToTimeline.end()) {
			newCfg.eventToTimeline[std::string(kStartEvent)] = "DEFAULT";
		}

		for (auto& [name, cmds] : newCfg.timelines) {
			SortAndClamp(cmds);
		}

		spdlog::info("[FB] Config loaded: enableTimelines={} resetOnPairEnd={} resetOnPairedStop={} eventMaps={} timelines={}",
			newCfg.enableTimelines,
			newCfg.resetOnPairEnd,
			newCfg.resetOnPairedStop,
			newCfg.eventToTimeline.size(),
			newCfg.timelines.size());

		g_cfg = std::move(newCfg);
		g_cfgLoaded = true;
	}

	static const Config& GetConfig()
	{
		std::lock_guard _{ g_cfgMutex };
		if (!g_cfgLoaded) {
			LoadConfigLocked();
		}
		return g_cfg;
	}

	// =========================
	// Target resolution
	// =========================
	static RE::ActorHandle FindLikelyPairedTarget(RE::Actor* caster)
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

		// Prefer same cell to reduce wrong picks in busy areas
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
			spdlog::info("[FB] TargetResolve: caster='{}' -> target='{}' dist={}",
				caster->GetName(),
				best->GetName(),
				std::sqrt(bestDist2));
			return best->CreateRefHandle();
		}

		spdlog::info("[FB] TargetResolve: caster='{}' -> no target found", caster->GetName());
		return {};
	}

	// =========================
	// Per-actor token + remembered target + debounce
	// =========================
	struct ActorState
	{
		std::uint64_t token{ 0 };
		RE::ActorHandle lastTarget{};
		std::chrono::steady_clock::time_point lastStart{};
	};

	std::mutex g_stateMutex;
	std::unordered_map<std::uint32_t, ActorState> g_actorState;  // caster FormID -> state

	static std::uint64_t BumpTokenForCaster(RE::Actor* caster)
	{
		const auto formID = caster->GetFormID();
		std::lock_guard _{ g_stateMutex };
		auto& st = g_actorState[formID];
		++st.token;
		return st.token;
	}

	static bool IsTokenCurrent(std::uint32_t casterFormID, std::uint64_t token)
	{
		std::lock_guard _{ g_stateMutex };
		auto it = g_actorState.find(casterFormID);
		return it != g_actorState.end() && it->second.token == token;
	}

	static void SetLastTarget(std::uint32_t casterFormID, RE::ActorHandle target)
	{
		std::lock_guard _{ g_stateMutex };
		g_actorState[casterFormID].lastTarget = target;
	}

	static RE::ActorHandle GetLastTarget(std::uint32_t casterFormID)
	{
		std::lock_guard _{ g_stateMutex };
		auto it = g_actorState.find(casterFormID);
		if (it == g_actorState.end()) {
			return {};
		}
		return it->second.lastTarget;
	}

	static bool ShouldDebounceStart(std::uint32_t casterFormID)
	{
		std::lock_guard _{ g_stateMutex };
		auto& st = g_actorState[casterFormID];

		const auto now = std::chrono::steady_clock::now();
		if (st.lastStart.time_since_epoch().count() != 0) {
			const float dt = std::chrono::duration<float>(now - st.lastStart).count();
			if (dt < kStartDebounceSeconds) {
				return true;
			}
		}
		st.lastStart = now;
		return false;
	}

	// =========================
	// Timeline start + schedule
	// =========================
	static void StartTimelineForCaster(RE::Actor* caster, std::string_view startEvent)
	{
		if (!caster) {
			return;
		}

		const auto& cfg = GetConfig();
		if (!cfg.enableTimelines) {
			return;
		}

		auto itMap = cfg.eventToTimeline.find(std::string(startEvent));
		if (itMap == cfg.eventToTimeline.end()) {
			spdlog::info("[FB] Timeline: no mapping for startEvent='{}'", std::string(startEvent));
			return;
		}

		const std::string& timelineName = itMap->second;
		auto itTl = cfg.timelines.find(timelineName);
		if (itTl == cfg.timelines.end() || itTl->second.empty()) {
			spdlog::info("[FB] Timeline: timeline '{}' has no commands", timelineName);
			return;
		}

		const std::uint32_t casterFormID = caster->GetFormID();

		// Debounce duplicate starts (common if event fires on multiple graphs)
		if (ShouldDebounceStart(casterFormID)) {
			spdlog::info("[FB] Timeline: debounced duplicate start for actor='{}'", caster->GetName());
			return;
		}

		// Determine target once at start
		const auto casterHandle = caster->CreateRefHandle();
		const auto targetHandle = FindLikelyPairedTarget(caster);

		// New token cancels any previous schedule
		const std::uint64_t token = BumpTokenForCaster(caster);
		SetLastTarget(casterFormID, targetHandle);

		// Copy commands to avoid lifetime issues
		const auto commands = itTl->second;

		spdlog::info("[FB] Timeline start: caster='{}' startEvent='{}' timeline='{}' cmds={} token={}",
			caster->GetName(),
			std::string(startEvent),
			timelineName,
			commands.size(),
			token);

		for (const auto& cmd : commands) {
			std::thread([casterHandle, targetHandle, casterFormID, token, cmd]() {
				if (cmd.timeSeconds > 0.0f) {
					std::this_thread::sleep_for(std::chrono::duration<float>(cmd.timeSeconds));
				}
				if (!IsTokenCurrent(casterFormID, token)) {
					return;
				}

				switch (cmd.type) {
				case CommandType::kHeadScale:
					if (cmd.target == TargetKind::kCaster) {
						ApplyHeadScaleHandle(casterHandle, cmd.scale);
					}
					else {
						ApplyHeadScaleHandle(targetHandle, cmd.scale);
					}
					break;
				default:
					break;
				}
				}).detach();
		}
	}

	static void CancelAndReset(RE::Actor* caster)
	{
		if (!caster) {
			return;
		}

		const std::uint32_t casterFormID = caster->GetFormID();

		// Cancel scheduled actions
		(void)BumpTokenForCaster(caster);

		// Reset caster
		ApplyHeadScaleHandle(caster->CreateRefHandle(), kResetScale);

		// Reset last-known target
		const auto targetHandle = GetLastTarget(casterFormID);
		ApplyHeadScaleHandle(targetHandle, kResetScale);
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
			const auto& cfg = GetConfig();

			// Reset reinforcement on end/stop events
			if ((cfg.resetOnPairEnd && tag == kPairEndEvent) ||
				(cfg.resetOnPairedStop && tag == kPairedStopEvent)) {
				spdlog::info("[FB] '{}' on '{}' -> cancel + reset caster/target head scale",
					std::string(tag), caster->GetName());
				CancelAndReset(caster);
				return RE::BSEventNotifyControl::kContinue;
			}

			// Start timeline on FB_HeadScale
			if (tag == kStartEvent) {
				StartTimelineForCaster(caster, tag);
				return RE::BSEventNotifyControl::kContinue;
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

void HeadScale(RE::Actor* actor, float scale)
{
	// Legacy compatibility
	if (actor) {
		ApplyHeadScaleHandle(actor->CreateRefHandle(), scale);
	}
}

void LoadHeadScaleConfig()
{
	// Optional eager-load hook (call from FullBodiedPlugin.cpp)
	(void)GetConfig();
}
