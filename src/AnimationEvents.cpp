// AnimationEvents.cpp
//
// FullBodiedPlugin – FB INI timeline parser (Scale + Vis)
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
// Visibility (FBVis):
//   FBVis_<Key>(true/false)
//   2_FBVis_<Key>(true/false)
//
// Visibility groups:
//   [FBVisGroups]
//   Pelvis = 3BA_PelvisShape, MyOutfit_Pelvis

#include "AnimationEvents.h"

#include "FBScaler.h"
#include "FBVis.h"

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
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

#include <spdlog/spdlog.h>

namespace
{
	// =========================
	// Constants
	// =========================
	static constexpr float kTargetSearchRadius = 160.0f;  // tweak if needed
	static constexpr float kStartDebounceSeconds = 0.10f;
	static constexpr std::string_view kTargetPrefix = "2_";

	// =========================
	// Helpers (string)
	// =========================
	static void TrimInPlace(std::string& s)
	{
		auto is_ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
		while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) {
			s.erase(s.begin());
		}
		while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) {
			s.pop_back();
		}
	}

	static bool IEquals(std::string_view a, std::string_view b)
	{
		if (a.size() != b.size()) {
			return false;
		}
		for (std::size_t i = 0; i < a.size(); ++i) {
			const auto ca = static_cast<unsigned char>(a[i]);
			const auto cb = static_cast<unsigned char>(b[i]);
			if (std::tolower(ca) != std::tolower(cb)) {
				return false;
			}
		}
		return true;
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

	static void StripInlineComment(std::string& line)
	{
		for (std::size_t i = 0; i < line.size(); ++i) {
			if (line[i] == ';' || line[i] == '#') {
				line.resize(i);
				return;
			}
		}
	}

	static std::vector<std::string> Split(const std::string& s, char delim)
	{
		std::vector<std::string> out;
		std::string cur;
		std::stringstream ss(s);
		while (std::getline(ss, cur, delim)) {
			out.push_back(cur);
		}
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
		kScale,
		kVis
	};

	struct DebugConfig
	{
		bool logOps{ true };
		bool strictIni{ false };
		bool logEventTags{ false };
		bool logTimelineStart{ true };
		bool logTargetResolve{ false };
	};

	struct Command
	{
		TargetKind target{ TargetKind::kCaster };
		CommandType type{ CommandType::kScale };
		float timeSeconds{ 0.0f };

		// Scale: points to FB::Scaler::kNode* constants (static lifetime).
		std::string_view nodeName{};

		// Vis: group key from [FBVisGroups] OR literal object name.
		// Owns memory because Command is stored long-term.
		std::string visKey{};

		// Payload
		float scale{ 1.0f };
		bool visible{ true };
	};

	struct Config
	{
		bool enableTimelines{ true };
		bool resetOnPairEnd{ true };
		bool resetOnPairedStop{ true };
		DebugConfig dbg{};

		// Event tag -> Timeline name
		std::unordered_map<std::string, std::string> eventToTimeline;

		// Timeline name -> commands
		std::unordered_map<std::string, std::vector<Command>> timelines;

		// Vis groups: key -> exact object names
		std::unordered_map<std::string, std::vector<std::string>> visGroups;
	};

	std::mutex g_cfgMutex;
	Config g_cfg;
	bool g_cfgLoaded = false;

	static void SortAndClamp(std::vector<Command>& cmds)
	{
		for (auto& c : cmds) {
			if (c.timeSeconds < 0.0f) {
				c.timeSeconds = 0.0f;
			}
		}
		std::sort(cmds.begin(), cmds.end(), [](const auto& a, const auto& b) { return a.timeSeconds < b.timeSeconds; });
	}

	static std::string GetConfigPathPreferred()
	{
		return "Data\\FullBodiedIni.ini";
	}

	static std::string GetConfigPathFallback()
	{
		return "Data\\SKSE\\Plugins\\FullBodiedIni.ini";
	}

	// =========================
	// NodeKey -> skeleton NiNode name (Scale)
	// =========================
	static std::optional<std::string_view> ResolveNodeKey(std::string_view key)
	{
		if (IEquals(key, "Head")) {
			return FB::Scaler::kNodeHead;
		}
		if (IEquals(key, "Pelvis")) {
			return FB::Scaler::kNodePelvis;
		}
		if (IEquals(key, "Spine")) {
			return FB::Scaler::kNodeSpine0;
		}
		if (IEquals(key, "Spine1")) {
			return FB::Scaler::kNodeSpine1;
		}
		if (IEquals(key, "Spine2")) {
			return FB::Scaler::kNodeSpine2;
		}
		if (IEquals(key, "Neck")) {
			return FB::Scaler::kNodeNeck;
		}
		if (IEquals(key, "LForearm")) {
			return FB::Scaler::kNodeLForearm;
		}
		if (IEquals(key, "RForearm")) {
			return FB::Scaler::kNodeRForearm;
		}
		if (IEquals(key, "LHand")) {
			return FB::Scaler::kNodeLHand;
		}
		if (IEquals(key, "RHand")) {
			return FB::Scaler::kNodeRHand;
		}
		if (IEquals(key, "LUpperArm")) {
			return FB::Scaler::kNodeLUpperArm;
		}
		if (IEquals(key, "RUpperArm")) {
			return FB::Scaler::kNodeRUpperArm;
		}
		if (IEquals(key, "LThigh")) {
			return FB::Scaler::kNodeLThigh;
		}
		if (IEquals(key, "RThigh")) {
			return FB::Scaler::kNodeRThigh;
		}
		if (IEquals(key, "LCalf")) {
			return FB::Scaler::kNodeLCalf;
		}
		if (IEquals(key, "RCalf")) {
			return FB::Scaler::kNodeRCalf;
		}
		if (IEquals(key, "LFoot")) {
			return FB::Scaler::kNodeLFoot;
		}
		if (IEquals(key, "RFoot")) {
			return FB::Scaler::kNodeRFoot;
		}
		return std::nullopt;
	}

	// =========================
	// Parse tokens
	// =========================
	struct ParsedScale
	{
		std::string_view nodeName{};
		float scale{ 1.0f };
	};

	static std::optional<ParsedScale> TryParseScaleToken(std::string_view tok, bool strictIni)
	{
		const std::string_view prefix = "FBScale_";
		if (tok.rfind(prefix, 0) != 0) {
			return std::nullopt;
		}

		const size_t open = tok.find('(');
		const size_t close = tok.rfind(')');
		if (open == std::string_view::npos || close == std::string_view::npos || close <= open + 1 || close != tok.size() - 1) {
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

		std::string arg = std::string(tok.substr(open + 1, close - (open + 1)));
		auto f = ParseFloat(arg);
		if (!f) {
			if (strictIni) {
				spdlog::warn("[FB] INI: bad float '{}' in '{}'", arg, std::string(tok));
			}
			return std::nullopt;
		}

		ParsedScale out;
		out.nodeName = *nodeName;
		out.scale = *f;
		return out;
	}

	struct ParsedVis
	{
		std::string key;
		bool visible{ true };
	};

	static std::optional<ParsedVis> TryParseVisToken(std::string_view tok, bool strictIni)
	{
		const std::string_view prefix = "FBVis_";
		if (tok.rfind(prefix, 0) != 0) {
			return std::nullopt;
		}

		const size_t open = tok.find('(');
		const size_t close = tok.rfind(')');
		if (open == std::string_view::npos || close == std::string_view::npos || close <= open + 1 || close != tok.size() - 1) {
			if (strictIni) {
				spdlog::warn("[FB] INI: bad call syntax '{}'", std::string(tok));
			}
			return std::nullopt;
		}

		std::string key{ tok.substr(prefix.size(), open - prefix.size()) };
		TrimInPlace(key);
		if (key.empty()) {
			if (strictIni) {
				spdlog::warn("[FB] INI: FBVis missing key in '{}'", std::string(tok));
			}
			return std::nullopt;
		}

		std::string arg{ tok.substr(open + 1, close - (open + 1)) };
		TrimInPlace(arg);

		ParsedVis out;
		out.key = std::move(key);
		out.visible = ParseBool(arg, true);
		return out;
	}

	static std::optional<Command> ParseCommand(float t, const std::string& cmdTok, TargetKind who, bool strictIni)
	{
		// Enforce 2_ prefix rule for target section.
		if (who == TargetKind::kTarget) {
			if (cmdTok.rfind(std::string(kTargetPrefix), 0) != 0) {
				if (strictIni) {
					spdlog::warn("[FB] INI: Target section requires '2_' prefix, got '{}'", cmdTok);
				}
				return std::nullopt;
			}

			std::string_view inner{ cmdTok.c_str() + kTargetPrefix.size(), cmdTok.size() - kTargetPrefix.size() };

			if (auto s = TryParseScaleToken(inner, strictIni)) {
				Command c;
				c.target = TargetKind::kTarget;
				c.type = CommandType::kScale;
				c.timeSeconds = t;
				c.nodeName = s->nodeName;
				c.scale = s->scale;
				return c;
			}

			if (auto v = TryParseVisToken(inner, strictIni)) {
				Command c;
				c.target = TargetKind::kTarget;
				c.type = CommandType::kVis;
				c.timeSeconds = t;
				c.visKey = std::move(v->key);
				c.visible = v->visible;
				return c;
			}

			if (strictIni) {
				spdlog::warn("[FB] INI: unsupported/unknown target token '{}'", cmdTok);
			}
			return std::nullopt;
		}

		// Caster section must NOT use 2_
		if (cmdTok.rfind(std::string(kTargetPrefix), 0) == 0) {
			if (strictIni) {
				spdlog::warn("[FB] INI: Caster section must NOT use '2_' prefix, got '{}'", cmdTok);
			}
			return std::nullopt;
		}

		if (auto s = TryParseScaleToken(cmdTok, strictIni)) {
			Command c;
			c.target = TargetKind::kCaster;
			c.type = CommandType::kScale;
			c.timeSeconds = t;
			c.nodeName = s->nodeName;
			c.scale = s->scale;
			return c;
		}

		if (auto v = TryParseVisToken(cmdTok, strictIni)) {
			Command c;
			c.target = TargetKind::kCaster;
			c.type = CommandType::kVis;
			c.timeSeconds = t;
			c.visKey = std::move(v->key);
			c.visible = v->visible;
			return c;
		}

		if (strictIni) {
			spdlog::warn("[FB] INI: unsupported/unknown caster token '{}'", cmdTok);
		}
		return std::nullopt;
	}

	// =========================
	// FB section name (2-part)
	// =========================
	static bool ParseFBSection(const std::string& section, std::string& outTimelineName, TargetKind& outWho)
	{
		if (section.rfind("FB:", 0) != 0) {
			return false;
		}

		const auto pipe = section.find('|');
		if (pipe == std::string::npos) {
			return false;
		}

		std::string left = section.substr(3, pipe - 3);
		std::string right = section.substr(pipe + 1);

		TrimInPlace(left);
		TrimInPlace(right);

		if (left.empty()) {
			return false;
		}

		outTimelineName = left;

		if (IEquals(right, "Caster")) {
			outWho = TargetKind::kCaster;
			return true;
		}
		if (IEquals(right, "Target")) {
			outWho = TargetKind::kTarget;
			return true;
		}
		return false;
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
			}
		}

		if (!in.good()) {
			spdlog::warn("[FB] Config not found (preferred or fallback). Using defaults.");
			g_cfg = std::move(newCfg);
			g_cfgLoaded = true;
			return;
		}

		spdlog::info("[FB] Loading config: {}", path);

		std::string currentSection;
		std::string timelineName;
		TargetKind sectionWho = TargetKind::kCaster;

		std::string line;
		while (std::getline(in, line)) {
			StripInlineComment(line);
			TrimInPlace(line);
			if (line.empty()) {
				continue;
			}

			// Section header
			if (line.front() == '[' && line.back() == ']') {
				currentSection = line.substr(1, line.size() - 2);
				TrimInPlace(currentSection);

				std::string tl;
				TargetKind who = TargetKind::kCaster;
				if (ParseFBSection(currentSection, tl, who)) {
					timelineName = tl;
					sectionWho = who;
					(void)newCfg.timelines[timelineName];
				}
				else {
					// Leaving timeline section
					timelineName.clear();
				}
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

				if (IEquals(key, "bLogOps")) {
					newCfg.dbg.logOps = ParseBool(val, newCfg.dbg.logOps);
				}
				else if (IEquals(key, "bStrictIni")) {
					newCfg.dbg.strictIni = ParseBool(val, newCfg.dbg.strictIni);
				}
				else if (IEquals(key, "bLogEventTags")) {
					newCfg.dbg.logEventTags = ParseBool(val, newCfg.dbg.logEventTags);
				}
				else if (IEquals(key, "bLogTimelineStart")) {
					newCfg.dbg.logTimelineStart = ParseBool(val, newCfg.dbg.logTimelineStart);
				}
				else if (IEquals(key, "bLogTargetResolve")) {
					newCfg.dbg.logTargetResolve = ParseBool(val, newCfg.dbg.logTargetResolve);
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

			// [FBVisGroups]
			if (IEquals(currentSection, "FBVisGroups")) {
				auto eq = line.find('=');
				if (eq == std::string::npos) {
					continue;
				}
				std::string key = line.substr(0, eq);
				std::string rhs = line.substr(eq + 1);
				TrimInPlace(key);
				TrimInPlace(rhs);
				if (key.empty()) {
					continue;
				}

				auto parts = Split(rhs, ',');
				std::vector<std::string> names;
				names.reserve(parts.size());
				for (auto& p : parts) {
					TrimInPlace(p);
					if (!p.empty()) {
						names.emplace_back(std::move(p));
					}
				}

				if (!names.empty()) {
					newCfg.visGroups[key] = std::move(names);
					if (newCfg.dbg.logOps) {
						spdlog::info("[FB] VisGroups: '{}' -> {} entries", key, newCfg.visGroups[key].size());
					}
				}
				else if (newCfg.dbg.strictIni) {
					spdlog::warn("[FB] VisGroups: '{}' has no entries", key);
				}
				continue;
			}

			// FB timeline body
			if (!timelineName.empty()) {
				std::stringstream ss(line);
				std::string tStr;
				std::string cmdTok;

				ss >> tStr;
				ss >> cmdTok;

				if (tStr.empty() || cmdTok.empty()) {
					if (newCfg.dbg.strictIni) {
						spdlog::warn("[FB] INI: bad timeline line '{}'", line);
					}
					continue;
				}

				auto t = ParseFloat(tStr);
				if (!t) {
					if (newCfg.dbg.strictIni) {
						spdlog::warn("[FB] INI: bad time '{}' in '{}'", tStr, line);
					}
					continue;
				}

				auto c = ParseCommand(*t, cmdTok, sectionWho, newCfg.dbg.strictIni);
				if (c) {
					newCfg.timelines[timelineName].push_back(std::move(*c));
				}
				continue;
			}
		}

		for (auto& [name, cmds] : newCfg.timelines) {
			SortAndClamp(cmds);
		}

		spdlog::info("[FB] Config loaded: enableTimelines={} resetOnPairEnd={} resetOnPairedStop={} eventMaps={} timelines={} visGroups={}",
			newCfg.enableTimelines,
			newCfg.resetOnPairEnd,
			newCfg.resetOnPairedStop,
			newCfg.eventToTimeline.size(),
			newCfg.timelines.size(),
			newCfg.visGroups.size());

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

	// Snapshot for safe lambda captures
	static Config GetConfigCopy()
	{
		std::lock_guard _{ g_cfgMutex };
		if (!g_cfgLoaded) {
			LoadConfigLocked();
		}
		return g_cfg;
	}

	// =========================
	// Runtime state per caster
	// =========================
	struct ActorState
	{
		std::uint64_t token{ 0 };
		RE::ActorHandle lastTarget{};
		std::chrono::steady_clock::time_point lastStart{};

		std::unordered_set<std::string_view> casterTouched;
		std::unordered_set<std::string_view> targetTouched;

		std::unordered_set<std::string> casterVisTouched;
		std::unordered_set<std::string> targetVisTouched;
	};

	std::mutex g_stateMutex;
	std::unordered_map<std::uint32_t, ActorState> g_actorState;  // caster FormID -> state

	static std::uint64_t BumpTokenForCaster(RE::Actor* caster)
	{
		const auto formID = caster->GetFormID();
		std::lock_guard _{ g_stateMutex };
		auto& st = g_actorState[formID];
		++st.token;
		st.casterTouched.clear();
		st.targetTouched.clear();
		st.casterVisTouched.clear();
		st.targetVisTouched.clear();
		return st.token;
	}

	static void SetLastTarget(std::uint32_t casterFormID, RE::ActorHandle targetHandle)
	{
		std::lock_guard _{ g_stateMutex };
		auto& st = g_actorState[casterFormID];
		st.lastTarget = targetHandle;
		st.lastStart = std::chrono::steady_clock::now();
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

	static std::uint64_t GetToken(std::uint32_t casterFormID)
	{
		std::lock_guard _{ g_stateMutex };
		auto it = g_actorState.find(casterFormID);
		return (it == g_actorState.end()) ? 0 : it->second.token;
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

	static void MarkVisTouched(std::uint32_t casterFormID, TargetKind who, const std::string& key)
	{
		std::lock_guard _{ g_stateMutex };
		auto& st = g_actorState[casterFormID];
		if (who == TargetKind::kCaster) {
			st.casterVisTouched.insert(key);
		}
		else {
			st.targetVisTouched.insert(key);
		}
	}

	static std::vector<std::string> GetVisTouched(std::uint32_t casterFormID, TargetKind who)
	{
		std::lock_guard _{ g_stateMutex };
		std::vector<std::string> out;
		auto it = g_actorState.find(casterFormID);
		if (it == g_actorState.end()) {
			return out;
		}
		const auto& set = (who == TargetKind::kCaster) ? it->second.casterVisTouched : it->second.targetVisTouched;
		out.reserve(set.size());
		for (const auto& v : set) {
			out.push_back(v);
		}
		return out;
	}

	// =========================
	// Target resolution (nearest actor)
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

		const auto visit = [&](RE::Actor* a) {
			if (!a || a == caster) {
				return;
			}
			if (a->IsDead()) {
				return;
			}

			auto p = a->GetPosition();
			const float dx = p.x - casterPos.x;
			const float dy = p.y - casterPos.y;
			const float dz = p.z - casterPos.z;
			const float d2 = dx * dx + dy * dy + dz * dz;

			if (d2 < bestDist2) {
				bestDist2 = d2;
				best = a;
			}
			};

		// Helper: resolve "handle-like" things to Actor*
		const auto resolveActor = [](const auto& handleLike) -> RE::Actor* {
			// CommonLib variants differ. Some handles do:
			//   - handleLike.get() -> RE::Actor*
			//   - handleLike.get() -> RE::NiPointer<RE::Actor> or BSTSmartPointer, then .get()
			if constexpr (requires { handleLike.get().get(); }) {
				return handleLike.get().get();
			}
			else if constexpr (requires { handleLike.get(); }) {
				return handleLike.get();
			}
			else {
				return nullptr;
			}
			};

		// High actors (handle containers)
		for (const auto& h : processLists->highActorHandles) {
			if (auto* actor = resolveActor(h)) {
				visit(actor);
			}
		}

		// Middle-high actors (handle containers)
		for (const auto& h : processLists->middleHighActorHandles) {
			if (auto* actor = resolveActor(h)) {
				visit(actor);
			}
		}

		if (best && log) {
			spdlog::info("[FB] TargetResolve: caster='{}' -> target='{}' dist={}",
				caster->GetName(),
				best->GetName(),
				std::sqrt(bestDist2));
		}

		return best ? best->CreateRefHandle() : RE::ActorHandle{};
	}


	// =========================
	// FBVis application helper
	// =========================
	static void ApplyVisByKey(const Config& cfg, RE::ActorHandle actor, const std::string& key, bool visible, bool logOps)
	{
		const auto it = cfg.visGroups.find(key);
		if (it == cfg.visGroups.end() || it->second.empty()) {
			// Treat key as literal object name
			FB::Vis::SetObjectVisibleExact(actor, key, visible, logOps);
			return;
		}

		// Treat key as group
		for (const auto& objName : it->second) {
			FB::Vis::SetObjectVisibleExact(actor, objName, visible, logOps);
		}
	}

	// =========================
	// Timeline start + schedule
	// =========================
	static void StartTimelineForCaster(const RE::Actor* caster, std::string_view eventTag)
	{
		if (!caster) {
			return;
		}

		const auto cfg = GetConfigCopy();
		if (!cfg.enableTimelines) {
			return;
		}

		// eventTag is e.g. "FBEvent"
		const auto it = cfg.eventToTimeline.find(std::string(eventTag));
		if (it == cfg.eventToTimeline.end()) {
			return;
		}

		const auto tlIt = cfg.timelines.find(it->second);
		if (tlIt == cfg.timelines.end()) {
			if (cfg.dbg.strictIni) {
				spdlog::warn("[FB] Timeline '{}' not found for event '{}'", it->second, std::string(eventTag));
			}
			return;
		}

		const auto& commands = tlIt->second;
		const std::uint32_t casterFormID = caster->GetFormID();

		// capture target at the moment the start event fires (if you’re doing that)
		RE::ActorHandle resolvedTarget = {};
		// resolvedTarget = FindLikelyPairedTarget(const_cast<RE::Actor*>(caster), cfg.dbg.logOps); // if needed

		// Store target for later resets
		SetLastTarget(casterFormID, resolvedTarget);

		// Token for canceling outstanding scheduled ops
		const auto token = BumpTokenForCaster(const_cast<RE::Actor*>(caster));

		// We only need non-const here to create handle
		auto casterHandle = const_cast<RE::Actor*>(caster)->CreateRefHandle();
		const auto targetHandle = GetLastTarget(casterFormID);
		const bool logOps = cfg.dbg.logOps;

		// Mark touched now so reset is deterministic even if canceled mid-way.
		for (const auto& cmd : commands) {
			if (cmd.type == CommandType::kScale) {
				MarkTouched(casterFormID, cmd.target, cmd.nodeName);
			}
			else if (cmd.type == CommandType::kVis) {
				MarkVisTouched(casterFormID, cmd.target, cmd.visKey);
			}
		}

		for (const auto& cmd : commands) {
			std::thread([cfg, casterHandle, targetHandle, casterFormID, token, cmd, logOps]() mutable {
				std::this_thread::sleep_for(std::chrono::duration<float>(cmd.timeSeconds));

				if (GetToken(casterFormID) != token) {
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

				case CommandType::kVis:
					if (cmd.target == TargetKind::kCaster) {
						ApplyVisByKey(cfg, casterHandle, cmd.visKey, cmd.visible, logOps);
					}
					else {
						ApplyVisByKey(cfg, targetHandle, cmd.visKey, cmd.visible, logOps);
					}
					break;

				default:
					break;
				}
				}).detach();
		}
	}


	static void CancelAndReset(const RE::Actor* caster)
	{
		if (!caster) {
			return;
		}

		const auto cfg = GetConfigCopy();
		const std::uint32_t casterFormID = caster->GetFormID();

		// BumpTokenForCaster expects non-const (because we mutate per-actor state),
		// but we are not actually mutating the Actor itself.
		(void)BumpTokenForCaster(const_cast<RE::Actor*>(caster));  // cancels outstanding commands

		auto casterHandle = const_cast<RE::Actor*>(caster)->CreateRefHandle();
		const auto targetHandle = GetLastTarget(casterFormID);

		// Reset only nodes we touched this run
		for (auto node : GetTouched(casterFormID, TargetKind::kCaster)) {
			SKSE::GetTaskInterface()->AddTask([=]() {
				FB::Scaler::SetNodeScale(casterHandle, cmd.nodeName, cmd.scale, logOps);
				});

		
		for (auto node : GetTouched(casterFormID, TargetKind::kTarget)) {
			SKSE::GetTaskInterface()->AddTask([=]() {
				FB::Scaler::SetNodeScale(casterHandle, cmd.nodeName, cmd.scale, logOps);
				});

		

		// Reset vis toggles
		for (auto key : GetVisTouched(casterFormID, TargetKind::kCaster)) {
			ApplyVisByKey(cfg, casterHandle, key, true, cfg.dbg.logOps);
		}
		for (auto key : GetVisTouched(casterFormID, TargetKind::kTarget)) {
			ApplyVisByKey(cfg, targetHandle, key, true, cfg.dbg.logOps);
		}
	}


	// =========================
	// Event sink
	// =========================
	class AnimationEventSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>*)
			override
		{
			if (!a_event) {
				return RE::BSEventNotifyControl::kContinue;
			}

			auto* caster = a_event->holder ? a_event->holder->As<RE::Actor>() : nullptr;
			if (!caster) {
				return RE::BSEventNotifyControl::kContinue;
			}

			const auto& cfg = GetConfig();
			const std::string tag = a_event->tag.c_str();

			if (cfg.dbg.logEventTags) {
				spdlog::info("[FB] AnimTag: actor='{}' tag='{}'", caster->GetName(), tag);
			}

			if (cfg.resetOnPairEnd && IEquals(tag, "PairEnd")) {
				CancelAndReset(caster);
				return RE::BSEventNotifyControl::kContinue;
			}
			if (cfg.resetOnPairedStop && IEquals(tag, "NPCpairedStop")) {
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

	// CommonLibSSE-NG / AE builds often use the out-param form:
	//   bool GetAnimationGraphManager(BSTSmartPointer<BSAnimationGraphManager>& out)
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
