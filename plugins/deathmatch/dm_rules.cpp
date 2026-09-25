#include "dm_rules.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace deathmatch {
namespace {

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n\"");
  if (a == std::string::npos) return {};
  const size_t b = s.find_last_not_of(" \t\r\n\"");
  return s.substr(a, b - a + 1);
}

bool ParseInt(const std::string& v, int lo, int hi, int* out) {
  const std::string t = Trim(v);
  if (t.empty()) return false;
  char* end = nullptr;
  const long n = std::strtol(t.c_str(), &end, 10);
  if (!end || *end != '\0' || n < lo || n > hi) return false;
  *out = static_cast<int>(n);
  return true;
}

bool ParseBoolValue(const std::string& v, bool* out) {
  const std::string t = Lower(Trim(v));
  if (t == "1" || t == "true" || t == "yes" || t == "on") return *out = true, true;
  if (t == "0" || t == "false" || t == "no" || t == "off") return *out = false, true;
  return false;
}

constexpr uint64_t kBotIdBase = 0xB0B0000000000000ull;

}  // namespace

const char* ModeName(Mode m) {
  switch (m) {
    case Mode::Ffa: return "ffa";
    case Mode::Tdm: return "tdm";
    default: return "off";
  }
}

const char* ModeLabel(Mode m) {
  switch (m) {
    case Mode::Ffa: return "Deathmatch";
    case Mode::Tdm: return "Team deathmatch";
    default: return "Off";
  }
}

bool ParseMode(const std::string& s, Mode* out) {
  const std::string l = Lower(s);
  if (l == "ffa" || l == "dm") return *out = Mode::Ffa, true;
  if (l == "tdm") return *out = Mode::Tdm, true;
  return false;
}

// ---- settings --------------------------------------------------------------------------------------

Settings LoadSettings(const ConfigLookup& get, std::vector<std::string>* warnings) {
  Settings s;
  auto warn = [&](const char* key, const std::string& v) {
    if (warnings) warnings->push_back(std::string(key) + "=" + v.substr(0, 40) + " is not valid; using the default");
  };
  auto num = [&](const char* key, int lo, int hi, int* out) {
    std::string v;
    if (get(key, &v) && !ParseInt(v, lo, hi, out)) warn(key, v);
  };
  auto flag = [&](const char* key, bool* out) {
    std::string v;
    if (get(key, &v) && !ParseBoolValue(v, out)) warn(key, v);
  };
  num("kill_limit_ffa", 0, 10000, &s.killLimitFfa);
  num("kill_limit_tdm", 0, 100000, &s.killLimitTdm);
  num("time_limit_minutes", 0, 600, &s.timeLimitMinutes);
  num("spawn_protection_seconds", 0, 30, &s.spawnProtectionSeconds);
  flag("headshot_only", &s.headshotOnly);
  flag("hud", &s.hud);
  num("hud_interval_ms", 0, 10000, &s.hudIntervalMs);
  num("end_delay_seconds", 3, 120, &s.endDelaySeconds);
  num("weapon_round_every_minutes", 0, 600, &s.weaponRoundEveryMinutes);
  num("weapon_round_seconds", 5, 36000, &s.weaponRoundSeconds);
  flag("weapon_round_restrict_buy", &s.weaponRoundRestrictBuy);
  num("restore_game_type", 0, 6, &s.restoreGameType);
  num("restore_game_mode", 0, 6, &s.restoreGameMode);
  std::string v;
  if (get("restart", &v)) {
    const std::string r = Lower(Trim(v));
    if (r == "reload") s.restartReloadsMap = true;
    else if (r == "restartgame" || r.empty()) s.restartReloadsMap = false;
    else warn("restart", v);
  }
  if (get("weapon_rounds", &v)) {
    std::string item;
    std::stringstream ss(v);
    while (std::getline(ss, item, ',')) {
      std::string w = Lower(Trim(item));
      if (w.empty()) continue;
      if (w.rfind("weapon_", 0) != 0) w = "weapon_" + w;
      if (ValidWeapon(w)) s.weaponRounds.push_back(w);
      else warn("weapon_rounds", item);
    }
  }
  return s;
}

int KillLimit(const Settings& s, Mode m) {
  return m == Mode::Tdm ? s.killLimitTdm : m == Mode::Ffa ? s.killLimitFfa : 0;
}

// ---- cvars -----------------------------------------------------------------------------------------

std::vector<std::string> GameModeCommands() { return {"game_type 1", "game_mode 2"}; }

std::vector<std::string> ModeCommands(Mode m, const Settings& s) {
  const bool tdm = m == Mode::Tdm;
  std::vector<std::string> c = {
      // FFA: everyone is an enemy. TDM: teams, and CS2's team deathmatch scoreboard.
      std::string("mp_teammates_are_enemies ") + (tdm ? "0" : "1"),
      std::string("mp_dm_teammode ") + (tdm ? "1" : "0"),
      "mp_respawn_immunitytime " + std::to_string(s.spawnProtectionSeconds),
      std::string("mp_damage_headshot_only ") + (s.headshotOnly ? "1" : "0"),
      // The kill / time limits are Ready Up's: CS2's own round and map timers never end the game.
      "mp_ignore_round_win_conditions 1",
      "mp_roundtime 60",
      "mp_timelimit 0",
      "mp_maxrounds 0",
      "mp_warmup_end",
  };
  for (auto& w : WeaponRoundCommands("", false)) c.push_back(std::move(w));
  return c;
}

std::vector<std::string> LeaveCommands(const Settings& s, bool withGameMode) {
  std::vector<std::string> c = {
      "mp_teammates_are_enemies 0", "mp_dm_teammode 0", "mp_damage_headshot_only 0",
      "mp_ignore_round_win_conditions 0",
  };
  for (auto& w : WeaponRoundCommands("", false)) c.push_back(std::move(w));
  if (withGameMode) {
    c.push_back("game_type " + std::to_string(s.restoreGameType));
    c.push_back("game_mode " + std::to_string(s.restoreGameMode));
  }
  return c;
}

bool ValidWeapon(const std::string& w) {
  if (w.size() <= 7 || w.size() > 40 || w.rfind("weapon_", 0) != 0) return false;
  for (unsigned char c : w) {
    if (!std::islower(c) && !std::isdigit(c) && c != '_') return false;
  }
  return true;
}

bool IsPistol(const std::string& w) {
  static const char* const k[] = {"weapon_deagle",   "weapon_revolver", "weapon_glock", "weapon_hkp2000",
                                  "weapon_usp_silencer", "weapon_p250", "weapon_fiveseven", "weapon_tec9",
                                  "weapon_cz75a",    "weapon_elite"};
  return std::find(std::begin(k), std::end(k), w) != std::end(k);
}

std::vector<std::string> WeaponRoundCommands(const std::string& weapon, bool restrictBuy) {
  if (weapon.empty() || !ValidWeapon(weapon)) {
    return {"mp_ct_default_primary \"\"", "mp_t_default_primary \"\"", "mp_ct_default_secondary weapon_hkp2000",
            "mp_t_default_secondary weapon_glock", "mp_buy_allow_guns 255"};
  }
  std::vector<std::string> c;
  if (IsPistol(weapon)) {
    c = {"mp_ct_default_primary \"\"", "mp_t_default_primary \"\"", "mp_ct_default_secondary " + weapon,
         "mp_t_default_secondary " + weapon};
  } else {
    c = {"mp_ct_default_primary " + weapon, "mp_t_default_primary " + weapon, "mp_ct_default_secondary weapon_hkp2000",
         "mp_t_default_secondary weapon_glock"};
  }
  c.push_back(restrictBuy ? "mp_buy_allow_guns 0" : "mp_buy_allow_guns 255");
  return c;
}

std::string WeaponLabel(const std::string& weapon) {
  static const std::pair<const char*, const char*> k[] = {
      {"weapon_deagle", "Desert Eagle"}, {"weapon_awp", "AWP"},       {"weapon_ssg08", "SSG 08"},
      {"weapon_ak47", "AK-47"},          {"weapon_m4a1", "M4A4"},     {"weapon_m4a1_silencer", "M4A1-S"},
      {"weapon_usp_silencer", "USP-S"},  {"weapon_glock", "Glock-18"}, {"weapon_p250", "P250"},
      {"weapon_revolver", "R8 Revolver"}, {"weapon_nova", "Nova"},    {"weapon_mp9", "MP9"},
      {"weapon_scar20", "SCAR-20"},      {"weapon_g3sg1", "G3SG1"},   {"weapon_negev", "Negev"},
  };
  for (const auto& p : k) {
    if (weapon == p.first) return p.second;
  }
  return weapon.rfind("weapon_", 0) == 0 ? weapon.substr(7) : weapon;
}

WeaponRound WeaponRoundAt(const Settings& s, double elapsed) {
  WeaponRound r;
  if (s.weaponRoundEveryMinutes <= 0 || s.weaponRounds.empty() || elapsed < 0) return r;
  const double period = s.weaponRoundEveryMinutes * 60.0;
  const double length = std::min<double>(s.weaponRoundSeconds, period);
  const long k = static_cast<long>(std::floor(elapsed / period));
  if (k < 1) return r;
  const double into = elapsed - static_cast<double>(k) * period;
  if (into >= length) return r;
  r.index = static_cast<int>((k - 1) % static_cast<long>(s.weaponRounds.size()));
  r.weapon = s.weaponRounds[static_cast<size_t>(r.index)];
  r.secondsLeft = static_cast<int>(std::ceil(length - into));
  return r;
}

// ---- scoring ---------------------------------------------------------------------------------------

uint64_t BotId(int userid) { return kBotIdBase | static_cast<uint32_t>(userid < 0 ? 0 : userid); }

void Scoreboard::Reset() {
  players_.clear();
  std::fill(std::begin(teamKills_), std::end(teamKills_), 0);
}

PlayerScore* Scoreboard::Get(uint64_t id) {
  for (auto& p : players_) {
    if (p.id == id) return &p;
  }
  players_.push_back(PlayerScore{});
  players_.back().id = id;
  return &players_.back();
}

const PlayerScore* Scoreboard::Find(uint64_t id) const {
  for (const auto& p : players_) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

void Scoreboard::SeePlayer(uint64_t id, const std::string& name, int team) {
  if (id == 0) return;
  PlayerScore* p = Get(id);
  if (!name.empty()) p->name = name;
  if (team == 2 || team == 3) p->team = team;
}

bool Scoreboard::OnKill(Mode m, uint64_t attacker, int attackerTeam, uint64_t victim, int victimTeam, bool headshot,
                        double now) {
  if (m == Mode::Off || victim == 0) return false;
  PlayerScore* v = Get(victim);
  ++v->deaths;
  if (victimTeam == 2 || victimTeam == 3) v->team = victimTeam;
  if (attacker == 0 || attacker == victim) return false;  // world / suicide
  if (m == Mode::Tdm && (attackerTeam != 2 && attackerTeam != 3)) return false;
  if (m == Mode::Tdm && attackerTeam == victimTeam) return false;  // team kill
  PlayerScore* a = Get(attacker);  // may move `v`
  if (attackerTeam == 2 || attackerTeam == 3) a->team = attackerTeam;
  ++a->kills;
  if (headshot) ++a->headshots;
  a->reachedAt = now;
  if (m == Mode::Tdm) ++teamKills_[attackerTeam];
  return true;
}

std::vector<PlayerScore> Scoreboard::Ranked() const {
  std::vector<PlayerScore> r = players_;
  std::stable_sort(r.begin(), r.end(), [](const PlayerScore& x, const PlayerScore& y) {
    if (x.kills != y.kills) return x.kills > y.kills;
    if (x.deaths != y.deaths) return x.deaths < y.deaths;
    if (x.kills > 0 && x.reachedAt != y.reachedAt) return x.reachedAt < y.reachedAt;
    return x.name < y.name;
  });
  return r;
}

int Scoreboard::RankOf(uint64_t id) const {
  const auto r = Ranked();
  for (size_t i = 0; i < r.size(); ++i) {
    if (r[i].id == id) return static_cast<int>(i) + 1;
  }
  return 0;
}

int Scoreboard::TeamKills(int team) const { return team >= 0 && team < 4 ? teamKills_[team] : 0; }

std::string Scoreboard::Serialize() const {
  std::string out = "T\t" + std::to_string(teamKills_[2]) + "\t" + std::to_string(teamKills_[3]) + "\n";
  for (const auto& p : players_) {
    std::string name;
    for (char c : p.name) name += (c == '\t' || c == '\n') ? ' ' : c;
    char buf[160];
    std::snprintf(buf, sizeof buf, "P\t%llu\t%d\t%d\t%d\t%d\t%.3f\t", static_cast<unsigned long long>(p.id), p.team,
                  p.kills, p.deaths, p.headshots, p.reachedAt);
    out += buf + name + "\n";
  }
  return out;
}

bool Scoreboard::Deserialize(const std::string& text) {
  Scoreboard b;
  std::stringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    std::vector<std::string> f;
    std::string cur;
    for (char c : line) {
      if (c == '\t') {
        f.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    f.push_back(cur);
    if (f[0] == "T" && f.size() == 3) {
      b.teamKills_[2] = std::atoi(f[1].c_str());
      b.teamKills_[3] = std::atoi(f[2].c_str());
    } else if (f[0] == "P" && f.size() == 8) {
      PlayerScore p;
      p.id = std::strtoull(f[1].c_str(), nullptr, 10);
      p.team = std::atoi(f[2].c_str());
      p.kills = std::atoi(f[3].c_str());
      p.deaths = std::atoi(f[4].c_str());
      p.headshots = std::atoi(f[5].c_str());
      p.reachedAt = std::atof(f[6].c_str());
      p.name = f[7];
      if (p.id == 0) return false;
      b.players_.push_back(std::move(p));
    } else {
      return false;
    }
  }
  *this = std::move(b);
  return true;
}

Outcome CheckEnd(Mode m, const Scoreboard& b, const Settings& s, double elapsed) {
  Outcome o;
  if (m == Mode::Off) return o;
  const int limit = KillLimit(s, m);
  const bool timeUp = s.timeLimitMinutes > 0 && elapsed >= s.timeLimitMinutes * 60.0;
  if (m == Mode::Tdm) {
    const int ct = b.TeamKills(3), t = b.TeamKills(2);
    const bool limitHit = limit > 0 && (ct >= limit || t >= limit);
    if (!limitHit && !timeUp) return o;
    o.over = true;
    o.reason = limitHit ? "kill_limit" : "time_limit";
    o.winnerKills = std::max(ct, t);
    o.runnerUpKills = std::min(ct, t);
    if (ct == t) {
      o.draw = true;
      return o;
    }
    o.winnerTeam = ct > t ? 3 : 2;
    o.winnerName = ct > t ? "Counter-Terrorists" : "Terrorists";
    return o;
  }
  const auto r = b.Ranked();
  const bool limitHit = limit > 0 && !r.empty() && r[0].kills >= limit;
  if (!limitHit && !timeUp) return o;
  o.over = true;
  o.reason = limitHit ? "kill_limit" : "time_limit";
  o.winnerKills = r.empty() ? 0 : r[0].kills;
  o.runnerUpKills = r.size() > 1 ? r[1].kills : 0;
  // A kill limit is reached by one player at a time; at the time limit equal kills are a draw.
  if (r.empty() || r[0].kills == 0 || (!limitHit && r.size() > 1 && r[1].kills == r[0].kills)) {
    o.draw = true;
    return o;
  }
  o.winnerId = r[0].id;
  o.winnerName = r[0].name.empty() ? std::string("?") : r[0].name;
  o.winnerTeam = r[0].team;
  return o;
}

// ---- output ----------------------------------------------------------------------------------------

std::string HtmlEscape(const std::string& s, size_t maxChars) {
  std::string out;
  size_t chars = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x20 || c == 0x7F) continue;  // control bytes (chat colors) are dropped
    if ((c & 0xC0) != 0x80) {  // not a UTF-8 continuation byte: a new character
      if (chars == maxChars) {
        out += "...";
        break;
      }
      ++chars;
    }
    switch (c) {
      case '&': out += "&#38;"; break;
      case '<': out += "&#60;"; break;
      case '>': out += "&#62;"; break;
      case '"': out += "&#34;"; break;
      case '\'': out += "&#39;"; break;
      default: out += static_cast<char>(c); break;
    }
  }
  return out;
}

std::string FormatClock(int seconds) {
  if (seconds < 0) seconds = 0;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%d:%02d", seconds / 60, seconds % 60);
  return buf;
}

namespace {

const char* TeamColor(int team) { return team == 3 ? "#5EA8FF" : team == 2 ? "#FF9D3B" : "#E4E4E7"; }

std::string Row(int rank, const PlayerScore& p, Mode m, bool self) {
  const char* color = self ? "#A3E635" : (m == Mode::Tdm ? TeamColor(p.team) : "#E4E4E7");
  return "<font class='fontSize-sm' color='" + std::string(color) + "'>" + std::to_string(rank) + ". " +
         HtmlEscape(p.name.empty() ? std::string("?") : p.name, 16) + " &#183; " + std::to_string(p.kills) +
         "</font><br>";
}

}  // namespace

std::string LeaderboardHtml(Mode m, const Scoreboard& b, uint64_t viewer, int killLimit, int secondsLeft,
                            const std::string& weaponRound) {
  std::string h = "<font class='fontSize-sm' color='#FACC15'>";
  h += m == Mode::Tdm ? "TDM" : "FFA";
  h += "</font><font class='fontSize-sm' color='#A1A1AA'>";
  if (killLimit > 0) h += " &#183; first to " + std::to_string(killLimit);
  if (secondsLeft >= 0) h += " &#183; " + FormatClock(secondsLeft);
  h += "</font><br>";
  if (!weaponRound.empty()) {
    h += "<font class='fontSize-sm' color='#F472B6'>" + HtmlEscape(WeaponLabel(weaponRound), 20) + " round</font><br>";
  }
  if (m == Mode::Tdm) {
    h += "<font class='fontSize-sm' color='#5EA8FF'>CT " + std::to_string(b.TeamKills(3)) +
         "</font><font class='fontSize-sm' color='#A1A1AA'> : </font><font class='fontSize-sm' color='#FF9D3B'>" +
         std::to_string(b.TeamKills(2)) + " T</font><br>";
  }
  const auto r = b.Ranked();
  int selfRank = 0;
  for (size_t i = 0; i < r.size(); ++i) {
    if (r[i].id == viewer) selfRank = static_cast<int>(i) + 1;
    if (i < 5) h += Row(static_cast<int>(i) + 1, r[i], m, r[i].id == viewer);
  }
  if (r.empty()) h += "<font class='fontSize-sm' color='#A1A1AA'>no kills yet</font><br>";
  if (selfRank > 5) {
    const auto& me = r[static_cast<size_t>(selfRank - 1)];
    h += "<font class='fontSize-sm' color='#A3E635'>you: #" + std::to_string(selfRank) + " &#183; " +
         std::to_string(me.kills) + "</font>";
  }
  return h;
}

std::string WinnerHtml(Mode m, const Outcome& o, int nextGameInSeconds) {
  std::string h = "<font class='fontSize-l' color='#FACC15'>";
  if (o.draw) {
    h += "DRAW";
  } else if (m == Mode::Tdm) {
    h += "<font color='" + std::string(TeamColor(o.winnerTeam)) + "'>" + HtmlEscape(o.winnerName, 24) + "</font> WIN";
  } else {
    h += HtmlEscape(o.winnerName, 24) + " WINS";
  }
  h += "</font><br><font class='fontSize-m' color='#E4E4E7'>";
  h += std::to_string(o.winnerKills) + (m == Mode::Tdm ? " : " + std::to_string(o.runnerUpKills) : std::string(" kills"));
  h += o.reason == "kill_limit" ? " &#183; kill limit" : " &#183; time limit";
  h += "</font><br><font class='fontSize-sm' color='#A1A1AA'>next game in " + std::to_string(std::max(0, nextGameInSeconds)) +
       " s</font>";
  return h;
}

std::string WinnerChat(Mode m, const Outcome& o) {
  const std::string why = o.reason == "kill_limit" ? "kill limit" : "time limit";
  if (o.draw) {
    if (m == Mode::Tdm) return "Deathmatch: draw, " + std::to_string(o.winnerKills) + " : " + std::to_string(o.runnerUpKills) + " (" + why + ").";
    return "Deathmatch: draw at " + std::to_string(o.winnerKills) + " kills (" + why + ").";
  }
  if (m == Mode::Tdm) {
    return "Team deathmatch: " + o.winnerName + " win " + std::to_string(o.winnerKills) + " : " +
           std::to_string(o.runnerUpKills) + " (" + why + ").";
  }
  return "Deathmatch: " + o.winnerName + " wins with " + std::to_string(o.winnerKills) + " kills (" + why + ").";
}

// ---- maps ------------------------------------------------------------------------------------------

std::string MapArgToEntry(const std::string& arg) {
  const std::string l = Lower(arg);
  if (l.find("steamcommunity.com/") == std::string::npos) return arg;
  size_t p = l.find("?id=");
  if (p == std::string::npos) p = l.find("&id=");
  if (p == std::string::npos) return arg;
  p += 4;
  size_t e = p;
  while (e < l.size() && std::isdigit(static_cast<unsigned char>(l[e]))) ++e;
  return e > p ? arg.substr(p, e - p) : arg;
}

MapChoice ResolveMap(const std::string& arg, const std::string& defaultEntry, const std::string& reloadEntry) {
  if (!arg.empty()) return {MapArgToEntry(arg), "argument"};
  if (!defaultEntry.empty()) return {defaultEntry, "default"};
  return {reloadEntry, "current"};
}

}  // namespace deathmatch
