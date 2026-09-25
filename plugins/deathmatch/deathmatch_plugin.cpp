// readyup-deathmatch: CS2's deathmatch game mode with Ready Up's rules on top, as a plugin of its
// own (Full bundle).
//
//   ru deathmatch|dm ffa [map]   admin: free for all (game_type 1 / game_mode 2,
//                                mp_teammates_are_enemies 1). Without a map: the essentials
//                                plugin's default map for "ffa" (default_maps.json), else the
//                                current map again. game_type / game_mode only take effect on a
//                                map load, so switching into deathmatch always loads a map.
//   ru dm tdm [map]              admin: team deathmatch (mp_teammates_are_enemies 0, mp_dm_teammode 1)
//   ru dm off                    admin: back to the match plugin's idle / scrim (restore_game_type /
//                                restore_game_mode, default competitive 0 / 1, and the current map
//                                loads again)
//   ru dm status | top | hud     anyone: state, the top 5 + your rank, your leaderboard panel on / off
//   (chat: `.ru dm ...` / `.ru deathmatch ...`)
//
// Rules (cfg/ReadyUp/deathmatch.cfg or readyup.cfg [deathmatch]; docs/DEATHMATCH.md): kill limit
// per mode, time limit, spawn protection (mp_respawn_immunitytime), headshot only
// (mp_damage_headshot_only), weapon rounds (every N minutes everyone spawns with a given weapon:
// the default-loadout cvars, optionally mp_buy_allow_guns 0). Kills are counted from player_death;
// the first player (FFA) / team (TDM) at the kill limit, or the leader when time runs out, wins:
// chat + a NOTICE panel for end_delay_seconds, then mp_restartgame 1 (restart=reload: the map
// loads again). A leaderboard (top 5 + your rank) is sent to every player at RU_HTML_PRIO_HUD
// every hud_interval_ms.
//
// With the match plugin (readyup.match.v1 set_external_mode, v1.5) the match flow steps aside
// (ru_mode "external": no scrim warmup, ready HUD, practice rules) while deathmatch is on; refused
// while a match is loaded or practice is on. `.ru mode idle` or a match load end it without asking
// this plugin: it notices (mode() is no longer "external") and cleans up. Without the match plugin
// it keeps the flag itself. Log lines: `deathmatch: ...`.
#include "dm_rules.h"

#include "readyup/essentials_iface.h"
#include "readyup/map_names.h"
#include "readyup/match_iface.h"
#include "readyup/plugin_api.h"
#include "readyup/selftest_iface.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <time.h>
#include <vector>

#ifndef DEATHMATCH_VERSION
#define DEATHMATCH_VERSION "dev"
#endif

namespace deathmatch {
namespace {

const ru_api* g_api = nullptr;
constexpr const char* kExternalName = "deathmatch";
constexpr double kMapLoadTimeoutSeconds = 600;  // workshop downloads can take a while
constexpr double kReapplySeconds = 3;           // map start: rules once more after the map's own cfgs

void Log(const char* fmt, ...) RU_PRINTF(1, 2);
void Log(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (g_api) g_api->log_untagged(g_api->self, RU_LOG_INFO, buf);
}

// CLOCK_MONOTONIC, the clock of ru_tick_info::now (event callbacks have no tick time).
double Now() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

void Cmd(const std::string& c) { g_api->server_command(g_api->self, c.c_str()); }
void Cmds(const std::vector<std::string>& cs) {
  for (const auto& c : cs) Cmd(c);
}
void ChatAll(const std::string& msg) { g_api->chat_all(g_api->self, msg.c_str(), 0); }

// ---- other plugins -------------------------------------------------------------------------------

const ru_match_v1* Match() {
  return static_cast<const ru_match_v1*>(g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1));
}
bool MatchOwnsMode() {
  const ru_match_v1* m = Match();
  return m && RU_API_HAS(m, set_external_mode) && m->set_external_mode && RU_API_HAS(m, mode) && m->mode;
}
std::string RuMode() {
  const ru_match_v1* m = Match();
  if (!m || !RU_API_HAS(m, mode) || !m->mode) return {};
  const char* s = m->mode();
  return s ? s : "";
}
const ru_essentials_v1* Essentials() {
  const auto* e = static_cast<const ru_essentials_v1*>(g_api->get_interface(g_api->self, RU_ESSENTIALS_IFACE_NAME, 1));
  return e && RU_API_HAS(e, load_map) && e->default_map && e->load_map ? e : nullptr;
}

// ---- state (game thread) --------------------------------------------------------------------------

enum class Phase { Waiting = 0, Playing = 1, Ended = 2 };

struct State {
  Mode mode = Mode::Off;
  bool mapPending = false;    // we loaded a map; the game starts at its map start
  double pendingSince = 0;
  bool dmMapLoaded = false;   // the current map loaded while deathmatch was on (game_type 1)
  Phase phase = Phase::Waiting;
  double startedAt = 0;
  double endedAt = 0;
  double reapplyAt = 0;
  double nextHud = 0;
  double nextSync = 0;
  int weaponIndex = -1;
  std::string weapon;
  Outcome outcome;
  Scoreboard board;
  Settings settings;
};
State g;
std::set<uint64_t> g_hudOff;  // players who turned their panel off (.ru dm hud)

// Selftest runs on any thread: a copy of the line under a lock.
std::mutex g_selftestMu;
std::string g_selftestLine = "off";

bool Active() { return g.mode != Mode::Off; }

void LoadConfig() {
  std::vector<std::string> warnings;
  g.settings = LoadSettings(
      [](const char* key, std::string* out) {
        char buf[512] = {};
        const int n = g_api->config_get(g_api->self, key, buf, sizeof(buf));
        if (n < 0) return false;
        *out = buf;
        return true;
      },
      &warnings);
  for (const auto& w : warnings) ru_logf(g_api, RU_LOG_WARN, "deathmatch.cfg: %s", w.c_str());
}

double Elapsed(double now) { return g.phase == Phase::Waiting ? 0 : std::max(0.0, now - g.startedAt); }

int SecondsLeft(double now) {
  if (g.settings.timeLimitMinutes <= 0) return -1;
  return static_cast<int>(std::ceil(g.settings.timeLimitMinutes * 60.0 - Elapsed(now)));
}

void UpdateSelftestLine() {
  std::string l = "off";
  if (Active()) {
    const char* cm = g_api->current_map(g_api->self);
    l = std::string(ModeName(g.mode)) + " on " + (cm && *cm ? cm : "?") +
        (g.mapPending ? " (map loading)" : g.phase == Phase::Ended ? " (game over)" : "") +
        (MatchOwnsMode() ? ", match plugin mode external" : ", standalone");
  }
  std::lock_guard<std::mutex> lk(g_selftestMu);
  g_selftestLine = l;
}

// ---- players --------------------------------------------------------------------------------------

struct Who {
  uint64_t id = 0;
  std::string name;
  int team = 0;
};

// A player by slot (humans: engine slot; bots: their log <N>, which is the slot in CS2).
bool WhoBySlot(int slot, Who* out) {
  if (slot < 0) return false;
  struct Ctx {
    int slot;
    Who* out;
    bool found;
  } c{slot, out, false};
  g_api->for_each_player(
      g_api->self,
      [](void* u, const ru_player* p) -> int {
        auto* c = static_cast<Ctx*>(u);
        if ((p->slot >= 0 ? p->slot : p->userid) != c->slot) return 1;
        c->out->id = p->is_bot || !p->steamid64 ? BotId(p->userid >= 0 ? p->userid : c->slot) : p->steamid64;
        c->out->name = p->name;
        c->out->team = p->team;
        c->found = true;
        return 0;
      },
      &c);
  return c.found;
}

struct Human {
  int slot;
  uint64_t steamid64;
};

// Connected humans with a slot, and every player's name / team into the board.
std::vector<Human> RefreshPlayers() {
  std::vector<Human> humans;
  g_api->for_each_player(
      g_api->self,
      [](void* u, const ru_player* p) -> int {
        auto* hs = static_cast<std::vector<Human>*>(u);
        const bool bot = p->is_bot || !p->steamid64;
        const uint64_t id = bot ? BotId(p->userid) : p->steamid64;
        if (p->team == 2 || p->team == 3) g.board.SeePlayer(id, p->name, p->team);
        if (!bot && p->slot >= 0 && p->connected) hs->push_back(Human{p->slot, p->steamid64});
        return 1;
      },
      &humans);
  return humans;
}

// ---- panels ---------------------------------------------------------------------------------------

bool HasPrio() { return RU_API_HAS(g_api, center_html_release) && g_api->center_html_to_slot_prio && g_api->center_html_all_prio; }

void ReleasePanels() {
  if (HasPrio() && g_api->center_html_release) g_api->center_html_release(g_api->self, -1);
}

int PanelSeconds() {
  // The panel's lifetime covers the gap to the next send, so the core keeps it ours in between.
  return std::max(1, static_cast<int>(std::ceil(g.settings.hudIntervalMs / 1000.0)) + 1);
}

void SendHud(double now) {
  if (now < g.nextHud) return;
  g.nextHud = now + g.settings.hudIntervalMs / 1000.0;
  const auto humans = RefreshPlayers();
  if (g.phase == Phase::Ended) {
    const int left = static_cast<int>(std::ceil(g.endedAt + g.settings.endDelaySeconds - now));
    const std::string html = WinnerHtml(g.mode, g.outcome, left);
    if (HasPrio()) g_api->center_html_all_prio(g_api->self, html.c_str(), PanelSeconds(), RU_HTML_PRIO_NOTICE);
    else g_api->center_html_all(g_api->self, html.c_str(), PanelSeconds());
    return;
  }
  if (!g.settings.hud || g.phase != Phase::Playing) return;
  const int limit = KillLimit(g.settings, g.mode);
  const int left = SecondsLeft(now);
  for (const auto& h : humans) {
    if (g_hudOff.count(h.steamid64)) continue;
    const std::string html = LeaderboardHtml(g.mode, g.board, h.steamid64, limit, left, g.weapon);
    if (HasPrio()) g_api->center_html_to_slot_prio(g_api->self, h.slot, html.c_str(), PanelSeconds(), RU_HTML_PRIO_HUD);
    else g_api->center_html_to_slot(g_api->self, h.slot, html.c_str(), PanelSeconds());
  }
}

// ---- game -----------------------------------------------------------------------------------------

void StopWeaponRound() {
  if (g.weaponIndex < 0) return;
  g.weaponIndex = -1;
  g.weapon.clear();
  Cmds(WeaponRoundCommands("", false));
}

void StartGame(double now, const char* why) {
  LoadConfig();
  g.board.Reset();
  g.phase = Phase::Playing;
  g.startedAt = now;
  g.outcome = Outcome{};
  StopWeaponRound();
  g.nextHud = 0;
  const int limit = KillLimit(g.settings, g.mode);
  std::string rules;
  if (limit > 0) rules += "first " + std::string(g.mode == Mode::Tdm ? "team" : "player") + " to " + std::to_string(limit) + " kills";
  if (g.settings.timeLimitMinutes > 0) {
    rules += (rules.empty() ? "" : ", ") + std::to_string(g.settings.timeLimitMinutes) + " min";
  }
  if (g.settings.headshotOnly) rules += (rules.empty() ? "" : ", ") + std::string("headshots only");
  ChatAll(std::string(ModeLabel(g.mode)) + (rules.empty() ? "" : ": " + rules) + ". .ru dm top for the leaderboard.");
  Log("deathmatch: game started (%s, %s)", ModeName(g.mode), why);
  UpdateSelftestLine();
}

void ApplyRules() {
  LoadConfig();
  Cmds(ModeCommands(g.mode, g.settings));
}

void EndGame(double now, const Outcome& o) {
  g.phase = Phase::Ended;
  g.endedAt = now;
  g.outcome = o;
  StopWeaponRound();
  const std::string line = WinnerChat(g.mode, o);
  ChatAll(line);
  Log("deathmatch: game over: %s", line.c_str());
  g.nextHud = 0;
  UpdateSelftestLine();
}

bool LoadMap(const std::string& entry) {
  readyup::mapnames::MapRef ref;
  if (!readyup::mapnames::ParseEntry(entry, &ref)) return false;
  // Our own copy of the workshop bindings learns the id too (ReloadEntry of that map later).
  if (!ref.workshop_id.empty()) readyup::mapnames::NoteWorkshopLoad(ref.workshop_id);
  if (const ru_essentials_v1* e = Essentials()) return e->load_map(entry.c_str()) == 1;  // with the download bar
  const std::string cmd = readyup::mapnames::LoadCommand(entry);
  return !cmd.empty() && g_api->server_command(g_api->self, cmd.c_str()) == 1;
}

std::string CurrentReloadEntry() {
  const char* cm = g_api->current_map(g_api->self);
  return readyup::mapnames::ReloadEntry(cm ? cm : "");
}

// Deathmatch off: our cvars back, the match plugin's mode back to idle (unless it left already).
// reload: the current map loads again, now in restore_game_type / restore_game_mode.
void Leave(bool reload, bool tellMatch, const char* why) {
  const bool wasActive = Active();
  LoadConfig();
  Cmds(LeaveCommands(g.settings, /*withGameMode=*/reload));
  if (tellMatch && MatchOwnsMode() && RuMode() == "external") (void)Match()->set_external_mode(nullptr);
  const std::string entry = reload && g.dmMapLoaded ? CurrentReloadEntry() : std::string();
  g.mode = Mode::Off;
  g.mapPending = false;
  g.phase = Phase::Waiting;
  g.weaponIndex = -1;
  g.weapon.clear();
  g.board.Reset();
  ReleasePanels();
  if (!entry.empty() && !LoadMap(entry)) ru_logf(g_api, RU_LOG_WARN, "deathmatch: could not load %s again", entry.c_str());
  if (!entry.empty()) g.dmMapLoaded = false;
  if (wasActive) Log("deathmatch: off (%s)%s", why, entry.empty() ? "" : ", map loads again");
  UpdateSelftestLine();
}

// `dm ffa|tdm [map]`. Returns the reply; *ok = the mode is (being) switched on.
std::string Enter(Mode m, const std::string& mapArg, bool* ok) {
  *ok = false;
  LoadConfig();
  if (MatchOwnsMode()) {
    const std::string was = RuMode();
    if (Match()->set_external_mode(kExternalName) != 1) {
      if (was == "practice") return "refused: practice mode is on (.prac to leave it first).";
      return "refused: a match is loaded (.ru match end first).";
    }
  }
  const ru_essentials_v1* e = Essentials();
  const std::string def = e ? e->default_map(ModeName(m)) : "";
  const MapChoice choice = ResolveMap(mapArg, def, CurrentReloadEntry());
  if (!mapArg.empty() && !readyup::mapnames::ValidEntry(choice.entry)) {
    if (!Active() && MatchOwnsMode()) (void)Match()->set_external_mode(nullptr);
    return "\"" + mapArg.substr(0, 64) + "\" is not a map name, workshop id or workshop link.";
  }
  const Mode before = g.mode;
  g.mode = m;
  const char* cm = g_api->current_map(g_api->self);
  const std::string current = cm ? cm : "";
  // Already on a deathmatch map and staying on it: switch the rules and restart the game.
  if (g.dmMapLoaded && !g.mapPending &&
      (choice.source == "current" || readyup::mapnames::EntryMatchesLoaded(choice.entry, current))) {
    ApplyRules();
    Cmd("mp_restartgame 1");
    Log("deathmatch: %s -> %s on %s", ModeName(before), ModeName(m), current.c_str());
    StartGame(Now() + 1.0, "mode switch");  // the restart's Match_Start comes within 2 s: no second reset
    *ok = true;
    return std::string(ModeLabel(m)) + " on " + current + " (game restarts).";
  }
  if (choice.entry.empty()) {
    g.mode = before;
    if (!Active() && MatchOwnsMode()) (void)Match()->set_external_mode(nullptr);
    return "current map not known yet: give a map (.ru dm " + std::string(ModeName(m)) + " <map>).";
  }
  Cmds(GameModeCommands());
  if (!LoadMap(choice.entry)) {
    g.mode = before;
    if (!Active()) {
      Cmds(LeaveCommands(g.settings, /*withGameMode=*/true));
      if (MatchOwnsMode()) (void)Match()->set_external_mode(nullptr);
    }
    return "map change unavailable yet.";
  }
  g.mapPending = true;
  g.pendingSince = 0;  // set on the next tick (tick time base)
  g.phase = Phase::Waiting;
  g.board.Reset();
  ReleasePanels();
  const std::string shown = readyup::mapnames::DisplayName(choice.entry);
  Log("deathmatch: %s on %s (%s map)", ModeName(m), choice.entry.c_str(), choice.source.c_str());
  UpdateSelftestLine();
  *ok = true;
  return std::string(ModeLabel(m)) + ": loading " + shown +
         (choice.source == "default" ? " (default map)" : choice.source == "current" ? " (this map again)" : "") + ".";
}

// ---- commands -------------------------------------------------------------------------------------

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

void Reply(const ru_command_ctx* c, const std::string& msg) {
  if (c->is_console) Log("%s", msg.c_str());
  else if (c->slot >= 0) g_api->chat_to_slot(g_api->self, c->slot, (" \x04[ReadyUp]\x01 " + msg).c_str());
}

bool IsAdmin(const ru_command_ctx* c) { return c->is_console || g_api->is_admin(g_api->self, c->steamid64) == 1; }

std::string StatusLine(double now) {
  if (!Active()) return "deathmatch is off (.ru dm ffa|tdm [map]).";
  const char* cm = g_api->current_map(g_api->self);
  std::string s = std::string(ModeLabel(g.mode)) + " on " + (cm && *cm ? cm : "?");
  if (g.mapPending) return s + ": map loading.";
  if (g.phase == Phase::Ended) return s + ": game over, next game soon.";
  const int limit = KillLimit(g.settings, g.mode);
  if (limit > 0) s += ", first to " + std::to_string(limit);
  if (SecondsLeft(now) >= 0) s += ", " + FormatClock(SecondsLeft(now)) + " left";
  if (g.mode == Mode::Tdm) s += ", CT " + std::to_string(g.board.TeamKills(3)) + " : " + std::to_string(g.board.TeamKills(2)) + " T";
  const auto r = g.board.Ranked();
  if (!r.empty() && r[0].kills > 0) s += ", leader " + r[0].name + " (" + std::to_string(r[0].kills) + ")";
  if (!g.weapon.empty()) s += ", " + WeaponLabel(g.weapon) + " round";
  return s + ".";
}

void OnRu(void*, const ru_command_ctx* c) {
  const std::string sub = c->argc >= 3 && c->argv[2] ? Lower(c->argv[2]) : "";
  if (sub.empty() || sub == "help") {
    for (const char* l : {".ru dm ffa [map]: free-for-all deathmatch (admin; default map: .ru map default ffa <map>)",
                          ".ru dm tdm [map]: team deathmatch (admin)", ".ru dm off: back to normal (admin)",
                          ".ru dm status | .ru dm top | .ru dm hud (your leaderboard panel on / off)"}) {
      Reply(c, l);
    }
    return;
  }
  if (sub == "status") return Reply(c, StatusLine(Now()));
  if (sub == "top") {
    if (!Active()) return Reply(c, "deathmatch is off.");
    const auto r = g.board.Ranked();
    if (r.empty()) return Reply(c, "no kills yet.");
    for (size_t i = 0; i < r.size() && i < 5; ++i) {
      const auto& p = r[i];
      const int hs = p.kills > 0 ? p.headshots * 100 / p.kills : 0;
      Reply(c, std::to_string(i + 1) + ". " + p.name + ": " + std::to_string(p.kills) + " kills, " + std::to_string(p.deaths) +
                   " deaths, " + std::to_string(hs) + "% HS");
    }
    if (!c->is_console && c->steamid64) {
      const int rank = g.board.RankOf(c->steamid64);
      if (rank > 5) {
        const PlayerScore* me = g.board.Find(c->steamid64);
        Reply(c, "you: #" + std::to_string(rank) + ", " + std::to_string(me ? me->kills : 0) + " kills");
      }
    }
    return;
  }
  if (sub == "hud") {
    if (c->is_console || !c->steamid64) return Reply(c, ".ru dm hud is for players (it turns your own panel on / off).");
    const bool off = !g_hudOff.count(c->steamid64);
    if (off) {
      g_hudOff.insert(c->steamid64);
      if (HasPrio() && c->slot >= 0) g_api->center_html_release(g_api->self, c->slot);
    } else {
      g_hudOff.erase(c->steamid64);
    }
    return Reply(c, off ? "leaderboard panel off." : "leaderboard panel on.");
  }
  Mode m = Mode::Off;
  const bool enter = ParseMode(sub, &m);
  if (!enter && sub != "off") return Reply(c, "unknown command. Type .ru help dm for the list.");
  if (!IsAdmin(c)) return Reply(c, "not authorized");
  const std::string who = c->is_console ? std::string("Console") : std::string(c->name ? c->name : "?");
  if (sub == "off") {
    if (!Active()) return Reply(c, "deathmatch is already off.");
    Leave(/*reload=*/true, /*tellMatch=*/true, ("dm off by " + who).c_str());
    ChatAll("Ready Up: deathmatch off.");
    return Reply(c, "deathmatch off; the map loads again in normal mode.");
  }
  if (c->argc > 4) return Reply(c, "usage: .ru dm " + sub + " [map]");
  const std::string map = c->argc >= 4 && c->argv[3] ? c->argv[3] : "";
  bool ok = false;
  const std::string r = Enter(m, map, &ok);
  Log("deathmatch: %s by %s: %s", sub.c_str(), who.c_str(), r.c_str());
  if (ok) ChatAll("Ready Up: " + r);
  else Reply(c, r);
}

// ---- engine callbacks -----------------------------------------------------------------------------

void OnPlayerDeath(void*, const char* name, const ru_game_event* ev) {
  if (!Active() || g.mapPending || g.phase != Phase::Playing || !name || !ev) return;
  const ru_api* a = g_api;
  Who victim, attacker;
  if (!WhoBySlot(a->ev_get_player_slot(a->self, ev, "userid"), &victim)) return;
  (void)WhoBySlot(a->ev_get_player_slot(a->self, ev, "attacker"), &attacker);
  const bool hs = a->ev_get_int(a->self, ev, "headshot", 0) != 0;
  g.board.SeePlayer(victim.id, victim.name, victim.team);
  if (attacker.id) g.board.SeePlayer(attacker.id, attacker.name, attacker.team);
  const double now = Now();
  if (!g.board.OnKill(g.mode, attacker.id, attacker.team, victim.id, victim.team, hs, now)) return;
  const Outcome o = CheckEnd(g.mode, g.board, g.settings, Elapsed(now));
  if (o.over) EndGame(now, o);
}

void OnMapStart(void*, const ru_event* e) {
  if (e && e->map && *e->map) readyup::mapnames::NoteMapLoaded(e->map);
  if (!Active()) {
    g.dmMapLoaded = false;
    return;
  }
  // game_type / game_mode stay set across map loads, so any map that loads now is deathmatch.
  g.mapPending = false;
  g.dmMapLoaded = true;
  ApplyRules();
  g.reapplyAt = Now() + kReapplySeconds;
  StartGame(Now(), "map start");
}

// Match_Start: CS2's warmup ended or mp_restartgame (ours or an admin's): a new game.
void OnMatchStart(void*, const ru_event*) {
  if (!Active() || g.mapPending || !g.dmMapLoaded) return;
  if (g.phase == Phase::Playing && Elapsed(Now()) < 2.0) return;  // the game just started
  StartGame(Now(), "game restart");
}

void OnTick(void*, const ru_tick_info* t) {
  if (!Active()) return;
  // The match plugin left its external mode on its own (`.ru mode idle`, a match load).
  if (t->now >= g.nextSync) {
    g.nextSync = t->now + 0.5;
    if (MatchOwnsMode()) {
      const std::string m = RuMode();
      if (m != "external") {
        const bool toIdle = m == "idle" || m == "scrim_warmup";
        ChatAll("Ready Up: deathmatch ended (" + (m.empty() ? std::string("mode changed") : m) + ").");
        // Idle / scrim: this map loads again in the normal mode. A match loads its own map (and
        // the match plugin set competitive before that), practice keeps the map.
        Leave(/*reload=*/toIdle, /*tellMatch=*/false, ("match plugin mode " + m).c_str());
        return;
      }
    }
  }
  if (g.mapPending) {
    if (g.pendingSince == 0) g.pendingSince = t->now;
    if (t->now - g.pendingSince > kMapLoadTimeoutSeconds) {
      ru_logf(g_api, RU_LOG_WARN, "deathmatch: no map start %.0f s after the map change; playing on this map",
              kMapLoadTimeoutSeconds);
      g.mapPending = false;
      g.dmMapLoaded = true;
      ApplyRules();
      StartGame(t->now, "map load timeout");
    }
    return;
  }
  if (g.reapplyAt > 0 && t->now >= g.reapplyAt) {
    g.reapplyAt = 0;
    ApplyRules();  // after server.cfg / the gamemode cfg of the new map
  }
  if (g.phase == Phase::Waiting && g.dmMapLoaded) StartGame(t->now, "resumed");
  if (g.phase == Phase::Playing) {
    const Outcome o = CheckEnd(g.mode, g.board, g.settings, Elapsed(t->now));
    if (o.over) EndGame(t->now, o);
  }
  if (g.phase == Phase::Playing) {
    const WeaponRound w = WeaponRoundAt(g.settings, Elapsed(t->now));
    if (w.index != g.weaponIndex) {
      if (w.index < 0) {
        StopWeaponRound();
        ChatAll("Weapon round over: your own weapons again from your next spawn.");
      } else {
        g.weaponIndex = w.index;
        g.weapon = w.weapon;
        Cmds(WeaponRoundCommands(w.weapon, g.settings.weaponRoundRestrictBuy));
        ChatAll(WeaponLabel(w.weapon) + " round for " + std::to_string(w.secondsLeft) + " s: everyone spawns with it.");
        Log("deathmatch: weapon round %s", w.weapon.c_str());
      }
    }
  }
  if (g.phase == Phase::Ended && t->now >= g.endedAt + g.settings.endDelaySeconds) {
    ReleasePanels();
    if (g.settings.restartReloadsMap) {
      const std::string entry = CurrentReloadEntry();
      if (!entry.empty() && LoadMap(entry)) {
        g.mapPending = true;
        g.pendingSince = t->now;
        g.phase = Phase::Waiting;
        Log("deathmatch: next game: %s loads again", entry.c_str());
        return;
      }
    }
    Cmd("mp_restartgame 1");
    StartGame(t->now + 1.0, "next game");
  }
  SendHud(t->now);
}

// ---- selftest + load ------------------------------------------------------------------------------

void RunSelftest(ru_selftest_add_fn add, void* ctx) {
  std::string l;
  {
    std::lock_guard<std::mutex> lk(g_selftestMu);
    l = g_selftestLine;
  }
  add(ctx, "INFO", "deathmatch", l.c_str());
}
const ru_selftest_iface_v1 g_selftestIface = {sizeof(ru_selftest_iface_v1), &RunSelftest};

struct Stash {
  int mode;
  int mapPending;
  int dmMapLoaded;
  int phase;
  double startedAt;
};

}  // namespace
}  // namespace deathmatch

using namespace deathmatch;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 2u,  // needs API 1.2; the 1.6 panel priorities are checked with RU_API_HAS
      "deathmatch",
      DEATHMATCH_VERSION,
      "Ready Up",
      "deathmatch: free for all / team deathmatch with kill + time limits and a leaderboard (.ru dm)",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, register_ru_subcommand)) return 1;
  g_api = api;
  g = State{};
  g_hudOff.clear();
  LoadConfig();
  // `ru plugin reload deathmatch`: keep the mode, the game clock and the scores.
  Stash st{};
  if (api->stash_get(api->self, "state", &st, sizeof(st)) == static_cast<int>(sizeof(st))) {
    g.mode = st.mode == 1 ? Mode::Ffa : st.mode == 2 ? Mode::Tdm : Mode::Off;
    g.mapPending = st.mapPending != 0;
    g.dmMapLoaded = st.dmMapLoaded != 0;
    g.phase = st.phase == 1 ? Phase::Playing : Phase::Waiting;  // a game-over card is not resumed
    g.startedAt = st.startedAt;
    std::vector<char> buf(1 << 20);
    const int n = api->stash_get(api->self, "board", buf.data(), static_cast<uint32_t>(buf.size()));
    if (n > 0 && n <= static_cast<int>(buf.size())) (void)g.board.Deserialize(std::string(buf.data(), static_cast<size_t>(n)));
    if (Active()) ru_logf(api, RU_LOG_INFO, "resumed %s after a reload", ModeName(g.mode));
  }
  for (const char* n : {"deathmatch", "dm"}) {
    if (!api->register_ru_subcommand(api->self, n, &OnRu, nullptr)) {
      ru_logf(api, RU_LOG_WARN, "could not register `ru %s` (another plugin owns it)", n);
    }
  }
  if (!api->subscribe_game_event(api->self, "player_death", &OnPlayerDeath, nullptr)) {
    ru_logf(api, RU_LOG_WARN, "could not subscribe to player_death: kills are not counted");
  }
  api->subscribe(api->self, RU_EVENT_MAP_START, &OnMapStart, nullptr);
  api->subscribe(api->self, RU_EVENT_MATCH_START, &OnMatchStart, nullptr);
  api->on_tick(api->self, &OnTick, nullptr);
  api->provide_interface(api->self, RU_SELFTEST_IFACE_PREFIX "deathmatch", RU_SELFTEST_IFACE_VERSION,
                         const_cast<ru_selftest_iface_v1*>(&g_selftestIface));
  UpdateSelftestLine();
  ru_logf(api, RU_LOG_INFO, "loaded " DEATHMATCH_VERSION);
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  Stash st{static_cast<int>(g.mode), g.mapPending ? 1 : 0, g.dmMapLoaded ? 1 : 0,
           g.phase == Phase::Playing ? 1 : 0, g.startedAt};
  g_api->stash_put(g_api->self, "state", &st, sizeof(st));
  const std::string board = g.board.Serialize();
  g_api->stash_put(g_api->self, "board", board.data(), static_cast<uint32_t>(std::min<size_t>(board.size(), 1u << 20)));
  ReleasePanels();
  ru_logf(g_api, RU_LOG_INFO, "unloaded");
  g_api = nullptr;
}

}  // extern "C"
