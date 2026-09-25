#include "readyup/status_feed.h"

#include "readyup/config.h"
#include "readyup/cs2_version.h"
#include "readyup/disabled.h"
#include "readyup/fleet_iface.h"
#include "readyup/logging.h"
#include "readyup/match_iface.h"
#include "readyup/minijson.h"
#include "readyup/path.h"
#include "readyup/plugin_api.h"
#include "readyup/plugin_loader.h"
#include "readyup/slot_registry.h"
#include "readyup/status_http.h"
#include "readyup/status_server.h"
#include "readyup/status_snapshot.h"
#include "readyup/version.h"

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

// Game thread only.
Clock::time_point g_lastBuild{};
Clock::time_point g_firstSimTick{};
bool g_sawSimTick = false, g_autoSelftestDone = false;
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

// The match plugin's part of /status (readyup.match.v1, core/include/readyup/match_iface.h).
// False when no match plugin provides it (not installed, unloaded, being reloaded).
bool MatchStatus(Json* summary, Json* state, bool* updateSafe) {
  const auto* iface = static_cast<const ru_match_v1*>(plugins::CoreGetInterface(RU_MATCH_IFACE_NAME, RU_MATCH_IFACE_VERSION));
  const size_t need = offsetof(ru_match_v1, get_status) + sizeof(iface->get_status);
  if (!iface || iface->struct_size < need || !iface->get_status) return false;
  ru_match_status st{};
  st.struct_size = sizeof(st);
  st.update_safe = 1;
  if (iface->get_status(&st) != 1) return false;
  std::string err;
  if (!st.summary_json || !Json::Parse(st.summary_json, summary, &err) || !summary->IsObject()) {
    Debug("status: match plugin summary unusable (%s)\n", err.c_str());
    return false;
  }
  if (st.state_json && *st.state_json) {
    if (!Json::Parse(st.state_json, state, &err) || !state->IsObject()) {
      Debug("status: match plugin state unusable (%s)\n", err.c_str());
      *state = Json();
    }
  }
  *updateSafe = st.update_safe != 0;
  return true;
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

  Json summary, state;
  bool safe = true;
  if (MatchStatus(&summary, &state, &safe)) {
    summary["match_plugin"] = "loaded";
    if (state.IsObject()) {
      // MatchState (docs/FLEET.md §9.1) carries the fleet server id right after match_id.
      Json st = Json::Object();
      for (const auto& kv : state.Members()) {
        st[kv.first] = kv.second;
        if (kv.first == "match_id" && !in->server_id.empty()) st["server_id"] = in->server_id;
      }
      in->state = std::move(st);
    }
  } else {
    // Standalone core: no match flow, nothing that an update could interrupt.
    summary = Json::Object();
    summary["mode"] = "idle";
    summary["ru_mode"] = "none";
    summary["phase"] = "idle";
    summary["map"] = plugins::CurrentMap();
    Json players = Json::Object();
    players["connected"] = static_cast<int>(ListHumans().size());
    players["expected"] = 0;
    summary["players"] = std::move(players);
    summary["match_plugin"] = "none";
    summary["note"] = "no match plugin";
    safe = true;
  }
  if (fleetBlocksUpdate) safe = false;
  in->update_safe = safe;
  in->summary = std::move(summary);
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
