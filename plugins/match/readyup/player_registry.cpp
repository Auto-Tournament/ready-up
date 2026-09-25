#include "readyup/player_registry.h"

#include "readyup/logging.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>

namespace readyup {
namespace {

std::mutex g_mu;
std::unordered_map<uint64_t, PlayerInfo> g_players;

static std::string ToLower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

static std::string Trim(std::string s) {
  auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t i = 0;
  while (i < s.size() && is_ws(static_cast<unsigned char>(s[i]))) ++i;
  if (i) s.erase(0, i);
  return s;
}

static int ScoreMatch(const std::string& nameLower, const std::string& fragLower) {
  if (fragLower.empty()) return -1;
  if (nameLower == fragLower) return 300;
  if (nameLower.rfind(fragLower, 0) == 0) return 200;  // prefix
  if (nameLower.find(fragLower) != std::string::npos) return 100;  // substring
  return -1;
}

}  // namespace

void ObservePlayer(uint64_t steamid64, const std::string& name) {
  if (steamid64 == 0) return;
  std::string n = Trim(name);
  if (n.empty()) return;
  std::lock_guard<std::mutex> lk(g_mu);
  auto& p = g_players[steamid64];
  p.steamid64 = steamid64;
  p.name = std::move(n);
}

std::optional<PlayerInfo> ClosestPlayerMatch(const std::string& fragment, std::string* err) {
  const std::string frag = Trim(fragment);
  if (frag.empty()) {
    if (err) *err = "missing name fragment";
    return std::nullopt;
  }

  const std::string fragLower = ToLower(frag);

  std::lock_guard<std::mutex> lk(g_mu);

  int bestScore = -1;
  std::optional<PlayerInfo> best;
  bool tie = false;

  for (const auto& kv : g_players) {
    const PlayerInfo& p = kv.second;
    const std::string nameLower = ToLower(p.name);
    const int s = ScoreMatch(nameLower, fragLower);
    if (s < 0) continue;

    if (s > bestScore) {
      bestScore = s;
      best = p;
      tie = false;
    } else if (s == bestScore && best && p.steamid64 != best->steamid64) {
      // Ambiguous at the same score tier.
      tie = true;
    }
  }

  if (!best) {
    if (err) *err = "no matching player observed";
    return std::nullopt;
  }
  if (tie) {
    if (err) *err = "ambiguous match (multiple players match)";
    return std::nullopt;
  }
  return best;
}

std::vector<PlayerInfo> ListObservedPlayers() {
  std::vector<PlayerInfo> out;
  std::lock_guard<std::mutex> lk(g_mu);
  out.reserve(g_players.size());
  for (const auto& kv : g_players) out.push_back(kv.second);
  std::sort(out.begin(), out.end(), [](const PlayerInfo& a, const PlayerInfo& b) {
    return a.name < b.name;
  });
  return out;
}

}  // namespace readyup

