#include "readyup/match_config_parser.h"

#include "readyup/minijson.h"
#include "readyup/steamid.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace readyup {

std::optional<WebhookMatchContext> ParseWebhookMatchContextFromJson(const std::string& json, std::string* errOut) {
  using namespace readyup::minijson;
  ParseError perr;
  auto rootOpt = Parse(json, &perr);
  if (!rootOpt) {
    if (errOut) {
      *errOut = perr.msg.empty() ? "json parse error" : perr.msg;
      *errOut += " at offset " + std::to_string(perr.offset);
    }
    return std::nullopt;
  }
  const Value& root = *rootOpt;

  // Accept either:
  // - MatchResponse wrapper: { id, slug, serverId, config: { ... MatchConfig ... }, ... }
  // - Raw MatchConfig object: { matchid, team1, team2, ... }
  const Value* cfg = &root;
  std::string slug;
  if (const Value* c = root.get("config"); IsObject(c)) {
    cfg = c;
    if (auto s = AsString(root.get("slug"))) slug = *s;
  }

  WebhookMatchContext ctx;
  ctx.slug = std::move(slug);

  // matchid
  if (auto mid = AsInt(cfg->get("matchid"))) ctx.matchid = static_cast<uint64_t>(*mid);
  if (ctx.matchid == 0) {
    if (auto id = AsInt(root.get("id"))) ctx.matchid = static_cast<uint64_t>(*id);
  }
  if (ctx.matchid == 0) {
    if (errOut) *errOut = "missing matchid";
    return std::nullopt;
  }

  if (auto nm = AsInt(cfg->get("num_maps"))) ctx.num_maps = static_cast<int>(*nm);
  if (const Value* cs = cfg->get("clinch_series"); cs && cs->type == Value::Type::Bool) ctx.clinch_series = cs->b;

  // cvars (object)
  if (const Value* cvars = cfg->get("cvars"); IsObject(cvars)) {
    auto fmtNum = [](double x) {
      // Prefer integer rendering when possible.
      const double r = std::round(x);
      if (std::fabs(x - r) < 1e-9) return std::to_string(static_cast<int64_t>(r));
      // Fallback: trim trailing zeros from a fixed-ish representation.
      std::string s = std::to_string(x);
      while (!s.empty() && s.back() == '0') s.pop_back();
      if (!s.empty() && s.back() == '.') s.pop_back();
      return s.empty() ? std::string("0") : s;
    };

    for (const auto& kv : cvars->obj) {
      const std::string& key = kv.first;
      const Value& v = kv.second;
      if (key.empty()) continue;
      if (v.type == Value::Type::String) {
        if (!v.str.empty()) ctx.cvars[key] = v.str;
      } else if (v.type == Value::Type::Number) {
        ctx.cvars[key] = fmtNum(v.num);
      } else if (v.type == Value::Type::Bool) {
        ctx.cvars[key] = v.b ? "1" : "0";
      }
    }
  }

  // Round limit metadata (preferred over deriving from cvars).
  // maxRounds: allow number or string.
  auto parsePositiveInt = [](const Value* v) -> std::optional<int> {
    if (!v) return std::nullopt;
    if (auto i = AsInt(v)) {
      if (*i > 0 && *i < 10000) return static_cast<int>(*i);
      return std::nullopt;
    }
    if (auto s = AsString(v)) {
      if (s->empty()) return std::nullopt;
      try {
        const int n = std::stoi(*s);
        if (n > 0 && n < 10000) return n;
      } catch (...) {
      }
    }
    return std::nullopt;
  };

  auto parseNonNegativeInt = [&](const Value* v) -> std::optional<int> {
    if (!v) return std::nullopt;
    if (auto i = AsInt(v)) {
      if (*i >= 0 && *i < 10000) return static_cast<int>(*i);
      return std::nullopt;
    }
    if (auto s = AsString(v)) {
      if (s->empty()) return std::nullopt;
      try {
        const int n = std::stoi(*s);
        if (n >= 0 && n < 10000) return n;
      } catch (...) {
      }
    }
    return std::nullopt;
  };

  auto parseBool = [](const Value* v) -> std::optional<bool> {
    if (!v) return std::nullopt;
    if (v->type == Value::Type::Bool) return v->b;
    if (v->type == Value::Type::Number) return v->num != 0.0;
    if (v->type == Value::Type::String) {
      const std::string& s = v->str;
      if (s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "YES") return true;
      if (s == "0" || s == "false" || s == "FALSE" || s == "no" || s == "NO") return false;
    }
    return std::nullopt;
  };

  bool maxRoundsExplicit = false;
  if (auto mr = parsePositiveInt(cfg->get("maxRounds"))) {
    ctx.maxRounds = *mr;
    maxRoundsExplicit = true;
  }

  // overtimeMode: "enabled" | "disabled"
  if (auto om = AsString(cfg->get("overtimeMode"))) {
    if (*om == "enabled") ctx.overtime_enabled = true;
    else ctx.overtime_enabled = false;
  }

  if (auto seg = parsePositiveInt(cfg->get("overtimeSegments"))) {
    // keep within a sane range; MatchZy commonly uses 3.
    ctx.overtimeSegments = std::max(1, std::min(60, *seg));
  }

  // Tie-break configuration.
  if (auto mo = parseNonNegativeInt(cfg->get("maxOvertimes"))) {
    ctx.maxOvertimes = *mo;
  }
  if (auto dt = parseBool(cfg->get("damageTiebreak"))) {
    ctx.damageTiebreakEnabled = *dt;
  }
  if (auto sd = parseBool(cfg->get("damageTiebreakSuddenDeath"))) {
    ctx.suddenDeathOnDamageTie = *sd;
  }

  // Knife side-pick timeout (seconds).
  if (auto sec = parsePositiveInt(cfg->get("knifeDecisionSeconds"))) {
    ctx.knifeDecisionSeconds = std::max(5, std::min(300, *sec));
  } else {
    // Keep a sane clamp for defaults as well.
    ctx.knifeDecisionSeconds = std::max(5, std::min(300, ctx.knifeDecisionSeconds));
  }

  // Pause / ready / forfeit rules (match_rules.h). Unset keys stay -1.
  auto ruleInt = [&](const char* key, int* out) {
    if (auto n = parseNonNegativeInt(cfg->get(key))) *out = *n;
  };
  auto ruleBool = [&](const char* key, int* out) {
    if (auto b = parseBool(cfg->get(key))) *out = *b ? 1 : 0;
  };
  ruleInt("max_tech_pauses_per_team", &ctx.rules.tech_pauses_per_team);
  ruleInt("tech_pause_max_seconds", &ctx.rules.tech_pause_max_seconds);
  ruleBool("both_teams_unpause_required", &ctx.rules.both_teams_unpause);
  ruleBool("allow_force_ready", &ctx.rules.allow_force_ready);
  ruleInt("min_players_to_ready", &ctx.rules.min_players_to_ready);
  ruleInt("forfeit_after_seconds", &ctx.rules.forfeit_after_seconds);
  ruleBool("gg_enabled", &ctx.rules.gg_enabled);
  if (const Value* g = cfg->get("gg_threshold")) {
    const int pct = g->type == Value::Type::Number   ? GgThresholdPctFromText(std::to_string(g->num))
                    : g->type == Value::Type::String ? GgThresholdPctFromText(g->str)
                                                     : -1;
    if (pct > 0) ctx.rules.gg_threshold_pct = pct;
  }
  ruleInt("gg_min_score_diff", &ctx.rules.gg_min_score_diff);
  ruleBool("stop_command_available", &ctx.rules.stop_command_available);
  ruleBool("stop_command_no_damage", &ctx.rules.stop_command_no_damage);
  ruleInt("stop_vote_seconds", &ctx.rules.stop_vote_seconds);

  // Fallbacks if maxRounds wasn't provided explicitly.
  if (ctx.maxRounds <= 0) ctx.maxRounds = 24;
  if (!maxRoundsExplicit) {
    auto it = ctx.cvars.find("mp_maxrounds");
    if (it != ctx.cvars.end()) {
      try {
        const int n = std::stoi(it->second);
        if (n > 0 && n < 10000) ctx.maxRounds = n;
      } catch (...) {
      }
    }
  }

  // map_sides (array of strings)
  if (const Value* sides = cfg->get("map_sides"); sides && sides->type == Value::Type::Array) {
    for (const auto& v : sides->arr) {
      if (v.type == Value::Type::String && !v.str.empty()) {
        ctx.map_sides.push_back(v.str);
      }
    }
  }

  // maplist (array of strings)
  if (const Value* maps = cfg->get("maplist"); maps && maps->type == Value::Type::Array) {
    for (const auto& v : maps->arr) {
      if (v.type == Value::Type::String && !v.str.empty()) {
        ctx.maplist.push_back(v.str);
      }
    }
  }

  // spectators whitelist
  if (const Value* spec = cfg->get("spectators"); IsObject(spec)) {
    const Value* players = spec->get("players");
    if (IsObject(players)) {
      for (const auto& kv : players->obj) {
        const uint64_t sid = ParseSteamId64Loose(kv.first);
        if (!sid) continue;
        ctx.spectators.insert(sid);
      }
    }
  }

  // admins (array of SteamID64 values)
  if (const Value* admins = cfg->get("admins"); admins && admins->type == Value::Type::Array) {
    for (const auto& v : admins->arr) {
      uint64_t sid = 0;
      if (v.type == Value::Type::String) sid = ParseSteamId64Loose(v.str);
      else if (v.type == Value::Type::Number) sid = static_cast<uint64_t>(v.num);
      if (sid) ctx.admins.insert(sid);
    }
  }

  auto readTeam = [&](const char* key, std::string& nameOut, WebhookTeam teamTag) {
    const Value* team = cfg->get(key);
    if (!IsObject(team)) return;
    if (auto n = AsString(team->get("name"))) nameOut = *n;
    if (auto f = AsString(team->get("flag"))) {
      (teamTag == WebhookTeam::Team1 ? ctx.team1_flag : ctx.team2_flag) = *f;
    }
    if (auto cap = AsString(team->get("captain_steamid64"))) {
      const uint64_t sid = ParseSteamId64Loose(*cap);
      if (sid != 0) {
        if (teamTag == WebhookTeam::Team1) ctx.team1_captain_steamid64 = sid;
        if (teamTag == WebhookTeam::Team2) ctx.team2_captain_steamid64 = sid;
      }
    } else if (auto capn = AsInt(team->get("captain_steamid64"))) {
      const uint64_t sid = static_cast<uint64_t>(*capn);
      if (sid != 0) {
        if (teamTag == WebhookTeam::Team1) ctx.team1_captain_steamid64 = sid;
        if (teamTag == WebhookTeam::Team2) ctx.team2_captain_steamid64 = sid;
      }
    }
    const Value* players = team->get("players");
    if (!IsObject(players)) return;
    for (const auto& kv : players->obj) {
      const uint64_t sid = ParseSteamId64Loose(kv.first);
      if (!sid) continue;
      ctx.roster_team[sid] = teamTag;
    }
  };

  readTeam("team1", ctx.team1_name, WebhookTeam::Team1);
  readTeam("team2", ctx.team2_name, WebhookTeam::Team2);

  return ctx;
}

}  // namespace readyup

