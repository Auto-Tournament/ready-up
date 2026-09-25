// Pure parts of load_order.h (no loader or engine access; tests/load_order_test.cpp).
#include "readyup/load_order.h"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace readyup {
namespace {

std::string Trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::vector<std::string> Tokens(const std::string& line) {
  std::vector<std::string> out;
  std::istringstream in(line);
  std::string t;
  while (in >> t) out.push_back(t);
  return out;
}

std::string Unquote(std::string s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

}  // namespace

GameSearchPaths ParseGameSearchPaths(const std::string& text) {
  GameSearchPaths out;
  std::istringstream in(text);
  std::string raw;
  bool sawHeader = false;
  int depth = 0;  // brace depth inside SearchPaths
  int gameIndex = 0;
  while (std::getline(in, raw)) {
    std::string line = raw;
    const auto cmt = line.find("//");
    if (cmt != std::string::npos) line = line.substr(0, cmt);
    line = Trim(line);
    if (line.empty()) continue;

    if (depth == 0) {
      if (!sawHeader) {
        if (Unquote(line) == "SearchPaths") sawHeader = true;
        continue;
      }
      if (line[0] == '{') {
        depth = 1;
        continue;
      }
      sawHeader = false;  // `SearchPaths` not followed by a block
      continue;
    }

    if (line[0] == '{') {
      ++depth;
      continue;
    }
    if (line[0] == '}') {
      if (--depth == 0) break;  // first SearchPaths block only
      continue;
    }
    if (depth != 1) continue;

    const auto tok = Tokens(line);
    if (tok.size() < 2) continue;
    const std::string key = Unquote(tok[0]);
    if (key != "Game" && key != "game" && key != "GAME") continue;  // not Game_LowViolence etc.
    const std::string val = Unquote(tok[1]);
    if (val == "csgo/readyup") {
      if (out.readyup < 0) out.readyup = gameIndex;
      ++out.readyup_count;
    } else if (val == "csgo/addons/metamod") {
      if (out.metamod < 0) out.metamod = gameIndex;
    } else if (val == "csgo") {
      if (out.csgo < 0) out.csgo = gameIndex;
    }
    ++gameIndex;
  }
  return out;
}

std::string DescribeLoadOrderProblem(const GameSearchPaths& p) {
  if (p.readyup < 0) return {};  // not listed here (e.g. gameinfo_branchspecific.gi): cannot judge
  if (p.readyup_count > 1) {
    return "csgo/readyup is listed " + std::to_string(p.readyup_count) +
           " times in gameinfo.gi; keep exactly one line";
  }
  const bool metamodActive = p.metamod >= 0 && (p.csgo < 0 || p.metamod < p.csgo);
  if (metamodActive && p.metamod > p.readyup) {
    return "Metamod (Game csgo/addons/metamod) is listed BELOW csgo/readyup in gameinfo.gi: the engine "
           "loads Ready Up first, so Metamod and everything it loads (CounterStrikeSharp, Metamod "
           "plugins) never start. Put Ready Up's line directly below Metamod's";
  }
  return {};
}

bool ShimInstanceMayRun(const std::string& marker, long pid, const std::string& ourPath, std::string* otherPath) {
  if (marker.empty()) return true;
  char* end = nullptr;
  const long markerPid = std::strtol(marker.c_str(), &end, 10);
  if (end == marker.c_str() || markerPid != pid) return true;  // left over from a parent process
  std::string path = (end && *end == ' ') ? std::string(end + 1) : std::string();
  if (path == ourPath) return true;
  if (otherPath) *otherPath = path;
  return false;
}

}  // namespace readyup
