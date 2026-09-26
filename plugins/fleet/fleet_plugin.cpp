// readyup-fleet (fleet.so): the link between a Ready Up server and the Auto Tournament platform
// (docs/FLEET.md). Step 1 of the build order: enrollment, credentials, one outbound WebSocket with
// hello/welcome, seq/ack, ping, reconnect with backoff, resume from a disk spool, and the offline
// timer hook. Match control (assign/cmd/events) arrives in later steps through the
// "readyup.fleet.v1" interface (core/include/readyup/fleet_iface.h).
//
// Config: the [fleet] section of readyup.cfg (or csgo/cfg/ReadyUp/fleet.cfg):
//   url=https://tournament.example.com   platform base URL; unset = standalone (the plugin idles)
//   enroll_code=RUE-XXXX-XXXX-XXXX-XXXX  one-time code from the UI (Add server)
//   enroll_key=rfk_...                   fleet enrollment key (csm / containers), reusable
//   insecure_dev=0                       1 allows http:// / ws:// to loopback and RFC 1918 hosts
//   ca_file=                             extra CA bundle (private CA)
//   pin_sha256=                          optional SPKI pin (base64 sha256)
//   offline_pause_minutes=3              offline timer (D12); 0 disables it
//   spool_max_msgs=50000  spool_max_mb=64
//   enabled=1
//
// Files (csgo/readyup/plugins/fleet/): install_id, credentials.json (0600), spool/.
//
// Commands: `ru fleet status|enroll [url] <code|key>|reconnect` (console / RCON; also plain
// `fleet ...`), `.fleet status|reconnect` / `.ru fleet ...` (admins, chat).
#include "readyup/fleet_iface.h"
#include "readyup/plugin_api.h"
#include "readyup/plugin_needs_iface.h"
#include "readyup/selftest_iface.h"

#include "fleet_client.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifndef FLEET_VERSION
#define FLEET_VERSION "0.0.0-dev"
#endif

namespace {

using fleet::LinkState;

const ru_api* g_api = nullptr;

// Guards g_client / g_startError / g_configured for the any-thread entry points (interface,
// selftest). The client itself is thread-safe.
std::mutex g_mu;
std::shared_ptr<fleet::Client> g_client;
std::string g_startError;
bool g_configured = false;
std::atomic<uint64_t> g_instanceId{0};

struct Settings {
  bool enabled = true;
  std::string url, code, key, caFile, pin;
  bool insecureDev = false;
  int offlinePauseMinutes = 3;
  size_t spoolMaxMsgs = 50000;
  uint64_t spoolMaxBytes = 64ull << 20;
};
Settings g_set;
std::string g_dataDir;
fleet::HelloInfo g_hello;  // game thread
int64_t g_adminsRev = -1;  // game thread: set_admins_rev
std::vector<std::string> g_caps;

struct Handler {
  uint64_t id;
  std::string type;
  ru_fleet_msg_fn fn;
  void* user;
};
std::vector<Handler> g_handlers;  // game thread
uint64_t g_nextHandler = 1;
std::atomic<bool> g_drainPosted{false};

// Tick health and the offline timer (game thread).
double g_lastTickNow = 0.0, g_lastHousekeeping = 0.0;
std::vector<float> g_frameMs;
size_t g_frameIdx = 0;
int64_t g_offlineFiredFor = -1;

void Log(int level, const char* fmt, ...) RU_PRINTF(2, 3);
void Log(int level, const char* fmt, ...) {
  if (!g_api) return;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_api->log(g_api->self, level, fleet::Redact(buf).c_str());
}

std::shared_ptr<fleet::Client> Client() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_client;
}

std::string Trim(std::string s) {
  auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"'; };
  while (!s.empty() && ws(s.back())) s.pop_back();
  size_t i = 0;
  while (i < s.size() && ws(s[i])) ++i;
  return s.substr(i);
}

std::string Cfg(std::initializer_list<const char*> keys, const std::string& def = {}) {
  char buf[1024];
  for (const char* k : keys) {
    const int n = g_api->config_get(g_api->self, k, buf, sizeof(buf));
    if (n >= 0) return Trim(buf);
  }
  return def;
}

bool CfgBool(std::initializer_list<const char*> keys, bool def) {
  const std::string v = Cfg(keys);
  if (v.empty()) return def;
  const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(v[0])));
  return c == '1' || c == 'y' || c == 't' || c == 'o';
}

long CfgInt(std::initializer_list<const char*> keys, long def) {
  const std::string v = Cfg(keys);
  if (v.empty()) return def;
  char* end = nullptr;
  const long n = std::strtol(v.c_str(), &end, 10);
  return end && end != v.c_str() ? n : def;
}

void LoadSettings() {
  Settings s;
  s.enabled = CfgBool({"enabled", "fleet_enabled"}, true);
  s.url = Cfg({"url", "fleet_url"});
  s.code = Cfg({"enroll_code", "fleet_enroll_code", "code"});
  s.key = Cfg({"enroll_key", "fleet_enroll_key", "key"});
  s.insecureDev = CfgBool({"insecure_dev", "fleet_insecure_dev"}, false);
  s.caFile = Cfg({"ca_file", "fleet_ca_file"});
  s.pin = Cfg({"pin_sha256", "fleet_pin_sha256"});
  s.offlinePauseMinutes = static_cast<int>(CfgInt({"offline_pause_minutes", "fleet_offline_pause_minutes"}, 3));
  s.spoolMaxMsgs = static_cast<size_t>(std::max(100L, CfgInt({"spool_max_msgs"}, 50000)));
  s.spoolMaxBytes = static_cast<uint64_t>(std::max(1L, CfgInt({"spool_max_mb"}, 64))) << 20;
  g_set = s;
}

// ---- environment facts for hello -----------------------------------------------------------

std::string CsgoDir() {
  // data_dir = .../csgo/readyup/plugins/fleet
  std::string d = g_dataDir;
  for (int i = 0; i < 3; ++i) {
    const size_t p = d.find_last_of('/');
    if (p == std::string::npos) return {};
    d = d.substr(0, p);
  }
  return d;
}

void ReadSteamInf(int64_t* build, std::string* patch) {
  std::string text;
  if (!fleet::ReadFile(CsgoDir() + "/steam.inf", &text, 1 << 16)) return;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    std::string line = Trim(text.substr(pos, nl - pos));
    pos = nl + 1;
    if (line.rfind("ServerVersion=", 0) == 0) *build = std::atoll(line.c_str() + 14);
    else if (line.rfind("PatchVersion=", 0) == 0) *patch = line.substr(13);
  }
}

void ReadPorts(int* gamePort, int* tvPort) {
  *gamePort = 27015;
  std::string cmd;
  if (!fleet::ReadFile("/proc/self/cmdline", &cmd, 1 << 16)) return;
  std::vector<std::string> args;
  size_t s = 0;
  for (size_t i = 0; i <= cmd.size(); ++i) {
    if (i == cmd.size() || cmd[i] == '\0') {
      if (i > s) args.push_back(cmd.substr(s, i - s));
      s = i + 1;
    }
  }
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == "-port" || args[i] == "+hostport") *gamePort = std::atoi(args[i + 1].c_str());
    if (args[i] == "+tv_port" || args[i] == "-tv_port") *tvPort = std::atoi(args[i + 1].c_str());
  }
}

std::string BootId() {
  char buf[64] = {0};
  const int n = g_api->stash_get(g_api->self, "boot_id", buf, sizeof(buf) - 1);
  if (n > 0 && n < static_cast<int>(sizeof(buf))) return std::string(buf, static_cast<size_t>(n));
  const std::string id = fleet::NewUlid(fleet::NowMs());
  g_api->stash_put(g_api->self, "boot_id", id.data(), static_cast<uint32_t>(id.size()));
  return id;
}

int64_t ProcessStartMs() {
  char buf[32] = {0};
  const int n = g_api->stash_get(g_api->self, "started_ms", buf, sizeof(buf) - 1);
  if (n > 0) return std::atoll(buf);
  const std::string v = std::to_string(fleet::NowMs());
  g_api->stash_put(g_api->self, "started_ms", v.data(), static_cast<uint32_t>(v.size()));
  return fleet::NowMs();
}

// Plugins the core refused to load on this CS2 build (hello.plugins_disabled). Older cores do not
// provide the interface: nothing to report.
std::vector<std::pair<std::string, std::string>> PluginsDisabled() {
  std::vector<std::pair<std::string, std::string>> out;
  const auto* ni = static_cast<const ru_plugin_needs_iface_v1*>(
      g_api->get_interface(g_api->self, RU_PLUGIN_NEEDS_IFACE_NAME, RU_PLUGIN_NEEDS_IFACE_VERSION));
  if (!ni || ni->struct_size < sizeof(ru_plugin_needs_iface_v1) || !ni->disabled_json) return out;
  std::string buf(4096, '\0');
  const uint32_t n = ni->disabled_json(buf.data(), static_cast<uint32_t>(buf.size()));
  if (n >= buf.size()) {
    buf.assign(n + 1, '\0');
    ni->disabled_json(buf.data(), static_cast<uint32_t>(buf.size()));
  }
  buf.resize(std::min<size_t>(n, buf.size() - 1));
  fleet::json::Value v;
  if (!fleet::json::Parse(buf, &v) || !v.IsArr()) return out;
  for (const auto& e : v.a) {
    const auto* name = e.Get("name");
    const auto* reason = e.Get("reason");
    if (name && reason) out.emplace_back(name->AsStr(), reason->AsStr());
  }
  return out;
}

void BuildHello() {
  fleet::HelloInfo h;
  h.coreVersion = g_api->core_version ? g_api->core_version : "";
  char api[16];
  std::snprintf(api, sizeof(api), "%u.%u", RU_API_VERSION_MAJOR(g_api->api_version),
                RU_API_VERSION_MINOR(g_api->api_version));
  h.pluginApi = api;
  h.plugins = {{"fleet", FLEET_VERSION}};
  ReadSteamInf(&h.cs2Build, &h.cs2Patch);
  char host[256] = {0};
  if (gethostname(host, sizeof(host) - 1) == 0) h.hostname = host;
  ReadPorts(&h.gamePort, &h.tvPort);
  h.capabilities = g_caps;
  h.bootId = BootId();
  h.startedMs = ProcessStartMs();
  h.adminsRev = g_adminsRev;
  h.pluginsDisabled = PluginsDisabled();
  h.stateJson.clear();     // keep whatever publish_state set
  h.availability.clear();
  g_hello = h;
}

// ---- handlers ------------------------------------------------------------------------------

void UpdateHandledTypes() {
  std::set<std::string> types;
  bool all = false;
  for (const auto& h : g_handlers) {
    if (h.type == "*") all = true;
    else types.insert(h.type);
  }
  if (auto c = Client()) c->SetHandledTypes(std::move(types), all);
}

void Dispatch(const std::string& type, const ru_fleet_msg& msg) {
  std::vector<uint64_t> ids;
  for (const auto& h : g_handlers) {
    if (h.type == "*" || h.type == type) ids.push_back(h.id);
  }
  for (uint64_t id : ids) {
    auto it = std::find_if(g_handlers.begin(), g_handlers.end(), [id](const Handler& h) { return h.id == id; });
    if (it == g_handlers.end()) continue;  // unregistered by an earlier handler
    const Handler h = *it;
    try {
      h.fn(h.user, &msg);
    } catch (...) {
      Log(RU_LOG_ERROR, "fleet: a handler for %s threw", type.c_str());
    }
  }
}

void DispatchLocal(const std::string& type, const std::string& payload) {
  ru_fleet_msg m{};
  m.struct_size = sizeof(m);
  m.type = type.c_str();
  m.id = "";
  m.ts = fleet::NowMs();
  m.ref = "";
  m.payload_json = payload.c_str();
  Dispatch(type, m);
}

void DrainTask(void*) {
  g_drainPosted = false;
  auto c = Client();
  if (!c) return;
  for (auto& in : c->TakeInbound()) {
    ru_fleet_msg m{};
    m.struct_size = sizeof(m);
    m.type = in.env.type.c_str();
    m.id = in.env.id.c_str();
    m.seq = in.env.seq;
    m.epoch = in.env.epoch;
    m.ts = in.env.ts;
    m.ref = in.env.ref.c_str();
    m.payload_json = in.payloadJson.c_str();
    Dispatch(in.env.type, m);
    if (in.reliable) c->MarkProcessed(in.env.seq);
  }
}

// ---- the readyup.fleet.v1 interface --------------------------------------------------------

uint32_t ToLinkState(LinkState s) {
  switch (s) {
    case LinkState::Standalone: return RU_FLEET_LINK_STANDALONE;
    case LinkState::Unenrolled: return RU_FLEET_LINK_UNENROLLED;
    case LinkState::Enrolling: return RU_FLEET_LINK_ENROLLING;
    case LinkState::Connecting: return RU_FLEET_LINK_CONNECTING;
    case LinkState::Online: return RU_FLEET_LINK_ONLINE;
    case LinkState::Offline: return RU_FLEET_LINK_OFFLINE;
    case LinkState::Rejected: return RU_FLEET_LINK_REJECTED;
  }
  return RU_FLEET_LINK_STANDALONE;
}

// The coarse state the core's /status reports (offline / online / enrolling / rejected).
uint32_t ToCoarseState(LinkState s) {
  switch (s) {
    case LinkState::Online: return RU_FLEET_STATE_ONLINE;
    case LinkState::Unenrolled:
    case LinkState::Enrolling: return RU_FLEET_STATE_ENROLLING;
    case LinkState::Rejected: return RU_FLEET_STATE_REJECTED;
    default: return RU_FLEET_STATE_OFFLINE;
  }
}

int64_t AutoPauseInMs(const fleet::ClientStatus& st) {
  if (g_set.offlinePauseMinutes <= 0 || !st.enrolled || st.state == LinkState::Online || st.offlineSinceMs <= 0) {
    return -1;
  }
  const int64_t left = static_cast<int64_t>(g_set.offlinePauseMinutes) * 60000 - (fleet::NowMs() - st.offlineSinceMs);
  return std::max<int64_t>(0, left);
}

uint64_t IfInstanceId() { return g_instanceId.load(); }

int IfConnectionState() {
  auto c = Client();
  return static_cast<int>(c ? ToLinkState(c->Status().state) : static_cast<uint32_t>(RU_FLEET_LINK_STANDALONE));
}

void CopyStr(char* dst, size_t cap, const std::string& s) {
  std::snprintf(dst, cap, "%s", s.c_str());
}

// Any thread. 0 while standalone, so the core's /status says mode "standalone".
int IfGetStatus(ru_fleet_status* out) {
  if (!out || out->struct_size < offsetof(ru_fleet_status, state) + sizeof(out->state)) return 0;
  auto c = Client();
  if (!c) return 0;
  const fleet::ClientStatus st = c->Status();
  ru_fleet_status full{};
  full.state = ToCoarseState(st.state);
  full.since_ms = static_cast<uint64_t>(st.sinceMs);
  full.reconnects = st.connectAttempts > 0 ? st.connectAttempts - 1 : 0;
  full.spool_msgs = st.spoolMsgs;
  const int64_t ap = AutoPauseInMs(st);
  full.auto_pause_in_s = ap < 0 ? -1 : static_cast<int32_t>((ap + 999) / 1000);
  full.update_blocked = 0;  // nothing the platform waits for yet (demo uploads come in step 4)
  CopyStr(full.server_id, sizeof(full.server_id), st.serverId);
  full.link_state = ToLinkState(st.state);
  full.sessions = st.sessions;
  full.offline_ms = st.state == LinkState::Online || st.offlineSinceMs <= 0 ? 0 : fleet::NowMs() - st.offlineSinceMs;
  full.tx_seq = st.txSeq;
  full.acked_seq = st.ackedSeq;
  full.rx_seq = st.rxSeq;
  full.spool_bytes = st.spoolBytes;
  CopyStr(full.session_id, sizeof(full.session_id), st.sessionId);
  CopyStr(full.last_error, sizeof(full.last_error), st.lastError);
  // Fill only what the caller's (possibly older, smaller) struct has room for.
  const uint32_t callerSize = out->struct_size;
  const size_t n = std::min<size_t>(callerSize, sizeof(full));
  std::memcpy(reinterpret_cast<char*>(out) + sizeof(uint32_t), reinterpret_cast<const char*>(&full) + sizeof(uint32_t),
              n - sizeof(uint32_t));
  out->struct_size = callerSize;
  return 1;
}

int IfSendEvent(const char* type, const char* payload, int64_t epoch, uint32_t flags) {
  auto c = Client();
  if (!c || !type) return 0;
  std::string err;
  if (!c->Send(type, payload ? payload : "{}", epoch, (flags & RU_FLEET_RELIABLE) != 0, &err)) {
    Log(RU_LOG_WARN, "fleet: send_event(%s) refused: %s", type, err.c_str());
    return 0;
  }
  return 1;
}

uint64_t IfRegisterHandler(const char* type, ru_fleet_msg_fn fn, void* user) {
  if (!type || !fn) return 0;
  const std::string t = type;
  if (t != "*" && !fleet::IsValidType(t)) return 0;
  const uint64_t id = g_nextHandler++;
  g_handlers.push_back(Handler{id, t, fn, user});
  UpdateHandledTypes();
  return id;
}

int IfUnregisterHandler(uint64_t id) {
  const auto it = std::find_if(g_handlers.begin(), g_handlers.end(), [id](const Handler& h) { return h.id == id; });
  if (it == g_handlers.end()) return 0;
  g_handlers.erase(it);
  UpdateHandledTypes();
  return 1;
}

int IfPublishState(const char* stateJson, const char* availability) {
  std::string state = stateJson && *stateJson ? stateJson : "null";
  fleet::json::Value v;
  if (!fleet::json::Parse(state, &v) || !(v.IsNull() || v.IsObj())) return 0;
  std::string avail = availability ? availability : "";
  if (!avail.empty() && avail != "available" && avail != "busy" && avail != "draining" && avail != "error") return 0;
  if (auto c = Client()) c->SetState(state, avail);
  return 1;
}

int IfSendReply(const char* type, const char* payload, int64_t epoch, uint32_t flags, const char* ref) {
  auto c = Client();
  if (!c || !type) return 0;
  std::string err;
  if (!c->Send(type, payload ? payload : "{}", epoch, (flags & RU_FLEET_RELIABLE) != 0, &err, ref ? ref : "")) {
    Log(RU_LOG_WARN, "fleet: send_reply(%s) refused: %s", type, err.c_str());
    return 0;
  }
  return 1;
}

int IfSendSnapshot(const char* reason, const char* extra) {
  auto c = Client();
  if (!c) return 0;
  const std::string r = reason && *reason ? reason : "request";
  if (r != "hello" && r != "request" && r != "reset" && r != "periodic" && r != "assign" && r != "restored") return 0;
  return c->SendSnapshot(r, extra ? extra : "") ? 1 : 0;
}

int IfAddCapability(const char* cap) {
  if (!cap || !*cap || std::strlen(cap) > 64) return 0;
  if (std::find(g_caps.begin(), g_caps.end(), cap) == g_caps.end()) g_caps.push_back(cap);
  g_hello.capabilities = g_caps;
  if (auto c = Client()) c->SetHelloInfo(g_hello);
  return 1;
}

int IfSetAdminsRev(int64_t rev) {
  if (rev < -1) return 0;
  g_adminsRev = rev;
  g_hello.adminsRev = rev;
  if (auto c = Client()) c->SetHelloInfo(g_hello);
  return 1;
}

const ru_fleet_v1 g_iface = {
    sizeof(ru_fleet_v1), &IfGetStatus,       &IfInstanceId,        &IfConnectionState, &IfSendEvent,
    &IfRegisterHandler,  &IfUnregisterHandler, &IfPublishState,    &IfAddCapability,  &IfSendReply,
    &IfSendSnapshot,     &IfSetAdminsRev,
};

// ---- selftest (any thread; see selftest_iface.h) -------------------------------------------

std::string Ago(int64_t sinceMs) {
  if (sinceMs <= 0) return "?";
  const int64_t s = (fleet::NowMs() - sinceMs) / 1000;
  char buf[32];
  if (s < 120) std::snprintf(buf, sizeof(buf), "%lld s", static_cast<long long>(s));
  else if (s < 7200) std::snprintf(buf, sizeof(buf), "%lld min", static_cast<long long>(s / 60));
  else std::snprintf(buf, sizeof(buf), "%lld h", static_cast<long long>(s / 3600));
  return buf;
}

void SelftestRun(ru_selftest_add_fn add, void* ctx) {
  std::shared_ptr<fleet::Client> c;
  std::string startError;
  bool configured;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    c = g_client;
    startError = g_startError;
    configured = g_configured;
  }
  if (!startError.empty()) {
    add(ctx, "FAIL", "link", ("cannot start: " + startError).c_str());
    return;
  }
  if (!c) {
    add(ctx, "INFO", "link",
        configured ? "disabled (enabled=0)" : "standalone: no [fleet] url, nothing to connect to");
    return;
  }
  const fleet::ClientStatus st = c->Status();
  const std::string err = st.lastError.empty() ? "" : " (last error: " + st.lastError + ")";
  if (st.enrolled) {
    add(ctx, "OK", "enrolled", ("server " + st.serverId + " at " + st.url + ", install_id " + st.installId).c_str());
  } else if (st.state == LinkState::Rejected) {
    add(ctx, "FAIL", "enrolled", ("enrollment refused" + err).c_str());
  } else {
    add(ctx, "PEND", "enrolled",
        (std::string(fleet::LinkStateName(st.state)) + ": no credentials yet; `ru fleet enroll <code>`" + err).c_str());
  }
  if (st.enrolled) {
    if (st.state == LinkState::Online) {
      add(ctx, "OK", "connected",
          ("online " + Ago(st.sinceMs) + ", session " + st.sessionId + ", rtt " +
           (st.rttMs >= 0 ? std::to_string(st.rttMs) + " ms" : std::string("?")))
              .c_str());
    } else if (st.state == LinkState::Rejected) {
      add(ctx, "FAIL", "connected", ("credentials rejected; re-enroll" + err).c_str());
    } else {
      add(ctx, "PEND", "connected",
          (std::string(fleet::LinkStateName(st.state)) + " for " + Ago(st.offlineSinceMs) + err).c_str());
    }
  }
  char spool[160];
  std::snprintf(spool, sizeof(spool), "%u unacked (%llu bytes), tx seq %lld, acked %lld, rx seq %lld%s", st.spoolMsgs,
                static_cast<unsigned long long>(st.spoolBytes), static_cast<long long>(st.txSeq),
                static_cast<long long>(st.ackedSeq), static_cast<long long>(st.rxSeq),
                st.spoolDropped ? " (messages were dropped: spool full)" : "");
  add(ctx, st.spoolDropped ? "WARN" : "INFO", "spool", spool);
}

const ru_selftest_iface_v1 g_selftest = {sizeof(ru_selftest_iface_v1), &SelftestRun};

// ---- starting the client -------------------------------------------------------------------

bool StartClient(const std::string& url) {
  fleet::ClientConfig cfg;
  cfg.url = url;
  cfg.enrollCode = g_set.code;
  cfg.enrollKey = g_set.key;
  cfg.insecureDev = g_set.insecureDev;
  cfg.caFile = g_set.caFile;
  cfg.pinSha256 = g_set.pin;
  cfg.dataDir = g_dataDir;
  cfg.spool.maxMsgs = g_set.spoolMaxMsgs;
  cfg.spool.maxBytes = g_set.spoolMaxBytes;
  cfg.userAgent = std::string("ReadyUp-fleet/") + FLEET_VERSION;
  const ru_api* api = g_api;
  cfg.log = [api](int level, const std::string& msg) {
    if (level == 3 && !api->debug_enabled(api->self)) return;  // debug_enabled is any-thread
    api->log(api->self, level == 3 ? RU_LOG_DEBUG : level, msg.c_str());
  };
  auto c = std::make_shared<fleet::Client>(cfg);
  c->SetHelloInfo(g_hello);
  c->SetWake([api] {
    if (!g_drainPosted.exchange(true)) api->post_to_game_thread(api->self, &DrainTask, nullptr);
  });
  std::string err;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_client = c;
    g_configured = true;
    g_startError.clear();
  }
  UpdateHandledTypes();
  if (!c->Start(&err)) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_client.reset();
    g_startError = err;
    Log(RU_LOG_ERROR, "fleet: cannot start: %s", err.c_str());
    return false;
  }
  return true;
}

// ---- commands ------------------------------------------------------------------------------

void PrintStatus(void (*out)(void*, const std::string&), void* user, bool brief) {
  auto c = Client();
  std::string startError;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    startError = g_startError;
  }
  if (!c) {
    if (!startError.empty()) out(user, "fleet: cannot start: " + startError);
    else if (!g_set.enabled) out(user, "fleet: disabled (enabled=0 in [fleet])");
    else out(user, "fleet: standalone (no [fleet] url). Enroll with: ru fleet enroll <url> <code|key>");
    return;
  }
  const fleet::ClientStatus st = c->Status();
  char buf[512];
  std::snprintf(buf, sizeof(buf), "fleet: %s for %s; platform %s; server %s; install_id %s",
                fleet::LinkStateName(st.state), Ago(st.sinceMs).c_str(), st.url.empty() ? "(none)" : st.url.c_str(),
                st.serverId.empty() ? "(not enrolled)" : st.serverId.c_str(), st.installId.c_str());
  out(user, buf);
  if (!st.lastError.empty()) out(user, "fleet: last error: " + st.lastError);
  if (brief) return;
  std::snprintf(buf, sizeof(buf),
                "fleet: session %s (resume %s), heartbeat %lld ms, rtt %s, sessions %u (%u reconnects), frames "
                "in/out %llu/%llu",
                st.sessionId.empty() ? "-" : st.sessionId.c_str(), st.resume.empty() ? "-" : st.resume.c_str(),
                static_cast<long long>(st.heartbeatMs),
                st.rttMs >= 0 ? (std::to_string(st.rttMs) + " ms").c_str() : "?", st.sessions,
                st.sessions > 0 ? st.sessions - 1 : 0, static_cast<unsigned long long>(st.framesIn),
                static_cast<unsigned long long>(st.framesOut));
  out(user, buf);
  std::snprintf(buf, sizeof(buf), "fleet: stream %s, tx seq %lld (acked %lld), rx seq %lld, spool %u msgs / %llu bytes%s",
                st.streamId.c_str(), static_cast<long long>(st.txSeq), static_cast<long long>(st.ackedSeq),
                static_cast<long long>(st.rxSeq), st.spoolMsgs, static_cast<unsigned long long>(st.spoolBytes),
                st.spoolDropped ? (", " + std::to_string(st.spoolDropped) + " dropped").c_str() : "");
  out(user, buf);
  if (st.state != LinkState::Online && st.nextAttemptMs > 0) {
    const int64_t in = std::max<int64_t>(0, st.nextAttemptMs - fleet::NowMs());
    std::snprintf(buf, sizeof(buf), "fleet: next attempt in %lld.%lld s (`ru fleet reconnect` to go now)",
                  static_cast<long long>(in / 1000), static_cast<long long>((in % 1000) / 100));
    out(user, buf);
  }
  const int64_t ap = AutoPauseInMs(st);
  if (g_set.offlinePauseMinutes <= 0) {
    out(user, "fleet: offline timer off (offline_pause_minutes=0)");
  } else if (ap >= 0) {
    std::snprintf(buf, sizeof(buf), "fleet: offline timer: fires in %lld s (offline_pause_minutes=%d)",
                  static_cast<long long>(ap / 1000), g_set.offlinePauseMinutes);
    out(user, buf);
  }
  std::string handlers;
  for (const auto& h : g_handlers) handlers += (handlers.empty() ? "" : ", ") + h.type;
  out(user, "fleet: handlers: " + (handlers.empty() ? std::string("(none)") : handlers) +
                "; capabilities: " + std::to_string(g_caps.size()));
}

void ConsoleOut(void*, const std::string& line) { g_api->log(g_api->self, RU_LOG_INFO, line.c_str()); }

void ChatOut(void* user, const std::string& line) {
  const int slot = *static_cast<int*>(user);
  if (slot < 0 || !g_api->chat_to_slot(g_api->self, slot, line.c_str())) g_api->chat_all(g_api->self, line.c_str(), 0);
}

void Enroll(const std::vector<std::string>& args) {
  // fleet enroll [url] <code|key>
  std::string url, secret;
  if (args.size() == 1) {
    secret = args[0];
  } else if (args.size() == 2) {
    url = args[0];
    secret = args[1];
  } else {
    Log(RU_LOG_INFO, "usage: ru fleet enroll [url] <code|key>");
    return;
  }
  if (url.empty()) {
    auto c = Client();
    url = c ? c->Status().url : g_set.url;
  }
  if (url.empty()) {
    Log(RU_LOG_WARN, "fleet: no platform url: set url in the [fleet] section or use `ru fleet enroll <url> <code>`");
    return;
  }
  if (const std::string why = fleet::CheckUrlAllowed(url, g_set.insecureDev); !why.empty()) {
    Log(RU_LOG_WARN, "fleet: %s", why.c_str());
    return;
  }
  auto c = Client();
  if (!c) {
    if (!StartClient(url)) return;
    c = Client();
  }
  Log(RU_LOG_INFO, "fleet: enrolling at %s ...", url.c_str());
  c->RequestEnroll(url, secret);
}

void OnConsole(void*, const ru_command_ctx* ctx) {
  try {
    std::vector<std::string> args;
    for (int i = 1; i < ctx->argc; ++i) args.emplace_back(ctx->argv[i]);
    const std::string sub = args.empty() ? "status" : args[0];
    if (sub == "status") {
      PrintStatus(&ConsoleOut, nullptr, false);
    } else if (sub == "enroll") {
      Enroll(std::vector<std::string>(args.begin() + 1, args.end()));
    } else if (sub == "reconnect") {
      if (auto c = Client()) {
        c->RequestReconnect();
        Log(RU_LOG_INFO, "fleet: reconnecting");
      } else {
        Log(RU_LOG_INFO, "fleet: standalone, nothing to reconnect");
      }
    } else {
      Log(RU_LOG_INFO, "usage: ru fleet status | enroll [url] <code|key> | reconnect");
    }
  } catch (const std::exception& e) {
    Log(RU_LOG_ERROR, "fleet: command failed: %s", e.what());
  }
}

void OnChat(void*, const ru_command_ctx* ctx) {
  try {
    int slot = ctx->slot;
    if (!g_api->is_admin(g_api->self, ctx->steamid64)) {
      ChatOut(&slot, "Ready Up: not authorized");
      return;
    }
    const std::string sub = ctx->argc > 1 ? ctx->argv[1] : "status";
    if (sub == "status") {
      PrintStatus(&ChatOut, &slot, true);
    } else if (sub == "reconnect") {
      if (auto c = Client()) c->RequestReconnect();
      ChatOut(&slot, "fleet: reconnecting");
    } else if (sub == "enroll") {
      ChatOut(&slot, "fleet: enroll from the server console (ru fleet enroll <code>), not from chat");
    } else {
      ChatOut(&slot, "fleet: .fleet status | reconnect");
    }
  } catch (...) {
  }
}

// ---- tick: health + offline timer ----------------------------------------------------------

int CountPlayers() {
  int n = 0;
  g_api->for_each_player(
      g_api->self,
      [](void* u, const ru_player* p) {
        if (p->connected && !p->is_bot) ++*static_cast<int*>(u);
        return 1;
      },
      &n);
  return n;
}

void OnTick(void*, const ru_tick_info* t) {
  try {
    if (g_lastTickNow > 0.0) {
      const double ms = (t->now - g_lastTickNow) * 1000.0;
      if (g_frameMs.size() < 512) g_frameMs.push_back(static_cast<float>(ms));
      else g_frameMs[g_frameIdx++ % 512] = static_cast<float>(ms);
    }
    g_lastTickNow = t->now;
    if (t->now - g_lastHousekeeping < 1.0) return;
    g_lastHousekeeping = t->now;
    auto c = Client();
    if (!c) return;
    double p99 = 0.0;
    if (!g_frameMs.empty()) {
      std::vector<float> v = g_frameMs;
      const size_t k = std::min(v.size() - 1, (v.size() * 99) / 100);
      std::nth_element(v.begin(), v.begin() + static_cast<long>(k), v.end());
      p99 = v[k];
    }
    c->UpdateHealth(CountPlayers(), p99);

    // Offline timer (D12). The auto-pause itself lands with the match plugin; this fires the
    // hook once per offline period.
    const fleet::ClientStatus st = c->Status();
    const int64_t left = AutoPauseInMs(st);
    if (left == 0 && g_offlineFiredFor != st.offlineSinceMs) {
      g_offlineFiredFor = st.offlineSinceMs;
      const int64_t offS = (fleet::NowMs() - st.offlineSinceMs) / 1000;
      Log(RU_LOG_WARN, "fleet: offline for %lld s (offline_pause_minutes=%d): offline timer fired (%zu handler(s) of "
                       "local.offline_timeout notified; match.so auto-pauses a live assigned match)",
          static_cast<long long>(offS), g_set.offlinePauseMinutes,
          static_cast<size_t>(std::count_if(g_handlers.begin(), g_handlers.end(), [](const Handler& h) {
            return h.type == "local.offline_timeout" || h.type == "*";
          })));
      char payload[96];
      std::snprintf(payload, sizeof(payload), "{\"offline_s\":%lld,\"threshold_s\":%d}", static_cast<long long>(offS),
                    g_set.offlinePauseMinutes * 60);
      DispatchLocal("local.offline_timeout", payload);
    }
  } catch (...) {
  }
}

const ru_plugin_info kInfo = {sizeof(ru_plugin_info), READYUP_PLUGIN_API_VERSION, "fleet", FLEET_VERSION,
                              "Ready Up", "Auto Tournament fleet link (enrollment, WebSocket, spool)"};

}  // namespace

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) { return &kInfo; }

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  (void)core_api_version;
  try {
    g_api = api;
    uint64_t iid = 0;
    fleet::RandomBytes(&iid, sizeof(iid));
    g_instanceId = iid ? iid : 1;
    g_handlers.clear();
    g_caps.clear();
    g_frameMs.clear();
    g_lastTickNow = g_lastHousekeeping = 0.0;
    g_offlineFiredFor = -1;
    g_dataDir = api->data_dir(api->self) ? api->data_dir(api->self) : "";
    while (g_dataDir.size() > 1 && g_dataDir.back() == '/') g_dataDir.pop_back();
    LoadSettings();
    BuildHello();

    api->register_console_command(api->self, "fleet", &OnConsole, nullptr);
    api->register_chat_command(api->self, ".fleet", &OnChat, nullptr);
    api->on_tick(api->self, &OnTick, nullptr);
    api->provide_interface(api->self, RU_FLEET_IFACE_NAME, RU_FLEET_IFACE_VERSION,
                           const_cast<ru_fleet_v1*>(&g_iface));
    api->provide_interface(api->self, RU_SELFTEST_IFACE_PREFIX "fleet", RU_SELFTEST_IFACE_VERSION,
                           const_cast<ru_selftest_iface_v1*>(&g_selftest));

    const bool haveCreds = fleet::FileExists(g_dataDir + "/credentials.json");
    if (!g_set.enabled) {
      std::lock_guard<std::mutex> lk(g_mu);
      g_configured = true;
      Log(RU_LOG_INFO, "fleet " FLEET_VERSION ": disabled (enabled=0)");
    } else if (g_set.url.empty() && !haveCreds) {
      Log(RU_LOG_INFO, "fleet " FLEET_VERSION ": standalone (no [fleet] url); idle");
    } else {
      Log(RU_LOG_INFO, "fleet " FLEET_VERSION ": platform %s, %s", g_set.url.empty() ? "(from credentials.json)" : g_set.url.c_str(),
          haveCreds ? "enrolled" : (!g_set.key.empty() ? "enrolling with the fleet key"
                                                       : !g_set.code.empty() ? "enrolling with the one-time code"
                                                                             : "not enrolled (ru fleet enroll <code>)"));
      StartClient(g_set.url);  // failure is logged and shown by status/selftest; the plugin stays loaded
    }
    return 0;
  } catch (const std::exception& e) {
    if (api) api->log(api->self, RU_LOG_ERROR, (std::string("fleet: load failed: ") + e.what()).c_str());
    return 1;
  } catch (...) {
    return 1;
  }
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  try {
    std::shared_ptr<fleet::Client> c;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      c.swap(g_client);
      g_configured = false;
      g_startError.clear();
    }
    if (c) c->Stop();  // joins the network thread (closes the socket with 1000)
    c.reset();
    g_handlers.clear();
    g_caps.clear();
  } catch (...) {
  }
  g_api = nullptr;
}

}  // extern "C"
