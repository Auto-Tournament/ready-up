#include "fleet_cmds.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace fleet::cmds {

namespace {
bool Fail(std::string* err, const std::string& why) {
  if (err) *err = why;
  return false;
}
bool ValidPluginName(const std::string& n) {
  if (n.empty() || n.size() > 32) return false;
  for (unsigned char c : n) {
    if (!(std::islower(c) || std::isdigit(c) || c == '_' || c == '-')) return false;
  }
  return true;
}
}  // namespace

bool OwnedByFleet(const std::string& name) {
  return name == "plugins.set" || name == "whitelist.set" || name == "practice.set" || name == "say";
}

bool ParsePluginsSet(const json::Value& args, std::vector<std::string>* enable, std::vector<std::string>* disable,
                     std::string* err) {
  std::vector<std::string> on, off;
  for (const char* key : {"enable", "disable"}) {
    const json::Value* list = args.Get(key);
    if (!list || list->IsNull()) continue;
    if (!list->IsArr()) return Fail(err, std::string(key) + " must be an array of plugin names");
    if (list->a.size() > 16) return Fail(err, std::string(key) + ": at most 16 plugins");
    auto& out = std::string(key) == "enable" ? on : off;
    for (const json::Value& v : list->a) {
      if (!v.IsStr() || !ValidPluginName(v.s)) return Fail(err, std::string(key) + ": not a plugin name (want [a-z0-9_-], 1..32)");
      out.push_back(v.s);
    }
  }
  if (on.empty() && off.empty()) return Fail(err, "nothing to enable or disable");
  for (const auto& n : off) {
    if (n == "fleet") return Fail(err, "cannot disable fleet over the fleet link (the link runs in it)");
    if (std::find(on.begin(), on.end(), n) != on.end()) return Fail(err, n + " is in both enable and disable");
  }
  if (enable) *enable = std::move(on);
  if (disable) *disable = std::move(off);
  return true;
}

bool ParseWhitelistSet(const json::Value& args, bool* enabled, std::vector<uint64_t>* steamids, std::string* err) {
  const json::Value* e = args.Get("enabled");
  if (!e || e->t != json::Value::T::Bool) return Fail(err, "enabled (boolean) is required");
  std::vector<uint64_t> ids;
  if (const json::Value* list = args.Get("steamids"); list && !list->IsNull()) {
    if (!list->IsArr()) return Fail(err, "steamids must be an array of SteamID64 strings");
    if (list->a.size() > 1000) return Fail(err, "steamids: at most 1000");
    for (const json::Value& v : list->a) {
      const std::string s = v.IsStr() ? v.s : std::string();
      bool ok = s.size() == 17 && s.compare(0, 7, "7656119") == 0;
      for (unsigned char c : s) ok = ok && std::isdigit(c);
      if (!ok) return Fail(err, "steamids: \"" + s.substr(0, 32) + "\" is not a SteamID64 string");
      ids.push_back(std::strtoull(s.c_str(), nullptr, 10));
    }
  }
  if (enabled) *enabled = e->b;
  if (steamids) *steamids = std::move(ids);
  return true;
}

bool ParsePracticeSet(const json::Value& args, int* on, int* always, std::string* err) {
  int o = -1, a = -1;
  if (const json::Value* v = args.Get("on"); v && !v->IsNull()) {
    if (v->t != json::Value::T::Bool) return Fail(err, "on must be a boolean");
    o = v->b ? 1 : 0;
  }
  if (const json::Value* v = args.Get("always"); v && !v->IsNull()) {
    if (v->t != json::Value::T::Bool) return Fail(err, "always must be a boolean");
    a = v->b ? 1 : 0;
  }
  if (o < 0 && a < 0) return Fail(err, "on or always (boolean) is required");
  if (on) *on = o;
  if (always) *always = a;
  return true;
}

std::string SanitizeSay(const std::string& text) {
  std::string s;
  for (unsigned char c : text) {
    if (c < 0x20 || c == 0x7f) continue;
    s.push_back(static_cast<char>(c));
  }
  if (s.size() > 190) {
    size_t cut = 190;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;  // UTF-8 boundary
    s.resize(cut);
  }
  return s;
}

Result Ok() { return Result{}; }
Result Rejected(const std::string& code, const std::string& message) {
  Result r;
  r.status = "rejected";
  r.code = code;
  r.message = message;
  return r;
}
Result Failed(const std::string& code, const std::string& message) {
  Result r = Rejected(code, message);
  r.status = "failed";
  return r;
}

std::string ResultPayload(const Result& r, const std::string& auditId) {
  json::Value p = json::Value::Object();
  p.Set("status", json::Value::Str(r.status));
  if (!r.code.empty()) {
    json::Value e = json::Value::Object();
    e.Set("code", json::Value::Str(r.code));
    e.Set("message", json::Value::Str(r.message.empty() ? r.code : r.message));
    p.Set("error", std::move(e));
  }
  if (!auditId.empty()) p.Set("audit_id", json::Value::Str(auditId));
  return json::Dump(p);
}

}  // namespace fleet::cmds
