#include "midas_rules.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
  static const char* const kNoPaint[] = {"weapon_c4",        "weapon_hegrenade", "weapon_flashbang",
                                         "weapon_smokegrenade", "weapon_molotov", "weapon_incgrenade",
                                         "weapon_decoy",     "weapon_tagrenade", "weapon_healthshot",
                                         "weapon_breachcharge", "weapon_shield", "weapon_snowball"};
  for (const char* n : kNoPaint) {
    if (c == n) return false;
  }
  return true;
}

bool StatTrakable(const std::string& classname) { return Paintable(classname) && Lower(classname) != "weapon_taser"; }

bool CountsAsKill(int attackerSlot, int victimSlot, int attackerTeam, int victimTeam) {
  if (attackerSlot < 0 || attackerSlot >= 64 || attackerSlot == victimSlot) return false;
  const bool known = (attackerTeam == 2 || attackerTeam == 3) && (victimTeam == 2 || victimTeam == 3);
  return !known || attackerTeam != victimTeam;
}

int StatTrakKills(bool statsLive, bool statsHavePlayer, int statsKills, int ownKills) {
  const int k = statsLive && statsHavePlayer ? statsKills : ownKills;
  return k < 0 ? 0 : k;
}

float KillEaterBits(int kills) {
  const uint32_t u = static_cast<uint32_t>(kills < 0 ? 0 : kills);
  float f = 0.0f;
  std::memcpy(&f, &u, sizeof(f));
  return f;
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

std::set<uint64_t> PaintOverrideSet(bool active, Finish finish, const std::set<uint64_t>& midas, uint64_t best) {
  std::set<uint64_t> out;
  if (!active || finish != Finish::kAuto) return out;
  out = midas;
  out.erase(0);
  if (best != 0) out.insert(best);
  return out;
}

unsigned MidasReasons(bool active, const std::set<uint64_t>& config, const std::set<uint64_t>& given, uint64_t best,
                      uint64_t sid) {
  if (!active || sid == 0) return 0;
  unsigned why = 0;
  if (config.count(sid)) why |= kWhyConfig;
  if (given.count(sid)) why |= kWhyGiven;
  if (best == sid) why |= kWhyBest;
  return why;
}

std::set<uint64_t> EffectiveMidas(bool active, const std::set<uint64_t>& config, const std::set<uint64_t>& given,
                                  uint64_t best) {
  std::set<uint64_t> out;
  if (!active) return out;
  out = config;
  out.insert(given.begin(), given.end());
  if (best != 0) out.insert(best);
  out.erase(0);
  return out;
}

std::string DescribeReasons(unsigned why) {
  std::string out;
  auto add = [&](unsigned bit, const char* text) {
    if (!(why & bit)) return;
    if (!out.empty()) out += ", ";
    out += text;
  };
  add(kWhyConfig, "midas_steamids");
  add(kWhyGiven, "given by an admin");
  add(kWhyBest, "best player");
  return out;
}

bool ToggleGiven(std::set<uint64_t>* given, uint64_t sid) {
  if (given->erase(sid)) return false;
  given->insert(sid);
  return true;
}

std::set<uint64_t> ParseGivenFile(const std::string& text) {
  std::set<uint64_t> out;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    std::string line = Trim(text.substr(pos, nl - pos));
    pos = nl + 1;
    if (line.empty() || line[0] == '#') continue;
    int bad = 0;
    const auto ids = ParseSteamIds(line, &bad);
    out.insert(ids.begin(), ids.end());
  }
  return out;
}

std::string FormatGivenFile(const std::set<uint64_t>& given) {
  std::string out = "# Midas given by admins (.ru midas give <player>): one SteamID64 per line\n";
  for (uint64_t sid : given) out += std::to_string(sid) + "\n";
  return out;
}

namespace {

size_t EditDistance(const std::string& a, const std::string& b) {
  std::vector<size_t> row(b.size() + 1);
  for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
  for (size_t i = 1; i <= a.size(); ++i) {
    size_t diag = row[0];
    row[0] = i;
    for (size_t j = 1; j <= b.size(); ++j) {
      const size_t up = row[j];
      row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] == b[j - 1] ? 0 : 1)});
      diag = up;
    }
  }
  return row[b.size()];
}

// A word of `name` starts at `pos` (the start, or after a non-alphanumeric character).
bool WordStart(const std::string& name, size_t pos) {
  return pos == 0 || !std::isalnum(static_cast<unsigned char>(name[pos - 1]));
}

}  // namespace

PlayerMatch ResolvePlayerName(const std::string& rawQuery, const std::vector<std::string>& rawNames) {
  PlayerMatch out;
  const std::string q = Lower(Trim(rawQuery));
  if (q.empty() || rawNames.empty()) return out;
  std::vector<std::string> names;
  for (const auto& n : rawNames) names.push_back(Lower(Trim(n)));
  auto pick = [&](const std::vector<int>& idx) {
    if (idx.size() == 1) out.index = idx[0];
    else
      for (size_t k = 0; k < idx.size() && k < 5; ++k) out.ambiguous.push_back(idx[k]);
    return out;
  };
  // Several candidates: the shortest name wins if no other is that short.
  auto shortest = [&](std::vector<int> idx) {
    if (idx.size() > 1) {
      std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return names[a].size() < names[b].size(); });
      if (names[idx[0]].size() < names[idx[1]].size()) idx.resize(1);
    }
    return idx;
  };
  const int n = static_cast<int>(names.size());
  std::vector<int> idx;
  // 1. exact (as typed), 2. exact ignoring case
  for (int i = 0; i < n; ++i) {
    if (Trim(rawNames[i]) == Trim(rawQuery)) idx.push_back(i);
  }
  if (!idx.empty()) return pick(idx);
  for (int i = 0; i < n; ++i) {
    if (names[i] == q) idx.push_back(i);
  }
  if (!idx.empty()) return pick(idx);
  // 3. prefix
  for (int i = 0; i < n; ++i) {
    if (names[i].compare(0, q.size(), q) == 0) idx.push_back(i);
  }
  if (!idx.empty()) return pick(shortest(idx));
  // 4. substring: a word starting with it first
  if (q.size() >= 2) {
    std::vector<int> words;
    for (int i = 0; i < n; ++i) {
      bool word = false, any = false;
      for (size_t p = names[i].find(q); p != std::string::npos; p = names[i].find(q, p + 1)) {
        any = true;
        word = word || WordStart(names[i], p);
      }
      if (any) idx.push_back(i);
      if (word) words.push_back(i);
    }
    if (!words.empty()) return pick(shortest(words));
    if (!idx.empty()) return pick(shortest(idx));
  }
  // 5. closest by edit distance, if close enough
  size_t best = std::string::npos;
  for (int i = 0; i < n; ++i) {
    const size_t d = EditDistance(q, names[i]);
    if (d < best) {
      best = d;
      idx.assign(1, i);
    } else if (d == best) {
      idx.push_back(i);
    }
  }
  if (best <= std::max<size_t>(1, q.size() / 3)) return pick(idx);
  return out;
}

EquipmentItem EquipmentItemFor(const std::string& entityClass, bool incendiary) {
  const std::string c = Lower(entityClass);
  if (c == "hegrenade_projectile") return {44, "weapon_hegrenade"};
  if (c == "flashbang_projectile") return {43, "weapon_flashbang"};
  if (c == "smokegrenade_projectile") return {45, "weapon_smokegrenade"};
  if (c == "decoy_projectile") return {47, "weapon_decoy"};
  if (c == "molotov_projectile") return incendiary ? EquipmentItem{48, "weapon_incgrenade"} : EquipmentItem{46, "weapon_molotov"};
  if (c == "planted_c4") return {49, "weapon_c4"};
  return {};
}

}  // namespace midas
