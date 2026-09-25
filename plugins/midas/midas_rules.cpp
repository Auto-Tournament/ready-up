#include "midas_rules.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace midas {
namespace {

std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

}  // namespace

std::set<uint64_t> ParseSteamIds(const std::string& text, int* bad) {
  std::set<uint64_t> out;
  int nbad = 0;
  std::string cur;
  auto flush = [&] {
    if (cur.empty()) return;
    bool digits = cur.size() == 17 && cur.compare(0, 7, "7656119") == 0;
    for (unsigned char c : cur) digits = digits && std::isdigit(c);
    if (digits) out.insert(std::strtoull(cur.c_str(), nullptr, 10));
    else ++nbad;
    cur.clear();
  };
  for (char c : text) {
    if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c))) flush();
    else cur.push_back(c);
  }
  flush();
  if (bad) *bad = nbad;
  return out;
}

bool ParseColor(const std::string& text, Rgba* out) {
  std::vector<int> v;
  std::string cur;
  const std::string t = Trim(text) + ",";
  for (char c : t) {
    if (c == ',') {
      const std::string p = Trim(cur);
      if (p.empty() || p.size() > 3) return false;
      for (unsigned char d : p) {
        if (!std::isdigit(d)) return false;
      }
      const int n = std::atoi(p.c_str());
      if (n > 255) return false;
      v.push_back(n);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (v.size() != 3 && v.size() != 4) return false;
  if (out) {
    out->r = static_cast<uint8_t>(v[0]);
    out->g = static_cast<uint8_t>(v[1]);
    out->b = static_cast<uint8_t>(v[2]);
    out->a = static_cast<uint8_t>(v.size() == 4 ? v[3] : 255);
  }
  return true;
}

bool ParseBool(const std::string& text, bool def) {
  const std::string t = Lower(Trim(text));
  if (t == "1" || t == "true" || t == "yes" || t == "on") return true;
  if (t == "0" || t == "false" || t == "no" || t == "off") return false;
  return def;
}

bool Active(bool enabled, const std::string& ruleset) { return enabled && Lower(Trim(ruleset)) != "valve"; }

bool ShouldTint(bool active, const std::set<uint64_t>& midas, uint64_t owner) {
  return active && owner != 0 && midas.count(owner) != 0;
}

int ParseInt(const std::string& text, int def, int lo, int hi) {
  const std::string t = Trim(text);
  if (t.empty() || t.size() > 10) return def;
  char* end = nullptr;
  errno = 0;
  const long v = std::strtol(t.c_str(), &end, 10);
  if (errno != 0 || !end || *end != '\0' || v < lo || v > hi) return def;
  return static_cast<int>(v);
}

float ParseFloat(const std::string& text, float def, float lo, float hi) {
  const std::string t = Trim(text);
  if (t.empty()) return def;
  char* end = nullptr;
  errno = 0;
  const double v = std::strtod(t.c_str(), &end);
  if (errno != 0 || !end || *end != '\0' || !std::isfinite(v) || v < lo || v > hi) return def;
  return static_cast<float>(v);
}

bool ParseFinish(const std::string& text, Finish* out) {
  const std::string t = Lower(Trim(text));
  Finish f;
  if (t == "auto" || t == "paint") f = Finish::kAuto;
  else if (t == "tint") f = Finish::kTint;
  else return false;
  if (out) *out = f;
  return true;
}

bool Paintable(const std::string& classname) {
  const std::string c = Lower(classname);
  if (c.compare(0, 7, "weapon_") != 0) return false;
  if (c.compare(0, 12, "weapon_knife") == 0 || c == "weapon_bayonet") return false;
  static const char* const kNoPaint[] = {"weapon_c4",        "weapon_hegrenade", "weapon_flashbang",
                                         "weapon_smokegrenade", "weapon_molotov", "weapon_incgrenade",
                                         "weapon_decoy",     "weapon_tagrenade", "weapon_healthshot",
                                         "weapon_breachcharge", "weapon_shield", "weapon_snowball"};
  for (const char* n : kNoPaint) {
    if (c == n) return false;
  }
  return true;
}

bool ParseBestStat(const std::string& text, BestStat* out) {
  const std::string t = Lower(Trim(text));
  BestStat v;
  if (t == "adr") v = BestStat::kAdr;
  else if (t == "kills") v = BestStat::kKills;
  else return false;
  if (out) *out = v;
  return true;
}

bool ParseBestWhen(const std::string& text, BestWhen* out) {
  const std::string t = Lower(Trim(text));
  BestWhen v;
  if (t == "round") v = BestWhen::kRound;
  else if (t == "half") v = BestWhen::kHalf;
  else return false;
  if (out) *out = v;
  return true;
}

double Adr(const PlayerTotals& p) { return p.rounds > 0 ? static_cast<double>(p.damage) / p.rounds : 0.0; }

bool BestPlayerAllowed(bool enabled, bool inMatches, bool live, bool scrim, const std::string& ruleset) {
  return enabled && live && Active(true, ruleset) && (scrim || inMatches);
}

bool PickNow(BestWhen when, int roundsPlayed, int minRounds, int half, int lastPickHalf) {
  if (when == BestWhen::kHalf) return half >= 2 && half != lastPickHalf;
  return roundsPlayed >= std::max(1, minRounds);
}

uint64_t PickBest(const std::vector<PlayerTotals>& players, BestStat stat, uint64_t current) {
  const PlayerTotals* best = nullptr;
  // a better than b?
  auto better = [&](const PlayerTotals& a, const PlayerTotals& b) {
    const double aAdr = Adr(a), bAdr = Adr(b);
    if (stat == BestStat::kAdr) {
      if (aAdr != bAdr) return aAdr > bAdr;
      if (a.kills != b.kills) return a.kills > b.kills;
    } else {
      if (a.kills != b.kills) return a.kills > b.kills;
      if (aAdr != bAdr) return aAdr > bAdr;
    }
    if (a.deaths != b.deaths) return a.deaths < b.deaths;
    if ((a.steamid64 == current) != (b.steamid64 == current)) return a.steamid64 == current;
    return a.steamid64 < b.steamid64;
  };
  for (const auto& p : players) {
    if (p.steamid64 == 0 || p.rounds <= 0) continue;
    if (!best || better(p, *best)) best = &p;
  }
  if (!best) return 0;
  const bool scored = stat == BestStat::kAdr ? best->damage > 0 : best->kills > 0;
  return scored ? best->steamid64 : 0;
}

}  // namespace midas
