// fleet.so inside the real plugin host (core/src/readyup/plugin_loader.cpp with stubbed core
// functions, like tools/plugin_host_test.cpp), against the mock platform:
//   1. no [fleet] url: loads, stays idle, `fleet status` says standalone, selftest INFO line
//   2. [fleet] url + enroll_code: `ru plugin reload fleet` -> enrolls, connects, selftest OK,
//      `fleet status` shows it online, the network thread stops on unload
//   build/plugins/fleet/fleet_host_test <fleet.so>
#include "readyup/plugin_loader.h"

#include "readyup/fleet_iface.h"
#include "readyup/plugin_api.h"

#include "mock_platform.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

static std::mutex g_logMu;
static std::vector<std::string> g_log;

static void Capture(const char* fmt, va_list ap) {
  char buf[4096];
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  std::string s(buf);
  while (!s.empty() && s.back() == '\n') s.pop_back();
  std::printf("  | %s\n", s.c_str());
  std::lock_guard<std::mutex> lk(g_logMu);
  g_log.push_back(s);
}

// ---- stubs for the core functions plugin_loader.cpp uses -----------------------------------
namespace readyup {
void Print(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  Capture(fmt, ap);
  va_end(ap);
}
void Debug(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  Capture(fmt, ap);
  va_end(ap);
}
void PrintLine(const char* msg) { Print("%s", msg); }
bool DebugEnabled() { return true; }
void SendToChat(const char*) {}
void SendRawToChat(const char*) {}
bool ClientPrintChat(int, const char*) { return true; }
bool SendToSlotChat(int, const char*) { return true; }
bool EnqueueServerCommand(const char*) { return true; }
std::optional<int> GameEventsSlotForSteam(unsigned long long) { return std::nullopt; }
std::string GetCsgoDirFromModuleDir() { return {}; }
static std::string g_moduleDir;
std::string GetThisModuleDir() { return g_moduleDir; }
bool IsCoreChatCommand(const std::string& t) { return t == ".ru"; }
}  // namespace readyup

namespace readyup::plugins::detail {
void FillEngineApi(ru_api* a) {
  a->for_each_player = [](ru_plugin*, ru_player_fn, void*) { return 0; };
}
}  // namespace readyup::plugins::detail

namespace rp = readyup::plugins;

static int g_failed = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failed;
}
static bool Logged(const std::string& needle) {
  std::lock_guard<std::mutex> lk(g_logMu);
  for (const auto& l : g_log) {
    if (l.find(needle) != std::string::npos) return true;
  }
  return false;
}
static bool AnyLogContains(const std::string& needle) { return Logged(needle); }
static void ClearLog() {
  std::lock_guard<std::mutex> lk(g_logMu);
  g_log.clear();
}
static bool FramesUntil(const std::function<bool()>& pred, int ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    rp::Frame(true);
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
  return pred();
}
static const rp::PluginSelftestCheck* FindCheck(const std::vector<rp::PluginSelftestCheck>& v, const std::string& name) {
  for (const auto& c : v) {
    if (c.plugin == "fleet" && c.name == name) return &c;
  }
  return nullptr;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <fleet.so>\n", argv[0]);
    return 2;
  }
  char tmpl[] = "/tmp/ru-fleet-host.XXXXXX";
  const char* dir = mkdtemp(tmpl);
  if (!dir) return 2;
  setenv("READYUP_PLUGINS_DIR", dir, 1);
  readyup::g_moduleDir = dir;
  const std::string so = std::string(dir) + "/fleet.so";
  if (std::system(("cp '" + std::string(argv[1]) + "' '" + so + "'").c_str()) != 0) return 2;

  std::puts("-- standalone: no [fleet] url");
  rp::Frame(false);
  Check(Logged("plugin: loaded fleet"), "fleet.so loaded");
  Check(Logged("standalone (no [fleet] url); idle"), "standalone, idle");
  Check(!Logged("error"), "no errors in standalone mode");
  Check(rp::TryDispatchConsole("fleet status"), "`fleet` console command is owned by the plugin");
  rp::Frame(false);
  Check(Logged("fleet: standalone (no [fleet] url)"), "`fleet status` reports standalone");
  auto checks = rp::RunPluginSelftests();
  const auto* link = FindCheck(checks, "link");
  Check(link && link->status == "INFO", "selftest: fleet link INFO (standalone)");
  struct stat st {};
  Check(stat((std::string(dir) + "/fleet/credentials.json").c_str(), &st) != 0, "standalone writes no credentials");
  {
    // What the core's /status does (status_feed.cpp): get_status 0 -> platform.mode "standalone".
    const auto* f = static_cast<const ru_fleet_v1*>(rp::CoreGetInterface(RU_FLEET_IFACE_NAME, RU_FLEET_IFACE_VERSION));
    ru_fleet_status fs{};
    fs.struct_size = sizeof(fs);
    Check(f && f->get_status && f->get_status(&fs) == 0, "standalone: readyup.fleet.v1 get_status returns 0");
  }

  std::puts("-- fleet mode: enroll + connect to the mock platform");
  mock::Platform platform;
  if (!platform.Start()) return 2;
  platform.heartbeatIntervalMs = 500;
  {
    FILE* cfg = std::fopen((std::string(dir) + "/readyup.cfg").c_str(), "w");
    std::fprintf(cfg, "debug=1\n[fleet]\nurl=%s\ninsecure_dev=1\nenroll_code=RUE-HOST-TEST-CODE\noffline_pause_minutes=3\n",
                 platform.BaseUrl().c_str());
    std::fclose(cfg);
  }
  ClearLog();
  rp::HandlePluginCommand({"reload", "fleet"}, false);
  rp::Frame(false);
  Check(Logged("plugin: reloaded fleet"), "reloaded with the [fleet] section");
  Check(FramesUntil([] { return AnyLogContains("fleet: online"); }, 8000), "enrolled and online");
  Check(platform.enrollments() == 1, "one enrollment request");
  Check(stat((std::string(dir) + "/fleet/credentials.json").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
        "credentials.json written with mode 0600");
  Check(!AnyLogContains(platform.token) && !AnyLogContains("HOST-TEST-CODE"), "no secrets in the log");
  Check(platform.WaitFor([&] { return platform.MessagesOfType("ping").size() >= 1; }, 3000), "pings flowing");
  const auto hellos = platform.MessagesOfType("hello");
  Check(!hellos.empty() && hellos[0].payload()->Get("versions")->Get("plugins")->Get("fleet") != nullptr,
        "hello carries the plugin versions");
  checks = rp::RunPluginSelftests();
  const auto* enrolled = FindCheck(checks, "enrolled");
  const auto* connected = FindCheck(checks, "connected");
  Check(enrolled && enrolled->status == "OK", "selftest: enrolled OK");
  Check(connected && connected->status == "OK", "selftest: connected OK");
  {
    const auto* f = static_cast<const ru_fleet_v1*>(rp::CoreGetInterface(RU_FLEET_IFACE_NAME, RU_FLEET_IFACE_VERSION));
    Check(f && f->struct_size >= offsetof(ru_fleet_v1, add_capability) + sizeof(f->add_capability),
          "readyup.fleet.v1 has the plugin members after get_status");
    ru_fleet_status fs{};
    fs.struct_size = sizeof(fs);
    Check(f && f->get_status(&fs) == 1 && fs.state == RU_FLEET_STATE_ONLINE && fs.link_state == RU_FLEET_LINK_ONLINE &&
              std::string(fs.server_id) == "srv_test_1" && fs.auto_pause_in_s == -1 && fs.sessions == 1,
          "get_status: online, server id, no auto-pause countdown");
    // A caller built against the first (shorter) ru_fleet_status gets only what fits.
    struct Old {
      uint32_t struct_size, state;
      uint64_t since_ms;
      uint32_t reconnects, spool_msgs;
      int32_t auto_pause_in_s, update_blocked;
      char server_id[64];
      uint32_t canary;
    } old{};
    old.struct_size = offsetof(Old, canary);
    old.canary = 0xDEADBEEFu;
    Check(f && f->get_status(reinterpret_cast<ru_fleet_status*>(&old)) == 1 && old.canary == 0xDEADBEEFu &&
              old.state == RU_FLEET_STATE_ONLINE && old.struct_size == offsetof(Old, canary),
          "get_status fills only what an older caller's struct has room for");
    Check(f && f->connection_state() == RU_FLEET_LINK_ONLINE, "connection_state() online");
    Check(f && f->send_event("event.test", "{\"n\":1}", 1, RU_FLEET_RELIABLE) == 1, "send_event queued");
    Check(platform.WaitFor([&] { return !platform.MessagesOfType("event.test").empty(); }, 2000),
          "send_event reached the platform with a seq");
    Check(f && f->publish_state("{\"match_id\":\"m9\"}", "busy") == 1 && f->publish_state("[1]", nullptr) == 0,
          "publish_state validates its JSON");
  }
  ClearLog();
  Check(rp::TryDispatchConsole("fleet status"), "fleet status dispatched");
  rp::Frame(false);
  Check(Logged("fleet: online for"), "`fleet status` shows online");
  Check(Logged("server srv_test_1"), "`fleet status` shows the server id");

  std::puts("-- reconnect command + platform restart");
  ClearLog();
  rp::TryDispatchConsole("fleet reconnect");
  Check(FramesUntil([&] { return platform.connections() >= 2 && AnyLogContains("fleet: online"); }, 5000),
        "`fleet reconnect` reconnects");

  std::puts("-- unload joins the network thread");
  rp::HandlePluginCommand({"unload", "fleet"}, false);
  rp::Frame(false);
  Check(Logged("plugin: unloaded fleet"), "unloaded");
  checks = rp::RunPluginSelftests();
  Check(FindCheck(checks, "link") == nullptr && FindCheck(checks, "connected") == nullptr,
        "selftest interface removed on unload");
  Check(platform.WaitFor([&] {
    const auto m = platform.Messages();
    return !m.empty();
  }, 100), "platform saw traffic");
  const int before = platform.connections();
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  Check(platform.connections() == before, "no reconnect after unload (thread joined)");

  platform.Stop();
  std::system(("rm -rf '" + std::string(dir) + "'").c_str());
  std::printf("%s\n", g_failed ? "fleet_host_test: FAIL" : "fleet_host_test: PASS");
  return g_failed ? 1 : 0;
}
