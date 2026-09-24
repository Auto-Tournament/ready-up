// Offline test for the plugin host (src/readyup/plugin_loader.cpp) - no CS2 server needed.
//
// Links the real loader against stubs of the few core functions it calls, then drives it
// the way the GameFrame hook does: load from a plugins dir, dispatch a chat command, a
// console command and lifecycle events, tick, then hot-swap hello.so for a rebuilt copy
// and `reload` it, checking that the new code runs and the old image was really unmapped.
//
//   build/readyup_plugin_host_test <hello.so v1> <hello.so v2>
//
// Run by `ctest` (see CMakeLists.txt).
#include "readyup/plugin_loader.h"

#include "readyup/plugin_api.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

static std::vector<std::string> g_log;   // everything the core would print to the console
static std::vector<std::string> g_chat;  // chat_all / chat_to_slot output

static void Capture(const char* fmt, va_list ap) {
  char buf[2048];
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  std::string s(buf);
  while (!s.empty() && s.back() == '\n') s.pop_back();
  std::printf("  | %s\n", s.c_str());
  g_log.push_back(s);
}

// ---- stubs for the core functions plugin_loader.cpp uses ---------------------------
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
bool DebugEnabled() { return false; }
void SendToChat(const char* msg) { g_chat.push_back(std::string("[all] ") + msg); }
void SendRawToChat(const char* msg) { g_chat.push_back(std::string("[raw] ") + msg); }
bool ClientPrintChat(int slot, const char* msg) {
  g_chat.push_back("[slot " + std::to_string(slot) + "] " + msg);
  return true;
}
bool EnqueueServerCommand(const char*) { return true; }
std::optional<int> GameEventsSlotForSteam(unsigned long long steamid64) {
  if (steamid64 == 76561198000000001ull) return 4;
  return std::nullopt;
}
std::string GetCsgoDirFromModuleDir() { return {}; }
bool IsCoreChatCommand(const std::string& t) { return t == ".ru" || t == ".r" || t == ".ready"; }
}  // namespace readyup

// ---- helpers -----------------------------------------------------------------------
static int g_failed = 0;

static bool Logged(const std::string& needle) {
  for (const auto& l : g_log) {
    if (l.find(needle) != std::string::npos) return true;
  }
  return false;
}
static bool Chatted(const std::string& needle) {
  for (const auto& l : g_chat) {
    if (l.find(needle) != std::string::npos) return true;
  }
  return false;
}
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failed;
}
static bool CopyFile(const std::string& from, const std::string& to) {
  // Write to a temp name and rename into place, like scripts/dev-deploy.sh does.
  FILE* in = std::fopen(from.c_str(), "rb");
  if (!in) return false;
  const std::string tmp = to + ".tmp";
  FILE* out = std::fopen(tmp.c_str(), "wb");
  if (!out) {
    std::fclose(in);
    return false;
  }
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) std::fwrite(buf, 1, n, out);
  std::fclose(in);
  std::fclose(out);
  return std::rename(tmp.c_str(), to.c_str()) == 0;
}

namespace rp = readyup::plugins;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <hello.so v1> <hello.so v2>\n", argv[0]);
    return 2;
  }
  char tmpl[] = "/tmp/ru-plugin-test.XXXXXX";
  const char* dir = mkdtemp(tmpl);
  if (!dir) return 2;
  setenv("READYUP_PLUGINS_DIR", dir, 1);
  const std::string so = std::string(dir) + "/hello.so";
  if (!CopyFile(argv[1], so)) return 2;

  std::puts("-- first frame loads everything in the plugins dir");
  rp::Frame(false);
  Check(Logged("plugin: loaded hello 1.0.0"), "hello 1.0.0 loaded on first frame");

  std::puts("-- chat command");
  Check(!rp::TryDispatchChat(76561198000000001ull, "alice", ".r"), "core command .r is not taken by plugins");
  Check(rp::TryDispatchChat(76561198000000001ull, "alice", ".hello there"), ".hello is owned by the plugin");
  Check(!rp::TryDispatchChat(0, "Console", ".hello"), "console sender never triggers chat commands");
  Check(g_chat.empty(), "callback deferred to the next frame");
  rp::Frame(true);
  Check(Chatted("[slot 4] hello, alice!"), ".hello replied to the sender's slot");

  std::puts("-- lifecycle events + ticks");
  rp::LifecycleEvent map;
  map.type = RU_EVENT_MAP_START;
  map.source = RU_SOURCE_LOG;
  map.map = "de_dust2";
  rp::PostEvent(map);
  rp::PostEvent(map);  // "Loading map" + "Started map": delivered once
  rp::LifecycleEvent rs;
  rs.type = RU_EVENT_ROUND_START;
  rs.round = 1;
  rp::PostEvent(rs);
  for (int i = 0; i < 5; ++i) rp::Frame(true);
  int mapEvents = 0;
  for (const auto& l : g_log) mapEvents += l.find("event map_start (log) map=de_dust2") != std::string::npos;
  Check(mapEvents == 1, "map_start delivered exactly once");
  Check(Logged("event round_start (engine) map=de_dust2"), "round_start delivered with current map");

  std::puts("-- console command");
  Check(rp::TryDispatchConsole("hello_status"), "hello_status is owned by the plugin");
  rp::Frame(false);
  Check(Logged("status: version 1.0.0, 6 ticks"), "console command ran; 6 simulating ticks seen");

  std::puts("-- list");
  rp::HandlePluginCommand({"list"}, false);
  Check(Logged("hello 1.0.0 (api 1.0) cmds=2 ticks=1 subs=1"), "list shows the plugin and its registrations");

  std::puts("-- hot reload with a rebuilt hello.so");
  if (!CopyFile(argv[2], so)) return 2;
  g_chat.clear();
  rp::HandlePluginCommand({"reload", "hello"}, false);
  Check(!Logged("reloaded hello"), "reload is deferred to the next frame");
  rp::TryDispatchChat(76561198000000001ull, "alice", ".hello");  // queued before the reload runs
  rp::Frame(true);
  Check(Logged("unloading after 6 ticks, 1 greetings"), "old image got its unload call");
  Check(!Logged("image is still mapped"), "old image really unmapped by dlclose");
  Check(Logged("plugin: loaded hello 1.0.1-reloaded"), "new code loaded");
  Check(Logged("reloaded hello"), "reload reported");
  Check(g_chat.empty(), "command queued for the old image was dropped, not run against the new one");
  rp::TryDispatchChat(76561198000000001ull, "alice", ".hello");
  rp::Frame(true);
  Check(Chatted("readyup-hello 1.0.1-reloaded, greeting #1"), "new code answers .hello with fresh state");

  std::puts("-- unload removes every registration");
  rp::HandlePluginCommand({"unload", "hello"}, false);
  rp::Frame(true);
  Check(Logged("unloaded hello"), "unloaded");
  Check(!rp::TryDispatchChat(76561198000000001ull, "alice", ".hello"), ".hello no longer routed");
  Check(!rp::TryDispatchConsole("hello_status"), "hello_status no longer routed");
  Check(dlopen(so.c_str(), RTLD_NOW | RTLD_NOLOAD) == nullptr, "hello.so no longer mapped");

  std::puts("-- load errors");
  rp::HandlePluginCommand({"load", "nope"}, false);
  rp::Frame(false);
  Check(Logged("load nope failed: no such file"), "missing plugin reported");
  rp::HandlePluginCommand({"load", "hello"}, false);
  rp::Frame(false);
  Check(Logged("loaded hello"), "load after unload works");

  unlink(so.c_str());
  rmdir(dir);
  std::printf("%s (%d failed)\n", g_failed ? "FAILED" : "ALL OK", g_failed);
  return g_failed ? 1 : 0;
}
