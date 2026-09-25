#include "whitelist_rules.h"

#include "readyup/minijson.h"

#include <cctype>
#include <cstdlib>

namespace whitelist {

uint64_t ParseSteamId64(const std::string& s) {
  if (s.size() != 17 || s.compare(0, 7, "7656119") != 0) return 0;
  for (unsigned char c : s) {
    if (!std::isdigit(c)) return 0;
  }
  return std::strtoull(s.c_str(), nullptr, 10);
}

State ParseState(const std::string& json, bool* ok) {
  using readyup::minijson::Value;
  State st;
  if (ok) *ok = true;
  if (json.find_first_not_of(" \t\r\n") == std::string::npos) return st;
  readyup::minijson::ParseError err;
  const auto v = readyup::minijson::Parse(json, &err);
  if (!v || v->type != Value::Type::Object) {
    if (ok) *ok = false;
    return st;
  }
  if (const Value* e = v->get("enabled"); e && e->type == Value::Type::Bool) st.enabled = e->b;
  if (const Value* ids = v->get("steamids"); ids && ids->type == Value::Type::Array) {
    for (const auto& x : ids->arr) {
      // Strings only: SteamID64s do not fit a JSON number (double) exactly.
      const uint64_t id = x.type == Value::Type::String ? ParseSteamId64(x.str) : 0;
      if (id) st.steamids.insert(id);
    }
  }
  return st;
}

std::string StateJson(const State& s) {
  std::string out = "{\n  \"version\": 1,\n  \"enabled\": ";
  out += s.enabled ? "true" : "false";
  out += ",\n  \"steamids\": [";
  bool first = true;
  for (uint64_t id : s.steamids) {
    out += first ? "\n    \"" : ",\n    \"";
    out += std::to_string(id) + "\"";
    first = false;
  }
  out += s.steamids.empty() ? "]\n}\n" : "\n  ]\n}\n";
  return out;
}

bool MatchOwnsRoster(const std::string& ruMode) {
  return ruMode == "match_warmup" || ruMode == "match_knife" || ruMode == "match_live" || ruMode == "postgame";
}

bool ShouldKick(const State& s, const std::string& ruMode, uint64_t steamid64, bool isBot, bool isAdmin) {
  if (!s.enabled || isBot || steamid64 == 0 || isAdmin) return false;
  if (MatchOwnsRoster(ruMode)) return false;
  return s.steamids.count(steamid64) == 0;
}

}  // namespace whitelist
