#include "FBConfig.h"
#include "ActorManager.h"   // TargetKind / TimedCommand / CommandKind
#include "FBMorph.h"        // (still included; not required anymore, but harmless)

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
	// Target line prefix standard (tokens inside [FB:...|Target] must be prefixed)
	static constexpr std::string_view kTargetPrefix = "2_";

	// -------------------------
	// Small string utils
	// -------------------------
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

	static std::filesystem::path GetConfigPathPreferred()
	{
		return std::filesystem::path("Data") / "FullBodiedIni.ini";
	}

	static std::filesystem::path GetConfigPathFallback()
	{
		return std::filesystem::path("Data") / "SKSE" / "Plugins" / "FullBodiedIni.ini";
	}

	// -------------------------
	// FB section header: [FB:<timeline>|Caster/Target]
	// -------------------------
	struct FBSection
	{
		std::string timeline;
		FB::TargetKind who{ FB::TargetKind::kCaster };
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
			out.who = FB::TargetKind::kCaster;
		}
		else if (IEquals(who, "Target")) {
			out.who = FB::TargetKind::kTarget;
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

	// -------------------------
	// Token parsers
	// -------------------------
	struct ParsedScale
	{
		std::string_view nodeName{};
		float scale{ 1.0f };
	};

	static std::optional<ParsedScale> TryParseScaleToken(
		std::string_view tok,
		bool strictIni,
		FB::Config::NodeKeyResolver resolver)
	{
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

		if (!resolver) {
			if (strictIni) {
				spdlog::warn("[FB] INI: node resolver not set; cannot resolve '{}'", std::string(nodeKey));
			}
			return std::nullopt;
		}

		auto nodeName = resolver(nodeKey);
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

	struct ParsedMorph
	{
		std::string morphName;  // OWNS
		float delta{ 0.0f };
	};

	static std::string ResolveMorphAlias(std::string_view authorKey)
	{
		// INI tokens must not contain spaces (parser uses >>), but RaceMenu morph names often do.
		// Add aliases here when you want author-friendly tokens.

		if (authorKey == "VorePreyBelly" || authorKey == "Vore_Prey_Belly") {
			return "Vore Prey Belly";
		}

		// Default: use token as-is
		return std::string(authorKey);
	}

	static std::optional<ParsedMorph> TryParseMorphToken(std::string_view tok, bool strictIni)
	{
		const std::string_view prefix = "FBMorph_";
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
				spdlog::warn("[FB] INI: missing MorphKey in '{}'", std::string(tok));
			}
			return std::nullopt;
		}

		const std::string_view authorKey = tok.substr(prefix.size(), open - prefix.size());
		std::string morphName = ResolveMorphAlias(authorKey);

		if (morphName.empty()) {
			if (strictIni) {
				spdlog::warn("[FB] INI: empty MorphKey in '{}'", std::string(tok));
			}
			return std::nullopt;
		}

		std::string arg{ tok.substr(open + 1, close - open - 1) };
		TrimInPlace(arg);

		auto f = ParseFloat(arg);
		if (!f) {
			if (strictIni) {
				spdlog::warn("[FB] INI: FBMorph arg not a float '{}' in '{}'", arg, std::string(tok));
			}
			return std::nullopt;
		}

		ParsedMorph out;
		out.morphName = std::move(morphName);
		out.delta = *f;
		return out;
	}

	static std::optional<FB::TimedCommand> ParseCommand(
		float t,
		const std::string& cmdTok,
		FB::TargetKind who,
		bool strictIni,
		FB::Config::NodeKeyResolver resolver)
	{
		FB::TimedCommand out{};
		out.timeSeconds = t;

		auto parse_inner = [&](std::string_view tokenView, FB::TargetKind dest) -> std::optional<FB::TimedCommand> {
			// Scale
			if (auto s = TryParseScaleToken(tokenView, strictIni, resolver)) {
				FB::TimedCommand c{};
				c.timeSeconds = t;
				c.kind = FB::CommandKind::kScale;
				c.target = dest;
				c.nodeKey = s->nodeName;
				c.scale = s->scale;
				return c;
			}

			// Morph
			if (auto m = TryParseMorphToken(tokenView, strictIni)) {
				FB::TimedCommand c{};
				c.timeSeconds = t;
				c.kind = FB::CommandKind::kMorph;
				c.target = dest;
				c.morphName = m->morphName;  // OWNED std::string
				c.delta = m->delta;
				return c;
			}

			return std::nullopt;
			};

		if (who == FB::TargetKind::kTarget) {
			// Target section REQUIRES "2_" prefix
			if (cmdTok.rfind(std::string(kTargetPrefix), 0) != 0) {
				if (strictIni) {
					spdlog::warn("[FB] INI: Target section requires '2_' prefix, got '{}'", cmdTok);
				}
				return std::nullopt;
			}

			std::string_view inner{ cmdTok.c_str() + kTargetPrefix.size(), cmdTok.size() - kTargetPrefix.size() };

			if (auto cmd = parse_inner(inner, FB::TargetKind::kTarget)) {
				return cmd;
			}

			if (strictIni) {
				spdlog::warn("[FB] INI: unsupported/unknown target token '{}'", cmdTok);
			}
			return std::nullopt;
		}

		// Caster section must NOT use "2_"
		if (cmdTok.rfind(std::string(kTargetPrefix), 0) == 0) {
			if (strictIni) {
				spdlog::warn("[FB] INI: Caster section must NOT use '2_' prefix, got '{}'", cmdTok);
			}
			return std::nullopt;
		}

		if (auto cmd = parse_inner(cmdTok, FB::TargetKind::kCaster)) {
			return cmd;
		}

		if (strictIni) {
			spdlog::warn("[FB] INI: unsupported/unknown caster token '{}'", cmdTok);
		}
		return std::nullopt;
	}

	static void SortAndClamp(std::vector<FB::TimedCommand>& cmds)
	{
		for (auto& c : cmds) {
			if (c.timeSeconds < 0.0f) {
				c.timeSeconds = 0.0f;
			}

			if (c.kind == FB::CommandKind::kScale) {
				c.scale = std::clamp(c.scale, 0.0f, 5.0f);
			}
			else if (c.kind == FB::CommandKind::kMorph) {
				// Safety clamp. FBMorph clamps final accumulated value to [0..100].
				c.delta = std::clamp(c.delta, -1000.0f, 1000.0f);
			}
		}

		std::sort(cmds.begin(), cmds.end(),
			[](const auto& a, const auto& b) { return a.timeSeconds < b.timeSeconds; });
	}

	// -------------------------
	// FBConfig cache
	// -------------------------
	std::mutex g_cfgMutex;
	FB::Config::ConfigData g_cfg;
	bool g_loaded = false;
	FB::Config::NodeKeyResolver g_resolver = nullptr;

	static void LoadConfigLocked()
	{
		FB::Config::ConfigData newCfg;

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
			g_loaded = true;
			return;
		}

		spdlog::info("[FB] Config path: {}", path.string());

		// Read whole file for 2-pass parse
		std::vector<std::string> lines;
		{
			std::string line;
			while (std::getline(in, line)) {
				lines.push_back(std::move(line));
			}
		}

		auto parse_non_fb_sections = [&](const std::string& currentSection, const std::string& rawLine) {
			std::string line = rawLine;
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
				else if (IEquals(key, "resetMorphsOnPairEnd")) {
					newCfg.resetMorphsOnPairEnd = ParseBool(val, newCfg.resetMorphsOnPairEnd);
				}
				else if (IEquals(key, "resetMorphsOnPairedStop")) {
					newCfg.resetMorphsOnPairedStop = ParseBool(val, newCfg.resetMorphsOnPairedStop);
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

		// Pass 1
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

		// Pass 2: FB sections
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

				if (line.size() >= 3 && line.front() == '[' && line.back() == ']') {
					currentSection = line.substr(1, line.size() - 2);
					TrimInPlace(currentSection);
					activeFBSection = ParseFBSectionName(currentSection, newCfg.dbg.strictIni);
					continue;
				}

				if (activeFBSection && activeFBSection->supported && !activeFBSection->timeline.empty()) {
					std::istringstream iss(line);
					std::string timeTok;
					std::string cmdTok;

					// IMPORTANT: command token must not contain spaces (e.g. "FBMorph_X(10)" is OK)
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

					if (auto cmd = ParseCommand(*t, cmdTok, activeFBSection->who, newCfg.dbg.strictIni, g_resolver)) {
						newCfg.timelines[activeFBSection->timeline].push_back(*cmd);
					}
				}
			}
		}

		for (auto& [name, cmds] : newCfg.timelines) {
			SortAndClamp(cmds);
		}

		spdlog::info(
			"[FB] Config loaded: enableTimelines={} resetOnPairEnd={} resetOnPairedStop={} resetMorphsOnPairEnd={} resetMorphsOnPairedStop={} eventMaps={} timelines={}",
			newCfg.enableTimelines,
			newCfg.resetOnPairEnd,
			newCfg.resetOnPairedStop,
			newCfg.resetMorphsOnPairEnd,
			newCfg.resetMorphsOnPairedStop,
			newCfg.eventToTimeline.size(),
			newCfg.timelines.size());

		g_cfg = std::move(newCfg);
		g_loaded = true;
	}
}

namespace FB::Config
{
	const ConfigData& Get(NodeKeyResolver resolver)
	{
		std::lock_guard _{ g_cfgMutex };
		if (resolver) {
			g_resolver = resolver;
		}
		if (!g_loaded) {
			LoadConfigLocked();
		}
		return g_cfg;
	}

	void Reload(NodeKeyResolver resolver)
	{
		std::lock_guard _{ g_cfgMutex };
		if (resolver) {
			g_resolver = resolver;
		}
		g_loaded = false;
		LoadConfigLocked();
	}
}
