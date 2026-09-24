#include "readyup/match_stats.h"

#include <algorithm>
#include <cstdio>

namespace readyup::stats {

namespace {

constexpr double kTradeWindowSeconds = 5.0;

bool IsUtilityWeapon(const std::string& w) {
  return w == "hegrenade" || w == "inferno" || w == "molotov" || w == "incgrenade" || w == "weapon_hegrenade" ||
         w == "weapon_molotov" || w == "weapon_incgrenade";
}

bool IsKnifeWeapon(const std::string& w) {
  return w.find("knife") != std::string::npos || w.find("bayonet") != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------- accumulator

void StatsAccumulator::BeginMap(bool team1IsCt) {
  Clear();
  team1IsCt_ = team1IsCt;
  live_ = true;
}

void StatsAccumulator::Clear() {
  live_ = false;
  players_.clear();
  round_ = Round{};
  t1_ = TeamLine{};
  t2_ = TeamLine{};
  rounds_.clear();
}

int StatsAccumulator::SlotForSide(int side) const {
  if (side == 3) return team1IsCt_ ? 1 : 2;
  if (side == 2) return team1IsCt_ ? 2 : 1;
  return 0;
}

int StatsAccumulator::SideOf(uint64_t id) const {
  auto it = round_.side.find(id);
  if (it != round_.side.end()) return it->second;
  auto p = players_.find(id);
  return p == players_.end() ? 0 : p->second.lastSide;
}

PlayerRound& StatsAccumulator::R(uint64_t id) {
  auto& r = round_.pr[id];
  r.id = id;
  return r;
}

void StatsAccumulator::ObservePlayer(uint64_t id, const std::string& name, int side, int teamSlot, bool bot) {
  if (id == 0) return;
  auto& p = P(id);
  if (!name.empty()) p.name = name;
  if (bot) p.bot = true;
  if (side == 2 || side == 3) p.lastSide = side;
  if (teamSlot == 1 || teamSlot == 2) p.team = teamSlot;
  else if (p.team == 0) p.team = SlotForSide(p.lastSide);
  if (live_ && (side == 2 || side == 3)) {
    round_.side[id] = side;
    auto& r = R(id);
    r.side = side;
    r.team = p.team;
  }
}

void StatsAccumulator::OnRoundStart() { round_ = Round{}; }

void StatsAccumulator::OnPlayerDeath(uint64_t victim, uint64_t attacker, uint64_t assister, bool assistedFlash,
                                     bool headshot, const std::string& weapon, double time) {
  if (!live_ || victim == 0) return;
  const int victimSide = SideOf(victim);
  if (victimSide == 2 || victimSide == 3) round_.side.emplace(victim, victimSide);
  P(victim).s.deaths += 1;
  round_.dead.insert(victim);
  R(victim).died = true;

  const bool suicide = attacker == 0 || attacker == victim;
  const int attackerSide = suicide ? 0 : SideOf(attacker);
  const bool teamKill = !suicide && attackerSide != 0 && attackerSide == victimSide;

  if (suicide) {
    P(victim).s.suicides += 1;
  } else if (teamKill) {
    P(attacker).s.team_kills += 1;
  } else {
    auto& a = P(attacker).s;
    auto& ar = R(attacker);
    a.kills += 1;
    ar.kills += 1;
    ar.kast = true;
    if (headshot) {
      a.headshot_kills += 1;
      ar.headshot_kills += 1;
    }
    if (IsKnifeWeapon(weapon)) a.knife_kills += 1;

    if (!round_.entryDone) {
      round_.entryDone = true;
      ar.entry_kill = true;
      R(victim).entry_death = true;
      if (attackerSide == 2) a.entry_kills_t += 1;
      else if (attackerSide == 3) a.entry_kills_ct += 1;
      if (victimSide == 2) P(victim).s.entry_deaths_t += 1;
      else if (victimSide == 3) P(victim).s.entry_deaths_ct += 1;
    }

    // Trade: the victim had just killed one of the attacker's teammates.
    bool traded = false;
    for (const auto& kv : round_.deathBy) {
      const uint64_t mate = kv.first;
      if (kv.second.first != victim || mate == attacker) continue;
      if (time - kv.second.second > kTradeWindowSeconds) continue;
      if (SideOf(mate) != attackerSide) continue;
      auto& mr = R(mate);
      if (!mr.traded) {
        mr.traded = true;
        mr.kast = true;
        P(mate).s.traded_deaths += 1;
      }
      traded = true;
    }
    if (traded) a.trade_kills += 1;
  }
  if (!suicide) round_.deathBy[victim] = {attacker, time};

  if (assister != 0 && assister != attacker && assister != victim) {
    const int assisterSide = SideOf(assister);
    if (assisterSide == 0 || assisterSide != victimSide) {
      auto& as = P(assister).s;
      auto& asr = R(assister);
      if (assistedFlash) {
        as.flash_assists += 1;
        asr.flash_assists += 1;
      } else {
        as.assists += 1;
        asr.assists += 1;
      }
      asr.kast = true;
    }
  }

  // Clutch: the victim's side is down to its last player.
  if ((victimSide == 2 || victimSide == 3) && round_.clutch.find(victimSide) == round_.clutch.end()) {
    uint64_t last = 0;
    int alive = 0;
    int enemies = 0;
    for (const auto& kv : round_.side) {
      if (round_.dead.count(kv.first)) continue;
      if (kv.second == victimSide) {
        ++alive;
        last = kv.first;
      } else if (kv.second == 2 || kv.second == 3) {
        ++enemies;
      }
    }
    if (alive == 1 && enemies >= 1) {
      round_.clutch[victimSide] = {last, std::min(enemies, 5)};
      R(last).clutch_vs = std::min(enemies, 5);
    }
  }
}

void StatsAccumulator::OnPlayerHurt(uint64_t victim, uint64_t attacker, int dmgHealth, int healthAfter,
                                    const std::string& weapon) {
  if (!live_ || victim == 0) return;
  auto hit = round_.health.find(victim);
  const int before = hit == round_.health.end() ? 100 : hit->second;
  const int actual = std::max(0, std::min(dmgHealth, before));
  round_.health[victim] = std::max(0, healthAfter);
  if (attacker == 0 || attacker == victim) return;
  const int as = SideOf(attacker);
  if (as != 0 && as == SideOf(victim)) return;  // team damage does not count
  auto& a = P(attacker).s;
  auto& ar = R(attacker);
  a.damage += actual;
  ar.damage += actual;
  if (IsUtilityWeapon(weapon)) {
    a.utility_damage += actual;
    ar.utility_damage += actual;
  }
}

void StatsAccumulator::OnPlayerBlind(uint64_t victim, uint64_t attacker, double duration) {
  if (!live_ || victim == 0 || attacker == 0 || attacker == victim || duration <= 0.0) return;
  const int as = SideOf(attacker);
  auto& a = P(attacker).s;
  if (as != 0 && as == SideOf(victim)) a.friendlies_flashed += 1;
  else a.enemies_flashed += 1;
}

void StatsAccumulator::OnBombPlanted(uint64_t id) {
  if (live_ && id) P(id).s.bomb_plants += 1;
}

void StatsAccumulator::OnBombDefused(uint64_t id) {
  if (live_ && id) P(id).s.bomb_defuses += 1;
}

void StatsAccumulator::OnRoundMvp(uint64_t id) {
  if (!live_ || !id) return;
  P(id).s.mvp += 1;
  R(id).mvp = true;
}

void StatsAccumulator::SetScore(uint64_t id, int score) {
  if (id && score >= 0) P(id).s.score = score;
}

void StatsAccumulator::OnRoundEnd(int winnerSide, int reason) {
  if (!live_) return;
  const int slot = SlotForSide(winnerSide);
  TeamLine* w = slot == 1 ? &t1_ : slot == 2 ? &t2_ : nullptr;
  if (w) {
    w->score += 1;
    (winnerSide == 3 ? w->score_ct : w->score_t) += 1;
  }

  RoundSummary sum;
  sum.round_number = static_cast<int>(rounds_.size()) + 1;
  sum.winner_side = (winnerSide == 2 || winnerSide == 3) ? winnerSide : 0;
  sum.winner_team = slot;
  sum.reason = reason;
  sum.team1_score = t1_.score;
  sum.team2_score = t2_.score;
  sum.team1_was_ct = team1IsCt_;

  for (const auto& kv : round_.side) {
    auto& p = P(kv.first);
    auto& r = R(kv.first);
    r.side = kv.second;
    if (r.team == 0) r.team = p.team;
    r.survived = round_.dead.count(kv.first) == 0;
    if (r.survived) r.kast = true;
    p.s.rounds_played += 1;
    if (r.kast) p.s.kast_rounds += 1;
  }
  for (auto& kv : round_.pr) {
    auto& r = kv.second;
    auto& s = P(kv.first).s;
    if (r.kills > 0) s.multi_kills[static_cast<size_t>(std::min(r.kills, 5) - 1)] += 1;
    if (r.clutch_vs > 0 && round_.clutch.count(winnerSide) && round_.clutch[winnerSide].first == kv.first) {
      r.clutch_won = true;
      s.clutches_won[static_cast<size_t>(r.clutch_vs - 1)] += 1;
    }
    if (r.team == 0) r.team = P(kv.first).team;
    sum.players.push_back(r);
  }
  std::sort(sum.players.begin(), sum.players.end(),
            [](const PlayerRound& a, const PlayerRound& b) { return a.team != b.team ? a.team < b.team : a.id < b.id; });
  rounds_.push_back(std::move(sum));
  round_ = Round{};
}

MapStats StatsAccumulator::Snapshot() const {
  MapStats m;
  m.live = live_;
  m.team1_is_ct = team1IsCt_;
  m.team1 = t1_;
  m.team2 = t2_;
  for (const auto& kv : players_) {
    PlayerLine l;
    l.id = kv.first;
    l.name = kv.second.name;
    l.team = kv.second.team;
    l.last_side = kv.second.lastSide;
    l.bot = kv.second.bot;
    l.stats = kv.second.s;
    m.players.push_back(std::move(l));
  }
  std::sort(m.players.begin(), m.players.end(),
            [](const PlayerLine& a, const PlayerLine& b) { return a.team != b.team ? a.team < b.team : a.id < b.id; });
  m.rounds = rounds_;
  return m;
}

// ---------------------------------------------------------------------------- JSON

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned>(c));
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

namespace {

class Obj {
 public:
  Obj& I(const char* k, long long v) { return R(k, std::to_string(v)); }
  Obj& B(const char* k, bool v) { return R(k, v ? "true" : "false"); }
  Obj& S(const char* k, const std::string& v) { return R(k, "\"" + JsonEscape(v) + "\""); }
  Obj& R(const char* k, const std::string& raw) {
    body_ += first_ ? "\"" : ",\"";
    first_ = false;
    body_ += k;
    body_ += "\":";
    body_ += raw;
    return *this;
  }
  std::string Done() const { return "{" + body_ + "}"; }

 private:
  std::string body_;
  bool first_ = true;
};

std::string IntArray(const std::array<int, 5>& a) {
  std::string s = "[";
  for (size_t i = 0; i < a.size(); ++i) s += (i ? "," : "") + std::to_string(a[i]);
  return s + "]";
}

std::string TeamJson(const TeamLine& t) {
  return Obj().I("score", t.score).I("score_ct", t.score_ct).I("score_t", t.score_t).Done();
}

}  // namespace

std::string ToJson(const PlayerStats& s) {
  return Obj()
      .I("kills", s.kills)
      .I("deaths", s.deaths)
      .I("assists", s.assists)
      .I("flash_assists", s.flash_assists)
      .I("team_kills", s.team_kills)
      .I("suicides", s.suicides)
      .I("headshot_kills", s.headshot_kills)
      .I("knife_kills", s.knife_kills)
      .I("damage", s.damage)
      .I("utility_damage", s.utility_damage)
      .I("enemies_flashed", s.enemies_flashed)
      .I("friendlies_flashed", s.friendlies_flashed)
      .I("bomb_plants", s.bomb_plants)
      .I("bomb_defuses", s.bomb_defuses)
      .I("entry_kills_t", s.entry_kills_t)
      .I("entry_kills_ct", s.entry_kills_ct)
      .I("entry_deaths_t", s.entry_deaths_t)
      .I("entry_deaths_ct", s.entry_deaths_ct)
      .I("trade_kills", s.trade_kills)
      .I("traded_deaths", s.traded_deaths)
      .I("kast_rounds", s.kast_rounds)
      .I("rounds_played", s.rounds_played)
      .I("mvp", s.mvp)
      .I("score", s.score)
      .R("multi_kills", IntArray(s.multi_kills))
      .R("clutches_won", IntArray(s.clutches_won))
      .Done();
}

std::string ToJson(const RoundSummary& r) {
  std::string players = "[";
  for (size_t i = 0; i < r.players.size(); ++i) {
    const auto& p = r.players[i];
    players += (i ? "," : "") + Obj()
                                    .S("id", std::to_string(p.id))
                                    .I("team", p.team)
                                    .I("side", p.side)
                                    .I("kills", p.kills)
                                    .I("assists", p.assists)
                                    .I("flash_assists", p.flash_assists)
                                    .I("damage", p.damage)
                                    .I("utility_damage", p.utility_damage)
                                    .I("headshot_kills", p.headshot_kills)
                                    .B("died", p.died)
                                    .B("survived", p.survived)
                                    .B("traded", p.traded)
                                    .B("kast", p.kast)
                                    .B("entry_kill", p.entry_kill)
                                    .B("entry_death", p.entry_death)
                                    .B("mvp", p.mvp)
                                    .I("clutch_vs", p.clutch_vs)
                                    .B("clutch_won", p.clutch_won)
                                    .Done();
  }
  players += "]";
  return Obj()
      .I("round_number", r.round_number)
      .I("winner_side", r.winner_side)
      .I("winner_team", r.winner_team)
      .I("reason", r.reason)
      .I("team1_score", r.team1_score)
      .I("team2_score", r.team2_score)
      .B("team1_was_ct", r.team1_was_ct)
      .R("players", players)
      .Done();
}

std::string ToJson(const MapStats& m) {
  std::string players = "[";
  for (size_t i = 0; i < m.players.size(); ++i) {
    const auto& p = m.players[i];
    players += (i ? "," : "") + Obj()
                                    .S("id", std::to_string(p.id))
                                    .S("name", p.name)
                                    .I("team", p.team)
                                    .I("last_side", p.last_side)
                                    .B("bot", p.bot)
                                    .R("stats", ToJson(p.stats))
                                    .Done();
  }
  players += "]";
  std::string rounds = "[";
  for (size_t i = 0; i < m.rounds.size(); ++i) rounds += (i ? "," : "") + ToJson(m.rounds[i]);
  rounds += "]";
  return Obj()
      .B("live", m.live)
      .B("team1_is_ct", m.team1_is_ct)
      .R("team1", TeamJson(m.team1))
      .R("team2", TeamJson(m.team2))
      .R("players", players)
      .R("rounds", rounds)
      .Done();
}

StatsAccumulator& Current() {
  static StatsAccumulator s;
  return s;
}

std::recursive_mutex& Mutex() {
  static std::recursive_mutex m;
  return m;
}

}  // namespace readyup::stats
