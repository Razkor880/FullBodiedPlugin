#include "AnimationEvents.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <fstream>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"
#include "spdlog/spdlog.h"

#include "FBCommandRouter.h"
#include "FBScaler.h"
#include "FBVis.h"

namespace
{
	using TargetKind = FB::CommandRouter::TargetKind;
	using CommandType = FB::CommandRouter::CommandType;
	using Command = FB::CommandRouter::Command;
	using RouterContext = FB::CommandRouter::Context;

	// Default max range (in game units) for distance-based target resolution.
	// 1 unit ≈ 1/70th foot (Skyrim units). 250 is "very close" for a paired animation.
	constexpr float kDefaultTargetResolveMaxDist = 250.0f;

	struct DebugCfg
	{
		bool logOps{ true };
		bool strictIni{ false };
	};

	struct Config
	{
		bool enableTimelines{ true };
		bool resetOnPairEnd{ true };
		bool resetOnPairedStop{ true };

		float targetResolveMaxDist{ kDefaultTargetResolveMaxDist };

		DebugCfg dbg{};

		// Animation event tag -> timeline name
		std::unordered_map<std::string, std::string> eventToTimeline;

		// timeline name -> commands
		std::unordered_map<std::string, std::vector<Command>> timelines;

		// vis group key -> list of exact object names
		std::unordered_map<std::string, std::vector<std::string>> visGroups;
	};

	static std::shared_mutex g_cfgLock;
	static Config g_cfg;

	static std::string Trim(std::string s)
	{
		auto notSpace = [](unsigned char c) { return !std::isspace(c); };

		s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
		s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
		return s;
	}

	static bool IEquals(std::string_view a, std::string_view b)
	{
		if (a.size() != b.size()) {
			return false;
		}
		for (std::size_t i = 0; i < a.size(); ++i) {
			if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
				return false;
			}
		}
		return true;
	}

	static bool TryParseBool(std::string_view s, bool& out)
	{
		std::string t(s);
		t = Trim(std::move(t));
		if (IEquals(t, "true") || t == "1" || IEquals(t, "yes") || IEquals(t, "on")) {
			out = true;
			return true;
		}
		if (IEquals(t, "false") || t == "0" || IEquals(t, "no") || IEquals(t, "off")) {
			out = false;
			return true;
		}
		return false;
	}

	static Config GetConfigCopy()
	{
		std::shared_lock lk(g_cfgLock);
		return g_cfg;
	}

	static std::string GetConfigPath()
	{
		// Skyrim "Data" relative path
		return "Data\\FullBodiedIni.ini";
	}

	static bool ParseFBSection(std::string_view header, std::string& outTimelineName, TargetKind& outDefaultTarget)
	{
		// Expected:
		//   FB:<TimelineName>|Caster
		//   FB:<TimelineName>|Target
		//
		// Example:
		//   [FB:paired_huga.hkx|Caster]
		//   [FB:paired_huga.hkx|Target]

		if (!header.starts_with("FB:")) {
			return false;
		}
		header.remove_prefix(3);

		const auto pipe = header.find('|');
		if (pipe == std::string_view::npos) {
			return false;
		}

		std::string_view timeline = header.substr(0, pipe);
		std::string_view who = header.substr(pipe + 1);

		outTimelineName = std::string(Trim(std::string(timeline)));

		who = std::string_view(Trim(std::string(who)));
		if (IEquals(who, "Caster")) {
			outDefaultTarget = TargetKind::kCaster;
		} else if (IEquals(who, "Target")) {
			outDefaultTarget = TargetKind::kTarget;
		} else {
			return false;
		}

		return !outTimelineName.empty();
	}

	static bool TryParseScaleToken(std::string_view token, std::string& outKey, float& outScale)
	{
		// FBScale_<Key>(<float>)
		constexpr std::string_view kPrefix = "FBScale_";
		if (!token.starts_with(kPrefix)) {
			return false;
		}
		token.remove_prefix(kPrefix.size());

		const auto lparen = token.find('(');
		const auto rparen = token.rfind(')');
		if (lparen == std::string_view::npos || rparen == std::string_view::npos || rparen <= lparen) {
			return false;
		}

		outKey = std::string(Trim(std::string(token.substr(0, lparen))));

		const auto arg = Trim(std::string(token.substr(lparen + 1, rparen - lparen - 1)));
		try {
			outScale = std::stof(arg);
		} catch (...) {
			return false;
		}

		return !outKey.empty();
	}

	static bool TryParseVisToken(std::string_view token, std::string& outKey, bool& outVisible)
	{
		// FBVis_<Key>(true|false)
		// FBVisible_<Key>(true|false)  (alias)
		std::string_view prefix;
		if (token.starts_with("FBVis_")) {
			prefix = "FBVis_";
		} else if (token.starts_with("FBVisible_")) {
			prefix = "FBVisible_";
		} else {
			return false;
		}

		token.remove_prefix(prefix.size());

		const auto lparen = token.find('(');
		const auto rparen = token.rfind(')');
		if (lparen == std::string_view::npos || rparen == std::string_view::npos || rparen <= lparen) {
			return false;
		}

		outKey = std::string(Trim(std::string(token.substr(0, lparen))));

		const auto arg = Trim(std::string(token.substr(lparen + 1, rparen - lparen - 1)));
		if (!TryParseBool(arg, outVisible)) {
			return false;
		}

		return !outKey.empty();
	}

	static bool ParseCommandLine(
		const Config& cfg,
		std::string_view line,
		TargetKind defaultTarget,
		Command& outCmd)
	{
		// Expected:
		//   <timeSeconds> <Token>
		// Example:
		//   2.0 FBScale_Head(0.25)
		//   5.0 2_FBVis_LThigh(false)

		std::string s = Trim(std::string(line));
		if (s.empty() || s[0] == ';' || s[0] == '#') {
			return false;
		}

		std::istringstream iss(s);
		float timeSec = 0.0f;
		if (!(iss >> timeSec)) {
			if (cfg.dbg.strictIni) {
				spdlog::warn("[FB] Bad timeline line (no time): {}", s);
			}
			return false;
		}

		std::string token;
		if (!(iss >> token)) {
			if (cfg.dbg.strictIni) {
				spdlog::warn("[FB] Bad timeline line (no token): {}", s);
			}
			return false;
		}

		TargetKind effectiveTarget = defaultTarget;

		// Flexible rule:
		// - If token starts with 2_, force Target regardless of section.
		// - If token starts with 1_, force Caster (reserved/optional).
		if (token.rfind("2_", 0) == 0) {
			effectiveTarget = TargetKind::kTarget;
			token.erase(0, 2);
		} else if (token.rfind("1_", 0) == 0) {
			effectiveTarget = TargetKind::kCaster;
			token.erase(0, 2);
		}

		Command cmd{};
		cmd.timeSeconds = std::max(0.0f, timeSec);
		cmd.target = effectiveTarget;

		// Scale
		{
			std::string key;
			float scale = 1.0f;
			if (TryParseScaleToken(token, key, scale)) {
				cmd.type = CommandType::kScale;
				cmd.key = std::move(key);
				cmd.scale = scale;
				outCmd = std::move(cmd);
				return true;
			}
		}

		// Vis
		{
			std::string key;
			bool visible = true;
			if (TryParseVisToken(token, key, visible)) {
				cmd.type = CommandType::kVis;
				cmd.key = std::move(key);
				cmd.visible = visible;
				outCmd = std::move(cmd);
				return true;
			}
		}

		if (cfg.dbg.strictIni) {
			spdlog::warn("[FB] Unsupported token: {}", token);
		}

		return false;
	}

	// --------------------------
	// Runtime per-caster state
	// --------------------------

	struct PerCasterState
	{
		std::uint64_t token{ 0 };
		RE::ActorHandle lastTarget;

		std::unordered_set<std::string> casterScaleKeys;
		std::unordered_set<std::string> targetScaleKeys;
		std::unordered_set<std::string> casterVisKeys;
		std::unordered_set<std::string> targetVisKeys;
	};

	static std::mutex g_stateLock;
	static std::unordered_map<std::uint32_t, PerCasterState> g_state;

	static std::uint64_t GetToken(std::uint32_t casterFormID)
	{
		std::lock_guard lk(g_stateLock);
		auto it = g_state.find(casterFormID);
		return (it == g_state.end()) ? 0 : it->second.token;
	}

	static std::uint64_t BumpTokenAndClear(std::uint32_t casterFormID)
	{
		std::lock_guard lk(g_stateLock);
		auto& st = g_state[casterFormID];
		++st.token;

		st.casterScaleKeys.clear();
		st.targetScaleKeys.clear();
		st.casterVisKeys.clear();
		st.targetVisKeys.clear();

		return st.token;
	}

	struct ResetSnapshot
	{
		RE::ActorHandle lastTarget;

		std::vector<std::string> casterScaleKeys;
		std::vector<std::string> targetScaleKeys;
		std::vector<std::string> casterVisKeys;
		std::vector<std::string> targetVisKeys;
	};

	static ResetSnapshot ConsumeTouchedAndBump(std::uint32_t casterFormID)
	{
		std::lock_guard lk(g_stateLock);
		auto& st = g_state[casterFormID];

		ResetSnapshot snap;
		snap.lastTarget = st.lastTarget;

		snap.casterScaleKeys.assign(st.casterScaleKeys.begin(), st.casterScaleKeys.end());
		snap.targetScaleKeys.assign(st.targetScaleKeys.begin(), st.targetScaleKeys.end());
		snap.casterVisKeys.assign(st.casterVisKeys.begin(), st.casterVisKeys.end());
		snap.targetVisKeys.assign(st.targetVisKeys.begin(), st.targetVisKeys.end());

		++st.token;
		st.casterScaleKeys.clear();
		st.targetScaleKeys.clear();
		st.casterVisKeys.clear();
		st.targetVisKeys.clear();

		return snap;
	}

	static void SetLastTarget(std::uint32_t casterFormID, RE::ActorHandle target)
	{
		std::lock_guard lk(g_stateLock);
		g_state[casterFormID].lastTarget = target;
	}

	static RE::ActorHandle GetLastTarget(std::uint32_t casterFormID)
	{
		std::lock_guard lk(g_stateLock);
		auto it = g_state.find(casterFormID);
		return (it == g_state.end()) ? RE::ActorHandle{} : it->second.lastTarget;
	}

	static void MarkTouched(std::uint32_t casterFormID, TargetKind who, const Command& cmd)
	{
		std::lock_guard lk(g_stateLock);
		auto& st = g_state[casterFormID];

		if (cmd.type == CommandType::kScale) {
			if (who == TargetKind::kCaster) {
				st.casterScaleKeys.insert(cmd.key);
			} else {
				st.targetScaleKeys.insert(cmd.key);
			}
		} else if (cmd.type == CommandType::kVis) {
			if (who == TargetKind::kCaster) {
				st.casterVisKeys.insert(cmd.key);
			} else {
				st.targetVisKeys.insert(cmd.key);
			}
		}
	}

	// --------------------------
	// Target resolution (distance)
	// --------------------------

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

	static RE::Actor* UnwrapActor(const auto& v)
	{
		auto* refr = UnwrapRefr(v);
		return refr ? refr->As<RE::Actor>() : nullptr;
	}

	static float DistSqr(const RE::NiPoint3& a, const RE::NiPoint3& b)
	{
		const float dx = a.x - b.x;
		const float dy = a.y - b.y;
		const float dz = a.z - b.z;
		return dx * dx + dy * dy + dz * dz;
	}

	static RE::ActorHandle FindLikelyPairedTargetByDistance(const RE::Actor* caster, float maxDist, bool logOps)
	{
		if (!caster) {
			return {};
		}

		const float maxDistSqr = maxDist * maxDist;
		const auto casterPos = caster->GetPosition();

		RE::Actor* best = nullptr;
		float bestDistSqr = maxDistSqr;

		// Only high process actors are a good start.
		auto* lists = RE::ProcessLists::GetSingleton();
		if (!lists) {
			return {};
		}

		for (auto& h : lists->highActorHandles) {
			auto* a = UnwrapActor(h);
			if (!a || a == caster) {
				continue;
			}
			if (a->IsDead()) {
				continue;
			}
			// Paired target should almost always be loaded.
			if (!a->Is3DLoaded()) {
				continue;
			}

			const float d2 = DistSqr(casterPos, a->GetPosition());
			if (d2 < bestDistSqr) {
				bestDistSqr = d2;
				best = a;
			}
		}

		if (!best) {
			return {};
		}

		if (logOps) {
			spdlog::info("[FB] TargetResolve: caster='{}' -> target='{}' dist={}",
				caster->GetName(),
				best->GetName(),
				std::sqrt(bestDistSqr));
		}

		return best->CreateRefHandle();
	}

	// --------------------------
	// Timeline execution
	// --------------------------

	static void CancelAndReset(RE::Actor* caster)
	{
		if (!caster) {
			return;
		}

		const auto cfg = GetConfigCopy();
		if (!cfg.enableTimelines) {
			return;
		}

		const auto casterFormID = caster->GetFormID();
		const auto casterHandle = caster->CreateRefHandle();

		// Consume touched keys and bump token (cancels any pending commands).
		const auto snap = ConsumeTouchedAndBump(casterFormID);
		const auto targetHandle = snap.lastTarget;

		RouterContext ctx{ casterHandle, targetHandle, cfg.dbg.logOps };

		// Run resets on the game thread.
		if (auto* taskIF = SKSE::GetTaskInterface(); taskIF) {
			taskIF->AddTask([ctx, snap, logOps = cfg.dbg.logOps]() mutable {
				for (const auto& k : snap.casterScaleKeys) {
					FB::CommandRouter::ExecuteCommandNow(ctx, Command{ CommandType::kScale, TargetKind::kCaster, 0.0f, k, 1.0f, true });
				}
				for (const auto& k : snap.targetScaleKeys) {
					FB::CommandRouter::ExecuteCommandNow(ctx, Command{ CommandType::kScale, TargetKind::kTarget, 0.0f, k, 1.0f, true });
				}
				for (const auto& k : snap.casterVisKeys) {
					FB::CommandRouter::ExecuteCommandNow(ctx, Command{ CommandType::kVis, TargetKind::kCaster, 0.0f, k, 1.0f, true });
				}
				for (const auto& k : snap.targetVisKeys) {
					FB::CommandRouter::ExecuteCommandNow(ctx, Command{ CommandType::kVis, TargetKind::kTarget, 0.0f, k, 1.0f, true });
				}
				(void)logOps;
			});
		} else {
			for (const auto& k : snap.casterScaleKeys) {
				FB::CommandRouter::ExecuteCommandNow(ctx, Command{ CommandType::kScale, TargetKind::kCaster, 0.0f, k, 1.0f, true });
			}
			for (const auto& k : snap.targetScaleKeys) {
				FB::CommandRouter::ExecuteCommandNow(ctx, Command{ CommandType::kScale, TargetKind::kTarget, 0.0f, k, 1.0f, true });
			}
			for (const auto& k : snap.casterVisKeys) {
				FB::CommandRouter::ExecuteCommandNow(ctx, Command{ CommandType::kVis, TargetKind::kCaster, 0.0f, k, 1.0f, true });
			}
			for (const auto& k : snap.targetVisKeys) {
				FB::CommandRouter::ExecuteCommandNow(ctx, Command{ CommandType::kVis, TargetKind::kTarget, 0.0f, k, 1.0f, true });
			}
		}
	}

	static void StartTimelineForCaster(RE::Actor* caster, const std::string& eventTag)
	{
		if (!caster) {
			return;
		}

		const auto cfg = GetConfigCopy();
		if (!cfg.enableTimelines) {
			return;
		}

		const auto it = cfg.eventToTimeline.find(eventTag);
		if (it == cfg.eventToTimeline.end()) {
			return;
		}

		const auto tlIt = cfg.timelines.find(it->second);
		if (tlIt == cfg.timelines.end()) {
			if (cfg.dbg.strictIni) {
				spdlog::warn("[FB] Timeline '{}' not found for event '{}'", it->second, eventTag);
			}
			return;
		}

		const auto& commands = tlIt->second;
		const auto casterFormID = caster->GetFormID();
		const auto casterHandle = caster->CreateRefHandle();

		// Resolve target ONLY if we have any target commands.
		RE::ActorHandle targetHandle{};
		bool needsTarget = false;
		for (const auto& cmd : commands) {
			if (cmd.target == TargetKind::kTarget) {
				needsTarget = true;
				break;
			}
		}

		if (needsTarget) {
			targetHandle = FindLikelyPairedTargetByDistance(caster, cfg.targetResolveMaxDist, cfg.dbg.logOps);
			SetLastTarget(casterFormID, targetHandle);
		} else {
			// keep previous target if any
			targetHandle = GetLastTarget(casterFormID);
		}

		// Cancel any prior running timeline and clear touched (we'll re-mark below).
		const auto token = BumpTokenAndClear(casterFormID);

		// Mark touched now so reset is deterministic even if canceled mid-way.
		for (const auto& cmd : commands) {
			MarkTouched(casterFormID, cmd.target, cmd);
		}

		RouterContext ctx{ casterHandle, targetHandle, cfg.dbg.logOps };
		FB::CommandRouter::ScheduleCommands(ctx, commands, casterFormID, token, &GetToken);
	}

	// --------------------------
	// INI parsing
	// --------------------------

	static std::vector<std::string> SplitCSV(const std::string& s)
	{
		std::vector<std::string> out;
		std::string cur;
		std::istringstream iss(s);
		while (std::getline(iss, cur, ',')) {
			cur = Trim(std::move(cur));
			if (!cur.empty()) {
				out.push_back(std::move(cur));
			}
		}
		return out;
	}

	static void LoadConfigLocked()
	{
		Config newCfg{};
		const auto path = GetConfigPath();

		std::ifstream in(path);
		if (!in.good()) {
			spdlog::warn("[FB] Failed to open config: {}", path);
			return;
		}

		std::string currentSection;
		std::string currentTimelineName;
		TargetKind currentDefaultTarget = TargetKind::kCaster;

		std::string line;
		while (std::getline(in, line)) {
			line = Trim(std::move(line));
			if (line.empty() || line[0] == ';' || line[0] == '#') {
				continue;
			}

			// Section header
			if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
				currentSection = line.substr(1, line.size() - 2);
				currentTimelineName.clear();

				TargetKind who{};
				std::string tl;
				if (ParseFBSection(currentSection, tl, who)) {
					currentTimelineName = std::move(tl);
					currentDefaultTarget = who;
				}
				continue;
			}

			// Key=Value lines
			const auto eq = line.find('=');
			if (eq != std::string::npos) {
				const auto key = Trim(line.substr(0, eq));
				const auto val = Trim(line.substr(eq + 1));

				// [General]
				if (IEquals(currentSection, "General")) {
					if (IEquals(key, "enableTimelines")) {
						TryParseBool(val, newCfg.enableTimelines);
					} else if (IEquals(key, "resetOnPairEnd")) {
						TryParseBool(val, newCfg.resetOnPairEnd);
					} else if (IEquals(key, "resetOnPairedStop")) {
						TryParseBool(val, newCfg.resetOnPairedStop);
					} else if (IEquals(key, "logOps")) {
						TryParseBool(val, newCfg.dbg.logOps);
					} else if (IEquals(key, "strictIni")) {
						TryParseBool(val, newCfg.dbg.strictIni);
					} else if (IEquals(key, "targetResolveMaxDist")) {
						try { newCfg.targetResolveMaxDist = std::stof(val); } catch (...) {}
					}
					continue;
				}

				// [EventMap]
				if (IEquals(currentSection, "EventMap")) {
					if (!key.empty() && !val.empty()) {
						newCfg.eventToTimeline[key] = val;
					}
					continue;
				}

				// [VisGroups] or [FBVisGroups]
				if (IEquals(currentSection, "VisGroups") || IEquals(currentSection, "FBVisGroups")) {
					if (!key.empty() && !val.empty()) {
						newCfg.visGroups[key] = SplitCSV(val);
					}
					continue;
				}
			}

			// Timeline command lines in [FB:...|...]
			if (!currentTimelineName.empty()) {
				Command cmd{};
				if (ParseCommandLine(newCfg, line, currentDefaultTarget, cmd)) {
					newCfg.timelines[currentTimelineName].push_back(std::move(cmd));
				}
			}
		}

		// Publish config + vis groups
		{
			std::unique_lock lk(g_cfgLock);
			g_cfg = std::move(newCfg);
		}

		// Vis keeps its own group map so runtime doesn't have to pass cfg around.
		FB::Vis::SetGroups(GetConfigCopy().visGroups);

		const auto published = GetConfigCopy();
		spdlog::info("[FB] Config loaded: enableTimelines={} resetOnPairEnd={} resetOnPairedStop={} eventMaps={} timelines={} visGroups={}",
			published.enableTimelines,
			published.resetOnPairEnd,
			published.resetOnPairedStop,
			published.eventToTimeline.size(),
			published.timelines.size(),
			published.visGroups.size());
	}

	// --------------------------
	// Event sink
	// --------------------------

	class AnimationEventSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(
			const RE::BSAnimationGraphEvent* a_event,
			RE::BSTEventSource<RE::BSAnimationGraphEvent>*)
		{
			if (!a_event) {
				return RE::BSEventNotifyControl::kContinue;
			}

			auto* holderRefr = UnwrapRefr(a_event->holder);
			if (!holderRefr) {
				return RE::BSEventNotifyControl::kContinue;
			}

			auto* actor = holderRefr->As<RE::Actor>();
			if (!actor) {
				return RE::BSEventNotifyControl::kContinue;
			}

			const std::string eventTag = a_event->tag.c_str();

			// Start mapped timelines
			StartTimelineForCaster(actor, eventTag);

			// Resets
			const auto cfg = GetConfigCopy();
			if (cfg.resetOnPairEnd && eventTag == "PairEnd") {
				CancelAndReset(actor);
			}
			if (cfg.resetOnPairedStop && eventTag == "NPCpairedStop") {
				CancelAndReset(actor);
			}

			return RE::BSEventNotifyControl::kContinue;
		}
	};

	static AnimationEventSink g_animationEventSink;

		void RegisterAnimationEventSinkImpl(RE::Actor* actor)
	{
		if (!actor) {
			return;
		}

		RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
		actor->GetAnimationGraphManager(manager);
		if (!manager) {
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
}

	// If a header declares a global `RegisterAnimationEventSink(Actor*)`, implement it
	// here as a wrapper to avoid ambiguous unqualified lookup when this TU also has
	// internal helpers.
	void RegisterAnimationEventSink(RE::Actor* actor)
	{
		RegisterAnimationEventSinkImpl(actor);
	}

// Public API
namespace FB
{
	void ReloadConfig()
	{
		spdlog::info("[FB] Loading config: {}", GetConfigPath());

		// (Re)load into globals.
		LoadConfigLocked();
	}

	void RegisterAnimationEventSinkToPlayer()
	{
		auto* pc = RE::PlayerCharacter::GetSingleton();
		if (!pc) {
			return;
		}
		RegisterAnimationEventSink(pc);
	}
}
