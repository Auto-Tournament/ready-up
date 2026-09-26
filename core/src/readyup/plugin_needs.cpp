// Plugin needs: parsing and the load decision (plugin_needs.h). Engine-free.

#include "readyup/plugin_needs.h"

#include "readyup/minijson.h"

#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace readyup::plugins {
namespace {

bool StringList(const minijson::Value& root, const char* key, std::vector<std::string>* out, std::string* err) {
  const minijson::Value* v = root.get(key);
  if (!v || v->type == minijson::Value::Type::Null) return true;
  if (v->type != minijson::Value::Type::Array) {
    if (err) *err = std::string("\"") + key + "\" is not an array";
    return false;
  }
  for (const auto& e : v->arr) {
    if (e.type != minijson::Value::Type::String || e.str.empty()) {
      if (err) *err = std::string("\"") + key + "\" has a non-string entry";
      return false;
    }
    out->push_back(e.str);
  }
  return true;
}

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else if (c != ' ') {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

std::string Hex(int v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%x", v);
  return buf;
}

// Schema entry "A.m_x|B.m_x": first alternative that resolves wins (same order the plugin tries).
NeedCheck CheckSchema(const std::string& kind, const std::string& entry, const NeedsProbe& p) {
  NeedCheck c{kind, entry, 0, "not found"};
  bool pending = false;
  for (const auto& alt : Split(entry, '|')) {
    const size_t dot = alt.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= alt.size()) {
      c.detail = "malformed entry";
      continue;
    }
    const int off = p.schema ? p.schema(alt.substr(0, dot), alt.substr(dot + 1)) : -2;
    if (off >= 0) {
      c.state = 1;
      c.detail = Hex(off) + (alt == entry ? "" : " (" + alt + ")");
      return c;
    }
    if (off == -2) pending = true;
  }
  if (pending) {
    c.state = -1;
    c.detail = "schema system not ready";
  }
  return c;
}

}  // namespace

std::optional<PluginNeeds> ParseNeeds(const std::string& json, std::string* err) {
  minijson::ParseError pe;
  const auto root = minijson::Parse(json, &pe);
  if (!root) {
    if (err) *err = "invalid JSON: " + pe.msg;
    return std::nullopt;
  }
  if (root->type != minijson::Value::Type::Object) {
    if (err) *err = "not a JSON object";
    return std::nullopt;
  }
  PluginNeeds n;
  if (const auto* p = root->get("plugin"); minijson::IsString(p)) n.plugin = p->str;
  if (!StringList(*root, "surface", &n.surface, err) || !StringList(*root, "schema", &n.schema, err) ||
      !StringList(*root, "schema_optional", &n.schemaOptional, err) || !StringList(*root, "events", &n.events, err)) {
    return std::nullopt;
  }
  return n;
}

std::optional<PluginNeeds> LoadNeedsFile(const std::string& path, std::string* err) {
  if (err) err->clear();
  struct stat st {};
  if (stat(path.c_str(), &st) != 0) return std::nullopt;
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (err) *err = "cannot read " + path;
    return std::nullopt;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return ParseNeeds(ss.str(), err);
}

NeedsVerdict EvaluateNeeds(const PluginNeeds& needs, const NeedsProbe& probe) {
  NeedsVerdict v;
  std::vector<std::string> missing;
  for (const auto& s : needs.surface) {
    const int st = probe.surface ? probe.surface(s) : -1;
    v.checks.push_back({"surface", s, st, st == 1 ? "resolved" : st == 0 ? "unresolved on this build" : "not checked yet"});
    if (st == 0) missing.push_back(s);
  }
  for (const auto& s : needs.schema) {
    v.checks.push_back(CheckSchema("schema", s, probe));
    if (v.checks.back().state == 0) missing.push_back(s);
  }
  for (const auto& s : needs.schemaOptional) {
    v.checks.push_back(CheckSchema("schema_optional", s, probe));
    if (v.checks.back().state == 0) v.warnings.push_back("optional schema field " + s + " not found");
  }
  for (const auto& e : needs.events) {
    const int st = probe.event ? probe.event(e) : -1;
    v.checks.push_back({"event", e, st, st == 1 ? "known" : st == 0 ? "unknown to this CS2 build" : "event manager not ready"});
    if (st == 0) v.warnings.push_back("game event " + e + " unknown to this CS2 build");
  }
  if (!missing.empty()) {
    v.load = false;
    std::string what;
    for (size_t i = 0; i < missing.size() && i < 3; ++i) what += (i ? ", " : "") + missing[i];
    if (missing.size() > 3) what += " (+" + std::to_string(missing.size() - 3) + " more)";
    v.reason = "missing " + what + " after CS2 build " + (probe.cs2Build.empty() ? "?" : probe.cs2Build);
  }
  return v;
}

std::string NeedsDisabledLine(const std::string& plugin, const std::string& reason) {
  return "plugin[" + plugin + "] disabled: " + reason;
}

}  // namespace readyup::plugins
