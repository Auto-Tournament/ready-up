#include "readyup/status_feed.h"

#include "readyup/config.h"
#include "readyup/cs2_version.h"
#include "readyup/demo_recorder.h"
#include "readyup/disabled.h"
#include "readyup/fleet_iface.h"
#include "readyup/logging.h"
#include "readyup/match_end.h"
#include "readyup/match_state.h"
#include "readyup/match_stats.h"
#include "readyup/minijson.h"
#include "readyup/modes.h"
#include "readyup/path.h"
#include "readyup/pause_state.h"
#include "readyup/player_registry.h"
#include "readyup/plugin_api.h"
#include "readyup/plugin_loader.h"
#include "readyup/scrim_flow.h"
#include "readyup/slot_registry.h"
#include "readyup/status_http.h"
#include "readyup/status_server.h"
#include "readyup/status_snapshot.h"
#include "readyup/version.h"
#include "readyup/webhook.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace readyup::status_feed {
namespace {

using status::Json;
using Clock = std::chrono::steady_clock;

constexpr auto kBuildInterval = std::chrono::milliseconds(250);
constexpr auto kAutoSelftestDelay = std::chrono::seconds(15);

long long UnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// ---- process-wide state (the server and hub are never freed: the thread lives until exit) --

std::atomic<bool> g_started{false};
std::shared_ptr<status::Hub> g_hub;
status::StatusServer* g_server = nullptr;
std::string g_bind, g_token, g_discoveryPath, g_hostname, g_startError;
int g_gamePort = 0, g_statusPort = 0;
long long g_startedAtMs = 0;

std::atomic<bool> g_selftestRequested{false};

// Selftest record (any thread writes, game thread reads).
std::mutex g_selftestMu;
Json g_selftestJson;  // null until the first run
std::string g_selftestReport;
bool g_selftestRan = false, g_selftestPass = true;

// Demo state per map (recording events: game thread; upload events: upload thread).
struct DemoInfo {
  std::string file;
  std::string state;  // recording | stopped | uploading | stored | failed
};
std::mutex g_demoMu;
long long g_demoMatchId = 0;
std::map<int, DemoInfo> g_demos;
int g_uploadsPending = 0;

// Series / map results (match flow listener, game thread).
struct MapResult {
  int team1 = 0, team2 = 0;
  std::string winner;
};
unsigned long long g_seriesMatchId = 0;
int g_seriesT1 = 0, g_seriesT2 = 0;
bool g_seriesOver = false;
std::map<int, MapResult> g_mapResults;

// Game thread only.
Clock::time_point g_lastBuild{};
Clock::time_point g_firstSimTick{};
bool g_sawSimTick = false, g_autoSelftestDone = false;
bool g_wasPaused = false;
long long g_pauseStartedMs = 0;
std::atomic<long long> g_buildUsLast{0}, g_buildUsMax{0};
std::atomic<unsigned long long> g_builds{0};
// Simulating-frame spacing (game thread): max / mean gap over the last completed 10 s window.
// A direct measure that the endpoint never stalls a frame (compare with the endpoint under load).
Clock::time_point g_lastSimFrame{}, g_gapWindowStart{};
double g_gapWinMaxMs = 0, g_gapWinSumMs = 0;
unsigned long g_gapWinN = 0;
double g_gapMaxMs = 0, g_gapAvgMs = 0;

void Log(const std::string& s) { Print("%s\n", s.c_str()); }

std::string ReadFile(const std::string& path, size_t cap = 1 << 20) {
  std::ifstream f(path, std::ios::binary);
  if (!f.good()) return {};
  std::string s;
  s.resize(cap);
  f.read(&s[0], static_cast<std::streamsize>(cap));
  s.resize(static_cast<size_t>(f.gcount()));
  return s;
}

std::string RandomToken() {
  unsigned char b[24] = {0};
  bool ok = false;
  const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    ok = read(fd, b, sizeof(b)) == static_cast<ssize_t>(sizeof(b));
    close(fd);
  }
  if (!ok) {
    // Very unlikely; still unpredictable enough for a local read-only token.
    unsigned long long x = static_cast<unsigned long long>(UnixMs()) ^ (static_cast<unsigned long long>(getpid()) << 32);
    for (auto& c : b) {
      x ^= x << 13;
      x ^= x >> 7;
      x ^= x << 17;
      c = static_cast<unsigned char>(x);
    }
  }
  static const char* hex = "0123456789abcdef";
  std::string t = "rst_";
  for (unsigned char c : b) {
    t += hex[c >> 4];
    t += hex[c & 15];
  }
  return t;
}

// Token: readyup.cfg, else the one already in status.json (stable across restarts), else new.
std::string LoadOrCreateToken(const std::string& configured, const std::string& discoveryPath) {
  if (!configured.empty()) return configured;
  const std::string existing = ReadFile(discoveryPath, 64 * 1024);
  if (!existing.empty()) {
    minijson::ParseError err;
    auto v = minijson::Parse(existing, &err);
    if (v) {
      auto tok = minijson::AsString(v->get("token"));
      if (tok && tok->rfind("rst_", 0) == 0 && tok->size() >= 20 && tok->size() < 200) return *tok;
    }
  }
  return RandomToken();
}

bool WriteDiscovery(std::string* err) {
  if (g_discoveryPath.empty()) {
    if (err) *err = "csgo dir not resolved";
    return false;
  }
  std::string j = "{\"port\":" + std::to_string(g_statusPort) + ",\"bind\":";
  status::JsonEscapeTo(j, g_bind);
  j += ",\"token\":";
  status::JsonEscapeTo(j, g_token);
  j += ",\"pid\":" + std::to_string(static_cast<long long>(getpid()));
  j += ",\"game_port\":" + std::to_string(g_gamePort);
  j += ",\"started_at\":" + std::to_string(g_startedAtMs / 1000);
  j += ",\"version\":";
  status::JsonEscapeTo(j, SemVer());
  j += "}\n";

  const std::string tmp = g_discoveryPath + ".tmp";
  unlink(tmp.c_str());
  const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
  if (fd < 0) {
    if (err) *err = std::string("open ") + tmp + ": " + std::strerror(errno);
    return false;
  }
  (void)fchmod(fd, 0640);  // regardless of umask
  size_t off = 0;
  bool ok = true;
  while (off < j.size()) {
    const ssize_t n = write(fd, j.data() + off, j.size() - off);
    if (n <= 0) {
      ok = false;
      break;
    }
    off += static_cast<size_t>(n);
  }
  close(fd);
  if (!ok || rename(tmp.c_str(), g_discoveryPath.c_str()) != 0) {
    if (err) *err = std::string("write/rename ") + g_discoveryPath + ": " + std::strerror(errno);
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

bool EnvOff(const char* name) {
  const char* v = std::getenv(name);
  return v && *v && (v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
}

// ---- collection (game thread) ----------------------------------------------------------

struct PhaseInfo {
  const char* mode = "idle";   // idle | scrim | match | practice
  const char* phase = "idle";  // FLEET.md §9.1 phases + idle / practice / warmup (scrim)
};

PhaseInfo ComputePhase(ReadyUpMode m, bool haveCtx, bool scrim, bool paused, bool seriesOver) {
  PhaseInfo p;
  switch (m) {
    case ReadyUpMode::Idle: p.mode = "idle"; p.phase = "idle"; break;
    case ReadyUpMode::Practice: p.mode = "practice"; p.phase = "practice"; break;
    case ReadyUpMode::ScrimWarmup: p.mode = "scrim"; p.phase = "warmup"; break;
    case ReadyUpMode::MatchWarmup: p.phase = "warmup"; break;
    case ReadyUpMode::MatchKnife: p.phase = GetKnifePhase() == KnifePhase::Picking ? "side_pick" : "knife"; break;
    case ReadyUpMode::MatchLive: p.phase = paused ? "paused" : "live"; break;
    case ReadyUpMode::Postgame: p.phase = seriesOver ? "series_end" : "map_end"; break;
  }
  if (m != ReadyUpMode::Idle && m != ReadyUpMode::Practice && m != ReadyUpMode::ScrimWarmup) {
    p.mode = (!haveCtx || scrim) ? "scrim" : "match";
  }
  // Knife side pick can outlive the match_knife mode briefly.
  if (m == ReadyUpMode::MatchWarmup && GetKnifePhase() == KnifePhase::Picking) p.phase = "side_pick";
  return p;
}

Json Versions() {
  Json v = Json::Object();
  v["core"] = SemVer();
  v["core_build"] = BuildVersion();
  v["plugin_api"] = std::to_string(READYUP_PLUGIN_API_VERSION_MAJOR) + "." + std::to_string(READYUP_PLUGIN_API_VERSION_MINOR);
  Json plugins = Json::Object();
  const auto host = plugins::GetPluginHostStatus();
  for (const auto& s : host.loaded) {
    const size_t sp = s.find(' ');
    plugins[s.substr(0, sp)] = sp == std::string::npos ? std::string() : s.substr(sp + 1);
  }
  v["plugins"] = std::move(plugins);
  const auto cs2 = GetCs2VersionSnapshot();
  v["cs2_build"] = cs2.build_id ? Json(*cs2.build_id) : Json();
  v["cs2_patch"] = cs2.version_string ? Json(*cs2.version_string) : Json();
  return v;
}

// platform + fleet-driven flags.
Json Platform(std::string* serverId, bool* updateBlocked) {
  Json p = Json::Object();
  void* raw = plugins::CoreGetInterface(RU_FLEET_IFACE_NAME, RU_FLEET_IFACE_VERSION);
  const auto* iface = static_cast<const ru_fleet_v1*>(raw);
  const size_t need = offsetof(ru_fleet_v1, get_status) + sizeof(iface->get_status);
  ru_fleet_status st{};
  st.struct_size = sizeof(st);
  st.auto_pause_in_s = -1;
  if (iface && iface->struct_size >= need && iface->get_status && iface->get_status(&st) == 1) {
    static const char* kStates[] = {"offline", "online", "enrolling", "rejected"};
    p["mode"] = "fleet";
    p["state"] = st.state < 4 ? kStates[st.state] : "offline";
    p["since"] = static_cast<long long>(st.since_ms / 1000);  // unix seconds
    p["reconnects"] = st.reconnects;
    p["spool_msgs"] = st.spool_msgs;
    if (st.auto_pause_in_s >= 0) p["auto_pause_in_s"] = st.auto_pause_in_s;
    st.server_id[sizeof(st.server_id) - 1] = '\0';
    *serverId = st.server_id;
    *updateBlocked = st.update_blocked != 0;
    return p;
  }
  p["mode"] = "standalone";
  p["state"] = "standalone";
  p["since"] = g_startedAtMs / 1000;  // unix seconds
  return p;
}

Json SideOf(bool known, bool ct) {
  if (!known) return Json();
  return Json(ct ? "ct" : "t");
}

std::shared_ptr<status::StatusInputs> Collect() {
  auto in = std::make_shared<status::StatusInputs>();
  in->hostname = g_hostname;
  in->game_port = g_gamePort;
  in->versions = Versions();

  bool fleetBlocksUpdate = false;
  in->platform = Platform(&in->server_id, &fleetBlocksUpdate);

  {
    std::lock_guard<std::mutex> lk(g_selftestMu);
    in->selftest = g_selftestJson;
    in->selftest_report = g_selftestReport;
    if (g_selftestRan && !g_selftestPass) {
      in->healthy = false;
      in->unhealthy_reason = "selftest failed";
    }
  }
  if (IsDisabled()) {
    in->healthy = false;
    in->unhealthy_reason = "disabled: " + DisabledReason();
  }

  const ReadyUpMode mode = GetMode();
  const auto ctx = WebhookGetMatchContext();
  const bool scrim = ctx && ctx->slug == "scrim";
  const auto ms = MatchStateGet();
  const auto pause = PauseStateGet();
  const bool paused = ctx && pause.paused;
  const long long nowMs = UnixMs();
  if (paused && !g_wasPaused) g_pauseStartedMs = nowMs - 1000LL * PauseStatePauseDurationSeconds();
  g_wasPaused = paused;

  // A new / different match resets the series bookkeeping.
  if (!ctx || ctx->matchid != g_seriesMatchId) {
    if (!ctx || g_seriesMatchId != 0) {
      g_seriesT1 = g_seriesT2 = 0;
      g_seriesOver = false;
      g_mapResults.clear();
    }
    g_seriesMatchId = ctx ? ctx->matchid : 0;
  }
  const PhaseInfo ph = ComputePhase(mode, ctx.has_value(), scrim, paused, g_seriesOver);

  // Humans (connected) and names.
  const auto humans = ListHumans();
  std::unordered_map<uint64_t, const HumanIdentity*> humanBy;
  for (const auto& h : humans) humanBy[h.steamid64] = &h;

  int ready = 0, readyTotal = 0;
  if (ctx) {
    for (const auto& kv : ctx->roster_team) {
      if (!kv.first) continue;
      ++readyTotal;
      if (IsReady(kv.first)) ++ready;
    }
  } else {
    for (const auto& h : humans) {
      if (h.team != 2 && h.team != 3) continue;
      ++readyTotal;
      if (IsReady(h.steamid64)) ++ready;
    }
  }

  // Demo / upload state.
  int uploadsPending = 0;
  std::map<int, DemoInfo> demos;
  {
    std::lock_guard<std::mutex> lk(g_demoMu);
    uploadsPending = g_uploadsPending;
    if (ctx && static_cast<unsigned long long>(g_demoMatchId) == ctx->matchid) demos = g_demos;
  }

  // update_safe (docs/FLEET.md §17.2): false from loading to series_end of a real match and while
  // a demo upload is pending; scrims, practice and idle are safe.
  const bool matchActive = ctx && !scrim && mode != ReadyUpMode::Idle && mode != ReadyUpMode::Practice &&
                           mode != ReadyUpMode::ScrimWarmup;
  bool safe = !matchActive || (mode == ReadyUpMode::Postgame && g_seriesOver && !MatchEndPending());
  if (uploadsPending > 0 || fleetBlocksUpdate) safe = false;
  in->update_safe = safe;

  // ---- summary
  Json s = Json::Object();
  s["mode"] = ph.mode;
  s["ru_mode"] = GetModeString();
  s["phase"] = ph.phase;
  s["map"] = ms.current_map;
  s["map_number"] = ms.map_number;
  s["num_maps"] = ctx ? ctx->num_maps : 0;
  s["round"] = ms.round_number;
  Json score = Json::Object();
  score["team1"] = ms.team1_score;
  score["team2"] = ms.team2_score;
  s["score"] = std::move(score);
  Json series = Json::Object();
  series["team1"] = g_seriesT1;
  series["team2"] = g_seriesT2;
  s["series_score"] = std::move(series);
  Json players = Json::Object();
  players["connected"] = static_cast<int>(humans.size());
  players["expected"] = ctx ? static_cast<int>(ctx->roster_team.size()) : 0;
  s["players"] = std::move(players);
  Json rdy = Json::Object();
  rdy["ready"] = ready;
  rdy["total"] = readyTotal;
  s["ready"] = std::move(rdy);
  if (ctx) {
    s["match_id"] = std::to_string(ctx->matchid);
    s["slug"] = ctx->slug;
    Json teams = Json::Object();
    teams["team1"] = ctx->team1_name;
    teams["team2"] = ctx->team2_name;
    s["teams"] = std::move(teams);
    s["paused"] = paused;
  }
  if (const char* kp = KnifePhaseString()) s["knife"] = kp;
  const int countdown = ScrimCountdownSecondsLeft();
  if (countdown >= 0) s["countdown_s"] = countdown;
  if (uploadsPending > 0) s["demo_uploads_pending"] = uploadsPending;
  in->summary = std::move(s);

  // ---- MatchState (docs/FLEET.md §9.1) while a match / scrim context is loaded.
  if (ctx) {
    Json st = Json::Object();
    st["match_id"] = std::to_string(ctx->matchid);
    if (!in->server_id.empty()) st["server_id"] = in->server_id;
    st["slug"] = ctx->slug;
    st["scrim"] = scrim;
    st["phase"] = ph.phase;

    // Sides of team1 on the current map.
    bool sideKnown = false, team1Ct = true;
    stats::TeamLine t1line, t2line;
    {
      std::lock_guard<std::recursive_mutex> lk(stats::Mutex());
      auto& acc = stats::Current();
      t1line = acc.Team1Line();
      t2line = acc.Team2Line();
      if (acc.Live()) {
        sideKnown = true;
        team1Ct = acc.Team1IsCt();
      }
    }
    const int mapIdx = ms.map_number - 1;
    if (!sideKnown && mapIdx >= 0 && static_cast<size_t>(mapIdx) < ctx->map_sides.size()) {
      const std::string& ms1 = ctx->map_sides[static_cast<size_t>(mapIdx)];
      if (ms1 == "team1_ct" || ms1 == "team2_ct") {
        sideKnown = true;
        team1Ct = ms1 == "team1_ct";
      }
    }

    // series
    Json ser = Json::Object();
    ser["num_maps"] = ctx->num_maps;
    ser["current_map"] = ms.map_number;
    Json sscore = Json::Object();
    sscore["team1"] = g_seriesT1;
    sscore["team2"] = g_seriesT2;
    ser["score"] = std::move(sscore);
    Json maps = Json::Object();
    const bool curDone = mode == ReadyUpMode::Postgame;
    for (size_t i = 0; i < ctx->maplist.size(); ++i) {
      const int n = static_cast<int>(i) + 1;
      Json m = Json::Object();
      m["name"] = ctx->maplist[i];
      m["sides"] = i < ctx->map_sides.size() ? ctx->map_sides[i] : std::string("knife");
      const auto res = g_mapResults.find(n);
      const char* status = "pending";
      if (res != g_mapResults.end() || n < ms.map_number || (n == ms.map_number && curDone)) status = "done";
      else if (n == ms.map_number && mode != ReadyUpMode::MatchWarmup) status = "live";
      m["status"] = status;
      Json msc = Json::Object();
      if (res != g_mapResults.end()) {
        msc["team1"] = res->second.team1;
        msc["team2"] = res->second.team2;
        m["score"] = std::move(msc);
        m["winner"] = res->second.winner;
      } else if (n == ms.map_number) {
        msc["team1"] = ms.team1_score;
        msc["team2"] = ms.team2_score;
        m["score"] = std::move(msc);
      }
      const auto d = demos.find(n);
      if (d != demos.end()) {
        Json dj = Json::Object();
        dj["file"] = d->second.file;
        dj["state"] = d->second.state;
        m["demo"] = std::move(dj);
      }
      maps[std::to_string(n)] = std::move(m);
    }
    ser["maps"] = std::move(maps);
    st["series"] = std::move(ser);

    // names for disconnected roster players
    std::unordered_map<uint64_t, std::string> seen;
    for (const auto& p : ListObservedPlayers()) seen[p.steamid64] = p.name;

    auto team = [&](WebhookTeam which, const std::string& name, const stats::TeamLine& line, int mapScore,
                    bool isTeam1) {
      Json t = Json::Object();
      t["name"] = name;
      t["side"] = SideOf(sideKnown, isTeam1 ? team1Ct : !team1Ct);
      t["score"] = mapScore;
      t["score_ct"] = line.score_ct;
      t["score_t"] = line.score_t;
      Json pl = Json::Object();
      for (const auto& kv : ctx->roster_team) {
        if (kv.second != which || !kv.first) continue;
        Json pj = Json::Object();
        const auto h = humanBy.find(kv.first);
        std::string nm = h != humanBy.end() ? h->second->name : std::string();
        if (nm.empty()) {
          const auto o = seen.find(kv.first);
          if (o != seen.end()) nm = o->second;
        }
        pj["name"] = nm;
        pj["role"] = "player";
        pj["connected"] = h != humanBy.end();
        pj["ready"] = IsReady(kv.first);
        pl[std::to_string(kv.first)] = std::move(pj);
      }
      t["players"] = std::move(pl);
      return t;
    };
    Json teams = Json::Object();
    teams["team1"] = team(WebhookTeam::Team1, ctx->team1_name, t1line, ms.team1_score, true);
    teams["team2"] = team(WebhookTeam::Team2, ctx->team2_name, t2line, ms.team2_score, false);
    st["teams"] = std::move(teams);

    Json specs = Json::Object();
    for (uint64_t sid : ctx->spectators) {
      Json sj = Json::Object();
      const auto h = humanBy.find(sid);
      sj["name"] = h != humanBy.end() ? h->second->name : (seen.count(sid) ? seen[sid] : std::string());
      sj["connected"] = h != humanBy.end();
      specs[std::to_string(sid)] = std::move(sj);
    }
    st["spectators"] = std::move(specs);

    int perTeam = 0, c1 = 0, c2 = 0;
    for (const auto& kv : ctx->roster_team) {
      if (kv.second == WebhookTeam::Team1) ++c1;
      else if (kv.second == WebhookTeam::Team2) ++c2;
    }
    perTeam = std::max(c1, c2);
    Json rj = Json::Object();
    rj["required_per_team"] = perTeam;
    rj["ready"] = ready;
    rj["total"] = readyTotal;
    st["ready"] = std::move(rj);

    Json kj = Json::Object();
    const KnifePhase kp = GetKnifePhase();
    kj["status"] = kp == KnifePhase::Picking ? "picking"
                   : (kp == KnifePhase::Starting || kp == KnifePhase::Running) ? "running"
                                                                                 : "none";
    if (kp == KnifePhase::Picking) kj["winner"] = KnifeWinnerTeamString();
    st["knife"] = std::move(kj);

    Json pj = Json::Object();
    pj["active"] = paused;
    if (paused) pj["started_at"] = g_pauseStartedMs;
    Json un = Json::Object();
    un["team1"] = paused && pause.team1_ready_to_unpause;
    un["team2"] = paused && pause.team2_ready_to_unpause;
    pj["unpause"] = std::move(un);
    st["pause"] = std::move(pj);

    Json round = Json::Object();
    round["number"] = ms.round_number;
    st["round"] = std::move(round);
    in->state = std::move(st);
  }
  return in;
}

void Build() {
  const auto t0 = Clock::now();
  auto in = Collect();
  // Timing of the previous builds (this build's own cost is only known after it).
  in->gauges.emplace_back("status_feed_build_us_last", static_cast<double>(g_buildUsLast.load()));
  in->gauges.emplace_back("status_feed_build_us_max", static_cast<double>(g_buildUsMax.load()));
  in->gauges.emplace_back("status_feed_builds_total", static_cast<double>(g_builds.load()));
  in->gauges.emplace_back("game_frame_gap_ms_max_10s", g_gapMaxMs);
  in->gauges.emplace_back("game_frame_gap_ms_avg_10s", g_gapAvgMs);
  g_hub->Submit(std::move(in));
  const long long us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
  g_buildUsLast.store(us);
  if (us > g_buildUsMax.load()) g_buildUsMax.store(us);
  g_builds.fetch_add(1);
  if (us > 2000) Debug("status: snapshot build took %lld us\n", us);
}

void OnFlowEvent(const MatchFlowEvent& e) {
  switch (e.type) {
    case MatchFlowEventType::MapResult: {
      g_seriesMatchId = e.matchid;
      g_seriesT1 = e.team1SeriesScore;
      g_seriesT2 = e.team2SeriesScore;
      g_seriesOver = e.seriesOver;
      g_mapResults[e.mapNumber] = MapResult{e.team1Score, e.team2Score, e.winner};
      break;
    }
    case MatchFlowEventType::SeriesEnd: g_seriesOver = true; break;
    case MatchFlowEventType::ServerReset:
      g_seriesT1 = g_seriesT2 = 0;
      g_seriesOver = false;
      g_mapResults.clear();
      break;
  }
}

void OnDemoEvent(const demo::DemoEvent& e) {
  std::lock_guard<std::mutex> lk(g_demoMu);
  if (e.matchid != g_demoMatchId) {
    g_demoMatchId = e.matchid;
    g_demos.clear();
  }
  DemoInfo& d = g_demos[e.mapNumber];
  if (!e.fileName.empty()) d.file = e.fileName;
  switch (e.type) {
    case demo::DemoEventType::RecordingStarted: d.state = "recording"; break;
    case demo::DemoEventType::RecordingStopped: d.state = "stopped"; break;
    case demo::DemoEventType::UploadStarted:
      d.state = "uploading";
      ++g_uploadsPending;
      break;
    case demo::DemoEventType::UploadSucceeded:
      d.state = "stored";
      g_uploadsPending = std::max(0, g_uploadsPending - 1);
      break;
    case demo::DemoEventType::UploadFailed:
      d.state = "failed";
      g_uploadsPending = std::max(0, g_uploadsPending - 1);
      break;
  }
}

}  // namespace

void NoteSelftest(const SelftestResult& r) {
  Json j = Json::Object();
  j["pass"] = r.pass;
  j["passed"] = r.passed;
  j["total"] = r.total;
  if (r.pending) j["pending"] = r.pending;
  Json f = Json::Array();
  for (const auto& x : r.failures) f.Push(x);
  j["failures"] = std::move(f);
  j["summary"] = r.summary;
  j["ran_at"] = UnixMs() / 1000;  // unix seconds
  std::string report;
  for (const auto& l : r.lines) {
    report += l;
    report += '\n';
  }
  if (report.empty()) report = r.summary;
  std::lock_guard<std::mutex> lk(g_selftestMu);
  g_selftestJson = std::move(j);
  g_selftestReport = std::move(report);
  g_selftestRan = true;
  g_selftestPass = r.pass;
}

void StartAtLoad() {
  if (g_started.exchange(true)) return;
  const ReadyUpCfg cfg = Cfg();
  const std::string csgo = GetCsgoDirFromModuleDir();
  g_discoveryPath = csgo.empty() ? std::string() : csgo + "/readyup/status.json";
  g_gamePort = status::GamePortFromCmdline(ReadFile("/proc/self/cmdline", 64 * 1024));
  g_startedAtMs = UnixMs();
  char host[256] = {0};
  if (gethostname(host, sizeof(host) - 1) == 0) g_hostname = host;

  if (!cfg.status_http_enabled || EnvOff("READYUP_STATUS_HTTP")) {
    Print("status: local status endpoint disabled (status_http_enabled=0)\n");
    if (!g_discoveryPath.empty()) unlink(g_discoveryPath.c_str());  // no stale port/token for csm
    return;
  }

  g_bind = cfg.status_http_bind.empty() ? std::string("127.0.0.1") : cfg.status_http_bind;
  g_statusPort = cfg.status_http_port > 0 ? cfg.status_http_port : g_gamePort + 7;
  if (const char* ep = std::getenv("READYUP_STATUS_HTTP_PORT")) {
    const long v = std::strtol(ep, nullptr, 10);
    if (v > 0 && v < 65536) g_statusPort = static_cast<int>(v);
  }
  g_token = LoadOrCreateToken(cfg.status_http_token, g_discoveryPath);

  AddMatchFlowListener(&OnFlowEvent);
  demo::AddListener(&OnDemoEvent);

  g_hub = std::make_shared<status::Hub>(256);
  status::ServerConfig sc;
  sc.bind = g_bind;
  sc.port = g_statusPort;
  sc.token = g_token;
  sc.metrics = cfg.status_http_metrics;
  sc.requestSelftest = [] { g_selftestRequested.store(true); };
  sc.log = [](const std::string& s) { Log(s); };

  // First snapshot before any frame (and the only one while disabled).
  {
    auto in = std::make_shared<status::StatusInputs>();
    in->hostname = g_hostname;
    in->game_port = g_gamePort;
    Json v = Json::Object();
    v["core"] = SemVer();
    v["core_build"] = BuildVersion();
    in->versions = std::move(v);
    Json p = Json::Object();
    p["mode"] = "standalone";
    p["state"] = "standalone";
    p["since"] = g_startedAtMs / 1000;
    in->platform = std::move(p);
    Json s = Json::Object();
    s["mode"] = "idle";
    s["phase"] = IsDisabled() ? "error" : "loading";
    in->summary = std::move(s);
    if (IsDisabled()) {
      in->healthy = false;
      in->unhealthy_reason = "disabled: " + DisabledReason();
    }
    g_hub->Submit(std::move(in));
  }

  g_server = new status::StatusServer(std::move(sc), g_hub);  // lives until exit
  std::string err;
  if (!g_server->Start(&err)) {
    g_startError = err;
    Print("status: local status endpoint NOT started: %s\n", err.c_str());
    return;
  }
  std::string werr;
  if (!WriteDiscovery(&werr)) Print("status: could not write %s: %s\n", g_discoveryPath.c_str(), werr.c_str());
  Print("status: listening on http://%s:%d (/health /status /stream%s /selftest); discovery %s\n", g_bind.c_str(),
        g_statusPort, cfg.status_http_metrics ? " /metrics" : "", g_discoveryPath.c_str());
}

void FrameTick(bool simulating) {
  if (!g_server || !g_server->Running()) return;
  const auto now = Clock::now();
  if (simulating) {
    if (g_lastSimFrame != Clock::time_point{}) {
      const double gap = std::chrono::duration<double, std::milli>(now - g_lastSimFrame).count();
      g_gapWinMaxMs = std::max(g_gapWinMaxMs, gap);
      g_gapWinSumMs += gap;
      ++g_gapWinN;
    } else {
      g_gapWindowStart = now;
    }
    g_lastSimFrame = now;
    if (now - g_gapWindowStart >= std::chrono::seconds(10) && g_gapWinN > 0) {
      g_gapMaxMs = g_gapWinMaxMs;
      g_gapAvgMs = g_gapWinSumMs / static_cast<double>(g_gapWinN);
      g_gapWinMaxMs = g_gapWinSumMs = 0;
      g_gapWinN = 0;
      g_gapWindowStart = now;
    }
  } else {
    g_lastSimFrame = Clock::time_point{};  // hibernation / map change: not a stall
  }
  if (simulating && !g_sawSimTick) {
    g_sawSimTick = true;
    g_firstSimTick = now;
  }
  // One selftest after the first map is up (so /health and /status have a result), and on
  // request from `/selftest?run=1`. Runs here, on the game thread, like `ru selftest`.
  bool runSelftest = false;
  if (simulating && !g_autoSelftestDone && now - g_firstSimTick >= kAutoSelftestDelay) {
    g_autoSelftestDone = true;
    bool ran = false;
    {
      std::lock_guard<std::mutex> lk(g_selftestMu);
      ran = g_selftestRan;
    }
    runSelftest = !ran;
  }
  if (g_selftestRequested.exchange(false)) runSelftest = true;
  if (runSelftest) {
    const SelftestResult r = RunSelftest(/*printToConsole=*/false);  // reports via NoteSelftest
    Print("status: %s\n", r.summary.c_str());
    g_lastBuild = Clock::time_point{};  // publish right away
  }
  if (now - g_lastBuild < kBuildInterval) return;
  g_lastBuild = now;
  Build();
}

std::vector<std::string> StatusLines() {
  std::vector<std::string> out;
  if (!g_server) {
    out.push_back(g_startError.empty() ? "status_http: disabled" : "status_http: not started: " + g_startError);
    return out;
  }
  if (!g_server->Running()) {
    out.push_back("status_http: not running: " + g_startError);
    return out;
  }
  const auto& c = g_server->counters();
  out.push_back("status_http: http://" + g_bind + ":" + std::to_string(g_statusPort) + " (game port " +
                std::to_string(g_gamePort) + "), token " + g_token.substr(0, std::min<size_t>(8, g_token.size())) +
                "... (full token in " + g_discoveryPath + ")");
  out.push_back("status_http: requests=" + std::to_string(c.requests.load()) +
                " streams=" + std::to_string(c.streamsActive.load()) +
                " rate_limited=" + std::to_string(c.rateLimited.load()) +
                " unauthorized=" + std::to_string(c.unauthorized.load()) +
                " dropped_streams=" + std::to_string(c.streamsDropped.load()));
  out.push_back("status_http: snapshot builds=" + std::to_string(g_builds.load()) +
                " last=" + std::to_string(g_buildUsLast.load()) + "us max=" + std::to_string(g_buildUsMax.load()) +
                "us (game thread, every 250 ms)");
  char gap[128];
  std::snprintf(gap, sizeof(gap), "status_http: game frame gap (last 10 s window) max=%.2fms avg=%.2fms", g_gapMaxMs,
                g_gapAvgMs);
  out.push_back(gap);
  return out;
}

}  // namespace readyup::status_feed
