// AnimationEvents.cpp
//
// FullBodiedPlugin – FB INI timeline parser (Scale system, expanding)
//
// INI file name: FullBodiedIni.ini
// Preferred: Data\FullBodiedIni.ini
// Fallback:  Data\SKSE\Plugins\FullBodiedIni.ini
//
// Event mapping:
//   [EventToTimeline]
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
//   FBVisible_<NodeKey>(true/false) etc (parser already structured for this)
//
// Reset reinforcement:
//   PairEnd and NPCPairedStop cancel pending tasks and reset caster + target (all nodes touched).

#include "AnimationEvents.h"
#include "FBScaler.h"

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
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

	// Command namespace prefix
	static constexpr std::string_view kFBPrefix = "FB";

	// Current supported function:
	static constexpr std::string_view kFuncScale = "FBScale";  // "FBScale_<Node>(x)"

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
		auto p = std::min(p1 == std::string::npos ? s.size() : p1, p2 == std::string::npos ? s.size() : p2);
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
	enum class TargetKind
	{
		kCaster,
		kTarget
	};

	enum class CommandType
	{
		kScale
		// future: kVisible, kSound, kFace, kRM, etc.
	};

	struct Command
	{
		TargetKind target{ TargetKind::kCaster };
		CommandType type{ CommandType::kScale };
		float timeSeconds{ 0.0f };

		// nodeName points to FB::Scaler::kNode* constants (static lifetime).
		std::string_view nodeName{};

		// payload
		float scale{ 1.0f };
	};

	struct DebugConfig
	{
		bool strictIni{ false };
		bool logTimelineStart{ false };
		bool logTargetResolve{ false };
		bool logOps{ false };
	};

	struct Config
	{
		bool enableTimelines{ true };
		bool resetOnPairEnd{ true };
		bool resetOnPairedStop{ true };
		DebugConfig dbg{};

		// Event tag -> Timeline name (e.g. FBEvent -> paired_huga.hkx)
		std::unordered_map<std::string, std::string> eventToTimeline;

		// Timeline name -> commands
		std::unordered_map<std::string, std::vector<Command>> timelines;
	};

	std::mutex g_cfgMutex;
	Config     g_cfg;
	bool       g_cfgLoaded = false;

	// =========================
	// Config paths
	// =========================
	static std::filesystem::path GetConfigPathPreferred()
	{
		return std::filesystem::path("Data") / "FullBodiedIni.ini";
	}

	static std::filesystem::path GetConfigPathFallback()
	{
		return std::filesystem::path("Data") / "SKSE" / "Plugins" / "FullBodiedIni.ini";
	}

	// =========================
	// Node mapping
	// =========================
	// Map author-friendly NodeKeys to canonical skeleton node names.
	// Keep keys stable; this is your “public API” for authors.
	static std::optional<std::string_view> ResolveNodeKey(std::string_view key)
	{
		// NOTE: keys are case-sensitive as written; you can relax later if you want.
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

		return std::nullopt;
	}

	// =========================
	// Parsing tokens
	// =========================
	// Parses: FBScale_<NodeKey>(<float>)
	// Returns: (nodeName, scale)
	struct ParsedScale
	{
		std::string_view nodeName{};
		float scale{ 1.0f };
	};

	static std::optional<ParsedScale> TryParseScaleToken(std::string_view tok, bool strictIni)
	{
		// Must begin with "FBScale_"
		const std::string_view prefix = "FBScale_";
		if (tok.rfind(prefix, 0) != 0) {
			return std::nullopt;
		}

		// Find '(' and ')'
		const size_t open = tok.find('(');
		const size_t close = tok.rfind(')');
		if (open == std::string_view::npos || close == std::string_view::npos || close <= open + 1 || close != tok.size() - 1) {
			if (strictIni) {
				spdlog::warn("[FB] INI: bad call syntax '{}'", std::string(tok));
			}
			return std::nullopt;
		}

		// NodeKey is between prefix and '(' and must include underscore format: FBScale_<NodeKey>(...)
		if (open <= prefix.size()) {
			if (strictIni) {
				spdlog::warn("[FB] INI: missing NodeKey in '{}'", std::string(tok));
			}
			return std::nullopt;
		}

		const std::string_view nodeKey = tok.substr(prefix.size(), open - prefix.size());

		auto nodeName = ResolveNodeKey(nodeKey);
		if (!nodeName) {
			if (strictIni) {
				spdlog::warn("[FB] INI: unknown NodeKey '{}' in '{}'", std::string(nodeKey), std::string(tok));
			}
			return std::nullopt;
		}

		std::string arg{ tok.substr(open + 1, close - open - 1) };
		TrimInPlace(arg);

		auto f = ParseFloat(arg);
		if (!f) {
			if (strictIni) {
				spdlog::warn("[FB] INI: FBScale arg not a float '{}' in '{}'", arg, std::string(tok));
			}
			return std::nullopt;
		}

		ParsedScale out;
		out.nodeName = *nodeName;
		out.scale = *f;
		return out;
	}

	// Parses one command token in context of section scope.
	static std::optional<Command> ParseCommand(float t, const std::string& cmdTok, TargetKind who, bool strictIni)
	{
		// Enforce 2_ prefix rule
		if (who == TargetKind::kTarget) {
			if (cmdTok.rfind(std::string(kTargetPrefix), 0) != 0) {
				if (strictIni) {
					spdlog::warn("[FB] INI: Target section requires '2_' prefix, got '{}'", cmdTok);
				}
				return std::nullopt;
			}
			std::string_view inner{ cmdTok.c_str() + kTargetPrefix.size(), cmdTok.size() - kTargetPrefix.size() };

			// Only implement FBScale_* now
			if (auto parsed = TryParseScaleToken(inner, strictIni)) {
				Command c;
				c.target = TargetKind::kTarget;
				c.type = CommandType::kScale;
				c.timeSeconds = t;
				c.nodeName = parsed->nodeName;
				c.scale = parsed->scale;
				return c;
			}

			// Future: FBVisible_*, FBSound_*, etc. (ignored for now)
			if (strictIni) {
				spdlog::warn("[FB] INI: unsupported/unknown target token '{}'", cmdTok);
			}
			return std::nullopt;
		}
		else {
			// Caster section must NOT have 2_
			if (cmdTok.rfind(std::string(kTargetPrefix), 0) == 0) {
				if (strictIni) {
					spdlog::warn("[FB] INI: Caster section must NOT use '2_' prefix, got '{}'", cmdTok);
				}
				return std::nullopt;
			}

			if (auto parsed = TryParseScaleToken(cmdTok, strictIni)) {
				Command c;
				c.target = TargetKind::kCaster;
				c.type = CommandType::kScale;
				c.timeSeconds = t;
				c.nodeName = parsed->nodeName;
				c.scale = parsed->scale;
				return c;
			}

			if (strictIni) {
				spdlog::warn("[FB] INI: unsupported/unknown caster token '{}'", cmdTok);
			}
			return std::nullopt;
		}
	}

	// =========================
	// FB section name (2-part)
	// =========================
	//   FB:<timeline>|<Caster/Target>
	struct FBSection
	{
		std::string timeline;
		TargetKind who{ TargetKind::kCaster };
		bool supported{ false };
	};

	static std::optional<FBSection> ParseFBSectionName(const std::string& section, bool strictIni)
	{
		if (section.rfind("FB:", 0) != 0) {
			return std::nullopt;
		}

		const std::string rest = section.substr(3);
		auto parts = Split(rest, '|');
		if (parts.size() != 2) {
			if (strictIni) {
				spdlog::warn("[FB] INI: FB section expects 2 parts: '[FB:<timeline>|Caster/Target]' got '[{}]'", section);
			}
			return std::nullopt;
		}

		FBSection out;
		out.timeline = parts[0];
		TrimInPlace(out.timeline);

		std::string who = parts[1];
		TrimInPlace(who);

		if (IEquals(who, "Caster")) {
			out.who = TargetKind::kCaster;
		}
		else if (IEquals(who, "Target")) {
			out.who = TargetKind::kTarget;
		}
		else {
			if (strictIni) {
				spdlog::warn("[FB] INI: Unknown scope '{}' in section '[{}]'", who, section);
			}
			return std::nullopt;
		}

		// Currently all FB:* sections are supported as containers; individual tokens decide action.
		out.supported = !out.timeline.empty();
		return out;
	}

	// =========================
	// Sorting + clamp
	// =========================
	static void SortAndClamp(std::vector<Command>& cmds)
	{
		for (auto& c : cmds) {
			if (c.timeSeconds < 0.0f) {
				c.timeSeconds = 0.0f;
			}
			// allow >1.0 for comedic/experimental scaling, clamp reasonable
			c.scale = std::clamp(c.scale, 0.0f, 5.0f);
		}
		std::sort(cmds.begin(), cmds.end(), [](const auto& a, const auto& b) { return a.timeSeconds < b.timeSeconds; });
	}

	// =========================
	// Load INI
	// =========================
	static void LoadConfigLocked()
	{
		Config newCfg;

		auto path = GetConfigPathPreferred();
		std::ifstream in(path);

		if (!in.good()) {
			auto fb = GetConfigPathFallback();
			in.open(fb);
			if (in.good()) {
				path = fb;
				spdlog::info("[FB] Using fallback config path: {}", path.string());
			}
		}

		if (!in.good()) {
			spdlog::warn("[FB] Config not found: {} (and fallback missing: {}) - using defaults",
				GetConfigPathPreferred().string(),
				GetConfigPathFallback().string());
			g_cfg = std::move(newCfg);
			g_cfgLoaded = true;
			return;
		}

		spdlog::info("[FB] Config path: {}", path.string());

		std::string currentSection;
		std::string line;

		std::optional<FBSection> activeFBSection;

		while (std::getline(in, line)) {
			StripInlineComment(line);
			TrimInPlace(line);
			if (line.empty()) {
				continue;
			}

			// Section header
			if (line.size() >= 3 && line.front() == '[' && line.back() == ']') {
				currentSection = line.substr(1, line.size() - 2);
				TrimInPlace(currentSection);

				activeFBSection = ParseFBSectionName(currentSection, newCfg.dbg.strictIni);
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
					continue;
				}
			}

			// [Debug]
			if (IEquals(currentSection, "Debug")) {
				auto eq = line.find('=');
				if (eq == std::string::npos) {
					continue;
				}
				std::string key = line.substr(0, eq);
				std::string val = line.substr(eq + 1);
				TrimInPlace(key);
				TrimInPlace(val);

				if (IEquals(key, "bStrictIni")) {
					newCfg.dbg.strictIni = ParseBool(val, newCfg.dbg.strictIni);
				}
				else if (IEquals(key, "bLogTimelineStart")) {
					newCfg.dbg.logTimelineStart = ParseBool(val, newCfg.dbg.logTimelineStart);
				}
				else if (IEquals(key, "bLogTargetResolve")) {
					newCfg.dbg.logTargetResolve = ParseBool(val, newCfg.dbg.logTargetResolve);
				}
				else if (IEquals(key, "bLogOps") || IEquals(key, "bLogHeadScale")) {
					newCfg.dbg.logOps = ParseBool(val, newCfg.dbg.logOps);
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

			// FB:* sections
			if (activeFBSection && activeFBSection->supported && !activeFBSection->timeline.empty()) {
				std::istringstream iss(line);
				std::string timeTok;
				std::string cmdTok;
				if (!(iss >> timeTok >> cmdTok)) {
					continue;
				}

				auto t = ParseFloat(timeTok);
				if (!t) {
					if (newCfg.dbg.strictIni) {
						spdlog::warn("[FB] INI: bad time token '{}' in section '{}'", timeTok, currentSection);
					}
					continue;
				}

				if (auto cmd = ParseCommand(*t, cmdTok, activeFBSection->who, newCfg.dbg.strictIni)) {
					newCfg.timelines[activeFBSection->timeline].push_back(*cmd);
				}
				continue;
			}

			// Unknown content ignored
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
	// Per-caster cancellation token + last target + debounce + touched nodes
	// =========================
	struct ActorState
	{
		std::uint64_t token{ 0 };
		RE::ActorHandle lastTarget{};
		std::chrono::steady_clock::time_point lastStart{};

		// Nodes touched on caster/target during the current timeline; used to reset only what we changed.
		std::unordered_set<std::string_view> casterTouched;
		std::unordered_set<std::string_view> targetTouched;
	};

	std::mutex g_stateMutex;
	std::unordered_map<std::uint32_t, ActorState> g_actorState;  // caster FormID -> state

	static std::uint64_t BumpTokenForCaster(RE::Actor* caster)
	{
		const auto formID = caster->GetFormID();
		std::lock_guard _{ g_stateMutex };
		auto& st = g_actorState[formID];
		++st.token;
		// new token implies new “run”; clear touched sets
		st.casterTouched.clear();
		st.targetTouched.clear();
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
		return it == g_actorState.end() ? RE::ActorHandle{} : it->second.lastTarget;
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

	static void MarkTouched(std::uint32_t casterFormID, TargetKind who, std::string_view nodeName)
	{
		std::lock_guard _{ g_stateMutex };
		auto& st = g_actorState[casterFormID];
		if (who == TargetKind::kCaster) {
			st.casterTouched.insert(nodeName);
		}
		else {
			st.targetTouched.insert(nodeName);
		}
	}

	static std::vector<std::string_view> GetTouched(std::uint32_t casterFormID, TargetKind who)
	{
		std::lock_guard _{ g_stateMutex };
		std::vector<std::string_view> out;
		auto it = g_actorState.find(casterFormID);
		if (it == g_actorState.end()) {
			return out;
		}
		const auto& set = (who == TargetKind::kCaster) ? it->second.casterTouched : it->second.targetTouched;
		out.reserve(set.size());
		for (auto v : set) {
			out.push_back(v);
		}
		return out;
	}

	// =========================
	// Timeline start + schedule
	// =========================
	static void StartTimelineForCaster(RE::Actor* caster, std::string_view startEventTag)
	{
		if (!caster) {
			return;
		}

		const auto& cfg = GetConfig();
		if (!cfg.enableTimelines) {
			return;
		}

		auto itMap = cfg.eventToTimeline.find(std::string(startEventTag));
		if (itMap == cfg.eventToTimeline.end()) {
			return;  // not a start event we care about
		}

		const std::string& timelineName = itMap->second;
		auto itTl = cfg.timelines.find(timelineName);
		if (itTl == cfg.timelines.end() || itTl->second.empty()) {
			if (cfg.dbg.logTimelineStart) {
				spdlog::info("[FB] Timeline: '{}' has no commands (event='{}')", timelineName, std::string(startEventTag));
			}
			return;
		}

		const std::uint32_t casterFormID = caster->GetFormID();
		if (ShouldDebounceStart(casterFormID)) {
			if (cfg.dbg.logTimelineStart) {
				spdlog::info("[FB] Timeline: debounced duplicate start for actor='{}' event='{}'",
					caster->GetName(), std::string(startEventTag));
			}
			return;
		}

		const auto casterHandle = caster->CreateRefHandle();
		const auto targetHandle = FindLikelyPairedTarget(caster, cfg.dbg.logTargetResolve);

		const std::uint64_t token = BumpTokenForCaster(caster);
		SetLastTarget(casterFormID, targetHandle);

		const auto commands = itTl->second;

		if (cfg.dbg.logTimelineStart) {
			spdlog::info("[FB] Timeline start: caster='{}' event='{}' timeline='{}' cmds={} token={}",
				caster->GetName(),
				std::string(startEventTag),
				timelineName,
				commands.size(),
				token);
		}

		for (const auto& cmd : commands) {
			// Mark touched immediately so reset will restore even if interrupted before command executes.
			MarkTouched(casterFormID, cmd.target, cmd.nodeName);

			std::thread([casterHandle, targetHandle, casterFormID, token, cmd, logOps = cfg.dbg.logOps]() {
				if (cmd.timeSeconds > 0.0f) {
					std::this_thread::sleep_for(std::chrono::duration<float>(cmd.timeSeconds));
				}
				if (!IsTokenCurrent(casterFormID, token)) {
					return;
				}

				switch (cmd.type) {
				case CommandType::kScale:
					if (cmd.target == TargetKind::kCaster) {
						FB::Scaler::SetNodeScale(casterHandle, cmd.nodeName, cmd.scale, logOps);
					}
					else {
						FB::Scaler::SetNodeScale(targetHandle, cmd.nodeName, cmd.scale, logOps);
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

		const auto& cfg = GetConfig();
		const std::uint32_t casterFormID = caster->GetFormID();
		(void)BumpTokenForCaster(caster);

		const auto casterHandle = caster->CreateRefHandle();
		const auto targetHandle = GetLastTarget(casterFormID);

		// Reset only nodes we touched this run (future-proof and avoids surprise resets).
		for (auto node : GetTouched(casterFormID, TargetKind::kCaster)) {
			FB::Scaler::SetNodeScale(casterHandle, node, 1.0f, cfg.dbg.logOps);
		}
		for (auto node : GetTouched(casterFormID, TargetKind::kTarget)) {
			FB::Scaler::SetNodeScale(targetHandle, node, 1.0f, cfg.dbg.logOps);
		}
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

			if ((cfg.resetOnPairEnd && tag == kPairEndEvent) ||
				(cfg.resetOnPairedStop && tag == kPairedStopEvent)) {
				spdlog::info("[FB] '{}' on '{}' -> cancel + reset (touched nodes only)",
					std::string(tag), caster->GetName());
				CancelAndReset(caster);
				return RE::BSEventNotifyControl::kContinue;
			}

			// Start timelines based on EventToTimeline mapping
			StartTimelineForCaster(caster, tag);
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
	std::lock_guard _{ g_cfgMutex };
	g_cfgLoaded = false;  // force reload
	LoadConfigLocked();
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
	const auto& cfg = GetConfig();
	FB::Scaler::SetNodeScale(actor->CreateRefHandle(), FB::Scaler::kNodeHead, scale, cfg.dbg.logOps);
}
