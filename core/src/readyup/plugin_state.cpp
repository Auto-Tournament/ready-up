#include "readyup/plugin_state.h"

#include "readyup/minijson.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace readyup::plugins {

std::set<std::string> ParseDisabled(const std::string& json, bool* ok) {
  std::set<std::string> out;
  if (ok) *ok = true;
  if (json.find_first_not_of(" \t\r\n") == std::string::npos) return out;
  minijson::ParseError err;
  const auto v = minijson::Parse(json, &err);
  if (!v || !minijson::IsObject(&*v)) {
    if (ok) *ok = false;
    return out;
  }
  const minijson::Value* d = v->get("disabled");
  if (!d || d->type != minijson::Value::Type::Array) return out;
  for (const auto& e : d->arr) {
    if (e.type == minijson::Value::Type::String && !e.str.empty()) out.insert(e.str);
  }
  return out;
}

std::string DisabledJson(const std::set<std::string>& disabled) {
  std::string s = "{\n  \"version\": 1,\n  \"disabled\": [";
  bool first = true;
  for (const auto& n : disabled) {
    if (n.find_first_of("\"\\\n") != std::string::npos) continue;  // never a plugin name
    s += first ? "\n    \"" : ",\n    \"";
    s += n;  // plugin names are [a-z0-9_-] (ValidPluginName)
    s += "\"";
    first = false;
  }
  s += disabled.empty() ? "]\n}\n" : "\n  ]\n}\n";
  return s;
}

std::string PluginStatePath(const std::string& pluginsDir) {
  if (pluginsDir.empty()) return {};
  return pluginsDir + "/plugins.json";
}

std::set<std::string> LoadDisabled(const std::string& path) {
  if (path.empty()) return {};
  std::ifstream f(path);
  if (!f) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ParseDisabled(ss.str(), nullptr);
}

bool SaveDisabled(const std::string& path, const std::set<std::string>& disabled) {
  if (path.empty()) return false;
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << DisabledJson(disabled);
    if (!f) return false;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace readyup::plugins
