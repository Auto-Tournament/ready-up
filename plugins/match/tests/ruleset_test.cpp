// Offline tests for readyup/ruleset.h: presets, override parsing and validation, preset +
// overrides resolution (with the legacy per-match keys and match cvars), what differs, the
// commands after the go-live cfg, knife refusal, the match config parser integration, the GOTV
// go-live check and whether sv_matchpause_auto_5v5 is on.
// ctest `match_ruleset`.
#include "readyup/match_config_parser.h"
#include "readyup/ruleset.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace readyup;
using status::Json;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++g_checks;                                                                     \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

static bool Has(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

static RuleMap Overrides(const std::string& json, std::string* err = nullptr) {
  RuleMap m;
  std::string e;
  const bool ok = ParseOverridesText(json, &m, &e);
  if (err) *err = ok ? std::string() : e;
  return m;
}

static EffectiveRuleSet Resolve(Ruleset rs, const std::string& overrides = "") {
  RulesInput in;
  in.ruleset = rs;
  in.overrides = Overrides(overrides);
  return ResolveEffective(in);
}

static void TestRulesetNames() {
  Ruleset r = Ruleset::Default;
  CHECK(ParseRuleset("valve", &r) && r == Ruleset::Valve);
  CHECK(ParseRuleset(" Default ", &r) && r == Ruleset::Default);
  CHECK(ParseRuleset("VALVE", &r) && r == Ruleset::Valve);
  CHECK(!ParseRuleset("esports", &r));
  CHECK(!ParseRuleset("", &r));
  CHECK(std::string(RulesetName(Ruleset::Valve)) == "valve");
  CHECK(std::string(LiveCfgFor(Ruleset::Valve)) == "ReadyUp/esports_live.cfg");
  CHECK(std::string(LiveCfgFor(Ruleset::Default)) == "ReadyUp/live.cfg");
  // Every preset has a value (or None) for every table key.
  for (Ruleset rs : {Ruleset::Default, Ruleset::Valve}) {
    const RuleMap p = PresetRules(rs);
    for (const auto& info : RuleTable()) {
      const std::string k = info.key;
      if (rs == Ruleset::Default && (k == "tech_pauses_per_team" || k == "tech_pause_seconds")) continue;
      CHECK(p.count(k) == 1);
    }
  }
}

static void TestValvePreset() {
  const EffectiveRuleSet e = Resolve(Ruleset::Valve);
  CHECK(e.Differs().empty());
  CHECK(e.Int("freezetime", -1) == 20);
  CHECK(e.Int("tac_timeouts", -1) == 3);
  CHECK(e.Int("tac_timeout_seconds", -1) == 31);
  CHECK(e.Int("tech_pauses_per_team", -1) == 1);
  CHECK(e.Int("tech_pause_seconds", -1) == 120);
  CHECK(e.Int("zeus", -1) == 5);
  CHECK(e.Int("spectators_max", -1) == 10);
  CHECK(e.Int("overtime.limit", -1) == 0);
  CHECK(e.Int("tv_delay", -1) == 105);
  CHECK(e.Bool("halftime_pausematch", false));
  CHECK(!e.Bool("allow_knife", true));
  CHECK(!e.Bool("default_models", true));
  CHECK(!CoachesAdmitted(e));
  CHECK(InventoryLocked(e));
  CHECK(!PlayerExtrasAllowed(e));  // no practice tools, damage report or .gg / .stop votes
  CHECK(e.match_rules.tech_pauses_per_team == 1 && e.match_rules.tech_pause_max_seconds == 120);
  // Pause rules outside the table keep the usual chain (built-in defaults here).
  CHECK(e.match_rules.both_teams_unpause == 1 && e.match_rules.forfeit_after_seconds == 240);
  // The preset is the cfg's job: nothing goes out after it.
  CHECK(RuleCommands(e).empty());
}

static void TestDefaultPreset() {
  RulesInput in;
  in.cfg.tech_pauses_per_team = 5;  // readyup.cfg
  in.match.tech_pause_max_seconds = 60;  // match config key
  const EffectiveRuleSet e = ResolveEffective(in);
  // The chain is the default preset: no differences, and the flow sees exactly ResolveRules().
  CHECK(e.Differs().empty());
  CHECK(e.Int("tech_pauses_per_team", -1) == 5);
  CHECK(e.Int("tech_pause_seconds", -1) == 60);
  CHECK(e.source.at("tech_pauses_per_team") == "cfg" && e.source.at("tech_pause_seconds") == "match");
  const MatchRules chain = ResolveRules(in.match, in.cfg);
  CHECK(e.match_rules.tech_pauses_per_team == chain.tech_pauses_per_team);
  CHECK(e.match_rules.tech_pause_max_seconds == chain.tech_pause_max_seconds);
  CHECK(e.match_rules.forfeit_after_seconds == chain.forfeit_after_seconds);
  CHECK(e.Int("freezetime", -1) == 18 && e.Int("zeus", -1) == 1 && e.Bool("allow_knife", false));
  CHECK(!e.values.at("tv_delay").Set() && !e.values.at("overtime.limit").Set());
  CHECK(!InventoryLocked(e) && CoachesAdmitted(e));
  CHECK(PlayerExtrasAllowed(e));
  CHECK(RuleCommands(e).empty());
}

static void TestOverridesOnTop() {
  const EffectiveRuleSet e = Resolve(
      Ruleset::Valve,
      R"({"freezetime": 3, "overtime": {"startmoney": 12500, "limit": 2}, "default_models": true,
          "tech_pauses_per_team": 2, "tac_timeouts": 3})");
  CHECK(e.Int("freezetime", -1) == 3);
  CHECK(e.Int("overtime.startmoney", -1) == 12500);
  CHECK(e.Int("overtime.limit", -1) == 2);
  CHECK(e.Bool("default_models", false));
  CHECK(!PlayerExtrasAllowed(e));  // overrides never turn the extras back on
  CHECK(e.match_rules.tech_pauses_per_team == 2);
  const auto d = e.Differs();
  // tac_timeouts 3 is overridden but equal to Valve's: not a difference. Table order.
  CHECK((d == std::vector<std::string>{"freezetime", "tech_pauses_per_team", "overtime.startmoney", "overtime.limit",
                                       "default_models"}));
  CHECK(e.source.at("freezetime") == "override" && e.source.at("zeus") == "preset");
  const auto cmds = RuleCommands(e);
  CHECK(Has(cmds, "mp_freezetime 3"));
  CHECK(Has(cmds, "mp_overtime_startmoney 12500"));
  CHECK(Has(cmds, "mp_overtime_limit 2"));
  CHECK(Has(cmds, "mp_team_timeout_max 3"));  // overridden (same value) still goes out after the cfg
  CHECK(!Has(cmds, "mp_team_timeout_time 31"));
  for (const auto& c : cmds) CHECK(c.rfind("default_models", 0) != 0 && c.rfind("tech_", 0) != 0);

  // The same overrides on the default preset.
  const EffectiveRuleSet d2 = Resolve(Ruleset::Default, R"({"freezetime": 20, "zeus": 5, "allow_knife": false})");
  CHECK((d2.Differs() == std::vector<std::string>{"freezetime", "allow_knife", "zeus"}));
  CHECK(Has(RuleCommands(d2), "mp_weapons_allow_zeus 5"));
}

static void TestValidation() {
  std::string err;
  (void)Overrides(R"({"freezetimee": 3})", &err);
  CHECK(err.find("unknown override \"freezetimee\"") != std::string::npos);
  CHECK(err.find("known: freezetime") != std::string::npos);
  (void)Overrides(R"({"overtime": {"money": 1}})", &err);
  CHECK(err.find("unknown override \"overtime.money\"") != std::string::npos);
  (void)Overrides(R"({"pause": {"technical": 1}})", &err);
  CHECK(err.find("unknown override \"pause\"") != std::string::npos);
  (void)Overrides(R"({"freezetime": "3"})", &err);
  CHECK(err.find("\"freezetime\": must be an integer") != std::string::npos);
  (void)Overrides(R"({"freezetime": 500})", &err);
  CHECK(err.find("must be 0..120") != std::string::npos);
  (void)Overrides(R"({"freezetime": 2.5})", &err);
  CHECK(!err.empty());
  (void)Overrides(R"({"allow_knife": "yes"})", &err);
  CHECK(err.find("true or false") != std::string::npos);
  (void)Overrides(R"({"cosmetics": "skins"})", &err);
  CHECK(err.find("inventory") != std::string::npos);
  (void)Overrides(R"({"camera_man_steamid": 76561198000000000})", &err);
  CHECK(err.find("must be a string") != std::string::npos);
  (void)Overrides(R"({"camera_man_steamid": "7656"})", &err);
  CHECK(err.find("SteamID64") != std::string::npos);
  (void)Overrides(R"({"tv_broadcast_url": "ftp://x"})", &err);
  CHECK(err.find("http") != std::string::npos);
  (void)Overrides(R"({"tv_broadcast_url": "http://a b"})", &err);
  CHECK(!err.empty());
  (void)Overrides(R"([1,2])", &err);
  CHECK(err == "overrides must be an object");
  // Valid ones.
  const RuleMap ok = Overrides(
      R"({"allow_knife": 1, "tv_broadcast_url": "https://relay.example/tv", "camera_man_steamid": "76561198000000001",
          "cosmetics": "plugin", "zeus": -1, "overtime": {"enabled": false}})",
      &err);
  CHECK(err.empty());
  CHECK(ok.size() == 6 && ok.at("allow_knife").i == 1 && ok.at("overtime.enabled").i == 0);
  // Round trip through the canonical text (what the match context keeps).
  const std::string text = OverridesToText(ok);
  RuleMap back;
  CHECK(ParseOverridesText(text, &back, &err) && back == ok);
  CHECK(text.find("\"overtime\":{\"enabled\":false}") != std::string::npos);
  CHECK(OverridesToText(RuleMap{}).empty());
  CHECK(ParseOverridesText("", &back, &err) && back.empty());
}

static void TestBroadcastAndCamera() {
  const EffectiveRuleSet e = Resolve(
      Ruleset::Valve, R"({"tv_broadcast_url": "https://relay.example/tv", "camera_man_steamid": "76561198000000001",
                          "tv_delay": 90})");
  const auto cmds = RuleCommands(e);
  CHECK(Has(cmds, "tv_broadcast_url \"https://relay.example/tv\""));
  CHECK(Has(cmds, "tv_broadcast 1"));
  CHECK(Has(cmds, "tv_allow_camera_man_steamid 76561198000000001"));
  CHECK(Has(cmds, "tv_delay 90"));
}

static void TestCvarsAndLegacyKeys() {
  RulesInput in;
  in.ruleset = Ruleset::Valve;
  in.cvars["mp_freezetime"] = "15";
  in.cvars["mp_overtime_enable"] = "0";
  in.cvars["mp_roundtime"] = "1.92";  // not a table rule
  in.match.tech_pauses_per_team = 4;  // match config max_tech_pauses_per_team
  in.cfg.tech_pause_max_seconds = 30;  // readyup.cfg: not under valve
  EffectiveRuleSet e = ResolveEffective(in);
  CHECK(e.Int("freezetime", -1) == 15 && e.source.at("freezetime") == "cvars");
  CHECK(!e.Bool("overtime.enabled", true) && e.source.at("overtime.enabled") == "cvars");
  CHECK(e.Int("tech_pauses_per_team", -1) == 4 && e.source.at("tech_pauses_per_team") == "match");
  CHECK(e.Int("tech_pause_seconds", -1) == 120);
  CHECK((e.Differs() == std::vector<std::string>{"freezetime", "tech_pauses_per_team", "overtime.enabled"}));
  CHECK(RuleCommands(e).empty());  // the cvars go out on their own
  // An override beats the cvars (it goes out after them).
  in.overrides = Overrides(R"({"freezetime": 20})");
  e = ResolveEffective(in);
  CHECK(e.Int("freezetime", -1) == 20 && e.source.at("freezetime") == "override");
  CHECK(!Has(e.Differs(), "freezetime"));
  CHECK(Has(RuleCommands(e), "mp_freezetime 20"));
}

static void TestReports() {
  const EffectiveRuleSet e = Resolve(Ruleset::Valve, R"({"freezetime": 3, "overtime": {"limit": 1}})");
  const Json j = EffectiveRulesJson(e);
  CHECK(j.Find("ruleset")->AsString() == "valve");
  CHECK(j.Find("rules")->Find("freezetime")->AsInt() == 3);
  CHECK(j.Find("rules")->Find("overtime")->Find("limit")->AsInt() == 1);
  CHECK(j.Find("rules")->Find("tv_delay")->AsInt() == 105);
  CHECK(j.Find("differs")->Items().size() == 2);
  CHECK(j.Find("preset")->Find("freezetime")->AsInt() == 20);
  CHECK(j.Find("preset")->Find("overtime")->Find("limit")->AsInt() == 0);
  CHECK(j.Find("source")->Find("freezetime")->AsString() == "override");
  CHECK(j.Dump().find("null") == std::string::npos);
  // Default: unmanaged values are left out (MatchState never contains null).
  const Json d = EffectiveRulesJson(Resolve(Ruleset::Default));
  CHECK(!d.Find("rules")->Find("tv_delay"));
  CHECK(d.Find("differs")->Items().empty());
  CHECK(d.Dump().find("null") == std::string::npos);
  const auto lines = EffectiveRulesText(e);
  CHECK(!lines.empty() && lines[0].find("ruleset=valve") != std::string::npos);
  bool star = false, diff = false;
  for (const auto& l : lines) {
    if (l.find("freezetime=3*") != std::string::npos) star = true;
    if (l.find("differs from valve: freezetime 20->3 (override)") != std::string::npos) diff = true;
  }
  CHECK(star && diff);
}

static void TestMapSides() {
  std::string err;
  const EffectiveRuleSet v = Resolve(Ruleset::Valve);
  CHECK(CheckMapSides(v, {"team1_ct", "team2_ct"}, &err));
  CHECK(!CheckMapSides(v, {"team1_ct", "knife"}, &err));
  CHECK(err.find("map 2") != std::string::npos && err.find("allow_knife") != std::string::npos);
  CHECK(CheckMapSides(Resolve(Ruleset::Valve, R"({"allow_knife": true})"), {"knife"}, &err));
  CHECK(CheckMapSides(Resolve(Ruleset::Default), {"knife"}, &err));
  CHECK(!CheckMapSides(Resolve(Ruleset::Default, R"({"allow_knife": false})"), {"knife"}, &err));
}

static std::string MatConfig(const std::string& extra) {
  return R"({"matchid": 42, "num_maps": 1, "maplist": ["de_mirage"], "maxRounds": 24,
             "team1": {"name": "A", "players": {"76561198000000001": "a"}},
             "team2": {"name": "B", "players": {"76561198000000002": "b"}},
             "spectators": {"players": {"76561198000000009": "caster"}},
             "coaches": ["76561198000000003"])" +
         extra + "}";
}

static void TestParser() {
  SetServerRuleset(Ruleset::Default);
  std::string err;
  // Valve + a veto side: loads; ruleset and overrides kept on the context.
  auto ctx = ParseWebhookMatchContextFromJson(
      MatConfig(R"(, "ruleset": "valve", "map_sides": ["team2_ct"], "overrides": {"freezetime": 3},
                   "maxOvertimes": 1, "damageTiebreak": true, "overtimeMode": "disabled")"),
      &err);
  CHECK(ctx.has_value());
  if (ctx) {
    CHECK(ctx->ruleset == "valve");
    CHECK(ctx->overrides_json == R"({"freezetime":3})");
    CHECK(ctx->maxOvertimes == -1 && !ctx->damageTiebreakEnabled);  // engine overtime under valve
    CHECK(ctx->overtime_enabled && ctx->overtimeSegments == 3);
    CHECK(ctx->coaches.count(76561198000000003ull) == 1);
    CHECK(ctx->spectators.count(76561198000000003ull) == 0);  // online: coaches not admitted
    CHECK(ctx->spectators.count(76561198000000009ull) == 1);
    CHECK(ctx->ruleset_notes.size() == 2);
  }
  // Knife under valve: refused with a clear error.
  ctx = ParseWebhookMatchContextFromJson(MatConfig(R"(, "ruleset": "valve", "map_sides": ["knife"])"), &err);
  CHECK(!ctx && err.find("knife") != std::string::npos && err.find("allow_knife") != std::string::npos);
  // ... unless the organiser allows it.
  ctx = ParseWebhookMatchContextFromJson(
      MatConfig(R"(, "ruleset": "valve", "map_sides": ["knife"], "overrides": {"allow_knife": true, "lan": true})"),
      &err);
  CHECK(ctx.has_value() && ctx->spectators.count(76561198000000003ull) == 1);  // LAN: coach admitted
  // Unknown override key / bad ruleset: refused.
  ctx = ParseWebhookMatchContextFromJson(MatConfig(R"(, "overrides": {"freeze_time": 3})"), &err);
  CHECK(!ctx && err.find("unknown override \"freeze_time\"") != std::string::npos);
  ctx = ParseWebhookMatchContextFromJson(MatConfig(R"(, "ruleset": "esl")"), &err);
  CHECK(!ctx && err.find("ruleset must be") != std::string::npos);
  // No "ruleset": readyup.cfg's applies (knife refused on a valve server) ...
  SetServerRuleset(Ruleset::Valve);
  ctx = ParseWebhookMatchContextFromJson(MatConfig(R"(, "map_sides": ["knife"])"), &err);
  CHECK(!ctx && err.find("ruleset valve") != std::string::npos);
  // ... but the match config wins, and scrims keep their knife round.
  ctx = ParseWebhookMatchContextFromJson(MatConfig(R"(, "ruleset": "default", "map_sides": ["knife"])"), &err);
  CHECK(ctx.has_value() && ctx->ruleset == "default" && ctx->spectators.count(76561198000000003ull) == 1);
  ctx = ParseWebhookMatchContextFromJson(
      R"({"slug": "scrim", "config": {"matchid": 7, "map_sides": ["knife"], "maplist": ["de_dust2"]}})", &err);
  CHECK(ctx.has_value());
  SetServerRuleset(Ruleset::Default);
  // Default ruleset, no overrides: nothing changes (coach stays a spectator, knife allowed).
  ctx = ParseWebhookMatchContextFromJson(MatConfig(R"(, "map_sides": ["knife"], "maxOvertimes": 2)"), &err);
  CHECK(ctx.has_value() && ctx->ruleset.empty() && ctx->overrides_json.empty() && ctx->maxOvertimes == 2);
}

static void TestGotvGoLive() {
  // State from the controller scan: unreadable -> unknown; GOTV seen -> up; not seen -> down only
  // after the grace period (the GOTV client joins a moment after the map loads).
  CHECK(GotvStateFrom(false, false, 100) == GotvState::Unknown);
  CHECK(GotvStateFrom(false, true, 100) == GotvState::Unknown);
  CHECK(GotvStateFrom(true, true, 0) == GotvState::Up);
  CHECK(GotvStateFrom(true, false, 0) == GotvState::Unknown);
  CHECK(GotvStateFrom(true, false, kGotvGraceSeconds - 0.1) == GotvState::Unknown);
  CHECK(GotvStateFrom(true, false, kGotvGraceSeconds) == GotvState::Down);
  CHECK(std::string(GotvStateName(GotvState::Down)) == "down");
  CHECK(std::string(GotvStateName(GotvState::Up)) == "up");
  CHECK(std::string(GotvStateName(GotvState::Unknown)) == "unknown");

  // Default ruleset: never refused, nothing said.
  for (GotvState g : {GotvState::Up, GotvState::Down, GotvState::Unknown}) {
    for (bool forced : {false, true}) {
      const GoLiveVerdict v = GotvGoLiveCheck(Ruleset::Default, g, forced);
      CHECK(v.allowed && v.log.empty() && v.chat.empty());
    }
  }
  // Valve + GOTV up: allowed, nothing said (forced or not).
  CHECK(GotvGoLiveCheck(Ruleset::Valve, GotvState::Up, false).allowed);
  CHECK(GotvGoLiveCheck(Ruleset::Valve, GotvState::Up, true).chat.empty());
  // Valve + GOTV down: refused with a console line and a chat line naming the override.
  GoLiveVerdict v = GotvGoLiveCheck(Ruleset::Valve, GotvState::Down, false);
  CHECK(!v.allowed);
  CHECK(v.log.find("gotv=down") != std::string::npos && v.log.find("refused") != std::string::npos);
  CHECK(v.log.find("ru match start force") != std::string::npos);
  CHECK(v.chat.find("GOTV is off") != std::string::npos && v.chat.find(".ru match start force") != std::string::npos);
  CHECK(v.chat.size() < 190);  // one chat message
  // Forced: allowed, but everyone is told the map is not recorded.
  v = GotvGoLiveCheck(Ruleset::Valve, GotvState::Down, true);
  CHECK(v.allowed && v.log.find("forced") != std::string::npos && v.chat.find("WITHOUT GOTV") != std::string::npos);
  // Unknown (engine surface missing): allowed with a note, no chat.
  v = GotvGoLiveCheck(Ruleset::Valve, GotvState::Unknown, false);
  CHECK(v.allowed && v.log.find("gotv=unknown") != std::string::npos && v.chat.empty());
}

static void TestAutoPause5v5() {
  // esports_live.cfg turns it on, live.cfg off; a match cvar wins either way.
  CHECK(AutoPause5v5On(Ruleset::Valve, nullptr));
  CHECK(!AutoPause5v5On(Ruleset::Default, nullptr));
  const std::string zero = "0", one = "1", quotedZero = "\"0\"", f = "false", t = "true", empty = "";
  CHECK(!AutoPause5v5On(Ruleset::Valve, &zero));
  CHECK(!AutoPause5v5On(Ruleset::Valve, &quotedZero));
  CHECK(!AutoPause5v5On(Ruleset::Valve, &f));
  CHECK(!AutoPause5v5On(Ruleset::Valve, &empty));
  CHECK(AutoPause5v5On(Ruleset::Default, &one));
  CHECK(AutoPause5v5On(Ruleset::Default, &t));
}

int main() {
  TestRulesetNames();
  TestValvePreset();
  TestDefaultPreset();
  TestOverridesOnTop();
  TestValidation();
  TestBroadcastAndCamera();
  TestCvarsAndLegacyKeys();
  TestReports();
  TestMapSides();
  TestParser();
  TestGotvGoLive();
  TestAutoPause5v5();
  std::printf("match_ruleset: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
