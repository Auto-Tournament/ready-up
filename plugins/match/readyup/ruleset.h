#pragma once

// Rulesets (docs/ESPORTS-MODE.md): a preset of match rules plus per-match named overrides.
//
//   ruleset  "default": Ready Up's shipped cfgs (ReadyUp/live.cfg, MatchZy-style values).
//            "valve":   Valve's CS Major rulebook: go-live execs ReadyUp/esports_live.cfg
//                       (Premier defaults + Valve's exception list), knife rounds refused,
//                       inventories left alone (skins plugin inert), counted technical pauses.
//   Where it comes from: match config `"ruleset"` (fleet: match.assign config.rules.ruleset) >
//   readyup.cfg `ruleset=` > "default".
//
//   overrides  match config `"overrides": {...}` (fleet: config.rules.overrides): one named key
//              per rule (RuleTable()), applied on top of the preset. Unknown keys, wrong types
//              and out-of-range values are errors: the match is not loaded.
//
// The effective rules (preset + legacy per-match keys + match cvars + overrides) are reported in
// MatchState `effective_rules`, `ru match rules` and `ru match state`, with the keys that differ from the
// preset, so the platform can show "differs from Valve".
//
// Pure logic, no engine calls (ctest `match_ruleset`); the engine side is esports.h.

#include "readyup/match_rules.h"
#include "readyup/status_snapshot.h"

#include <map>
#include <string>
#include <vector>

namespace readyup {

enum class Ruleset { Default, Valve };

const char* RulesetName(Ruleset r);
// "default" | "valve" (case-insensitive, surrounding blanks ignored). False for anything else.
bool ParseRuleset(const std::string& s, Ruleset* out);
// readyup.cfg `ruleset=` (config.cpp sets it on every (re)load; the parser and the fleet assign
// validation use it when a match config has no "ruleset"). Any thread.
void SetServerRuleset(Ruleset r);
Ruleset ServerRuleset();

struct RuleValue {
  enum class Kind { None, Int, Bool, Str };
  Kind kind = Kind::None;  // None: not managed (the server's own value)
  long long i = 0;         // Int, Bool (0 / 1)
  std::string s;           // Str

  static RuleValue MakeInt(long long v);
  static RuleValue MakeBool(bool v);
  static RuleValue MakeStr(std::string v);
  bool Set() const { return kind != Kind::None; }
  bool operator==(const RuleValue& o) const;
  bool operator!=(const RuleValue& o) const { return !(*this == o); }
  // Report form: 20, true, "text", (server) for None.
  std::string Text() const;
  status::Json ToJson() const;  // null for None
};

// One rule. Keys are flat; "overtime.limit" is the `"overtime": {"limit": ...}` member.
struct RuleInfo {
  const char* key;
  RuleValue::Kind kind;
  const char* cvar;  // engine cvar the value is written to after the go-live cfg (nullptr: none)
  long long min;     // Int range (inclusive)
  long long max;
  const char* help;
};
const std::vector<RuleInfo>& RuleTable();
const RuleInfo* FindRule(const std::string& key);

using RuleMap = std::map<std::string, RuleValue>;

// The preset values. For Default, tech_pauses_per_team / tech_pause_seconds are not in the map:
// they come from the pause-rule chain (match config keys > readyup.cfg > built-in, match_rules.h).
RuleMap PresetRules(Ruleset r);

// A match config "overrides" object. Nested objects only where the table has dotted keys
// ("overtime"). False + *err (naming the key) on an unknown key, a wrong type or a bad value.
bool ParseOverrides(const status::Json& obj, RuleMap* out, std::string* err);
bool ParseOverridesText(const std::string& json, RuleMap* out, std::string* err);
// Canonical JSON text of parsed overrides ("" for none), nested again ("overtime": {...}).
std::string OverridesToText(const RuleMap& overrides);

struct RulesInput {
  Ruleset ruleset = Ruleset::Default;
  RuleMap overrides;
  MatchRules match;  // per-match pause/ready/forfeit keys (max_tech_pauses_per_team, ...), -1 unset
  MatchRules cfg;    // readyup.cfg values of the same keys
  std::map<std::string, std::string> cvars;  // match config cvars (applied after the cfg)
};

struct EffectiveRuleSet {
  Ruleset ruleset = Ruleset::Default;
  RuleMap values;                              // every table key (None = the server's value)
  RuleMap preset;                              // the preset value per key
  std::map<std::string, std::string> source;  // key -> "preset" | "override" | "match" | "cvars" | "cfg"
  MatchRules match_rules;                      // what the match flow enforces (no -1)

  // Keys whose effective value differs from the preset, in table order.
  std::vector<std::string> Differs() const;
  long long Int(const std::string& key, long long def) const;
  bool Bool(const std::string& key, bool def) const;
  std::string Str(const std::string& key) const;
};

// Preset, then (Valve) legacy per-match pause keys, then match cvars for cvar-backed rules, then
// the overrides. The pause/ready/forfeit rules outside the table keep their usual chain.
EffectiveRuleSet ResolveEffective(const RulesInput& in);

// Commands that go out after the go-live cfg and the match cvars (so they win over both):
// overridden cvar-backed rules, tv_broadcast_url + tv_broadcast, tv_allow_camera_man_steamid.
std::vector<std::string> RuleCommands(const EffectiveRuleSet& e);

// The cfg go-live execs.
const char* LiveCfgFor(Ruleset r);  // "ReadyUp/live.cfg" | "ReadyUp/esports_live.cfg"

// MatchState `effective_rules`: {ruleset, rules: {...}, differs: [...], preset: {differing keys}}.
// Never contains null (unmanaged values are left out).
status::Json EffectiveRulesJson(const EffectiveRuleSet& e);
// `ru match rules` lines.
std::vector<std::string> EffectiveRulesText(const EffectiveRuleSet& e);

// Match load checks. map_sides "knife" needs allow_knife (Valve has no knife round).
bool CheckMapSides(const EffectiveRuleSet& e, const std::vector<std::string>& mapSides, std::string* err);
// Coaches are admitted (as spectators) on LAN or when coaches_online is on.
bool CoachesAdmitted(const EffectiveRuleSet& e);
// Players' inventories must not be modified (cosmetics "inventory"): the skins plugin is inert.
bool InventoryLocked(const EffectiveRuleSet& e);
// Player extras outside Valve's rulebook (practice tools, end-of-round damage report, .gg / .stop
// votes) run only outside the valve ruleset; under it they do nothing, like the skins plugin.
bool PlayerExtrasAllowed(const EffectiveRuleSet& e);


// ---- GOTV at go-live -------------------------------------------------------------------------
//
// Under valve every map is recorded (rulebook L356-357), and GOTV only records when tv_enable was
// 1 when the map loaded. So a valve match does not go live on a map without GOTV, unless an admin
// forces it (`ru match start force`, fleet cmd `start` {"force": true}).

enum class GotvState { Unknown, Up, Down };
const char* GotvStateName(GotvState s);  // "unknown" | "up" | "down"

// What the engine side saw. scanOk: the controllers could be read (entity system + the
// CBasePlayerController::m_bIsHLTV offset); hltvSeen: one of them is the GOTV client. The GOTV
// client joins a moment after the map loads, so "down" needs the map to have run
// kGotvGraceSeconds first (until then: unknown).
constexpr double kGotvGraceSeconds = 15.0;
GotvState GotvStateFrom(bool scanOk, bool hltvSeen, double secondsSinceMapStart);

struct GoLiveVerdict {
  bool allowed = true;
  std::string log;   // console line ("" = nothing to say)
  std::string chat;  // chat line ("" = nothing to say)
};
// Default ruleset: always allowed, nothing to say. Valve: GOTV down refuses unless forced (then a
// warning); unknown is allowed with a note (the check needs the engine surface).
GoLiveVerdict GotvGoLiveCheck(Ruleset r, GotvState gotv, bool forced);

// ---- sv_matchpause_auto_5v5 ------------------------------------------------------------------

// Whether the engine's "not 5v5" pause is on for a match: esports_live.cfg sets it to 1 (valve),
// live.cfg to 0 (default); a match cvar `sv_matchpause_auto_5v5` (nullptr: not set) wins.
bool AutoPause5v5On(Ruleset r, const std::string* matchCvar);

}  // namespace readyup
