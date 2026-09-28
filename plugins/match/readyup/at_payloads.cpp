#include "readyup/at_payloads.h"

#include <cmath>
#include <cstdio>

// Pure (no engine or server state): unit-tested by tests/match_flow_test.cpp.

namespace readyup::at {
namespace {

using stats::JsonEscape;

std::string Str(const std::string& s) { return "\"" + JsonEscape(s) + "\""; }

// Appends "key":raw to a JSON object body.
class Obj {
 public:
  Obj& I(const char* k, long long v) { return R(k, std::to_string(v)); }
  Obj& S(const char* k, const std::string& v) { return R(k, Str(v)); }
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

const char* SlotName(int slot) { return slot == 1 ? "team1" : slot == 2 ? "team2" : "none"; }

int SlotForSide(int side, bool team1IsCt) {
  if (side == 3) return team1IsCt ? 1 : 2;
  if (side == 2) return team1IsCt ? 2 : 1;
  return 0;
}

std::string SizeMb(double mb) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", mb);
  return buf;
}

Obj DemoBase(const char* event, const demo::DemoEvent& e) {
  Obj o;
  o.S("event", event).I("matchid", e.matchid).I("map_number", e.mapNumber).S("filename", e.fileName);
  return o;
}

}  // namespace

int KastPercent(const stats::PlayerStats& s) {
  if (s.rounds_played <= 0) return 0;
  const double pct = 100.0 * static_cast<double>(s.kast_rounds) / static_cast<double>(s.rounds_played);
  const int v = static_cast<int>(std::lround(pct));
  return v < 0 ? 0 : v > 100 ? 100 : v;
}

std::string PlayerStatsJson(const stats::PlayerStats& s) {
  return Obj()
      .I("kills", s.kills)
      .I("deaths", s.deaths)
      .I("assists", s.assists)
      .I("flash_assists", s.flash_assists)
      .I("team_kills", s.team_kills)
      .I("suicides", s.suicides)
      .I("damage", s.damage)
      .I("utility_damage", s.utility_damage)
      .I("enemies_flashed", s.enemies_flashed)
      .I("friendlies_flashed", s.friendlies_flashed)
      .I("knife_kills", s.knife_kills)
      .I("headshot_kills", s.headshot_kills)
      .I("rounds_played", s.rounds_played)
      .I("bomb_defuses", s.bomb_defuses)
      .I("bomb_plants", s.bomb_plants)
      .I("1k", s.multi_kills[0])
      .I("2k", s.multi_kills[1])
      .I("3k", s.multi_kills[2])
      .I("4k", s.multi_kills[3])
      .I("5k", s.multi_kills[4])
      .I("1v1", s.clutches_won[0])
      .I("1v2", s.clutches_won[1])
      .I("1v3", s.clutches_won[2])
      .I("1v4", s.clutches_won[3])
      .I("1v5", s.clutches_won[4])
      .I("first_kills_t", s.entry_kills_t)
      .I("first_kills_ct", s.entry_kills_ct)
      .I("first_deaths_t", s.entry_deaths_t)
      .I("first_deaths_ct", s.entry_deaths_ct)
      .I("trade_kills", s.trade_kills)
      .I("kast", KastPercent(s))
      .I("score", s.score)
      .I("mvp", s.mvp)
      .Done();
}

std::string WinnerJson(int side, int teamSlot) {
  const int sd = (side == 2 || side == 3) ? side : 0;
  return Obj().S("side", std::to_string(sd)).S("team", SlotName(teamSlot)).Done();
}

std::string StatsTeamJson(const TeamInfo& team, int slot, int score, const stats::TeamLine& line,
                          const std::vector<stats::PlayerLine>& players) {
  std::string arr = "[";
  bool first = true;
  for (const auto& p : players) {
    if (p.team != slot || p.bot || p.id == 0) continue;
    arr += first ? "" : ",";
    first = false;
    const std::string id = std::to_string(p.id);
    arr += Obj().S("steamid", id).S("name", p.name.empty() ? id : p.name).R("stats", PlayerStatsJson(p.stats)).Done();
  }
  arr += "]";
  return Obj()
      .S("id", team.id)
      .S("name", team.name)
      .I("series_score", team.series_score)
      .I("score", score)
      .I("score_ct", line.score_ct)
      .I("score_t", line.score_t)
      .R("players", arr)
      .Done();
}

std::string RoundEndJson(const RoundEnd& r) {
  const stats::MapStats& m = r.stats;
  // The sides the round was played on (the model may already hold the next half's sides).
  const bool team1WasCt = m.rounds.empty() ? m.team1_is_ct : m.rounds.back().team1_was_ct;
  const int winnerSlot = SlotForSide(r.winner_side, team1WasCt);
  return Obj()
      .S("event", "round_end")
      .I("matchid", r.matchid)
      .I("map_number", r.map_number)
      .I("round_number", r.round_number)
      .I("round_time", r.round_time_ms)
      .I("reason", r.reason)
      .R("winner", WinnerJson(r.winner_side, winnerSlot))
      .R("team1", StatsTeamJson(r.team1, 1, m.team1.score, m.team1, m.players))
      .R("team2", StatsTeamJson(r.team2, 2, m.team2.score, m.team2, m.players))
      .I("team1_score", m.team1.score)
      .I("team2_score", m.team2.score)
      .Done();
}

std::string MapResultJson(const MapResult& mr) {
  const stats::MapStats& m = mr.stats;
  const int slot = mr.winner == "team1" ? 1 : mr.winner == "team2" ? 2 : 0;
  // AT: the winner's side at the end of the map.
  const int side = slot == 0 ? 0 : ((slot == 1) == m.team1_is_ct ? 3 : 2);
  return Obj()
      .S("event", "map_result")
      .I("matchid", mr.matchid)
      .I("map_number", mr.map_number)
      .S("map_name", mr.map_name)
      .R("winner", WinnerJson(side, slot))
      .R("team1", StatsTeamJson(mr.team1, 1, mr.team1_score, m.team1, m.players))
      .R("team2", StatsTeamJson(mr.team2, 2, mr.team2_score, m.team2, m.players))
      .I("team1_score", mr.team1_score)
      .I("team2_score", mr.team2_score)
      .Done();
}

std::vector<std::string> DemoEventJsons(const demo::DemoEvent& e) {
  using T = demo::DemoEventType;
  std::vector<std::string> out;
  switch (e.type) {
    case T::RecordingStarted:
      out.push_back(DemoBase("demo_recording_start", e).Done());
      break;
    case T::RecordingStopped:
      out.push_back(DemoBase("demo_recording_stop", e).Done());
      break;
    case T::UploadStarted:
      out.push_back(DemoBase("demo_upload_start", e).R("size_mb", SizeMb(e.sizeMb)).Done());
      break;
    case T::UploadSucceeded:
      out.push_back(DemoBase("demo_upload_success", e)
                        .R("size_mb", SizeMb(e.sizeMb))
                        .S("status", std::to_string(e.httpStatus))
                        .Done());
      out.push_back(DemoBase("demo_upload_ended", e).R("success", "true").Done());
      break;
    case T::UploadFailed: {
      const bool notFound = e.error == "file_not_found";
      const std::string status =
          notFound ? "file_not_found" : e.httpStatus > 0 ? std::to_string(e.httpStatus) : "no_response";
      std::string reason = e.error;
      if (reason.empty()) reason = e.httpStatus > 0 ? "HTTP " + std::to_string(e.httpStatus) : "no_response";
      out.push_back(DemoBase("demo_upload_fail", e)
                        .R("size_mb", notFound || e.sizeMb <= 0.0 ? "null" : SizeMb(e.sizeMb))
                        .S("status", status)
                        .S("reason", reason)
                        .Done());
      out.push_back(DemoBase("demo_upload_ended", e).R("success", "false").Done());
      break;
    }
  }
  return out;
}

}  // namespace readyup::at
