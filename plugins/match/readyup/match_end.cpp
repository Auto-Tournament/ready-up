#include "readyup/match_end.h"

#include "readyup/chat.h"
#include "readyup/command_buffer_hook.h"
#include "readyup/demo_recorder.h"
#include "readyup/game_timers.h"
#include "readyup/logging.h"
#include "readyup/modes.h"
#include "readyup/slot_registry.h"
#include "readyup/webhook.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>

namespace readyup {
namespace {

constexpr double kResetAfterKickSeconds = 2.0;

std::mutex g_mu;
int g_kickNoDemo = 5;
int g_kickDemoNoUpload = 10;
int g_kickDemoUpload = 60;
std::vector<MatchFlowListener> g_listeners;
std::atomic<unsigned> g_generation{0};
std::atomic<int> g_pending{0};

void Emit(const MatchFlowEvent& e) {
  std::vector<MatchFlowListener> ls;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    ls = g_listeners;
  }
  // One line per event (the full stats are in `ru_match_stats` / the listener).
  Print("match-flow: %s matchid=%llu map=%d %s %d-%d series %d-%d%s\n", MatchFlowEventTypeName(e.type), e.matchid,
        e.mapNumber, e.winner.c_str(), e.team1Score, e.team2Score, e.team1SeriesScore, e.team2SeriesScore,
        e.type == MatchFlowEventType::SeriesEnd ? (" reset_in=" + std::to_string(e.secondsUntilReset) + "s").c_str()
                                                : "");
  for (auto& fn : ls) fn(e);
}

void KickAllHumans(const std::string& reason) {
  std::string r = reason;
  std::replace(r.begin(), r.end(), '"', '\'');
  int kicked = 0;
  for (const auto& h : ListHumans()) {
    if (h.userid < 0 || h.steamid64 == 0) continue;
    const std::string cmd = "kickid " + std::to_string(h.userid) + " \"" + r + "\"";
    if (EnqueueServerCommand(cmd.c_str())) ++kicked;
  }
  Print("series-end: kicked %d player(s)\n", kicked);
}

std::vector<std::string> SplitArgs(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool inQ = false;
  bool any = false;
  for (char c : line) {
    if (c == '"') {
      inQ = !inQ;
      any = true;
      continue;
    }
    if (!inQ && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
      if (any || !cur.empty()) out.push_back(cur);
      cur.clear();
      any = false;
      continue;
    }
    cur.push_back(c);
  }
  if (any || !cur.empty()) out.push_back(cur);
  return out;
}

}  // namespace

void AddMatchFlowListener(MatchFlowListener fn) {
  if (!fn) return;
  std::lock_guard<std::mutex> lk(g_mu);
  g_listeners.push_back(std::move(fn));
}

void SetSeriesEndKickDelays(int noDemo, int demoNoUpload, int demoUpload) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (noDemo >= 0) g_kickNoDemo = noDemo;
  if (demoNoUpload >= 0) g_kickDemoNoUpload = demoNoUpload;
  if (demoUpload >= 0) g_kickDemoUpload = demoUpload;
}

void GetSeriesEndKickDelays(int* noDemo, int* demoNoUpload, int* demoUpload) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (noDemo) *noDemo = g_kickNoDemo;
  if (demoNoUpload) *demoNoUpload = g_kickDemoNoUpload;
  if (demoUpload) *demoUpload = g_kickDemoUpload;
}

MapEndPlan MatchEndOnMapComplete(const MapEndInput& in) {
  int kNo = 0, kNoUp = 0, kUp = 0;
  GetSeriesEndKickDelays(&kNo, &kNoUp, &kUp);
  const bool recording = demo::IsRecording();
  const bool hasUpload = !demo::Get().uploadUrl.empty();
  const int tvDelay = demo::TvDelaySeconds();
  const MapEndPlan plan = ComputeMapEndPlan(recording, hasUpload, tvDelay, kNo, kNoUp, kUp);
  const unsigned gen = g_generation.load();

  MatchFlowEvent base;
  if (auto ctx = WebhookGetMatchContext()) {
    base.matchid = ctx->matchid;
    base.slug = ctx->slug;
    base.scrim = ctx->slug == "scrim";
    base.team1Name = ctx->team1_name;
    base.team2Name = ctx->team2_name;
  }
  base.team1SeriesScore = in.team1SeriesScore;
  base.team2SeriesScore = in.team2SeriesScore;
  base.demoRecorded = recording;
  base.demoUploadConfigured = hasUpload;

  MatchFlowEvent mapEv = base;
  mapEv.type = MatchFlowEventType::MapResult;
  mapEv.mapNumber = in.mapNumber;
  mapEv.mapName = in.mapName;
  mapEv.winner = in.winner;
  mapEv.team1Score = in.team1Score;
  mapEv.team2Score = in.team2Score;
  mapEv.seriesOver = in.seriesOver;
  mapEv.nextMap = in.nextMap;
  {
    std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
    stats::Current().EndMap();
    mapEv.stats = stats::Current().Snapshot();
  }
  Emit(mapEv);

  // Keep the engine from restarting / changing level before this flow is done with the map.
  const std::string restartCmd = "mp_match_restart_delay " + std::to_string(plan.restartDelay + 1);
  (void)EnqueueServerCommand(restartCmd.c_str());
  if (recording) {
    (void)demo::StopAfterDelayAndUpload(std::max(0.0, plan.tvFlushDelay - 0.5), in.team1Score + in.team2Score,
                                        in.team1Score, in.team2Score);
  }
  Print("map-end: map %d done; demo=%d upload=%d tv_delay=%d restart_delay=%ds flush=%ds series_over=%d\n",
        in.mapNumber, recording ? 1 : 0, hasUpload ? 1 : 0, tvDelay, plan.restartDelay, plan.tvFlushDelay,
        in.seriesOver ? 1 : 0);

  if (!in.seriesOver) {
    const std::string next = in.nextMap;
    g_pending.fetch_add(1);
    ScheduleOnGameThread(std::max(1, plan.restartDelay - 1), [next, gen]() {
      g_pending.fetch_sub(1);
      if (g_generation.load() != gen) return;
      ModesBeginNextMapWarmup();
      if (!next.empty()) (void)EnqueueServerCommand(("changelevel " + next).c_str());
    });
    return plan;
  }

  // Series over.
  std::string seriesWinner = "none";
  if (in.team1SeriesScore > in.team2SeriesScore) seriesWinner = "team1";
  else if (in.team2SeriesScore > in.team1SeriesScore) seriesWinner = "team2";
  const std::string winnerName = seriesWinner == "team1" ? base.team1Name : seriesWinner == "team2" ? base.team2Name : "";
  const int resetIn = base.scrim ? std::max(1, plan.restartDelay - 1) : plan.kickDelay;

  MatchFlowEvent seriesEv = base;
  seriesEv.type = MatchFlowEventType::SeriesEnd;
  seriesEv.mapNumber = in.mapNumber;
  seriesEv.winner = seriesWinner;
  seriesEv.secondsUntilReset = resetIn;
  Emit(seriesEv);
  // Existing webhook (shape unchanged); time_until_restore now carries the real delay.
  WebhookEmitSeriesEnd(in.team1SeriesScore, in.team2SeriesScore, seriesWinner.c_str(), resetIn);

  if (!winnerName.empty()) {
    SendToChat(("Ready Up: " + winnerName + " won the series " +
                std::to_string(std::max(in.team1SeriesScore, in.team2SeriesScore)) + "-" +
                std::to_string(std::min(in.team1SeriesScore, in.team2SeriesScore)) + ".")
                   .c_str());
  } else {
    SendToChat("Ready Up: the series ended in a draw.");
  }

  auto reset = [base, gen]() {
    g_pending.fetch_sub(1);
    if (g_generation.load() != gen) return;
    MatchFlowEvent ev = base;
    ev.type = MatchFlowEventType::ServerReset;
    Emit(ev);
    ModesFinishSeriesResetToIdle();
  };

  g_pending.fetch_add(1);
  if (base.scrim) {
    // Pickup games: nobody is kicked, the scrim just unloads.
    ScheduleOnGameThread(resetIn, reset);
    return plan;
  }

  SendToChat(("Ready Up: all players will be disconnected in " + std::to_string(resetIn) +
              "s to prepare the server for the next match.")
                 .c_str());
  const std::string kickReason = winnerName.empty() ? std::string("Series over. Thanks for playing!")
                                                    : winnerName + " won the match";
  ScheduleOnGameThread(resetIn, [kickReason, gen, reset]() {
    if (g_generation.load() == gen) KickAllHumans(kickReason);
    ScheduleOnGameThread(kResetAfterKickSeconds, reset);
  });
  return plan;
}

void MatchEndCancelPending() { g_generation.fetch_add(1); }

bool MatchEndPending() { return g_pending.load() > 0; }

bool MatchFlowHandleConsoleLine(const std::string& line) {
  const auto args = SplitArgs(line);
  if (args.empty()) return false;
  if (demo::HandleConsoleLine(args)) return true;
  const std::string& cmd = args[0];
  if (cmd == "ru_series_end_kick_delay_no_demo" || cmd == "ru_series_end_kick_delay_demo_no_upload" ||
      cmd == "ru_series_end_kick_delay_demo_upload") {
    const int v = args.size() > 1 ? std::max(0, std::atoi(args[1].c_str())) : -1;
    if (cmd == "ru_series_end_kick_delay_no_demo") SetSeriesEndKickDelays(v, -1, -1);
    else if (cmd == "ru_series_end_kick_delay_demo_no_upload") SetSeriesEndKickDelays(-1, v, -1);
    else SetSeriesEndKickDelays(-1, -1, v);
    int a = 0, b = 0, c = 0;
    GetSeriesEndKickDelays(&a, &b, &c);
    Print("series end kick delays: no_demo=%d demo_no_upload=%d demo_upload=%d\n", a, b, c);
    return true;
  }
  if (cmd == "ru_match_stats") {
    std::string json;
    {
      std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
      json = stats::ToJson(stats::Current().Snapshot());
    }
    // One line: consumers take everything after the first '{'.
    // Print() formats into a 2 KB buffer, so the JSON goes out in chunks on one line.
    PrintRaw("%s match_stats ", kLogPrefix);
    for (size_t i = 0; i < json.size(); i += 1000) PrintRaw("%s", json.substr(i, 1000).c_str());
    PrintRaw("\n");
    return true;
  }
  return false;
}

}  // namespace readyup
