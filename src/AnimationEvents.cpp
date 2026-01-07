// AnimationEvents.cpp
//
// FullBodiedPlugin – FB INI timeline parser (Scale system, expanding)
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
#include "FBScaler.h"
#include "ActorManager.h"

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

	// Debounce is an event-level policy; keep it in AnimationEvents (not ActorManager).
		// Debounce is an event-level policy; keep it in AnimationEvents (not ActorManager).
	// IMPORTANT: debounce must apply only to *mapped start tags*, otherwise random vanilla tags
	// will block the real trigger (as seen in your log).
	std::mutex g_debounceMutex;

	// Keyed by (casterFormID + startEventTagHash) so different mapped tags don't block each other.
	std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> g_lastStartByKey;

	static std::uint32_t HashTag32(std::string_view s)
	{
		// FNV-1a 32-bit
		std::uint32_t h = 2166136261u;
		for (unsigned char c : s) {
			h ^= c;
			h *= 16777619u;
		}
		return h;
	}

	static std::uint64_t MakeDebounceKey(std::uint32_t casterFormID, std::string_view startEventTag)
	{
		const std::uint64_t hi = static_cast<std::uint64_t>(casterFormID) << 32;
		const std::uint64_t lo = static_cast<std::uint64_t>(HashTag32(startEventTag));
		return hi | lo;
	}

	static bool ShouldDebounceStart(std::uint32_t casterFormID, std::string_view startEventTag)
	{
		std::lock_guard _{ g_debounceMutex };

		const auto now = std::chrono::steady_clock::now();
		const auto key = MakeDebounceKey(casterFormID, startEventTag);

		auto& last = g_lastStartByKey[key];

		if (last.time_since_epoch().count() != 0) {
			const float dt = std::chrono::duration<float>(now - last).count();
			if (dt < kStartDebounceSeconds) {
				return true;
			}
		}

		last = now;
		return false;
	}


	struct DebugConfig
	{
		bool strictIni{ true };
		bool logOps{ true };
		bool logIni{ true };
		bool logTargetResolve{ false };
		bool logTimelineStart{ true };  // used by parser + timeline start log gating
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
		std::unordered_map<std::string, std::vector<TimedCommand>> timelines;
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
	static std::optional<std::string_view> ResolveNodeKey(std::string_view key)
	{
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

		const size_t open = tok.find('(');
		const size_t close = tok.rfind(')');
		if (open == std::string_view::npos || close == std::string_view::npos ||
			close <= open + 1 || close != tok.size() - 1) {
			if (strictIni) {
				spdlog::warn("[FB] INI: bad call syntax '{}'", std::string(tok));
			}
			return std::nullopt;
		}

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
	static std::optional<TimedCommand> ParseCommand(float t, const std::string& cmdTok, TargetKind who, bool strictIni)
	{
		TimedCommand out{};
		out.timeSeconds = t;

		if (who == TargetKind::kTarget) {
			// Target section must have 2_
			if (cmdTok.rfind(std::string(kTargetPrefix), 0) != 0) {
				if (strictIni) {
					spdlog::warn("[FB] INI: Target section requires '2_' prefix, got '{}'", cmdTok);
				}
				return std::nullopt;
			}

			std::string_view inner{ cmdTok.c_str() + kTargetPrefix.size(), cmdTok.size() - kTargetPrefix.size() };

			if (auto parsed = TryParseScaleToken(inner, strictIni)) {
				out.target = TargetKind::kTarget;
				out.nodeKey = parsed->nodeName;
				out.scale = parsed->scale;
				return out;
			}

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
				out.target = TargetKind::kCaster;
				out.nodeKey = parsed->nodeName;
				out.scale = parsed->scale;
				return out;
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

		out.supported = !out.timeline.empty();
		return out;
	}

	// =========================
	// Sorting + clamp
	// =========================
	static void SortAndClamp(std::vector<TimedCommand>& cmds)
	{
		for (auto& c : cmds) {
			if (c.timeSeconds < 0.0f) {
				c.timeSeconds = 0.0f;
			}
			// allow >1.0 for comedic/experimental scaling, clamp reasonable
			c.scale = std::clamp(c.scale, 0.0f, 5.0f);
		}
		std::sort(cmds.begin(), cmds.end(),
			[](const auto& a, const auto& b) { return a.timeSeconds < b.timeSeconds; });
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

		// Read all lines so we can do 2 passes:
		// Pass 1: parse General/Debug/EventToTimeline (get strictIni/log settings right)
		// Pass 2: parse FB:* sections using the correct strictIni setting
		std::vector<std::string> lines;
		{
			std::string line;
			while (std::getline(in, line)) {
				lines.push_back(std::move(line));
			}
		}

		auto parse_non_fb_sections = [&](const std::string& currentSection, const std::string& lineRaw) {
			std::string line = lineRaw;
			StripInlineComment(line);
			TrimInPlace(line);
			if (line.empty()) {
				return;
			}

			auto eq = line.find('=');
			if (eq == std::string::npos) {
				return;
			}

			std::string key = line.substr(0, eq);
			std::string val = line.substr(eq + 1);
			TrimInPlace(key);
			TrimInPlace(val);

			// [General]
			if (IEquals(currentSection, "General")) {
				if (IEquals(key, "enableTimelines") ||
					IEquals(key, "bEnableHeadScaleTimelines") ||
					IEquals(key, "bEnableTimelines")) {
					newCfg.enableTimelines = ParseBool(val, newCfg.enableTimelines);
				}
				else if (IEquals(key, "resetOnPairEnd")) {
					newCfg.resetOnPairEnd = ParseBool(val, newCfg.resetOnPairEnd);
				}
				else if (IEquals(key, "resetOnPairedStop")) {
					newCfg.resetOnPairedStop = ParseBool(val, newCfg.resetOnPairedStop);
				}
				return;
			}

			// [Debug]
			if (IEquals(currentSection, "Debug")) {
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
				else if (IEquals(key, "bLogIni")) {
					newCfg.dbg.logIni = ParseBool(val, newCfg.dbg.logIni);
				}
				return;
			}

			// [EventToTimeline] (also supports legacy [EventMap])
			if (IEquals(currentSection, "EventToTimeline") || IEquals(currentSection, "EventMap")) {
				if (!key.empty() && !val.empty()) {
					newCfg.eventToTimeline[key] = val;
				}
				return;
			}
			};

		// ---- Pass 1 ----
		{
			std::string currentSection;

			for (const auto& raw : lines) {
				std::string line = raw;
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

				parse_non_fb_sections(currentSection, raw);
			}
		}

		// ---- Pass 2: FB sections ----
		{
			std::string currentSection;
			std::optional<FBSection> activeFBSection;

			for (const auto& raw : lines) {
				std::string line = raw;
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
				}
			}
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
	// Timeline start + dispatch
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

		const std::uint32_t casterFormID = caster->GetFormID();

		// Map event tag -> timeline name
		auto itMap = cfg.eventToTimeline.find(std::string(startEventTag));
		if (itMap == cfg.eventToTimeline.end()) {
			// Not a start tag; ignore without touching debounce.
			return;
		}

		// Debounce duplicate starts ONLY for mapped start tags
		if (ShouldDebounceStart(casterFormID, startEventTag)) {
			if (cfg.dbg.logOps) {
				spdlog::info("[FB] Debounce: ignoring mapped start '{}' on '{}'",
					std::string(startEventTag), caster->GetName());
			}
			return;
		}


		const std::string_view timelineName{ itMap->second };

		// Fetch commands
		auto itTL = cfg.timelines.find(std::string(timelineName));
		if (itTL == cfg.timelines.end() || itTL->second.empty()) {
			if (cfg.dbg.logOps) {
				spdlog::info("[FB] Timeline '{}' has no commands", std::string(timelineName));
			}
			return;
		}

		// Resolve target (existing logic)
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
			itTL->second,  // copy into ActorManager to ensure lifetime across detached threads
			cfg.dbg.logOps);
	}

	static void CancelAndReset(RE::Actor* caster)
	{
		if (!caster) {
			return;
		}

		const auto& cfg = GetConfig();
		const std::uint32_t casterFormID = caster->GetFormID();

		FB::ActorManager::CancelAndReset(
			caster->CreateRefHandle(),
			casterFormID,
			cfg.dbg.logOps);
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

				if (cfg.dbg.logOps) {
					spdlog::info("[FB] '{}' on '{}' -> cancel + reset (touched nodes only)",
						std::string(tag), caster->GetName());
				}

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
