#include "readyup/license_status.h"

#include "readyup/license.h"
#include "readyup/logging.h"
#include "readyup/path.h"

#include <chrono>
#include <fstream>
#include <mutex>
#include <optional>

#ifndef READYUP_LINE_DATE
#define READYUP_LINE_DATE ""
#endif

namespace readyup::license {
namespace {

constexpr const char* kCfgFile = "readyup_license.cfg";

struct State {
  std::mutex mu;
  std::optional<std::string> consoleKey;  // last readyup_license_key value seen (may be "")
  bool show = false;                      // readyup_show_license
  std::string lastLogged;                 // key whose status line was printed last
  bool noKeyLogged = false;
  bool frameDone = false;
  std::optional<std::chrono::steady_clock::time_point> firstFrame;
};

State& S() {
  static State s;
  return s;
}

std::string TrimWs(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
  return s.substr(b, e - b);
}

// `name "value"` / `name value` / `name`. False when the line is another command. *hasValue is
// false for a bare `name` (a query). The value is the quoted text, or the first word.
bool ParseSetting(const std::string& rawLine, const char* name, bool* hasValue, std::string* value) {
  const std::string line = TrimWs(rawLine);
  const std::string n = name;
  if (line.compare(0, n.size(), n) != 0) return false;
  if (line.size() > n.size() && line[n.size()] != ' ' && line[n.size()] != '\t' && line[n.size()] != ';') {
    return false;
  }
  std::string rest = TrimWs(line.substr(n.size()));
  *hasValue = !rest.empty() && rest[0] != ';';
  value->clear();
  if (!*hasValue) return true;
  if (rest[0] == '"') {
    const size_t close = rest.find('"', 1);
    *value = rest.substr(1, close == std::string::npos ? std::string::npos : close - 1);
  } else {
    const size_t end = rest.find_first_of(" \t;");
    *value = rest.substr(0, end);
  }
  *value = TrimWs(*value);
  return true;
}

std::string CfgFilePath() {
  const std::string csgo = GetCsgoDirFromModuleDir();
  return csgo.empty() ? std::string() : csgo + "/cfg/" + kCfgFile;
}

// The readyup_license_key line of csgo/cfg/readyup_license.cfg, or "".
std::string KeyFromCfgFile() {
  const std::string path = CfgFilePath();
  if (path.empty()) return {};
  std::ifstream f(path);
  if (!f.good()) return {};
  std::string line, key;
  size_t read = 0;
  while (read < 64 * 1024 && std::getline(f, line)) {
    read += line.size() + 1;
    bool hasValue = false;
    std::string v;
    if (ParseSetting(line, kKeySetting, &hasValue, &v) && hasValue) key = v;  // last one wins, like exec
  }
  return key;
}

struct KeySource {
  std::string key;     // empty = no key
  std::string origin;  // where it came from
};

KeySource FindKeyLocked(State& s) {
  if (s.consoleKey) return {*s.consoleKey, kKeySetting};
  std::string k = KeyFromCfgFile();
  if (k.empty()) return {};
  return {k, CfgFilePath()};
}

Result Check(const std::string& key) {
  Options o;
  o.line_date = LineDate();
  o.today = TodayUtc();
  return Verify(key, o);
}

// Prints the status line for `key` (unless it was the last one printed and !force).
void LogKeyLocked(State& s, const std::string& key, bool force) {
  if (key.empty()) return;
  if (!force && key == s.lastLogged) return;
  s.lastLogged = key;
  PrintLine(ConsoleLine(Check(key)).c_str());
}

}  // namespace

const char* LineDate() { return READYUP_LINE_DATE; }

void LogAtLoad() {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  LogKeyLocked(s, FindKeyLocked(s).key, /*force=*/false);
}

void LogOnReload() {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  LogKeyLocked(s, FindKeyLocked(s).key, /*force=*/true);
}

void LicenseFrame() {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  if (s.frameDone) return;
  const auto now = std::chrono::steady_clock::now();
  if (!s.firstFrame) s.firstFrame = now;
  // server.cfg (and the readyup_license.cfg it execs) ran during the first map load; give
  // late cfgs a few seconds before saying there is no key.
  if (now - *s.firstFrame < std::chrono::seconds(10)) return;
  s.frameDone = true;
  if (FindKeyLocked(s).key.empty() && s.lastLogged.empty() && !s.noKeyLogged) {
    s.noKeyLogged = true;
    PrintLine(kNoKeyLine);
  }
}

bool HandleConsoleLine(const std::string& line) {
  bool hasValue = false;
  std::string value;
  State& s = S();
  if (ParseSetting(line, kKeySetting, &hasValue, &value)) {
    std::lock_guard<std::mutex> lk(s.mu);
    if (!hasValue) {
      const KeySource src = FindKeyLocked(s);
      if (src.key.empty()) {
        Print("%s is not set. %s\n", kKeySetting, kNoKeyLine);
      } else {
        const Result r = Check(src.key);
        Print("%s is set%s. %s\n", kKeySetting,
              r.valid() ? (" (license " + Printable(r.license.id, 64) + ")").c_str() : "", ConsoleLine(r).c_str());
      }
      return true;
    }
    const bool changed = !s.consoleKey || *s.consoleKey != value;
    s.consoleKey = value;
    if (!changed) return true;  // the same key again (server.cfg runs on every map load)
    if (value.empty()) {
      if (!s.lastLogged.empty()) PrintLine("License key cleared (readyup_license_key \"\").");
      s.lastLogged.clear();
      return true;
    }
    LogKeyLocked(s, value, /*force=*/false);
    return true;
  }
  if (ParseSetting(line, kShowSetting, &hasValue, &value)) {
    std::lock_guard<std::mutex> lk(s.mu);
    if (hasValue) {
      s.show = !(value.empty() || value[0] == '0' || value[0] == 'n' || value[0] == 'N' || value[0] == 'f' ||
                 value[0] == 'F');
    } else {
      Print("%s %d (1: players see \"Licensed to <licensee>\" in .help and .ru version)\n", kShowSetting,
            s.show ? 1 : 0);
    }
    return true;
  }
  return false;
}

std::vector<std::string> StatusLines() {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  const KeySource src = FindKeyLocked(s);
  const std::string line = std::string("this build's version line: ") + (*LineDate() ? LineDate() : "unknown");
  if (src.key.empty()) {
    return {kNoKeyLine, std::string("Commercial use: ") + kKeySetting + " \"ATL1...\" in server.cfg (or `csm license set`)",
            line};
  }
  const Result r = Check(src.key);
  std::vector<std::string> out = {ConsoleLine(r), "key from " + src.origin + "; " + line};
  if (!r.valid() && !r.issues.empty()) out.push_back("reason: " + r.issues.front().code);
  const std::string player = PlayerLine(r);
  if (!s.show) {
    out.push_back(std::string("players see nothing about the license (") + kShowSetting + " 0)");
  } else if (player.empty()) {
    out.push_back(std::string(kShowSetting) + " 1, but the key names no licensee to show");
  } else {
    out.push_back("players see \"" + player + "\" in .help and .ru version (" + kShowSetting + " 1)");
  }
  return out;
}

std::string PlayerLineIfShown() {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  if (!s.show) return {};
  const KeySource src = FindKeyLocked(s);
  if (src.key.empty()) return {};
  return PlayerLine(Check(src.key));
}

}  // namespace readyup::license
