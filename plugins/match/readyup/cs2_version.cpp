#include "readyup/cs2_version.h"

#include "readyup/engine.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace readyup {
namespace {

static std::string Trim(std::string s) {
  auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t i = 0;
  while (i < s.size() && is_ws(static_cast<unsigned char>(s[i]))) ++i;
  if (i) s.erase(0, i);
  return s;
}

static std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

static bool FileExists(const std::string& path) {
  std::ifstream f(path);
  return f.good();
}

static std::string ParentDir(std::string p) {
  while (!p.empty() && p.back() == '/') p.pop_back();
  const size_t slash = p.find_last_of('/');
  if (slash == std::string::npos) return {};
  if (slash == 0) return "/";
  return p.substr(0, slash);
}

static std::string FindSteamInfPathFromModuleDir(const std::string& moduleDir) {
  if (moduleDir.empty()) return {};

  // Probe common locations relative to this module:
  // - <root>/steam.inf
  // - <root>/csgo/steam.inf
  // - <root>/game/csgo/steam.inf
  static constexpr const char* kSuffixes[] = {
      "/steam.inf",
      "/csgo/steam.inf",
      "/game/csgo/steam.inf",
  };

  std::string base = moduleDir;
  for (int up = 0; up <= 10 && !base.empty(); ++up) {
    for (const char* suf : kSuffixes) {
      const std::string cand = base + suf;
      if (FileExists(cand)) return cand;
    }
    base = ParentDir(base);
  }

  return {};
}

static std::unordered_map<std::string, std::string> ParseSteamInfKvs(const std::string& path) {
  std::unordered_map<std::string, std::string> out;
  std::ifstream f(path);
  if (!f.good()) return out;

  std::string line;
  while (std::getline(f, line)) {
    line = Trim(std::move(line));
    if (line.empty()) continue;
    // steam.inf uses ';' for comments in many Valve products.
    if (!line.empty() && line[0] == ';') continue;

    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;

    std::string key = Trim(line.substr(0, eq));
    std::string val = Trim(line.substr(eq + 1));
    if (key.empty()) continue;

    // Strip optional quotes.
    if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') || (val.front() == '\'' && val.back() == '\''))) {
      val.erase(val.begin());
      val.pop_back();
    }

    out[Lower(std::move(key))] = std::move(val);
  }

  return out;
}

static std::optional<long long> ParseBuildId(const std::unordered_map<std::string, std::string>& kv) {
  auto it = kv.find("buildid");
  if (it == kv.end()) return std::nullopt;
  const std::string& s = it->second;
  if (s.empty()) return std::nullopt;
  char* end = nullptr;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (!end || end == s.c_str()) return std::nullopt;
  if (v <= 0) return std::nullopt;
  return v;
}

static std::optional<std::string> PickVersionString(const std::unordered_map<std::string, std::string>& kv) {
  // Prefer PatchVersion; fall back to ServerVersion/ClientVersion/ProductVersion.
  static constexpr const char* kKeys[] = {"patchversion", "serverversion", "clientversion", "productversion"};
  for (const char* k : kKeys) {
    auto it = kv.find(k);
    if (it != kv.end() && !it->second.empty()) {
      // Keep it compact but explicit for display/logging.
      std::string label = k;
      if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
      return label + " " + it->second;
    }
  }
  return std::nullopt;
}

struct Cache {
  std::mutex mu;
  std::chrono::steady_clock::time_point nextRefresh{};
  Cs2VersionSnapshot snap;
  bool hasValue = false;
};

Cache& C() {
  static Cache c;
  return c;
}

}  // namespace

Cs2VersionSnapshot GetCs2VersionSnapshot() {
  auto& c = C();
  const auto now = std::chrono::steady_clock::now();

  {
    std::lock_guard<std::mutex> lk(c.mu);
    if (c.hasValue && now < c.nextRefresh) {
      return c.snap;
    }
  }

  Cs2VersionSnapshot next;

  const std::string moduleDir = GetThisModuleDir();
  const std::string steamInf = FindSteamInfPathFromModuleDir(moduleDir);
  if (!steamInf.empty()) {
    const auto kv = ParseSteamInfKvs(steamInf);
    next.build_id = ParseBuildId(kv);
    next.version_string = PickVersionString(kv);

    if (!next.version_string && next.build_id) {
      next.version_string = std::string("BuildID ") + std::to_string(*next.build_id);
    }
  }

  {
    std::lock_guard<std::mutex> lk(c.mu);
    c.snap = std::move(next);
    c.hasValue = true;
    // CS2 version/build changes rarely; keep this infrequent.
    c.nextRefresh = now + std::chrono::seconds(60);
    return c.snap;
  }
}

}  // namespace readyup

