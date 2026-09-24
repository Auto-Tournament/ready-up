#include "readyup/webhook.h"

#include "readyup/cs2_version.h"
#include "readyup/config.h"
#include "readyup/http_client.h"
#include "readyup/logging.h"
#include "readyup/match_state.h"
#include "readyup/path.h"
#include "readyup/version.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <cstdio>
#include <mutex>
#include <queue>
#include <thread>

namespace readyup {
namespace {

struct State {
  std::mutex mu;
  std::string baseUrl;  // e.g. https://.../api/events
  std::string heartbeatUrl;  // e.g. https://.../api/servers/<id>/heartbeat
  std::optional<std::string> bearerToken;
  std::optional<WebhookMatchContext> match;

  enum class HbStatus { Idle = 0, Loading, Warmup, Live, Postgame, Error };
  HbStatus hbStatus = HbStatus::Idle;

  std::condition_variable cv;
  struct QItem {
    std::chrono::steady_clock::time_point due;
    int attempt = 0;
    std::string json;
  };
  struct QCmp {
    bool operator()(const QItem& a, const QItem& b) const { return a.due > b.due; }
  };
  std::priority_queue<QItem, std::vector<QItem>, QCmp> q;
  size_t maxQueue = 512;

  std::atomic<bool> started{false};
};

State& St() {
  static State st;
  return st;
}

static std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 16);
  for (unsigned char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[7];
          std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned int>(c));
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

static const char* TeamToString(WebhookTeam t) {
  switch (t) {
    case WebhookTeam::Team1: return "team1";
    case WebhookTeam::Team2: return "team2";
    default: return "unknown";
  }
}

static std::string EffectiveIdentifierLocked(const State& st) {
  if (st.match && !st.match->slug.empty()) return st.match->slug;
  if (st.match && st.match->matchid != 0) return std::to_string(st.match->matchid);
  return "unknown";
}

static std::string EndpointForLocked(const State& st) {
  if (st.baseUrl.empty()) return {};
  std::string url = st.baseUrl;
  // Ensure no trailing slash duplication.
  while (!url.empty() && url.back() == '/') url.pop_back();
  const std::string ident = EffectiveIdentifierLocked(st);
  url += "/";
  url += ident;
  return url;
}

static const char* HbStatusToString(State::HbStatus s) {
  switch (s) {
    case State::HbStatus::Idle: return "idle";
    case State::HbStatus::Loading: return "loading";
    case State::HbStatus::Warmup: return "warmup";
    case State::HbStatus::Live: return "live";
    case State::HbStatus::Postgame: return "postgame";
    case State::HbStatus::Error: return "error";
    default: return "error";
  }
}

static void EnqueueLocked(State& st, std::string json) {
  if (st.baseUrl.empty()) return;
  if (json.empty()) return;
  if (st.q.size() >= st.maxQueue) {
    // Drop some items to keep bounded (best-effort).
    // priority_queue can't drop "oldest" cheaply; we drop one arbitrary top.
    st.q.pop();
  }
  st.q.push(State::QItem{std::chrono::steady_clock::now(), /*attempt=*/0, std::move(json)});
  st.cv.notify_one();
}

static void SenderThread() {
  // MAT allocator heartbeat tick.
  auto nextHeartbeat = std::chrono::steady_clock::now() + std::chrono::seconds(5);

  for (;;) {
    State::QItem item;
    bool haveItem = false;
    std::string url;
    std::string hbUrl;
    std::optional<std::string> token;
    std::string hbPayload;

    {
      auto& st = St();
      std::unique_lock<std::mutex> lk(st.mu);
      // Wait for work (that is due) or next heartbeat.
      auto nextDue = nextHeartbeat;
      if (!st.q.empty()) {
        nextDue = std::min(nextDue, st.q.top().due);
      }
      st.cv.wait_until(lk, nextDue, [&] {
        if (!st.q.empty() && st.q.top().due <= std::chrono::steady_clock::now()) return true;
        return false;
      });

      const auto now = std::chrono::steady_clock::now();
      if (now >= nextHeartbeat) {
        // Schedule next heartbeat before releasing the lock.
        nextHeartbeat = now + std::chrono::seconds(5);

        if (!st.heartbeatUrl.empty()) {
          hbUrl = st.heartbeatUrl;
          token = st.bearerToken;

          const long long tsMs = static_cast<long long>(std::time(nullptr)) * 1000ll;
          const bool ready = (st.hbStatus == State::HbStatus::Idle) && !st.match.has_value();

          std::string matchSlug;
          long long matchid = 0;
          if (st.match) {
            matchSlug = st.match->slug;
            matchid = static_cast<long long>(st.match->matchid);
          }

          const Cs2VersionSnapshot cs2 = GetCs2VersionSnapshot();
          const std::string cs2BuildJson = cs2.build_id ? std::to_string(*cs2.build_id) : "null";
          const std::string cs2VersionJson =
              cs2.version_string ? (std::string("\"") + JsonEscape(*cs2.version_string) + "\"") : "null";

          hbPayload = std::string("{") +
                      "\"timestamp\":" + std::to_string(tsMs) + "," +
                      "\"plugin_version\":\"" + JsonEscape(SemVer()) + "\"," +
                      "\"plugin_build\":\"" + JsonEscape(BuildVersion()) + "\"," +
                      "\"status\":\"" + JsonEscape(HbStatusToString(st.hbStatus)) + "\"," +
                      "\"ready_for_allocation\":" + std::string(ready ? "true" : "false") +
                      ",\"cs2_build_id\":" + cs2BuildJson +
                      ",\"cs2_version_string\":" + cs2VersionJson +
                      (matchSlug.empty() ? "" : (std::string(",\"match_slug\":\"") + JsonEscape(matchSlug) + "\"")) +
                      (matchid ? (std::string(",\"matchid\":") + std::to_string(matchid)) : "") +
                      "}";
        }
      } else if (!st.q.empty() && st.q.top().due <= now) {
        item = std::move(const_cast<State::QItem&>(st.q.top()));
        st.q.pop();
        haveItem = true;
        url = EndpointForLocked(st);
        token = st.bearerToken;
      }
    }

    // Heartbeat is sent out-of-band (not via queue).
    if (!hbPayload.empty() && !hbUrl.empty()) {
      const HttpResponse hr = HttpPostJson(hbUrl, token, hbPayload);
      if (!hr.error.empty() || (hr.status < 200 || hr.status >= 300)) {
        if (DebugEnabled()) {
          Debug("heartbeat: POST failed status=%ld err=\"%s\"\n", hr.status, hr.error.c_str());
        }
      } else if (DebugEnabled()) {
        Debug("heartbeat: POST ok status=%ld\n", hr.status);
      }
    }

    if (!haveItem || item.json.empty() || url.empty()) continue;

    const HttpResponse r = HttpPostJson(url, token, item.json);
    if (!r.error.empty() || (r.status < 200 || r.status >= 300)) {
      if (DebugEnabled()) {
        Debug("webhook: POST failed status=%ld err=\"%s\"\n", r.status, r.error.c_str());
      }
      // Retry with exponential backoff similar to MatchZy Enhanced.
      // Schedule: 30s, 1m, 2m, 4m, 8m, 16m, 32m ... (cap at 32m), max 20 attempts.
      item.attempt += 1;
      if (item.attempt >= 20) {
        if (DebugEnabled()) Debug("webhook: dropping event after max attempts\n");
        continue;
      }

      int backoffSeconds = 30;
      const int pow2 = item.attempt - 1;
      for (int i = 0; i < pow2; ++i) {
        if (backoffSeconds >= 32 * 60) break;
        backoffSeconds *= 2;
      }
      if (backoffSeconds > 32 * 60) backoffSeconds = 32 * 60;
      item.due = std::chrono::steady_clock::now() + std::chrono::seconds(backoffSeconds);

      auto& st = St();
      std::lock_guard<std::mutex> lk(st.mu);
      if (st.q.size() < st.maxQueue) {
        st.q.push(std::move(item));
        st.cv.notify_one();
      }
    } else if (DebugEnabled()) {
      Debug("webhook: POST ok status=%ld\n", r.status);
    }
  }
}

}  // namespace

void WebhookStartSenderThread() {
  auto& st = St();
  bool expected = false;
  if (!st.started.compare_exchange_strong(expected, true)) return;
  std::thread(SenderThread).detach();
}

void WebhookConfigure(std::string baseEventsUrl) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.baseUrl = std::move(baseEventsUrl);
  // Kick sender thread if enabled.
  if (!st.baseUrl.empty()) st.cv.notify_one();
}

std::string WebhookBaseUrl() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.baseUrl;
}

void WebhookConfigureHeartbeatUrl(std::string heartbeatUrl) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.heartbeatUrl = std::move(heartbeatUrl);
  if (!st.heartbeatUrl.empty()) st.cv.notify_one();
}

std::string WebhookHeartbeatUrl() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.heartbeatUrl;
}

void WebhookSetBearerToken(std::optional<std::string> token) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (token && token->empty()) token.reset();
  st.bearerToken = std::move(token);
}

void WebhookSetMatchContext(WebhookMatchContext ctx) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.match = std::move(ctx);
  st.hbStatus = State::HbStatus::Warmup;
}

void WebhookClearMatchContext() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  st.match.reset();
  st.hbStatus = State::HbStatus::Idle;
}

void WebhookSetHeartbeatStatus(const char* status) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  const std::string s = status ? std::string(status) : std::string();
  if (s == "idle") st.hbStatus = State::HbStatus::Idle;
  else if (s == "loading") st.hbStatus = State::HbStatus::Loading;
  else if (s == "warmup") st.hbStatus = State::HbStatus::Warmup;
  else if (s == "live") st.hbStatus = State::HbStatus::Live;
  else if (s == "postgame") st.hbStatus = State::HbStatus::Postgame;
  else if (s == "error") st.hbStatus = State::HbStatus::Error;
  else st.hbStatus = State::HbStatus::Idle;
}

std::optional<WebhookMatchContext> WebhookGetMatchContext() {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  return st.match;
}

void WebhookEnqueueEvent(std::string json) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  EnqueueLocked(st, std::move(json));
}

void WebhookEmitServerConfigured(const char* configuredBy) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (st.baseUrl.empty()) return;
  const std::string serverId = EffectiveIdentifierLocked(st);
  const std::string by = configuredBy ? configuredBy : "unknown";
  const std::string json =
      std::string("{") +
      "\"event\":\"server_configured\"," +
      "\"matchid\":-1," +
      "\"server_id\":\"" + JsonEscape(serverId) + "\"," +
      "\"hostname\":\"" + JsonEscape(serverId) + "\"," +
      "\"plugin_version\":\"" + JsonEscape(BuildVersion()) + "\"," +
      "\"timestamp\":" + std::to_string(static_cast<long long>(std::time(nullptr)) * 1000ll) + "," +
      "\"configured_by\":\"" + JsonEscape(by) + "\"," +
      "\"remote_log_url\":\"" + JsonEscape(st.baseUrl) + "\"" +
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitServerHealth(const char* reason) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (st.baseUrl.empty()) return;
  const std::string serverId = EffectiveIdentifierLocked(st);
  std::string json =
      std::string("{") +
      "\"event\":\"server_health\"," +
      "\"matchid\":-1," +
      "\"server_id\":\"" + JsonEscape(serverId) + "\"," +
      "\"plugin_version\":\"" + JsonEscape(BuildVersion()) + "\"," +
      "\"timestamp\":" + std::to_string(static_cast<long long>(std::time(nullptr)) * 1000ll) + "," +
      "\"db_ok\":true," +
      "\"db_type\":\"readyup\"";
  if (reason && *reason) {
    json += ",\"reason\":\"" + JsonEscape(reason) + "\"";
  }
  json += "}";
  EnqueueLocked(st, std::move(json));
}

void WebhookEmitSeriesStart() {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;
  if (m.matchid == 0) return;
  st.hbStatus = State::HbStatus::Warmup;
  const std::string json =
      std::string("{") +
      "\"event\":\"series_start\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"team1_name\":\"" + JsonEscape(m.team1_name) + "\"," +
      "\"team2_name\":\"" + JsonEscape(m.team2_name) + "\"," +
      "\"num_maps\":" + std::to_string(m.num_maps) +
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitSeriesEnd(int team1_series_score, int team2_series_score, const char* winner, int time_until_restore) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;
  st.hbStatus = State::HbStatus::Postgame;
  const std::string win = winner ? winner : "none";
  const std::string json =
      std::string("{") +
      "\"event\":\"series_end\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"team1_series_score\":" + std::to_string(team1_series_score) + "," +
      "\"team2_series_score\":" + std::to_string(team2_series_score) + "," +
      "\"winner\":\"" + JsonEscape(win) + "\"," +
      "\"time_until_restore\":" + std::to_string(time_until_restore) +
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitMapResult(int map_number, const char* map_name, int team1_score, int team2_score, const char* winner) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;
  st.hbStatus = State::HbStatus::Postgame;
  const std::string map = map_name ? map_name : "";
  const std::string win = winner ? winner : "none";
  const std::string json =
      std::string("{") +
      "\"event\":\"map_result\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"map_number\":" + std::to_string(map_number) + "," +
      "\"map_name\":\"" + JsonEscape(map) + "\"," +
      "\"team1_score\":" + std::to_string(team1_score) + "," +
      "\"team2_score\":" + std::to_string(team2_score) + "," +
      "\"winner\":\"" + JsonEscape(win) + "\"" +
      "}";
  EnqueueLocked(st, json);
}

static std::string PlayerObj(const WebhookPlayer& p) {
  return std::string("{") +
         "\"steamid\":\"" + JsonEscape(std::to_string(p.steamid64)) + "\"," +
         "\"name\":\"" + JsonEscape(p.name) + "\"," +
         "\"team\":\"" + JsonEscape(TeamToString(p.team)) + "\"" +
         "}";
}

void WebhookEmitMatchPaused(int map_number, const WebhookPlayer& paused_by, bool is_tactical, bool is_admin, int pause_time) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;
  WebhookPlayer p = paused_by;
  if (p.team == WebhookTeam::Unknown && p.steamid64 != 0) {
    auto it = m.roster_team.find(p.steamid64);
    if (it != m.roster_team.end()) p.team = it->second;
  }

  const std::string json =
      std::string("{") +
      "\"event\":\"match_paused\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"map_number\":" + std::to_string(map_number) + "," +
      "\"paused_by\":" + PlayerObj(p) + "," +
      "\"is_tactical\":" + std::string(is_tactical ? "true" : "false") + "," +
      "\"is_admin\":" + std::string(is_admin ? "true" : "false") + "," +
      "\"pause_time\":" + std::to_string(pause_time) +
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitUnpauseRequested(int map_number, WebhookTeam team, int teams_ready, int teams_needed) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;

  const std::string t = TeamToString(team);
  const std::string json =
      std::string("{") +
      "\"event\":\"unpause_requested\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"map_number\":" + std::to_string(map_number) + "," +
      "\"team\":\"" + JsonEscape(t) + "\"," +
      "\"teams_ready\":" + std::to_string(teams_ready) + "," +
      "\"teams_needed\":" + std::to_string(teams_needed) +
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitMatchUnpaused(int map_number, int pause_duration) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;

  const std::string json =
      std::string("{") +
      "\"event\":\"match_unpaused\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"map_number\":" + std::to_string(map_number) + "," +
      "\"pause_duration\":" + std::to_string(pause_duration) +
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitWarmupEnded(int map_number) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;

  const std::string json =
      std::string("{") +
      "\"event\":\"warmup_ended\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"map_number\":" + std::to_string(map_number) +
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitGoingLive(int map_number) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;
  st.hbStatus = State::HbStatus::Live;

  std::string json =
      std::string("{") +
      "\"event\":\"going_live\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"map_number\":" + std::to_string(map_number);

  const MatchStateSnapshot snap = MatchStateGet();
  if (!snap.current_map.empty()) {
    json += ",\"map_name\":\"" + JsonEscape(snap.current_map) + "\"";
  }

  json +=
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitHalftimeStarted(int map_number, int team1_score, int team2_score) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;

  const std::string json =
      std::string("{") +
      "\"event\":\"halftime_started\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"map_number\":" + std::to_string(map_number) + "," +
      "\"team1_score\":" + std::to_string(team1_score) + "," +
      "\"team2_score\":" + std::to_string(team2_score) +
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitOvertimeStarted(int map_number, int overtime_number) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;
  st.hbStatus = State::HbStatus::Live;

  const std::string json =
      std::string("{") +
      "\"event\":\"overtime_started\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"map_number\":" + std::to_string(map_number) + "," +
      "\"overtime_number\":" + std::to_string(std::max(1, overtime_number)) +
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitSideSwap(int map_number, const char* team1_side, const char* team2_side) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;
  const std::string t1 = team1_side ? team1_side : "";
  const std::string t2 = team2_side ? team2_side : "";

  const std::string json =
      std::string("{") +
      "\"event\":\"side_swap\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"map_number\":" + std::to_string(map_number) + "," +
      "\"team1_side\":\"" + JsonEscape(t1) + "\"," +
      "\"team2_side\":\"" + JsonEscape(t2) + "\"" +
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitKnifeRoundStarted(int map_number) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;

  const std::string json =
      std::string("{") +
      "\"event\":\"knife_round_started\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"map_number\":" + std::to_string(std::max(1, map_number)) +
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitKnifeRoundEnded(int map_number, const char* winner) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;
  const std::string win = winner ? winner : "unknown";

  const std::string json =
      std::string("{") +
      "\"event\":\"knife_round_ended\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"map_number\":" + std::to_string(std::max(1, map_number)) + "," +
      "\"winner\":\"" + JsonEscape(win) + "\"" +
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitSidePicked(int map_number, const char* map_name, const char* side, const char* picked_by, const char* team) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;

  const std::string map = map_name ? map_name : "";
  const std::string s = side ? side : "";
  const std::string by = picked_by ? picked_by : "server";
  const std::string t = team ? team : "unknown";

  // MatchZy `side_picked` payload is used by MAT for logging and UI.
  // Include `team` even if it isn't in the strict doc type, since MAT expects it.
  const std::string json =
      std::string("{") +
      "\"event\":\"side_picked\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"map_name\":\"" + JsonEscape(map) + "\"," +
      "\"map_number\":" + std::to_string(std::max(1, map_number)) + "," +
      "\"side\":\"" + JsonEscape(s) + "\"," +
      "\"picked_by\":\"" + JsonEscape(by) + "\"," +
      "\"team\":\"" + JsonEscape(t) + "\"" +
      "}";
  EnqueueLocked(st, json);
}

bool WebhookUpdateMapSide(int map_number, const char* map_side) {
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match) return false;
  if (map_number <= 0) return false;
  const std::string side = map_side ? std::string(map_side) : std::string();
  if (!(side == "team1_ct" || side == "team2_ct")) return false;

  auto& m = *st.match;
  const size_t idx = static_cast<size_t>(map_number - 1);
  if (idx >= m.map_sides.size()) return false;
  m.map_sides[idx] = side;
  return true;
}

void WebhookEmitRecoverRequested(int map_number, int round_number) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;

  const int rn = std::max(0, round_number);
  const std::string json =
      std::string("{") +
      "\"event\":\"recover_requested\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"map_number\":" + std::to_string(std::max(0, map_number)) + "," +
      "\"round_number\":" + std::to_string(rn) +
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitPlayerConnect(const WebhookPlayer& p) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;
  const std::string json =
      std::string("{") +
      "\"event\":\"player_connect\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"player\":" + PlayerObj(p) +
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitPlayerDisconnect(const WebhookPlayer& p) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;
  const std::string json =
      std::string("{") +
      "\"event\":\"player_disconnect\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"player\":" + PlayerObj(p) +
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitPlayerReady(const WebhookPlayer& p,
                            bool ready,
                            int ready_count_team1,
                            int ready_count_team2,
                            int total_ready,
                            int expected_total) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;

  // Ensure team is set when possible (roster mapping).
  WebhookPlayer pp = p;
  if (pp.team == WebhookTeam::Unknown && pp.steamid64 != 0) {
    auto it = m.roster_team.find(pp.steamid64);
    if (it != m.roster_team.end()) pp.team = it->second;
  }

  const char* evName = ready ? "player_ready" : "player_unready";
  const std::string team = TeamToString(pp.team);
  const std::string json =
      std::string("{") +
      "\"event\":\"" + std::string(evName) + "\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"player\":" + PlayerObj(pp) + "," +
      "\"team\":\"" + JsonEscape(team) + "\"," +
      "\"ready_count_team1\":" + std::to_string(ready_count_team1) + "," +
      "\"ready_count_team2\":" + std::to_string(ready_count_team2) + "," +
      "\"total_ready\":" + std::to_string(total_ready) + "," +
      "\"expected_total\":" + std::to_string(expected_total) +
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitRoundStarted(int map_number, int round_number, int team1_score, int team2_score) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;
  st.hbStatus = State::HbStatus::Live;
  const std::string json =
      std::string("{") +
      "\"event\":\"round_started\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"map_number\":" + std::to_string(map_number) + "," +
      "\"round_number\":" + std::to_string(round_number) + "," +
      "\"team1_score\":" + std::to_string(team1_score) + "," +
      "\"team2_score\":" + std::to_string(team2_score) +
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitRoundEnd(int map_number, int round_number, int round_time, int reason, const char* winner, int team1_score, int team2_score) {
  std::vector<WebhookPlayerStatLine> empty;
  WebhookEmitRoundEndWithPlayerStats(map_number, round_number, round_time, reason, winner, team1_score, team2_score, empty);
}

void WebhookEmitRoundEndWithPlayerStats(int map_number,
                                        int round_number,
                                        int round_time,
                                        int reason,
                                        const char* winner,
                                        int team1_score,
                                        int team2_score,
                                        const std::vector<WebhookPlayerStatLine>& players) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;
  const std::string win = winner ? winner : "team1";
  std::string playersField;
  if (!players.empty()) {
    std::string arr = "[";
    bool first = true;
    for (const auto& p : players) {
      if (p.steamid64 == 0) continue;
      if (!first) arr += ",";
      first = false;
      const std::string team = TeamToString(p.team);
      arr += std::string("{") +
             "\"steamid\":\"" + JsonEscape(std::to_string(p.steamid64)) + "\"," +
             "\"name\":\"" + JsonEscape(p.name) + "\"," +
             "\"team\":\"" + JsonEscape(team) + "\"," +
             "\"kills\":" + std::to_string(p.kills) + "," +
             "\"deaths\":" + std::to_string(p.deaths) +
             "}";
    }
    arr += "]";
    if (!first) {
      playersField = std::string(",\"players\":") + arr;
    }
  }
  const std::string json =
      std::string("{") +
      "\"event\":\"round_end\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"map_number\":" + std::to_string(map_number) + "," +
      "\"round_number\":" + std::to_string(round_number) + "," +
      "\"round_time\":" + std::to_string(round_time) + "," +
      "\"reason\":" + std::to_string(reason) + "," +
      "\"winner\":\"" + JsonEscape(win) + "\"," +
      "\"team1_score\":" + std::to_string(team1_score) + "," +
      "\"team2_score\":" + std::to_string(team2_score) +
      playersField +
      "}";
  EnqueueLocked(st, json);
}

void WebhookEmitRoundEndMatchzy(int map_number,
                                int round_number,
                                int round_time,
                                int reason,
                                const char* winner,
                                int team1_score,
                                int team2_score,
                                const std::vector<WebhookPlayerStats>& team1_players,
                                const std::vector<WebhookPlayerStats>& team2_players) {
  WebhookStartSenderThread();
  auto& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (!st.match || st.baseUrl.empty()) return;
  const auto& m = *st.match;
  const std::string win = winner ? winner : "team1";

  auto buildTeam = [&](const std::vector<WebhookPlayerStats>& ps) -> std::string {
    std::string arr = "[";
    bool first = true;
    for (const auto& p : ps) {
      if (p.steamid64 == 0) continue;
      if (!first) arr += ",";
      first = false;
      arr += std::string("{") +
             "\"steamid\":\"" + JsonEscape(std::to_string(p.steamid64)) + "\"," +
             "\"name\":\"" + JsonEscape(p.name) + "\"," +
             "\"stats\":{" +
             "\"kills\":" + std::to_string(p.kills) + "," +
             "\"deaths\":" + std::to_string(p.deaths) + "," +
             "\"assists\":" + std::to_string(p.assists) + "," +
             "\"headshot_kills\":" + std::to_string(p.headshot_kills) + "," +
             "\"damage\":" + std::to_string(p.damage) + "," +
             "\"mvps\":" + std::to_string(p.mvps) + "," +
             "\"score\":" + std::to_string(p.score) +
             "}" +
             "}";
    }
    arr += "]";
    return arr;
  };

  const std::string t1 = buildTeam(team1_players);
  const std::string t2 = buildTeam(team2_players);

  const std::string json =
      std::string("{") +
      "\"event\":\"round_end\"," +
      "\"matchid\":" + std::to_string(m.matchid) + "," +
      "\"map_number\":" + std::to_string(map_number) + "," +
      "\"round_number\":" + std::to_string(round_number) + "," +
      "\"round_time\":" + std::to_string(round_time) + "," +
      "\"reason\":" + std::to_string(reason) + "," +
      "\"winner\":\"" + JsonEscape(win) + "\"," +
      "\"team1_score\":" + std::to_string(team1_score) + "," +
      "\"team2_score\":" + std::to_string(team2_score) + "," +
      "\"team1\":{" +
      "\"players\":" + t1 +
      "}," +
      "\"team2\":{" +
      "\"players\":" + t2 +
      "}" +
      "}";

  EnqueueLocked(st, json);
}

}  // namespace readyup

