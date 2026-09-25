// match.so inside the real plugin host (core/src/readyup/plugin_loader.cpp with stubbed core
// functions and a fake engine API, like tools/plugin_host_test.cpp). No CS2 server:
//   1. load: commands, `ru` subcommands, console settings, readyup.match.v1, selftest lines
//   2. scrim warmup from the fake player registry, `.r` from chat
//   3. `ru plugin reload match` keeps mode, ready states and runtime settings
//   4. `ru match load <url>` (served by this test), reload again: the loaded match survives
//   5. unload removes everything, lifts round-termination suppression, unmaps the image
//   build/plugins/match/match_host_test <match.so>
#include "readyup/plugin_loader.h"

#include "readyup/match_iface.h"
#include "readyup/plugin_api.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

static std::mutex g_logMu;
static std::vector<std::string> g_log;
static std::vector<std::string> g_chat;
static std::vector<std::string> g_cmds;

static void Capture(const char* fmt, va_list ap) {
  char stack[4096];
  va_list ap2;
  va_copy(ap2, ap);
  const int n = std::vsnprintf(stack, sizeof(stack), fmt, ap);
  std::string s;
  if (n >= static_cast<int>(sizeof(stack))) {
    s.resize(static_cast<size_t>(n) + 1);
    std::vsnprintf(&s[0], s.size(), fmt, ap2);
    s.resize(static_cast<size_t>(n));
  } else {
    s = stack;
  }
  va_end(ap2);
  while (!s.empty() && s.back() == '\n') s.pop_back();
  std::printf("  | %s\n", s.substr(0, 300).c_str());
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
bool DebugEnabled() { return false; }
void SendToChat(const char* msg) {
  std::lock_guard<std::mutex> lk(g_logMu);
  g_chat.push_back(msg);
}
void SendRawToChat(const char* msg) { SendToChat(msg); }
bool ClientPrintChat(int, const char* msg) {
  SendToChat(msg);
  return true;
}
bool EnqueueServerCommand(const char* c) {
  std::lock_guard<std::mutex> lk(g_logMu);
  g_cmds.push_back(c);
  return true;
}
std::optional<int> GameEventsSlotForSteam(unsigned long long steamid64) {
  if (steamid64 == 76561198000000001ull) return 2;
  return std::nullopt;
}
static std::string g_csgoDir;
static std::string g_moduleDir;
std::string GetCsgoDirFromModuleDir() { return g_csgoDir; }
std::string GetThisModuleDir() { return g_moduleDir; }
bool IsCoreChatCommand(const std::string& t) { return t == ".ru"; }
}  // namespace readyup

// ---- fake engine API ---------------------------------------------------------------------
struct FakePlayer {
  int slot, userid;
  uint64_t steamid64;
  int team;
  bool bot;
  const char* name;
};
static std::vector<FakePlayer> g_players = {
    {2, 2, 76561198000000001ull, 3, false, "alice"},
};
static std::atomic<int> g_suppressed{-1};

static void FillPlayer(ru_player* out, const FakePlayer& p) {
  ru_player r{};
  r.struct_size = out->struct_size;
  r.slot = p.slot;
  r.steamid64 = p.steamid64;
  r.team = p.team;
  r.is_bot = p.bot ? 1 : 0;
  r.connected = 1;
  std::snprintf(r.name, sizeof(r.name), "%s", p.name);
  r.userid = p.userid;
  std::memcpy(out, &r, out->struct_size < sizeof(r) ? out->struct_size : sizeof(r));
}

namespace readyup::plugins::detail {
void FillEngineApi(ru_api* a) {
  a->center_html_to_slot = [](ru_plugin*, int, const char*, int) { return 1; };
  a->center_html_all = [](ru_plugin*, const char*, int) { return 0; };
  a->get_player = [](ru_plugin*, int slot, ru_player* out) {
    for (const auto& p : g_players) {
      if (p.slot == slot) {
        FillPlayer(out, p);
        return 1;
      }
    }
    return 0;
  };
  a->get_player_by_steamid = [](ru_plugin*, uint64_t sid, ru_player* out) {
    for (const auto& p : g_players) {
      if (p.steamid64 == sid) {
        FillPlayer(out, p);
        return 1;
      }
    }
    return 0;
  };
  a->for_each_player = [](ru_plugin*, ru_player_fn fn, void* user) {
    int n = 0;
    ru_player r{};
    r.struct_size = sizeof(r);
    for (const auto& p : g_players) {
      FillPlayer(&r, p);
      ++n;
      if (!fn(user, &r)) break;
    }
    return n;
  };
  a->ev_get_int = [](ru_plugin*, const ru_game_event*, const char*, int def) { return def; };
  a->ev_get_float = [](ru_plugin*, const ru_game_event*, const char*, double def) { return def; };
  a->ev_get_uint64 = [](ru_plugin*, const ru_game_event*, const char*, uint64_t def) { return def; };
  a->ev_get_string = [](ru_plugin*, const ru_game_event*, const char*, const char* def) { return def ? def : ""; };
  a->ev_get_player_slot = [](ru_plugin*, const ru_game_event*, const char*) { return -1; };
  a->ev_get_player_controller = [](ru_plugin*, const ru_game_event*, const char*) -> void* { return nullptr; };
  a->ev_get_player_pawn = [](ru_plugin*, const ru_game_event*, const char*) -> void* { return nullptr; };
  a->schema_offset = [](ru_plugin*, const char*, const char*) { return -1; };
  a->entity_system_status = [](ru_plugin*) { return static_cast<int>(RU_ENTSYS_PENDING); };
  a->entity_by_index = [](ru_plugin*, int) -> void* { return nullptr; };
  a->entity_from_handle = [](ru_plugin*, uint32_t) -> void* { return nullptr; };
  a->entity_handle_of = [](ru_plugin*, void*) { return 0xFFFFFFFFu; };
  a->entity_classname = [](ru_plugin*, void*) -> const char* { return nullptr; };
  a->entity_mark_changed = [](ru_plugin*, void*) { return 0; };
  a->econ_attr_set_by_name = [](ru_plugin*, void*, const char*, double) { return 0; };
  a->entity_change_subclass = [](ru_plugin*, void*, const char*) { return 0; };
  a->entity_set_model = [](ru_plugin*, void*, const char*) { return 0; };
  a->entity_set_bodygroup_by_name = [](ru_plugin*, void*, const char*, int) { return static_cast<int>(RU_BODYGROUP_UNAVAILABLE); };
  a->set_round_termination_suppressed = [](ru_plugin*, int s) {
    g_suppressed.store(s);
    return 1;
  };
  a->is_admin = [](ru_plugin*, uint64_t) { return 0; };
  a->feature_state = [](ru_plugin*, const char* name) {
    const std::string n = name;
    if (n == "events_live" || n == "fn:UTIL_ClientPrintAll") return 0;
    return 1;  // match_flow, knife, ready_hud, welcome_html, pauses, ...
  };
}
}  // namespace readyup::plugins::detail

namespace rp = readyup::plugins;

// ---- helpers --------------------------------------------------------------------------------
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
static void ClearLog() {
  std::lock_guard<std::mutex> lk(g_logMu);
  g_log.clear();
  g_chat.clear();
}
static bool FramesUntil(const std::function<bool()>& pred, int ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    rp::Frame(true);
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred();
}
static std::string Summary() {
  const auto* m = static_cast<const ru_match_v1*>(rp::CoreGetInterface(RU_MATCH_IFACE_NAME, RU_MATCH_IFACE_VERSION));
  if (!m) return "(none)";
  ru_match_status st{};
  st.struct_size = sizeof(st);
  if (m->get_status(&st) != 1) return "(error)";
  return std::string(st.summary_json) + (st.state_json ? std::string(" STATE ") + st.state_json : std::string());
}
static bool Has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
static bool Sent(const std::string& cmd) {
  std::lock_guard<std::mutex> lk(g_logMu);
  for (const auto& c : g_cmds) {
    if (c == cmd) return true;
  }
  return false;
}
static bool Chatted(const std::string& needle) {
  std::lock_guard<std::mutex> lk(g_logMu);
  for (const auto& c : g_chat) {
    if (c.find(needle) != std::string::npos) return true;
  }
  return false;
}
static void ClearCmds() {
  std::lock_guard<std::mutex> lk(g_logMu);
  g_cmds.clear();
}

// One-shot HTTP server for `ru match load`.
static int ServeOnce(const std::string& body, std::thread* th) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(fd, 1) != 0) return -1;
  socklen_t len = sizeof(a);
  getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
  *th = std::thread([fd, body] {
    const int c = accept(fd, nullptr, nullptr);
    if (c >= 0) {
      char buf[4096];
      (void)!read(c, buf, sizeof(buf));
      const std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                               std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
      (void)!write(c, resp.data(), resp.size());
      close(c);
    }
    close(fd);
  });
  return ntohs(a.sin_port);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <match.so>\n", argv[0]);
    return 2;
  }
  char tmpl[] = "/tmp/ru-match-host.XXXXXX";
  const char* root = mkdtemp(tmpl);
  if (!root) return 2;
  const std::string csgo = std::string(root) + "/csgo";
  const std::string pluginsDir = csgo + "/readyup/plugins";
  readyup::g_csgoDir = csgo;
  readyup::g_moduleDir = csgo + "/readyup/bin/linuxsteamrt64";
  if (std::system(("mkdir -p '" + pluginsDir + "' '" + readyup::g_moduleDir + "' && cp '" + argv[1] + "' '" +
                   pluginsDir + "/match.so'")
                      .c_str()) != 0) {
    return 2;
  }
  setenv("READYUP_PLUGINS_DIR", pluginsDir.c_str(), 1);
  {
    FILE* cfg = std::fopen((readyup::g_moduleDir + "/readyup.cfg").c_str(), "w");
    std::fputs("debug=0\nwelcome=0\nconsume_ready_chat=1\n[match]\nknife_pick_seconds=33\n", cfg);
    std::fclose(cfg);
  }

  std::puts("-- load");
  rp::LifecycleEvent map;
  map.type = RU_EVENT_MAP_START;
  map.map = "de_test";
  rp::PostEvent(map);  // the core's current map before the plugin loads
  rp::Frame(false);
  Check(Logged("plugin: loaded match"), "match.so loaded");
  uint32_t flags = 0;
  Check(rp::ChatCommandOwned(".r", &flags) && (flags & RU_CMD_HIDE), ".r owned, hidden (consume_ready_chat=1)");
  Check(rp::ChatCommandOwned(".pause", &flags) && flags == 0, ".pause owned, visible");
  Check(rp::TryDispatchRu(true, 0, "Console", "ru state"), "`ru state` is a match subcommand");
  rp::Frame(true);
  Check(Logged("state: mode=idle") && !Logged("plugin[match]: state:"), "`ru state` answered, log lines untagged");
  {
    // scrim_auto=1 and alice on CT: the same frame may already have moved idle -> scrim_warmup.
    const std::string s0 = Summary();
    const bool ok = Has(s0, "\"map\":\"de_test\"") &&
                    (Has(s0, "\"ru_mode\":\"idle\"") || Has(s0, "\"ru_mode\":\"scrim_warmup\""));
    Check(ok, "readyup.match.v1: status on the core's current map");
    if (!ok) std::printf("  summary: %s\n", s0.c_str());
  }
  {
    bool db = false, hud = false;
    for (const auto& c : rp::RunPluginSelftests()) {
      if (c.plugin != "match") continue;
      db |= c.name == "store";
      hud |= c.name == "ready HUD";
    }
    Check(db && hud, "selftest lines from readyup.selftest.match");
  }
  Check(rp::TryDispatchConsole("ru_warmup_roundtime_minutes 7"), "console setting owned by match");
  rp::TryDispatchConsole("ru_series_end_kick_delay_no_demo 9");
  rp::Frame(true);
  Check(Logged("warmup roundtime minutes: set"), "console setting applied");

  std::puts("-- scrim warmup + .r");
  Check(FramesUntil([] { return Logged("state: mode=scrim_warmup"); }, 3000), "human on CT -> scrim_warmup");
  Check(g_suppressed.load() == 0, "no round-termination suppression in scrim warmup");
  Check(rp::TryDispatchChat(76561198000000001ull, "alice", ".r", 2), ".r dispatched");
  rp::Frame(true);
  Check(Has(Summary(), "\"ready\":{\"ready\":1,\"total\":1}"), "alice is ready (1/1)");

  std::puts("-- reload keeps mode, ready states, settings");
  ClearLog();
  rp::HandlePluginCommand({"reload", "match"}, false);
  rp::Frame(true);
  Check(Logged("plugin: reloaded match"), "reloaded");
  Check(!Logged("still mapped"), "old image unmapped (every worker thread joined)");
  Check(Logged("restored the previous image's state (mode=scrim_warmup"), "state restored after reload");
  Check(Has(Summary(), "\"ru_mode\":\"scrim_warmup\"") && Has(Summary(), "\"ready\":{\"ready\":1,\"total\":1}"),
        "mode + ready survive the reload");
  rp::TryDispatchConsole("ru_warmup_roundtime_minutes");
  rp::TryDispatchConsole("ru_series_end_kick_delay_no_demo");
  rp::Frame(true);
  Check(Logged("warmup roundtime minutes: 7"), "ru_warmup_* survives the reload");
  Check(Logged("no_demo=9"), "series-end kick delay survives the reload");

  std::puts("-- admin commands: map, reloadmap, restart, load (admin-only; the console always)");
  ClearLog();
  ClearCmds();
  for (const char* c : {".ru map de_other", ".ru reloadmap", ".ru restart", ".ru load http://127.0.0.1:1/x"}) {
    rp::TryDispatchRu(false, 76561198000000001ull, "alice", c, 2);
  }
  rp::Frame(true);
  Check(Chatted("not authorized") && !Sent("changelevel de_other") && !Sent("changelevel de_test") &&
            !Sent("mp_restartgame 1") && !Logged("match-load["),
        "non-admin: map / reloadmap / restart / load refused");
  Check(rp::TryDispatchRu(true, 0, "Console", "ru map 3084291314"), "`ru map` is a match subcommand");
  rp::TryDispatchRu(true, 0, "Console", "ru map de_x;quit");
  rp::TryDispatchRu(true, 0, "Console", "ru reloadmap");
  rp::TryDispatchRu(true, 0, "Console", "ru restart");
  rp::Frame(true);
  Check(Sent("host_workshop_map 3084291314"), "console: ru map <workshop id> -> host_workshop_map");
  Check(Logged("not a map name or workshop id") && !Sent("changelevel de_x;quit"), "console: bad map name refused");
  Check(Sent("changelevel de_test"), "console: ru reloadmap -> changelevel to the current map");
  Check(Sent("mp_restartgame 1"), "console: ru restart -> mp_restartgame 1");
  rp::TryDispatchRu(true, 0, "Console", "ru admins add 76561198000000001");
  rp::Frame(true);
  ClearLog();
  ClearCmds();
  rp::TryDispatchRu(false, 76561198000000001ull, "alice", ".ru map de_other", 2);
  rp::TryDispatchChat(76561198000000001ull, "alice", ".help", 2);
  rp::Frame(true);
  Check(Sent("changelevel de_other"), "admin: .ru map de_other -> changelevel");
  Check(Chatted("Ready Up admin: .ru map"), "admin: .help lists the admin commands");
  rp::TryDispatchRu(true, 0, "Console", "ru admins remove 76561198000000001");
  rp::Frame(true);

  std::puts("-- ru match load, then reload with the match loaded");
  g_players.push_back({3, 3, 76561198000000002ull, 2, false, "bob"});
  const std::string body =
      "{\"id\":4242,\"slug\":\"hosttest\",\"config\":{\"matchid\":4242,\"num_maps\":1,\"maplist\":[\"de_test\"],"
      "\"map_sides\":[\"team1_ct\"],\"maxRounds\":24,\"overtimeMode\":\"disabled\","
      "\"max_tech_pauses_per_team\":2,\"tech_pause_max_seconds\":45,\"forfeit_after_seconds\":0,"
      "\"team1\":{\"name\":\"Alpha\",\"players\":{\"76561198000000001\":\"alice\"}},"
      "\"team2\":{\"name\":\"Bravo\",\"players\":{\"76561198000000002\":\"bob\"}}}}";
  std::thread http;
  const int port = ServeOnce(body, &http);
  Check(port > 0, "test http server up");
  ClearLog();
  Check(rp::TryDispatchRu(true, 0, "Console", "ru match load http://127.0.0.1:" + std::to_string(port) + "/m.json"),
        "`ru match load` dispatched");
  rp::Frame(true);
  http.join();
  Check(Logged("match context set: matchid=4242 slug=hosttest"), "match loaded");
  Check(FramesUntil([] { return Has(Summary(), "\"ru_mode\":\"match_warmup\""); }, 2000), "mode match_warmup");
  Check(FramesUntil([] { return g_suppressed.load() == 1; }, 2000), "warmup suppresses round termination");
  rp::TryDispatchChat(76561198000000002ull, "bob", ".ready", 3);
  rp::Frame(true);
  Check(Has(Summary(), "\"ready\":{\"ready\":1,\"total\":2}"), "bob ready (1/2)");
  ClearLog();
  rp::HandlePluginCommand({"reload", "match"}, false);
  rp::Frame(true);
  Check(Logged("restored the previous image's state (mode=match_warmup match=hosttest"), "match restored after reload");
  const std::string s = Summary();
  Check(Has(s, "\"match_id\":\"4242\"") && Has(s, "\"slug\":\"hosttest\"") && Has(s, "\"ready\":{\"ready\":1,\"total\":2}"),
        "loaded match + ready states survive the reload");
  Check(Has(s, "\"Alpha\"") && Has(s, "\"76561198000000002\""), "MatchState roster survives the reload");
  Check(rp::ChatCommandOwned(".r", &flags), "commands registered again by the new image");
  ClearLog();
  Check(rp::TryDispatchRu(true, 0, "Console", "ru state"), "`ru state` after the reload");
  rp::Frame(true);
  Check(Logged("rules: tech_pauses=2 tech_max_s=45 unpause=both force_ready=1 min_ready=0 forfeit_s=0"),
        "match rules survive the reload");

  std::puts("-- console settings go to state.json (a server restart restores them)");
  rp::TryDispatchConsole("ru_demo_path persist/");
  rp::TryDispatchConsole("ru_demo_path /rejected/");
  rp::TryDispatchConsole("ru_warmup_startmoney 20000");
  rp::TryDispatchConsole("ru_demo_upload_header \"X-Token\" \"abc\"");
  rp::TryDispatchConsole("ru_series_end_kick_delay_demo_upload 77");
  rp::TryDispatchConsole("ru_series_end_kick_delay_demo_upload default");
  rp::Frame(true);
  Check(Logged("ru_series_end_kick_delay_demo_upload: back to the default"), "`<setting> default` answered");

  std::puts("-- unload");
  ClearLog();
  rp::HandlePluginCommand({"unload", "match"}, false);
  rp::Frame(true);
  Check(Logged("plugin: unloaded match"), "unloaded");
  Check(!Logged("still mapped"), "image unmapped");
  Check(g_suppressed.load() == 0, "unload lifted round-termination suppression");
  Check(!rp::ChatCommandOwned(".r", &flags), ".r no longer routed");
  Check(rp::CoreGetInterface(RU_MATCH_IFACE_NAME, RU_MATCH_IFACE_VERSION) == nullptr, "readyup.match.v1 removed");
  Check(dlopen((pluginsDir + "/match.so").c_str(), RTLD_NOW | RTLD_NOLOAD) == nullptr, "match.so no longer mapped");
  {
    std::string st;
    if (FILE* f = std::fopen((pluginsDir + "/match/state.json").c_str(), "r")) {
      char buf[4096];
      size_t n = 0;
      while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) st.append(buf, n);
      std::fclose(f);
    }
    Check(Has(st, "\"ru_demo_path\"") && Has(st, "\"persist/\"") && !Has(st, "rejected"),
          "state.json: ru_demo_path saved, rejected value not");
    Check(Has(st, "\"ru_warmup_startmoney\"") && Has(st, "\"ru_warmup_maxmoney\""),
          "state.json: startmoney + the maxmoney it raised");
    Check(Has(st, "X-Token: abc"), "state.json: upload headers");
    Check(Has(st, "\"ru_series_end_kick_delay_no_demo\"") && !Has(st, "ru_series_end_kick_delay_demo_upload"),
          "state.json: kick delay saved; `default` removed the other");
    Check(!Has(st, "ru_warmup_respawn"), "state.json: settings left at their default are not stored");
    if (g_failed) std::printf("  state.json: %s\n", st.c_str());
  }

  std::puts("-- load again: the stash still has the match");
  ClearLog();
  rp::HandlePluginCommand({"load", "match"}, false);
  rp::Frame(true);
  Check(Logged("plugin: loaded match") && Logged("match=hosttest"), "load after unload restores the match");

  // Return with match.so still loaded and its workers running, like a server `quit` (the core never
  // unloads plugins): the plugin's exit handler must join them before its statics are destroyed,
  // or exit() aborts (joinable std::thread) or hangs (a worker waiting on a destroyed condvar).
  std::system(("rm -rf '" + std::string(root) + "'").c_str());
  std::printf("%s\n", g_failed ? "match_host_test: FAIL" : "match_host_test: PASS");
  return g_failed ? 1 : 0;
}
