// readyup-essentials: the everyday server basics, apart from the match flow, so every kind of
// server has them (a practice-only server is core + essentials + practice; an esports server can
// run core + match alone).
//
//   ru admins [list]                         who the Ready Up admins are (admins.json)
//   ru admins add|remove <steamid64|name>    admin; the first admin only from the console
//   ru map change <name|workshop id> [force] changelevel / host_workshop_map (admin)
//   ru map reload [force]                    the current map again (workshop maps by id)
//   ru map restart [force]                   mp_restartgame 1
//
// While the server downloads a Workshop map (host_workshop_map), everyone sees a progress bar in
// the center panel, resent ~10x a second (core API 1.4 workshop_download_progress).
//
// Chat: `.ru admins ...`, `.ru map ...`. Map commands are refused during a knife round or a live
// map (the match plugin's mode) unless `force` is added.
//
// Admins: csgo/readyup/plugins/essentials/admins.json, re-read when it changes; on first load the
// match plugin's old plugins/match/admins.json is copied over. This plugin is an admin provider
// (a player is an admin when any provider says so: the match plugin adds the match config's
// admins, the MAT list and, in fleet mode, the platform's list). admins.json counts in fleet mode
// too (docs/FLEET.md D5 adds the platform's list, it does not replace the local one).
#include "essentials_rules.h"

#include "readyup/fleet_iface.h"
#include "readyup/map_names.h"
#include "readyup/match_iface.h"
#include "readyup/plugin_api.h"
#include "readyup/selftest_iface.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifndef ESSENTIALS_VERSION
#define ESSENTIALS_VERSION "dev"
#endif

namespace essentials {
namespace {

const ru_api* g_api = nullptr;

std::mutex g_mu;  // g_admins (the provider runs on any thread)
std::vector<Admin> g_admins;
std::string g_path;
int64_t g_mtime = -1;
double g_nextReload = 0;

// Workshop download being shown (game thread only).
struct Download {
  uint64_t id = 0;
  std::string name;
  double started = 0, nextPoll = 0, nextLog = 0;
  bool announced = false;
} g_dl;
constexpr double kDownloadPollSeconds = 0.1;
constexpr double kDownloadTimeoutSeconds = 600;

int64_t MtimeNs(const std::string& path) {
  struct stat st {};
  if (stat(path.c_str(), &st) != 0) return -1;
  return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
}

std::string ReadFile(const std::string& path) {
  std::ifstream f(path);
  if (!f) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void LoadAdmins() {
  const int64_t m = MtimeNs(g_path);
  if (m == g_mtime) return;
  bool ok = true;
  auto list = ParseAdmins(ReadFile(g_path), &ok);
  if (!ok) ru_logf(g_api, RU_LOG_WARN, "%s is not valid JSON; keeping the previous list", g_path.c_str());
  std::lock_guard<std::mutex> lk(g_mu);
  if (ok) g_admins = std::move(list);
  g_mtime = m;
}

bool SaveAdmins() {
  std::string text;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    text = AdminsJson(g_admins);
  }
  const std::string tmp = g_path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << text;
    if (!f) return false;
  }
  if (std::rename(tmp.c_str(), g_path.c_str()) != 0) return false;
  g_mtime = MtimeNs(g_path);
  return true;
}

// The match plugin's old admins.json (<plugins>/match/admins.json) becomes ours once.
void MigrateFromMatch(const std::string& dataDir) {
  if (MtimeNs(g_path) >= 0) return;
  const std::string old = dataDir + "/../match/admins.json";
  if (MtimeNs(old) < 0) return;
  std::ifstream in(old, std::ios::binary);
  std::ofstream out(g_path, std::ios::binary);
  out << in.rdbuf();
  if (out) ru_logf(g_api, RU_LOG_INFO, "admins: copied %s to %s", old.c_str(), g_path.c_str());
}

bool FleetMode() {
  const auto* f = static_cast<const ru_fleet_v1*>(g_api->get_interface(g_api->self, RU_FLEET_IFACE_NAME, 1));
  return f && f->connection_state && f->connection_state() != RU_FLEET_LINK_STANDALONE;
}

std::string RuMode() {
  const auto* m = static_cast<const ru_match_v1*>(g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1));
  if (!m) return {};
  if (RU_API_HAS(m, mode) && m->mode) return m->mode() ? m->mode() : "";
  ru_match_status st{};
  st.struct_size = sizeof(st);
  return m->get_status && m->get_status(&st) == 1 && st.ru_mode ? st.ru_mode : "";
}

// admins.json always counts. In fleet mode the platform's list (match plugin) adds more, so a
// player not in the file is left to it (-1) instead of refused.
int Provider(void*, uint64_t steamid64) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (IsAdmin(g_admins, steamid64)) return 1;
  }
  return FleetMode() ? -1 : 0;
}

bool SenderIsAdmin(const ru_command_ctx* c) { return c->is_console || g_api->is_admin(g_api->self, c->steamid64) == 1; }

void Reply(const ru_command_ctx* c, const std::string& msg) {
  if (c->is_console) ru_logf(g_api, RU_LOG_INFO, "%s", msg.c_str());
  else if (c->slot >= 0) g_api->chat_to_slot(g_api->self, c->slot, (" \x04[ReadyUp]\x01 " + msg).c_str());
}

std::vector<Player> ConnectedPlayers() {
  std::vector<Player> out;
  g_api->for_each_player(
      g_api->self,
      [](void* u, const ru_player* p) -> int {
        if (!p->is_bot && p->steamid64) static_cast<std::vector<Player>*>(u)->push_back(Player{p->steamid64, p->name});
        return 1;
      },
      &out);
  return out;
}

void OnAdmins(const ru_command_ctx* c, const std::string& sub, const std::vector<std::string>& args) {
  if (sub.empty() || sub == "list") {
    std::vector<Admin> list;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      list = g_admins;
    }
    Reply(c, "admins (admins.json): " + std::to_string(list.size()) +
                 (FleetMode() ? " (plus the platform's list)" : ""));
    for (size_t i = 0; i < list.size() && i < 10; ++i) {
      Reply(c, "- " + (list[i].name.empty() ? std::string("?") : list[i].name) + " (" + std::to_string(list[i].steamid64) + ")");
    }
    if (list.size() > 10) Reply(c, "... and " + std::to_string(list.size() - 10) + " more");
    return;
  }
  if (sub == "help") {
    for (const char* l : {".ru admins [list]: the admins", ".ru admins add|remove <steamid64|name>: admin (the first one from the console)"}) Reply(c, l);
    return;
  }
  if (sub != "add" && sub != "remove") return Reply(c, "unknown command. Type .ru help admins for the list.");
  if (!c->is_console) {
    bool empty;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      empty = g_admins.empty();
    }
    if (empty) return Reply(c, "no admins yet: add the first one from the server console (ru admins add <steamid64>)");
    if (!SenderIsAdmin(c)) return Reply(c, "not authorized");
  }
  if (args.empty()) return Reply(c, "usage: .ru admins " + sub + " <steamid64|name>");
  std::string target;
  for (const auto& a : args) target += (target.empty() ? "" : " ") + a;
  Player p;
  if (const uint64_t id = ParseSteamId64(target)) {
    p.steamid64 = id;
    for (const auto& q : ConnectedPlayers()) {
      if (q.steamid64 == id) p.name = q.name;
    }
  } else {
    std::string err;
    if (!FindPlayer(ConnectedPlayers(), target, &p, &err)) return Reply(c, err);
  }
  bool changed;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    changed = sub == "add" ? AddAdmin(&g_admins, p.steamid64, p.name.empty() ? "Admin" : p.name)
                           : RemoveAdmin(&g_admins, p.steamid64);
  }
  const std::string who = (p.name.empty() ? std::string() : p.name + " ") + "(" + std::to_string(p.steamid64) + ")";
  if (!changed) return Reply(c, (sub == "add" ? "already an admin: " : "not an admin: ") + who);
  if (!SaveAdmins()) return Reply(c, "could not save " + g_path);
  Reply(c, (sub == "add" ? "added " : "removed ") + who);
}

bool LoadEntry(const std::string& entry) {
  const std::string cmd = readyup::mapnames::LoadCommand(entry);
  if (cmd.empty()) return false;
  readyup::mapnames::MapRef ref;
  if (readyup::mapnames::ParseEntry(entry, &ref) && !ref.workshop_id.empty()) {
    readyup::mapnames::NoteWorkshopLoad(ref.workshop_id);
    g_dl = Download{};
    g_dl.id = std::strtoull(ref.workshop_id.c_str(), nullptr, 10);
    g_dl.name = readyup::mapnames::DisplayName(entry);
  }
  return g_api->server_command(g_api->self, cmd.c_str()) == 1;
}

void OnMap(const ru_command_ctx* c, const std::string& sub, std::vector<std::string> args) {
  if (sub.empty() || sub == "help") {
    for (const char* l : {".ru map change <name|workshop id|link> [force]: change map (admin)",
                          ".ru map reload [force]: load the current map again (admin)",
                          ".ru map restart [force]: restart the game, mp_restartgame 1 (admin)"}) {
      Reply(c, l);
    }
    return;
  }
  if (sub != "change" && sub != "reload" && sub != "restart") return Reply(c, "unknown command. Type .ru help map for the list.");
  if (!SenderIsAdmin(c)) return Reply(c, "not authorized");
  const bool force = !args.empty() && args.back() == "force";
  if (force) args.pop_back();
  const std::string mode = RuMode();
  if (MapCommandBlocked(mode) && !force) {
    return Reply(c, "a match is live (" + mode + "): add `force` to " + sub + " anyway");
  }
  const std::string who = c->is_console ? std::string("Console") : std::string(c->name ? c->name : "?");
  if (sub == "restart") {
    g_api->server_command(g_api->self, "mp_restartgame 1");
    ru_logf(g_api, RU_LOG_INFO, "map restart by %s", who.c_str());
    return g_api->chat_all(g_api->self, "Ready Up: game restarting.", 0), void();
  }
  std::string entry;
  if (sub == "change") {
    if (args.size() != 1) return Reply(c, "usage: .ru map change <name|workshop id> [force]");
    entry = MapArgToEntry(args[0]);  // a pasted Workshop link works too
    if (!readyup::mapnames::ValidEntry(entry)) {
      return Reply(c, "\"" + args[0].substr(0, 64) + "\" is not a map name, workshop id or workshop link");
    }
  } else {
    const char* cur = g_api->current_map(g_api->self);
    entry = readyup::mapnames::ReloadEntry(cur ? cur : "");
    if (entry.empty()) return Reply(c, "current map not known yet");
  }
  if (!LoadEntry(entry)) return Reply(c, "map change unavailable yet");
  ru_logf(g_api, RU_LOG_INFO, "map %s by %s: %s", sub.c_str(), who.c_str(), entry.c_str());
  g_api->chat_all(g_api->self, ("Ready Up: " + std::string(sub == "change" ? "changing map to " : "reloading ") +
                                readyup::mapnames::DisplayName(entry) + ".")
                                   .c_str(),
                  0);
}

void OnRu(void*, const ru_command_ctx* c) {
  const std::string main = c->argc >= 2 && c->argv[1] ? c->argv[1] : "";
  const std::string sub = c->argc >= 3 && c->argv[2] ? c->argv[2] : "";
  std::vector<std::string> args;
  for (int i = 3; i < c->argc; ++i) args.emplace_back(c->argv[i] ? c->argv[i] : "");
  if (main == "admins") OnAdmins(c, sub, args);
  else if (main == "map") OnMap(c, sub, args);
}

void OnMapStart(void*, const ru_event* e) {
  g_dl = Download{};  // downloaded (or cached) and loaded
  if (e && e->map && *e->map) readyup::mapnames::NoteMapLoaded(e->map);  // binds a workshop id to its map
}

// Progress bar for the Workshop map being downloaded. Nothing is shown while Steam reports no
// download (an installed map loads straight away).
void PollDownload(double now) {
  if (!g_dl.id || !RU_API_HAS(g_api, workshop_download_progress) || !g_api->workshop_download_progress) return;
  if (g_dl.started == 0) g_dl.started = now;
  if (now - g_dl.started > kDownloadTimeoutSeconds) {
    ru_logf(g_api, RU_LOG_WARN, "workshop %llu: no map start after %.0f s; progress bar stopped",
            static_cast<unsigned long long>(g_dl.id), kDownloadTimeoutSeconds);
    g_dl = Download{};
    return;
  }
  if (now < g_dl.nextPoll) return;
  g_dl.nextPoll = now + kDownloadPollSeconds;
  uint64_t done = 0, total = 0;
  if (!g_api->workshop_download_progress(g_api->self, g_dl.id, &done, &total)) return;
  if (!g_dl.announced) {
    g_dl.announced = true;
    ru_logf(g_api, RU_LOG_INFO, "workshop %llu: downloading", static_cast<unsigned long long>(g_dl.id));
  }
  if (now >= g_dl.nextLog && total > 0) {
    g_dl.nextLog = now + 5.0;
    ru_logf(g_api, RU_LOG_INFO, "workshop %llu: %.1f / %.1f MB", static_cast<unsigned long long>(g_dl.id),
            done / 1048576.0, total / 1048576.0);
  }
  // Over the ready HUD and the welcome card while it downloads (API 1.6: one panel per player).
  const std::string html = DownloadPanelHtml(g_dl.name, done, total);
  if (RU_API_HAS(g_api, center_html_all_prio) && g_api->center_html_all_prio) {
    g_api->center_html_all_prio(g_api->self, html.c_str(), 1, RU_HTML_PRIO_ALERT);
  } else {
    g_api->center_html_all(g_api->self, html.c_str(), 1);
  }
}

void OnTick(void*, const ru_tick_info* t) {
  PollDownload(t->now);
  if (t->now < g_nextReload) return;
  g_nextReload = t->now + 30.0;
  LoadAdmins();  // re-read when admins.json changed on disk
}

void RunSelftest(ru_selftest_add_fn add, void* ctx) {
  std::string d;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    d = std::to_string(g_admins.size()) + " admin(s) in " + g_path;
  }
  if (FleetMode()) d += ", plus the platform's list (fleet mode)";
  add(ctx, "INFO", "essentials", d.c_str());
}
const ru_selftest_iface_v1 g_selftestIface = {sizeof(ru_selftest_iface_v1), &RunSelftest};

}  // namespace
}  // namespace essentials

using namespace essentials;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 2u,  // needs API 1.2 (register_ru_subcommand)
      "essentials",
      ESSENTIALS_VERSION,
      "Ready Up",
      "server basics: admins (admins.json), map change / reload / restart",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, register_ru_subcommand)) return 1;
  g_api = api;
  const char* dir = api->data_dir(api->self);
  const std::string dataDir = dir && *dir ? dir : ".";
  g_path = dataDir + "/admins.json";
  g_mtime = -1;
  g_nextReload = 0;
  MigrateFromMatch(dataDir);
  LoadAdmins();
  for (const char* m : {"admins", "map"}) {
    if (!api->register_ru_subcommand(api->self, m, &OnRu, nullptr)) {
      ru_logf(api, RU_LOG_WARN, "could not register `ru %s` (another plugin owns it)", m);
    }
  }
  api->set_admin_provider(api->self, &Provider, nullptr);
  api->subscribe(api->self, RU_EVENT_MAP_START, &OnMapStart, nullptr);
  api->on_tick(api->self, &OnTick, nullptr);
  api->provide_interface(api->self, RU_SELFTEST_IFACE_PREFIX "essentials", RU_SELFTEST_IFACE_VERSION,
                         const_cast<ru_selftest_iface_v1*>(&g_selftestIface));
  {
    std::lock_guard<std::mutex> lk(g_mu);
    ru_logf(api, RU_LOG_INFO, "loaded " ESSENTIALS_VERSION ": %zu admin(s) in %s", g_admins.size(), g_path.c_str());
  }
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  ru_logf(g_api, RU_LOG_INFO, "unloaded");
  g_api = nullptr;
}

}  // extern "C"
