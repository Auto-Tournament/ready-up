#include "readyup/ruleset.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace readyup {

using status::Json;

namespace {

using K = RuleValue::Kind;

std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

bool IsDigits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// Values that go into a quoted cvar: printable ASCII, no quote / ';' / backslash / blank.
bool SafeQuoted(const std::string& s) {
  for (unsigned char c : s) {
    if (c <= 0x20 || c >= 0x7F || c == '"' || c == ';' || c == '\\') return false;
  }
  return true;
}

std::string KnownKeys() {
  std::string s;
  for (const auto& r : RuleTable()) {
    if (!s.empty()) s += ", ";
    s += r.key;
  }
  return s;
}

// Parses one leaf value for rule `info` (JSON).
bool ParseLeaf(const RuleInfo& info, const Json& v, RuleValue* out, std::string* err) {
  auto fail = [&](const std::string& why) {
    if (err) *err = std::string("override \"") + info.key + "\": " + why;
    return false;
  };
  switch (info.kind) {
    case K::Int: {
      long long n = 0;
      if (v.type() == Json::Type::Int) {
        n = v.AsInt();
      } else if (v.type() == Json::Type::Double && std::floor(v.AsDouble()) == v.AsDouble() &&
                 std::fabs(v.AsDouble()) < 1e12) {
        n = static_cast<long long>(v.AsDouble());
      } else {
        return fail("must be an integer");
      }
      if (n < info.min || n > info.max) {
        return fail("must be " + std::to_string(info.min) + ".." + std::to_string(info.max) + " (got " +
                    std::to_string(n) + ")");
      }
      *out = RuleValue::MakeInt(n);
      return true;
    }
    case K::Bool: {
      if (v.type() == Json::Type::Bool) {
        *out = RuleValue::MakeBool(v.AsBool());
        return true;
      }
      if (v.type() == Json::Type::Int && (v.AsInt() == 0 || v.AsInt() == 1)) {
        *out = RuleValue::MakeBool(v.AsInt() == 1);
        return true;
      }
      return fail("must be true or false");
    }
    case K::Str: {
      if (v.type() != Json::Type::String) return fail("must be a string");
      const std::string s = Trim(v.AsString());
      const std::string key = info.key;
      if (key == "cosmetics") {
        if (s != "inventory" && s != "plugin") return fail("must be \"inventory\" or \"plugin\"");
      } else if (key == "camera_man_steamid") {
        if (!s.empty() && (!IsDigits(s) || s.size() != 17)) return fail("must be a SteamID64 (17 digits) or \"\"");
      } else if (key == "tv_broadcast_url") {
        if (!s.empty() && s.rfind("http://", 0) != 0 && s.rfind("https://", 0) != 0) {
          return fail("must start with http:// or https:// (or be \"\")");
        }
        if (s.size() > 256 || !SafeQuoted(s)) return fail("has characters that cannot go into a cvar (or is too long)");
      }
      *out = RuleValue::MakeStr(s);
      return true;
    }
    case K::None: break;
  }
  return fail("unsupported");
}

// Engine cvar text -> the rule's kind (match config cvars). False if it does not parse.
bool FromCvarText(const RuleInfo& info, const std::string& raw, RuleValue* out) {
  const std::string s = Lower(Trim(raw));
  if (s.empty()) return false;
  if (info.kind == K::Bool) {
    if (s == "1" || s == "true") return *out = RuleValue::MakeBool(true), true;
    if (s == "0" || s == "false") return *out = RuleValue::MakeBool(false), true;
    return false;
  }
  if (info.kind == K::Int) {
    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    if (!end || *end != '\0' || std::floor(d) != d) return false;
    *out = RuleValue::MakeInt(static_cast<long long>(d));
    return true;
  }
  return false;
}

// Sets a (possibly dotted) key in a JSON object.
void SetPath(Json* obj, const std::string& key, Json v) {
  const auto dot = key.find('.');
  if (dot == std::string::npos) {
    (*obj)[key] = std::move(v);
    return;
  }
  (*obj)[key.substr(0, dot)][key.substr(dot + 1)] = std::move(v);
}

std::string CvarValue(const RuleValue& v) {
  if (v.kind == K::Bool) return v.i ? "1" : "0";
  if (v.kind == K::Int) return std::to_string(v.i);
  return v.s;
}

}  // namespace

// ---------------------------------------------------------------------------- basics

namespace {
std::atomic<int> g_serverRuleset{0};
}  // namespace

void SetServerRuleset(Ruleset r) { g_serverRuleset.store(r == Ruleset::Valve ? 1 : 0); }
Ruleset ServerRuleset() { return g_serverRuleset.load() == 1 ? Ruleset::Valve : Ruleset::Default; }

const char* RulesetName(Ruleset r) { return r == Ruleset::Valve ? "valve" : "default"; }

bool ParseRuleset(const std::string& s, Ruleset* out) {
  const std::string v = Lower(Trim(s));
  if (v == "default") {
    *out = Ruleset::Default;
    return true;
  }
  if (v == "valve") {
    *out = Ruleset::Valve;
    return true;
  }
  return false;
}

RuleValue RuleValue::MakeInt(long long v) {
  RuleValue r;
  r.kind = K::Int;
  r.i = v;
  return r;
}
RuleValue RuleValue::MakeBool(bool v) {
  RuleValue r;
  r.kind = K::Bool;
  r.i = v ? 1 : 0;
  return r;
}
RuleValue RuleValue::MakeStr(std::string v) {
  RuleValue r;
  r.kind = K::Str;
  r.s = std::move(v);
  return r;
}

bool RuleValue::operator==(const RuleValue& o) const {
  if (kind != o.kind) return false;
  if (kind == K::Str) return s == o.s;
  if (kind == K::None) return true;
  return i == o.i;
}

std::string RuleValue::Text() const {
  switch (kind) {
    case K::None: return "(server)";
    case K::Int: return std::to_string(i);
    case K::Bool: return i ? "true" : "false";
    case K::Str: return "\"" + s + "\"";
  }
  return "?";
}

Json RuleValue::ToJson() const {
  switch (kind) {
    case K::None: return Json();
    case K::Int: return Json(i);
    case K::Bool: return Json(i != 0);
    case K::Str: return Json(s);
  }
  return Json();
}

// ---------------------------------------------------------------------------- table

const std::vector<RuleInfo>& RuleTable() {
  static const std::vector<RuleInfo> t = {
      {"freezetime", K::Int, "mp_freezetime", 0, 120, "freeze time (s)"},
      {"tac_timeouts", K::Int, "mp_team_timeout_max", 0, 10, "tactical timeouts per team (.tac, CS2's own)"},
      {"tac_timeout_seconds", K::Int, "mp_team_timeout_time", 1, 300, "tactical timeout length (s)"},
      {"tech_pauses_per_team", K::Int, nullptr, 0, 20, "technical pauses per team per map (.tech), 0 = unlimited"},
      {"tech_pause_seconds", K::Int, nullptr, 0, 3600, "technical pause auto-unpause (s), 0 = never"},
      {"allow_knife", K::Bool, nullptr, 0, 1, "map_sides \"knife\" allowed"},
      {"overtime.enabled", K::Bool, "mp_overtime_enable", 0, 1, "overtime on a tie"},
      {"overtime.maxrounds", K::Int, "mp_overtime_maxrounds", 2, 30, "rounds per overtime (both halves)"},
      {"overtime.startmoney", K::Int, "mp_overtime_startmoney", 0, 65535, "money at each overtime half"},
      {"overtime.limit", K::Int, "mp_overtime_limit", 0, 100, "overtimes before a draw, 0 = unlimited"},
      {"zeus", K::Int, "mp_weapons_allow_zeus", -1, 10, "Zeus purchases per round (-1 unlimited, 0 off)"},
      {"spectators_max", K::Int, "mp_spectators_max", 0, 64, "spectator slots"},
      {"halftime_pausematch", K::Bool, "mp_halftime_pausematch", 0, 1, "pause after halftime (both teams .unpause)"},
      {"tv_delay", K::Int, "tv_delay", 0, 960, "CSTV delay (s)"},
      {"tv_broadcast_url", K::Str, nullptr, 0, 0, "CSTV broadcast relay URL (tv_broadcast 1 when set)"},
      {"camera_man_steamid", K::Str, nullptr, 0, 0, "tv_allow_camera_man_steamid (SteamID64)"},
      {"lan", K::Bool, nullptr, 0, 1, "LAN event (coaches admitted)"},
      {"coaches_online", K::Bool, nullptr, 0, 1, "coaches admitted as spectators online"},
      {"default_models", K::Bool, nullptr, 0, 1, "reset player models to the default agents on spawn"},
      {"cosmetics", K::Str, nullptr, 0, 0, "\"inventory\" (players' own items only) | \"plugin\" (skins plugin)"},
  };
  return t;
}

const RuleInfo* FindRule(const std::string& key) {
  for (const auto& r : RuleTable()) {
    if (key == r.key) return &r;
  }
  return nullptr;
}

RuleMap PresetRules(Ruleset r) {
  RuleMap m;
  auto I = RuleValue::MakeInt;
  auto B = RuleValue::MakeBool;
  auto S = RuleValue::MakeStr;
  if (r == Ruleset::Valve) {
    // Premier defaults (gamemode_competitive + _tmm) and Valve's exceptions (docs/ESPORTS-MODE.md).
    m["freezetime"] = I(20);
    m["tac_timeouts"] = I(3);
    m["tac_timeout_seconds"] = I(31);
    m["tech_pauses_per_team"] = I(1);   // mp_technical_timeout_per_team
    m["tech_pause_seconds"] = I(120);   // mp_technical_timeout_duration_s
    m["allow_knife"] = B(false);
    m["overtime.enabled"] = B(true);
    m["overtime.maxrounds"] = I(6);
    m["overtime.startmoney"] = I(10000);  // engine default (Premier sets none; `help` on 1.41.8)
    m["overtime.limit"] = I(0);
    m["zeus"] = I(5);
    m["spectators_max"] = I(10);
    m["halftime_pausematch"] = B(true);
    m["tv_delay"] = I(105);
    m["tv_broadcast_url"] = S("");
    m["camera_man_steamid"] = S("");
    m["lan"] = B(false);
    m["coaches_online"] = B(false);
    m["default_models"] = B(false);
    m["cosmetics"] = S("inventory");
    return m;
  }
  // Ready Up's shipped ReadyUp/live.cfg.
  m["freezetime"] = I(18);
  m["tac_timeouts"] = I(3);
  m["tac_timeout_seconds"] = I(30);
  m["allow_knife"] = B(true);
  m["overtime.enabled"] = B(true);
  m["overtime.maxrounds"] = I(6);
  m["overtime.startmoney"] = I(10000);
  m["overtime.limit"] = RuleValue{};  // Ready Up's maxOvertimes / damage tiebreak instead
  m["zeus"] = I(1);
  m["spectators_max"] = I(20);
  m["halftime_pausematch"] = B(false);
  m["tv_delay"] = RuleValue{};
  m["tv_broadcast_url"] = S("");
  m["camera_man_steamid"] = S("");
  m["lan"] = B(false);
  m["coaches_online"] = B(true);
  m["default_models"] = B(false);
  m["cosmetics"] = S("plugin");
  return m;
}

// ---------------------------------------------------------------------------- overrides

bool ParseOverrides(const Json& obj, RuleMap* out, std::string* err) {
  if (!obj.IsObject()) {
    if (err) *err = "overrides must be an object";
    return false;
  }
  RuleMap m;
  for (const auto& kv : obj.Members()) {
    const std::string& key = kv.first;
    if (kv.second.IsObject()) {
      // A group ("overtime": {...}): every member must be a dotted table key.
      bool isGroup = false;
      for (const auto& r : RuleTable()) {
        if (std::string(r.key).rfind(key + ".", 0) == 0) isGroup = true;
      }
      if (!isGroup) {
        if (err) *err = "unknown override \"" + key + "\" (known: " + KnownKeys() + ")";
        return false;
      }
      for (const auto& sub : kv.second.Members()) {
        const std::string full = key + "." + sub.first;
        const RuleInfo* info = FindRule(full);
        if (!info) {
          if (err) *err = "unknown override \"" + full + "\" (known: " + KnownKeys() + ")";
          return false;
        }
        RuleValue v;
        if (!ParseLeaf(*info, sub.second, &v, err)) return false;
        m[full] = std::move(v);
      }
      continue;
    }
    const RuleInfo* info = FindRule(key);
    if (!info) {
      if (err) *err = "unknown override \"" + key + "\" (known: " + KnownKeys() + ")";
      return false;
    }
    RuleValue v;
    if (!ParseLeaf(*info, kv.second, &v, err)) return false;
    m[key] = std::move(v);
  }
  *out = std::move(m);
  return true;
}

bool ParseOverridesText(const std::string& json, RuleMap* out, std::string* err) {
  if (Trim(json).empty()) {
    out->clear();
    return true;
  }
  Json j;
  std::string perr;
  if (!Json::Parse(json, &j, &perr)) {
    if (err) *err = "overrides: " + perr;
    return false;
  }
  return ParseOverrides(j, out, err);
}

std::string OverridesToText(const RuleMap& overrides) {
  if (overrides.empty()) return {};
  Json j = Json::Object();
  for (const auto& r : RuleTable()) {
    auto it = overrides.find(r.key);
    if (it != overrides.end() && it->second.Set()) SetPath(&j, r.key, it->second.ToJson());
  }
  return j.Dump();
}

// ---------------------------------------------------------------------------- resolution

std::vector<std::string> EffectiveRuleSet::Differs() const {
  std::vector<std::string> out;
  for (const auto& r : RuleTable()) {
    auto v = values.find(r.key);
    auto p = preset.find(r.key);
    const RuleValue a = v == values.end() ? RuleValue{} : v->second;
    const RuleValue b = p == preset.end() ? RuleValue{} : p->second;
    if (a != b) out.push_back(r.key);
  }
  return out;
}

long long EffectiveRuleSet::Int(const std::string& key, long long def) const {
  auto it = values.find(key);
  return it != values.end() && it->second.kind == K::Int ? it->second.i : def;
}

bool EffectiveRuleSet::Bool(const std::string& key, bool def) const {
  auto it = values.find(key);
  return it != values.end() && it->second.kind == K::Bool ? it->second.i != 0 : def;
}

std::string EffectiveRuleSet::Str(const std::string& key) const {
  auto it = values.find(key);
  return it != values.end() && it->second.kind == K::Str ? it->second.s : std::string();
}

EffectiveRuleSet ResolveEffective(const RulesInput& in) {
  EffectiveRuleSet e;
  e.ruleset = in.ruleset;
  e.preset = PresetRules(in.ruleset);
  const MatchRules chain = ResolveRules(in.match, in.cfg);
  if (in.ruleset == Ruleset::Default) {
    // The usual pause-rule chain is the preset.
    e.preset["tech_pauses_per_team"] = RuleValue::MakeInt(chain.tech_pauses_per_team);
    e.preset["tech_pause_seconds"] = RuleValue::MakeInt(chain.tech_pause_max_seconds);
  }
  e.values = e.preset;
  for (const auto& r : RuleTable()) e.source[r.key] = "preset";
  if (in.ruleset == Ruleset::Default) {
    auto src = [](int m, int c) { return m >= 0 ? "match" : c >= 0 ? "cfg" : "preset"; };
    e.source["tech_pauses_per_team"] = src(in.match.tech_pauses_per_team, in.cfg.tech_pauses_per_team);
    e.source["tech_pause_seconds"] = src(in.match.tech_pause_max_seconds, in.cfg.tech_pause_max_seconds);
  } else {
    // Valve: readyup.cfg (the scrim values) does not apply; a match config key is a deviation.
    if (in.match.tech_pauses_per_team >= 0) {
      e.values["tech_pauses_per_team"] = RuleValue::MakeInt(in.match.tech_pauses_per_team);
      e.source["tech_pauses_per_team"] = "match";
    }
    if (in.match.tech_pause_max_seconds >= 0) {
      e.values["tech_pause_seconds"] = RuleValue::MakeInt(in.match.tech_pause_max_seconds);
      e.source["tech_pause_seconds"] = "match";
    }
  }
  // Match config cvars run after the cfg, so they are what the engine ends up with.
  for (const auto& r : RuleTable()) {
    if (!r.cvar) continue;
    auto it = in.cvars.find(r.cvar);
    if (it == in.cvars.end()) continue;
    RuleValue v;
    if (!FromCvarText(r, it->second, &v)) continue;
    e.values[r.key] = v;
    e.source[r.key] = "cvars";
  }
  for (const auto& kv : in.overrides) {
    if (!FindRule(kv.first) || !kv.second.Set()) continue;
    e.values[kv.first] = kv.second;
    e.source[kv.first] = "override";
  }
  e.match_rules = chain;
  e.match_rules.tech_pauses_per_team = static_cast<int>(e.Int("tech_pauses_per_team", chain.tech_pauses_per_team));
  e.match_rules.tech_pause_max_seconds = static_cast<int>(e.Int("tech_pause_seconds", chain.tech_pause_max_seconds));
  return e;
}

std::vector<std::string> RuleCommands(const EffectiveRuleSet& e) {
  std::vector<std::string> out;
  for (const auto& r : RuleTable()) {
    if (!r.cvar) continue;
    auto s = e.source.find(r.key);
    if (s == e.source.end() || s->second != "override") continue;
    auto v = e.values.find(r.key);
    if (v == e.values.end() || !v->second.Set()) continue;
    out.push_back(std::string(r.cvar) + " " + CvarValue(v->second));
  }
  const std::string url = e.Str("tv_broadcast_url");
  if (!url.empty() && SafeQuoted(url)) {
    out.push_back("tv_broadcast_url \"" + url + "\"");
    out.push_back("tv_broadcast 1");
  }
  const std::string cam = e.Str("camera_man_steamid");
  if (!cam.empty() && IsDigits(cam)) out.push_back("tv_allow_camera_man_steamid " + cam);
  return out;
}

const char* LiveCfgFor(Ruleset r) { return r == Ruleset::Valve ? "ReadyUp/esports_live.cfg" : "ReadyUp/live.cfg"; }

Json EffectiveRulesJson(const EffectiveRuleSet& e) {
  Json j = Json::Object();
  j["ruleset"] = RulesetName(e.ruleset);
  Json rules = Json::Object();
  for (const auto& r : RuleTable()) {
    auto it = e.values.find(r.key);
    if (it != e.values.end() && it->second.Set()) SetPath(&rules, r.key, it->second.ToJson());
  }
  j["rules"] = std::move(rules);
  Json differs = Json::Array();
  Json preset = Json::Object();
  Json source = Json::Object();
  for (const auto& k : e.Differs()) {
    differs.Push(k);
    auto p = e.preset.find(k);
    if (p != e.preset.end() && p->second.Set()) SetPath(&preset, k, p->second.ToJson());
    auto s = e.source.find(k);
    source[k] = s == e.source.end() ? std::string("preset") : s->second;
  }
  j["differs"] = std::move(differs);
  j["preset"] = std::move(preset);
  j["source"] = std::move(source);
  return j;
}

std::vector<std::string> EffectiveRulesText(const EffectiveRuleSet& e) {
  std::vector<std::string> out;
  const auto differs = e.Differs();
  out.push_back(std::string("rules: ruleset=") + RulesetName(e.ruleset) + " cfg=" + LiveCfgFor(e.ruleset) +
                " (* = differs from the preset)");
  std::string line = "rules:";
  for (const auto& r : RuleTable()) {
    auto it = e.values.find(r.key);
    const RuleValue v = it == e.values.end() ? RuleValue{} : it->second;
    const bool d = std::find(differs.begin(), differs.end(), r.key) != differs.end();
    std::string item = std::string(r.key) + "=" + v.Text() + (d ? "*" : "");
    if (line.size() + item.size() > 110) {
      out.push_back(line);
      line = "rules:";
    }
    line += " " + item;
  }
  out.push_back(line);
  if (differs.empty()) {
    out.push_back(std::string("rules: no differences from ") + RulesetName(e.ruleset));
  } else {
    std::string d = std::string("rules: differs from ") + RulesetName(e.ruleset) + ":";
    for (const auto& k : differs) {
      auto p = e.preset.find(k);
      auto v = e.values.find(k);
      auto s = e.source.find(k);
      d += " " + k + " " + (p == e.preset.end() ? std::string("(server)") : p->second.Text()) + "->" +
           (v == e.values.end() ? std::string("(server)") : v->second.Text()) + " (" +
           (s == e.source.end() ? std::string("?") : s->second) + ");";
    }
    out.push_back(d);
  }
  return out;
}

bool CheckMapSides(const EffectiveRuleSet& e, const std::vector<std::string>& mapSides, std::string* err) {
  if (e.Bool("allow_knife", true)) return true;
  for (size_t i = 0; i < mapSides.size(); ++i) {
    if (mapSides[i] != "knife") continue;
    if (err) {
      *err = std::string("ruleset ") + RulesetName(e.ruleset) + ": map " + std::to_string(i + 1) +
             " has map_sides \"knife\", but knife rounds are off (Valve has no knife round; sides come from "
             "the veto: team1_ct / team2_ct). Set \"overrides\": {\"allow_knife\": true} to allow it.";
    }
    return false;
  }
  return true;
}

bool CoachesAdmitted(const EffectiveRuleSet& e) { return e.Bool("lan", false) || e.Bool("coaches_online", true); }

bool InventoryLocked(const EffectiveRuleSet& e) { return e.Str("cosmetics") == "inventory"; }

bool PlayerExtrasAllowed(const EffectiveRuleSet& e) { return e.ruleset != Ruleset::Valve; }

const char* GotvStateName(GotvState s) {
  return s == GotvState::Up ? "up" : s == GotvState::Down ? "down" : "unknown";
}

GotvState GotvStateFrom(bool scanOk, bool hltvSeen, double secondsSinceMapStart) {
  if (!scanOk) return GotvState::Unknown;
  if (hltvSeen) return GotvState::Up;
  return secondsSinceMapStart >= kGotvGraceSeconds ? GotvState::Down : GotvState::Unknown;
}

GoLiveVerdict GotvGoLiveCheck(Ruleset r, GotvState gotv, bool forced) {
  GoLiveVerdict v;
  if (r != Ruleset::Valve || gotv == GotvState::Up) return v;
  if (gotv == GotvState::Unknown) {
    v.log = "esports: gotv=unknown (GOTV client not readable): going live without the GOTV check";
    return v;
  }
  if (forced) {
    v.log = "esports: gotv=down: going live anyway (forced by an admin); this map has no demo";
    v.chat = "Ready Up: going live WITHOUT GOTV (forced by an admin) - this map is not recorded.";
    return v;
  }
  v.allowed = false;
  v.log = "esports: gotv=down: go-live refused (valve ruleset records every map; tv_enable was 0 when the map "
          "loaded). tv_enable 1 is set now: reload the map, or force it: ru match start force";
  v.chat = "Ready Up: not going live - GOTV is off on this map (the valve ruleset records every map). "
           "An admin reloads the map (.ru map reload) or forces it: .ru match start force";
  return v;
}

bool AutoPause5v5On(Ruleset r, const std::string* matchCvar) {
  if (matchCvar) {
    const std::string& v = *matchCvar;
    size_t b = v.find_first_not_of(" \t\"");
    if (b == std::string::npos) return false;
    return v[b] != '0' && v.compare(b, 5, "false") != 0;
  }
  return r == Ruleset::Valve;
}

}  // namespace readyup
