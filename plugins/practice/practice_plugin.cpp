// readyup-practice: practice mode and its tools, as a plugin of its own so a server can run it
// without the match flow (a dedicated practice server) or next to it.
//
//   .prac / .tactics        admin: practice mode on / off. On: ReadyUp/prac.cfg (cheats, full
//                           grenade set, infinite ammo, ...) and everyone respawns with it. Off:
//                           ReadyUp/idle.cfg and everyone respawns without it.
//   ru practice on|off|status   the same from the console / `.ru practice ...` (admin)
//   ru practice as <slot> <.command> [args]   console only: run a tool as that player (bot tests)
//   cfg/ReadyUp/practice.cfg (or readyup.cfg [practice]):
//     always=0              1 = dedicated practice server: practice is switched on at load and on
//                           every map, whenever nothing blocks it
//
// Tools (only in practice mode, never under the valve ruleset, docs/ESPORTS-MODE.md):
//   .rethrow / .rt        sv_rethrow_last_grenade: the last grenade thrown on the SERVER (CS2 has
//                         no per-player variant; with several players it rethrows whoever threw last)
//   .savepos [name]       remember your position (pawn scene node origin), per map
//   .loadpos [name]       move back to it
//   .back                 back to where you threw your last grenade (grenade_thrown), else to
//                         where you were before your last .loadpos / .spawn
//   .clear                ent_remove_all smoke / molotov / decoy projectiles and fires (inferno)
//   .noflash              toggle: your pawn's m_flFlashMaxAlpha 0 (+ m_flFlashDuration 0 on player_blind)
//   .god                  toggle: your pawn's m_bTakesDamage (falls back to toggling buddha)
//   .bot .cbot .boost ... a bot on the other team, moved to where you stand once it spawns
//   .nobots               bot_kick
//   .spawn N / .ctspawn N / .tspawn N   to spawn point N (1-based) of your / the CT / the T team
//
// CS2 1.41.8.4: setpos, setpos_player, setang, ent_setpos, ent_fire and bot_place need a client of
// their own, so from the server console they do nothing. Moves use ru_api entity_set_abs_origin
// (v1.3, engine surface CBaseEntity_SetAbsOrigin). View angles cannot be set for another player.
//
// With the match plugin (readyup.match.v1 set_practice) the match flow owns the mode ("practice"
// in ru_mode, no scrim warmup) and refuses while a match is loaded; without it this plugin keeps
// the flag itself. It publishes readyup.practice.v1 (the match plugin hands `.ru mode practice`
// to it). Log lines: `practice: ...`.
#include "practice_rules.h"

#include "readyup/match_iface.h"
#include "readyup/plugin_api.h"
#include "readyup/practice_iface.h"
#include "readyup/selftest_iface.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef PRACTICE_VERSION
#define PRACTICE_VERSION "dev"
#endif

namespace practice {
namespace {

const ru_api* g_api = nullptr;
const ru_api* A() { return g_api; }

void Log(const char* fmt, ...) RU_PRINTF(1, 2);
void Log(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (g_api) g_api->log_untagged(g_api->self, RU_LOG_INFO, buf);
}

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- the match plugin ----------------------------------------------------------------------------

const ru_match_v1* Match() {
  return static_cast<const ru_match_v1*>(g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1));
}
bool MatchOwnsMode() {
  const ru_match_v1* m = Match();
  return m && RU_API_HAS(m, set_practice) && m->set_practice;
}
std::string RuMode() {
  const ru_match_v1* m = Match();
  if (!m || !m->get_status) return {};
  ru_match_status st{};
  st.struct_size = sizeof(st);
  return m->get_status(&st) == 1 && st.ru_mode ? st.ru_mode : "";
}
std::string Ruleset() {
  const ru_match_v1* m = Match();
  if (m && RU_API_HAS(m, ruleset) && m->ruleset && m->ruleset()) return m->ruleset();
  char buf[32] = {};
  return g_api->config_get(g_api->self, "ruleset", buf, sizeof(buf)) > 0 ? buf : "default";
}

bool g_standaloneActive = false;  // practice on, when no match plugin owns the mode
bool g_manualOff = false;         // an admin turned it off: always=1 waits for the next map
bool IsActive() { return MatchOwnsMode() ? RuMode() == "practice" : g_standaloneActive; }

// ---- schema -------------------------------------------------------------------------------------

struct Offsets {
  bool looked = false;
  int ctrlPawn = -1;       // CCSPlayerController::m_hPlayerPawn
  int bodyComponent = -1;  // CBaseEntity::m_CBodyComponent (pointer)
  int sceneNode = -1;      // CBodyComponent::m_pSceneNode (pointer)
  int absOrigin = -1;      // CGameSceneNode::m_vecAbsOrigin
  int lifeState = -1;      // CBaseEntity::m_lifeState
  int eyeAngles = -1;      // CCSPlayerPawn::m_angEyeAngles
  int flashAlpha = -1;     // CCSPlayerPawn::m_flFlashMaxAlpha
  int flashDuration = -1;  // CCSPlayerPawn::m_flFlashDuration
  int takesDamage = -1;    // CBaseEntity::m_bTakesDamage
  int spawnEnabled = -1;   // SpawnPoint::m_bEnabled
  int spawnPriority = -1;  // SpawnPoint::m_iPriority
};
Offsets g_off;

int FirstOffset(std::initializer_list<const char*> classes, const char* field) {
  for (const char* c : classes) {
    const int off = g_api->schema_offset(g_api->self, c, field);
    if (off >= 0) return off;
  }
  return -1;
}

const Offsets& Off() {
  Offsets& o = g_off;
  if (o.looked || g_api->entity_system_status(g_api->self) != RU_ENTSYS_OK) return o;
  o.looked = true;
  o.ctrlPawn = FirstOffset({"CCSPlayerController"}, "m_hPlayerPawn");
  o.bodyComponent = FirstOffset({"CBaseEntity"}, "m_CBodyComponent");
  o.sceneNode = FirstOffset({"CBodyComponent"}, "m_pSceneNode");
  o.absOrigin = FirstOffset({"CGameSceneNode"}, "m_vecAbsOrigin");
  o.lifeState = FirstOffset({"CBaseEntity"}, "m_lifeState");
  o.eyeAngles = FirstOffset({"CCSPlayerPawn", "CCSPlayerPawnBase"}, "m_angEyeAngles");
  o.flashAlpha = FirstOffset({"CCSPlayerPawn", "CCSPlayerPawnBase"}, "m_flFlashMaxAlpha");
  o.flashDuration = FirstOffset({"CCSPlayerPawn", "CCSPlayerPawnBase"}, "m_flFlashDuration");
  o.takesDamage = FirstOffset({"CBaseEntity"}, "m_bTakesDamage");
  o.spawnEnabled = FirstOffset({"SpawnPoint", "CInfoPlayerCounterterrorist"}, "m_bEnabled");
  o.spawnPriority = FirstOffset({"SpawnPoint", "CInfoPlayerCounterterrorist"}, "m_iPriority");
  Log("practice: schema pawn=%d body=%d node=%d origin=%d life=%d eyes=%d flash=%d/%d takesdamage=%d spawn=%d/%d",
      o.ctrlPawn, o.bodyComponent, o.sceneNode, o.absOrigin, o.lifeState, o.eyeAngles, o.flashAlpha, o.flashDuration,
      o.takesDamage, o.spawnEnabled, o.spawnPriority);
  return o;
}

template <typename T>
T Rd(const void* base, int off) {
  T v{};
  std::memcpy(&v, static_cast<const unsigned char*>(base) + off, sizeof(T));
  return v;
}
template <typename T>
void Wr(void* base, int off, T v) {
  std::memcpy(static_cast<unsigned char*>(base) + off, &v, sizeof(T));
}

struct Vec3 {
  float x = 0, y = 0, z = 0;
};

bool Finite(const Vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) < 65536.f &&
         std::fabs(v.y) < 65536.f && std::fabs(v.z) < 65536.f;
}

void* PawnForSlot(int slot) {
  const Offsets& o = Off();
  if (slot < 0 || o.ctrlPawn < 0) return nullptr;
  void* ctrl = g_api->entity_by_index(g_api->self, slot + 1);
  if (!ctrl) return nullptr;
  const char* cls = g_api->entity_classname(g_api->self, ctrl);
  if (!cls || std::strcmp(cls, "cs_player_controller") != 0) return nullptr;
  return g_api->entity_from_handle(g_api->self, Rd<uint32_t>(ctrl, o.ctrlPawn));
}

bool Alive(void* pawn) {
  const Offsets& o = Off();
  return pawn && (o.lifeState < 0 || Rd<uint8_t>(pawn, o.lifeState) == 0);
}

bool OriginOf(void* ent, Vec3* out) {
  const Offsets& o = Off();
  if (!ent || o.bodyComponent < 0 || o.sceneNode < 0 || o.absOrigin < 0) return false;
  void* body = Rd<void*>(ent, o.bodyComponent);
  if (!body) return false;
  void* node = Rd<void*>(body, o.sceneNode);
  if (!node) return false;
  *out = Rd<Vec3>(node, o.absOrigin);
  return Finite(*out);
}

struct Spot {
  Vec3 pos;
  Vec3 ang;  // pitch yaw roll (informational: CS2 cannot set another player's view)
  bool set = false;
};

bool SpotOfSlot(int slot, Spot* out) {
  void* pawn = PawnForSlot(slot);
  if (!Alive(pawn) || !OriginOf(pawn, &out->pos)) return false;
  const Offsets& o = Off();
  out->ang = o.eyeAngles >= 0 ? Rd<Vec3>(pawn, o.eyeAngles) : Vec3{};
  out->set = true;
  return true;
}

bool Teleport(int slot, const Vec3& p) {
  if (!RU_API_HAS(g_api, entity_set_abs_origin) || !g_api->entity_set_abs_origin) return false;
  void* pawn = PawnForSlot(slot);
  if (!Alive(pawn)) return false;
  const float xyz[3] = {p.x, p.y, p.z};
  return g_api->entity_set_abs_origin(g_api->self, pawn, xyz) == 1;
}

constexpr const char* kNoTeleport =
    "cannot move you: teleport is unavailable on this server build (ru selftest: CBaseEntity::SetAbsOrigin).";

std::string Fmt(const Vec3& p) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%.1f %.1f %.1f", p.x, p.y, p.z);
  return buf;
}

// ---- players --------------------------------------------------------------------------------------

// A player by slot (humans: engine slot; bots: their log <N>, which is the slot in CS2).
bool PlayerBySlot(int slot, ru_player* out) {
  struct Ctx {
    int slot;
    ru_player* out;
    bool found;
  } c{slot, out, false};
  g_api->for_each_player(
      g_api->self,
      [](void* u, const ru_player* p) -> int {
        auto* c = static_cast<Ctx*>(u);
        if ((p->slot >= 0 ? p->slot : p->userid) != c->slot) return 1;
        std::memcpy(c->out, p, std::min<size_t>(sizeof(ru_player), p->struct_size));
        c->found = true;
        return 0;
      },
      &c);
  return c.found;
}

int TeamOfSlot(int slot) {
  ru_player p{};
  p.struct_size = sizeof(p);
  return PlayerBySlot(slot, &p) ? p.team : 0;
}

std::set<int> BotUserids() {
  std::set<int> out;
  g_api->for_each_player(
      g_api->self,
      [](void* u, const ru_player* p) -> int {
        if (p->is_bot && p->userid >= 0) static_cast<std::set<int>*>(u)->insert(p->userid);
        return 1;
      },
      &out);
  return out;
}

// ---- state (game thread only) ---------------------------------------------------------------------

std::string g_map;
std::map<uint64_t, std::map<std::string, Spot>> g_saved;  // player id -> name -> spot
std::unordered_map<uint64_t, Spot> g_lastThrow;          // grenade_thrown position
std::unordered_map<uint64_t, Spot> g_beforeTeleport;     // where .loadpos / .spawn took them from
std::unordered_set<int> g_noflash;                        // slots
std::unordered_set<int> g_god;                            // slots (m_bTakesDamage false)
bool g_buddhaOff = false;                                 // .god fallback toggled buddha off
bool g_touched = false;                                   // any of the above set this session

struct PendingBot {
  int callerSlot = -1;
  Vec3 pos;
  std::set<int> before;  // bot userids when the bot was requested
  double deadline = 0;
};
std::vector<PendingBot> g_pendingBots;

// Humans by SteamID64, bots by a pseudo id from their slot (never a real SteamID).
uint64_t PlayerId(int slot, uint64_t steamid64) { return steamid64 ? steamid64 : 0xB0B0000000000000ull | static_cast<uint32_t>(slot); }

void Cmd(const char* c) { g_api->server_command(g_api->self, c); }
void ChatAll(const std::string& msg) { g_api->chat_all(g_api->self, msg.c_str(), 0); }

void Reply(int slot, uint64_t steamid64, const std::string& msg) {
  Log("practice: slot %d: %s", slot, msg.c_str());
  if (steamid64 != 0 && slot >= 0) g_api->chat_to_slot(g_api->self, slot, (" \x04[ReadyUp]\x01 " + msg).c_str());
}

void ClearState(bool restorePawns) {
  if (restorePawns) {
    const Offsets& o = Off();
    for (int slot : g_god) {
      if (void* pawn = PawnForSlot(slot); pawn && o.takesDamage >= 0) Wr<bool>(pawn, o.takesDamage, true);
    }
    for (int slot : g_noflash) {
      if (void* pawn = PawnForSlot(slot); pawn && o.flashAlpha >= 0) {
        Wr<float>(pawn, o.flashAlpha, 255.f);
        g_api->entity_mark_changed(g_api->self, pawn);
      }
    }
  }
  g_saved.clear();
  g_lastThrow.clear();
  g_beforeTeleport.clear();
  g_noflash.clear();
  g_god.clear();
  g_pendingBots.clear();
  g_buddhaOff = false;
  g_touched = false;
}

// ---- mode ---------------------------------------------------------------------------------------

bool Enter(const char** why) {
  if (MatchOwnsMode()) {
    if (Match()->set_practice(1) != 1) {
      if (why) *why = "a match is loaded (.ru match end first)";
      return false;
    }
  } else {
    g_standaloneActive = true;
  }
  g_manualOff = false;
  Cmd("exec ReadyUp/prac.cfg");
  // Respawn everyone so they get prac.cfg's grenade set and weapons now, not on their next death.
  Cmd("mp_restartgame 1");
  Log("practice: on (%s)", MatchOwnsMode() ? "match plugin mode" : "standalone");
  return true;
}

void Leave() {
  if (MatchOwnsMode()) (void)Match()->set_practice(0);
  g_standaloneActive = false;
  Cmd("exec ReadyUp/idle.cfg");
  // Everyone respawns with the normal loadout: the practice grenades and rifles go.
  Cmd("mp_restartgame 1");
  ClearState(/*restorePawns=*/true);
  Log("practice: off");
}

// ---- tools --------------------------------------------------------------------------------------

struct SpawnPt {
  int index;
  int priority;
  Vec3 pos;
};

std::vector<SpawnPt> ListSpawns(int team) {
  std::vector<SpawnPt> out;
  const Offsets& o = Off();
  const char* want = team == 3 ? "info_player_counterterrorist" : "info_player_terrorist";
  // Every entity slot: server-only entities (spawn points) sit above the 16384 networked ones.
  for (int i = 0; i < 32768; ++i) {
    void* e = g_api->entity_by_index(g_api->self, i);
    if (!e) continue;
    const char* cls = g_api->entity_classname(g_api->self, e);
    if (!cls || std::strcmp(cls, want) != 0) continue;
    if (o.spawnEnabled >= 0 && !Rd<bool>(e, o.spawnEnabled)) continue;
    SpawnPt s{i, o.spawnPriority >= 0 ? Rd<int32_t>(e, o.spawnPriority) : 0, {}};
    if (!OriginOf(e, &s.pos)) continue;
    out.push_back(s);
  }
  std::sort(out.begin(), out.end(), [](const SpawnPt& x, const SpawnPt& y) {
    return x.priority != y.priority ? x.priority < y.priority : x.index < y.index;
  });
  return out;
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool TeleportRemembering(int slot, uint64_t id, const Vec3& to) {
  Spot here;
  const bool had = SpotOfSlot(slot, &here);
  if (!Teleport(slot, to)) return false;
  if (had) g_beforeTeleport[id] = here;
  g_touched = true;
  return true;
}

// Refusal reply, or "" when tools may run.
std::string ToolsRefusal() {
  if (!IsActive()) return "only works in practice mode (.prac).";
  if (!ToolsAllowed(true, Ruleset())) return "not available under the valve ruleset.";
  return {};
}

void RunTool(int slot, uint64_t steamid64, const std::vector<std::string>& args) {
  const std::string cmd = Lower(args[0]);
  if (const std::string why = ToolsRefusal(); !why.empty()) return Reply(slot, steamid64, cmd + " " + why);
  const uint64_t id = PlayerId(slot, steamid64);
  const std::string name = args.size() > 1 ? args[1].substr(0, 32) : std::string("default");

  if (cmd == ".rethrow" || cmd == ".rt") {
    Cmd("sv_rethrow_last_grenade");
    return Reply(slot, steamid64, "rethrowing the last grenade thrown on the server.");
  }
  if (cmd == ".clear") {
    // ent_remove_all works from the server console; ent_fire needs a client of its own there.
    for (const char* c : {"ent_remove_all smokegrenade_projectile", "ent_remove_all molotov_projectile",
                          "ent_remove_all inferno", "ent_remove_all decoy_projectile"}) {
      Cmd(c);
    }
    return Reply(slot, steamid64, "smokes, molotovs and decoys cleared.");
  }
  if (cmd == ".savepos") {
    Spot s;
    if (!SpotOfSlot(slot, &s)) return Reply(slot, steamid64, "cannot read your position (are you alive?).");
    g_saved[id][name] = s;
    g_touched = true;
    return Reply(slot, steamid64, "position \"" + name + "\" saved (" + Fmt(s.pos) + ").");
  }
  if (cmd == ".loadpos") {
    auto pit = g_saved.find(id);
    if (pit == g_saved.end() || !pit->second.count(name)) {
      std::string have;
      if (pit != g_saved.end()) {
        for (const auto& kv : pit->second) have += (have.empty() ? "" : ", ") + kv.first;
      }
      return Reply(slot, steamid64,
                   "no saved position \"" + name + "\"" + (have.empty() ? " (.savepos [name] first)." : " (saved: " + have + ")."));
    }
    const Spot s = pit->second[name];
    if (!TeleportRemembering(slot, id, s.pos)) return Reply(slot, steamid64, kNoTeleport);
    return Reply(slot, steamid64, "loaded position \"" + name + "\" (" + Fmt(s.pos) + "; view angle not restored).");
  }
  if (cmd == ".back") {
    const Spot* s = nullptr;
    const char* what = "";
    if (auto t = g_lastThrow.find(id); t != g_lastThrow.end()) {
      s = &t->second;
      what = "your last grenade throw";
    } else if (auto b = g_beforeTeleport.find(id); b != g_beforeTeleport.end()) {
      s = &b->second;
      what = "where you were before the last teleport";
    }
    if (!s) return Reply(slot, steamid64, "nothing to go back to (throw a grenade or use .loadpos / .spawn first).");
    const Vec3 to = s->pos;
    if (!Teleport(slot, to)) return Reply(slot, steamid64, kNoTeleport);
    return Reply(slot, steamid64, std::string("back to ") + what + " (" + Fmt(to) + ").");
  }
  if (cmd == ".noflash") {
    const Offsets& o = Off();
    void* pawn = PawnForSlot(slot);
    if (o.flashAlpha < 0 || !pawn) return Reply(slot, steamid64, "noflash is unavailable (pawn / schema field not found).");
    const bool on = !g_noflash.count(slot);
    if (on) g_noflash.insert(slot);
    else g_noflash.erase(slot);
    Wr<float>(pawn, o.flashAlpha, on ? 0.f : 255.f);
    g_api->entity_mark_changed(g_api->self, pawn);
    g_touched = true;
    return Reply(slot, steamid64, on ? "noflash on." : "noflash off.");
  }
  if (cmd == ".god") {
    const Offsets& o = Off();
    void* pawn = PawnForSlot(slot);
    if (o.takesDamage >= 0 && pawn) {
      const bool on = !g_god.count(slot);
      if (on) g_god.insert(slot);
      else g_god.erase(slot);
      Wr<bool>(pawn, o.takesDamage, !on);
      g_touched = true;
      return Reply(slot, steamid64, on ? "god mode on (you take no damage)." : "god mode off.");
    }
    // Fallback: buddha (server-wide, prac.cfg turns it on).
    g_buddhaOff = !g_buddhaOff;
    Cmd(g_buddhaOff ? "buddha 0" : "buddha 1");
    g_touched = true;
    return Reply(slot, steamid64, g_buddhaOff ? "buddha off for everyone." : "buddha on for everyone (nobody dies).");
  }
  if (cmd == ".spawn" || cmd == ".ctspawn" || cmd == ".tspawn") {
    const int team = cmd == ".ctspawn" ? 3 : cmd == ".tspawn" ? 2 : TeamOfSlot(slot);
    if (team != 2 && team != 3) return Reply(slot, steamid64, "join CT or T first (or use .ctspawn / .tspawn).");
    const int n = args.size() > 1 ? std::atoi(args[1].c_str()) : 1;
    const auto spawns = ListSpawns(team);
    const char* side = team == 3 ? "CT" : "T";
    if (spawns.empty()) return Reply(slot, steamid64, std::string("no ") + side + " spawn points found on this map.");
    if (n < 1 || n > static_cast<int>(spawns.size())) {
      return Reply(slot, steamid64, std::string(side) + " spawns: 1-" + std::to_string(spawns.size()) + ".");
    }
    const Vec3 to = spawns[static_cast<size_t>(n - 1)].pos;
    if (!TeleportRemembering(slot, id, to)) return Reply(slot, steamid64, kNoTeleport);
    return Reply(slot, steamid64, std::string(side) + " spawn " + std::to_string(n) + "/" +
                                      std::to_string(spawns.size()) + " (" + Fmt(to) + ").");
  }
}

void RunBot(int slot, uint64_t steamid64, const std::string& cmd) {
  if (const std::string why = ToolsRefusal(); !why.empty()) return Reply(slot, steamid64, cmd + " " + why);
  if (cmd == ".nobots") {
    Cmd("bot_kick");
    g_pendingBots.clear();
    ChatAll("Ready Up: bots removed.");
    return;
  }
  // One bot on the other team (CT player -> T bot, else CT bot), placed where the caller stands.
  const int tn = slot >= 0 ? TeamOfSlot(slot) : 0;
  const bool crouch = (cmd == ".cbot" || cmd == ".crouchbot" || cmd == ".crouchboost");
  if (crouch) Cmd("bot_crouch 1");
  if (tn == 3) {
    Cmd("bot_join_team T");
    Cmd("bot_add_t");
  } else {
    Cmd("bot_join_team CT");
    Cmd("bot_add_ct");
  }
  Cmd("bot_stop 1");
  Cmd("bot_freeze 1");
  Cmd("bot_zombie 1");
  if (crouch) Cmd("bot_crouch 0");  // future bots are not forced to crouch
  Spot s;
  if (slot >= 0 && SpotOfSlot(slot, &s)) {
    PendingBot p;
    p.callerSlot = slot;
    p.pos = s.pos;
    p.before = BotUserids();
    p.deadline = Now() + 5.0;
    g_pendingBots.push_back(std::move(p));
    g_touched = true;
  }
  ChatAll(crouch ? "Ready Up: crouch bot added." : "Ready Up: bot added.");
}

// ---- commands ------------------------------------------------------------------------------------

// always=1 in cfg/ReadyUp/practice.cfg or readyup.cfg [practice].
bool AlwaysOn() {
  char b[16] = {};
  return g_api->config_get(g_api->self, "always", b, sizeof(b)) > 0 && ParseBool(b, false);
}
double g_nextAlwaysCheck = 0;

std::vector<std::string> Args(const ru_command_ctx* c, int from) {
  std::vector<std::string> out;
  for (int i = from; i < c->argc; ++i) out.emplace_back(c->argv[i] ? c->argv[i] : "");
  return out;
}

int SenderSlot(const ru_command_ctx* c) {
  return c->slot >= 0 ? c->slot : (c->steamid64 ? g_api->slot_for_steamid(g_api->self, c->steamid64) : -1);
}

void OnToolChat(void*, const ru_command_ctx* c) {
  const auto args = Args(c, 0);
  if (args.empty()) return;
  const int slot = SenderSlot(c);
  if (slot < 0) return Log("practice: %s from %s: slot unknown", args[0].c_str(), c->name ? c->name : "?");
  if (IsBotCommand(args[0])) RunBot(slot, c->steamid64, Lower(args[0]));
  else RunTool(slot, c->steamid64, args);
}

bool Toggle(bool on, std::string* reply) {
  if (on == IsActive()) {
    *reply = on ? "practice mode is already on." : "practice mode is already off.";
    return true;
  }
  if (!on) {
    g_manualOff = true;
    Leave();
    *reply = "practice mode disabled.";
    return true;
  }
  const char* why = "";
  if (!Enter(&why)) {
    *reply = std::string("practice mode refused: ") + why + ".";
    return false;
  }
  *reply = "practice mode enabled.";
  return true;
}

void OnPracChat(void*, const ru_command_ctx* c) {
  const int slot = SenderSlot(c);
  if (g_api->is_admin(g_api->self, c->steamid64) != 1) {
    if (slot >= 0) g_api->chat_to_slot(g_api->self, slot, " \x04[ReadyUp]\x01 not authorized");
    return;
  }
  std::string reply;
  (void)Toggle(!IsActive(), &reply);
  ChatAll("Ready Up: " + reply);
}

// `ru practice on|off|status` and `ru practice as <slot> <.command> [args]`.
void OnRu(void*, const ru_command_ctx* c) {
  const std::string sub = c->argc >= 3 && c->argv[2] ? Lower(c->argv[2]) : "";
  auto reply = [&](const std::string& m) {
    if (c->is_console) Log("%s", m.c_str());
    else if (c->slot >= 0) g_api->chat_to_slot(g_api->self, c->slot, (" \x04[ReadyUp]\x01 " + m).c_str());
  };
  if (sub.empty() || sub == "help") {
    for (const char* l : {".ru practice on|off: practice mode (admin; also .prac)", ".ru practice status",
                          "ru practice as <slot> <.command>: run a practice tool as that player (console)"}) {
      reply(l);
    }
    return;
  }
  if (sub == "status") {
    reply(std::string("practice ") + (IsActive() ? "on" : "off") +
          (MatchOwnsMode() ? " (match plugin mode)" : " (standalone)") + ", always=" + (AlwaysOn() ? "1" : "0"));
    return;
  }
  if (!c->is_console && g_api->is_admin(g_api->self, c->steamid64) != 1) return reply("not authorized");
  if (sub == "on" || sub == "off") {
    std::string r;
    (void)Toggle(sub == "on", &r);
    return reply(r);
  }
  if (sub == "as") {
    if (!c->is_console) return reply(".ru practice as is console / RCON only.");
    if (c->argc < 5) return reply("usage: ru practice as <slot> <.command> [args]");
    const int slot = std::atoi(c->argv[3]);
    ru_player p{};
    p.struct_size = sizeof(p);
    if (!PlayerBySlot(slot, &p)) return reply("ru practice as: no player in slot " + std::to_string(slot));
    const auto args = Args(c, 4);
    const std::string cmd = Lower(args[0]);
    Log("ru practice as: slot %d (%s%s) runs %s", slot, p.name, p.is_bot ? ", bot" : "", cmd.c_str());
    if (IsToolCommand(cmd)) RunTool(slot, p.is_bot ? 0 : p.steamid64, args);
    else if (IsBotCommand(cmd)) RunBot(slot, p.is_bot ? 0 : p.steamid64, cmd);
    else reply("ru practice as: " + cmd + " is not a practice command");
    return;
  }
  reply("unknown command. Type .ru help practice for the list.");
}

// ---- engine callbacks ---------------------------------------------------------------------------

void OnGameEvent(void*, const char* evName, const ru_game_event* ev) {
  if (!evName || !ev || !IsActive()) return;
  const ru_api* a = A();
  if (std::strcmp(evName, "grenade_thrown") == 0) {
    const int slot = a->ev_get_player_slot(a->self, ev, "userid");
    if (slot < 0) return;
    Spot s;
    if (!SpotOfSlot(slot, &s)) return;
    ru_player p{};
    p.struct_size = sizeof(p);
    g_lastThrow[PlayerId(slot, PlayerBySlot(slot, &p) ? p.steamid64 : 0)] = s;
  } else if (std::strcmp(evName, "player_blind") == 0) {
    const int slot = a->ev_get_player_slot(a->self, ev, "userid");
    if (slot < 0 || !g_noflash.count(slot)) return;
    void* pawn = a->ev_get_player_pawn(a->self, ev, "userid");
    const Offsets& o = Off();
    if (!pawn) return;
    if (o.flashAlpha >= 0) Wr<float>(pawn, o.flashAlpha, 0.f);
    if (o.flashDuration >= 0) Wr<float>(pawn, o.flashDuration, 0.f);
    a->entity_mark_changed(a->self, pawn);
  } else if (std::strcmp(evName, "player_spawn") == 0) {
    // A new pawn (respawn) starts with the default values: apply the toggles again.
    const int slot = a->ev_get_player_slot(a->self, ev, "userid");
    if (slot < 0 || (!g_god.count(slot) && !g_noflash.count(slot))) return;
    void* pawn = a->ev_get_player_pawn(a->self, ev, "userid");
    const Offsets& o = Off();
    if (!pawn) return;
    if (g_god.count(slot) && o.takesDamage >= 0) Wr<bool>(pawn, o.takesDamage, false);
    if (g_noflash.count(slot) && o.flashAlpha >= 0) {
      Wr<float>(pawn, o.flashAlpha, 0.f);
      a->entity_mark_changed(a->self, pawn);
    }
  }
}

void OnMapStart(void*, const ru_event*) {
  // New map: new entities and pawns. A standalone practice server keeps practice on (the map load
  // may have run other cfgs); with the match plugin, always=1 re-enters from the tick.
  g_off = Offsets{};
  ClearState(/*restorePawns=*/false);
  g_manualOff = false;
  if (!MatchOwnsMode() && g_standaloneActive) Cmd("exec ReadyUp/prac.cfg");
}


void OnTick(void*, const ru_tick_info* t) {
  if (t->now >= g_nextAlwaysCheck) {
    g_nextAlwaysCheck = t->now + 2.0;
    if (!g_manualOff && ShouldAutoEnter(AlwaysOn(), IsActive(), RuMode())) {
      const char* why = "";
      if (Enter(&why)) Log("practice: always=1: practice mode switched on");
    }
  }
  if (!IsActive()) {
    if (g_touched) {
      ClearState(/*restorePawns=*/true);
      Log("practice: left practice mode; positions and toggles cleared");
    }
    return;
  }
  if (g_pendingBots.empty()) return;
  const auto bots = BotUserids();
  for (auto it = g_pendingBots.begin(); it != g_pendingBots.end();) {
    bool done = false;
    for (int b : bots) {
      if (it->before.count(b)) continue;
      if (!Alive(PawnForSlot(b))) continue;
      const bool moved = Teleport(b, it->pos);
      Log("practice: bot slot %d %s slot %d's position (%s)", b, moved ? "placed at" : "could not be moved to",
          it->callerSlot, Fmt(it->pos).c_str());
      it->before.insert(b);
      done = true;
      break;
    }
    if (done) {
      it = g_pendingBots.erase(it);
    } else if (t->now > it->deadline) {
      Log("practice: new bot did not spawn within 5s; left at its spawn");
      it = g_pendingBots.erase(it);
    } else {
      ++it;
    }
  }
}

// ---- readyup.practice.v1 + selftest ----------------------------------------------------------------

int IfaceActive() { return IsActive() ? 1 : 0; }
int IfaceSetActive(int on, const char** why) {
  std::string r;
  const bool ok = Toggle(on != 0, &r);
  if (!ok && why) *why = "a match is loaded (.ru match end first)";
  return ok ? 1 : 0;
}
const char* IfaceHelp() { return HelpLine(); }
const ru_practice_v1 g_iface = {sizeof(ru_practice_v1), &IfaceActive, &IfaceSetActive, &IfaceHelp};

void RunSelftest(ru_selftest_add_fn add, void* ctx) {
  std::string d = std::string(IsActive() ? "on" : "off") + (MatchOwnsMode() ? " (match plugin mode)" : " (standalone)");
  add(ctx, "INFO", "practice", d.c_str());
  add(ctx, RU_API_HAS(g_api, entity_set_abs_origin) && g_api->entity_set_abs_origin ? "OK" : "WARN", "practice teleport",
      "ru_api entity_set_abs_origin (.loadpos / .spawn / bot placement)");
}
const ru_selftest_iface_v1 g_selftestIface = {sizeof(ru_selftest_iface_v1), &RunSelftest};

}  // namespace
}  // namespace practice

using namespace practice;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 2u,  // needs API 1.2; teleport (1.3 entity_set_abs_origin) is checked with RU_API_HAS
      "practice",
      PRACTICE_VERSION,
      "Ready Up",
      "practice mode + tools (.prac, .savepos/.loadpos, .spawn, .rethrow, .bot, ...)",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, register_ru_subcommand)) return 1;
  g_api = api;
  g_off = Offsets{};
  ClearState(false);
  g_nextAlwaysCheck = 0;
  g_manualOff = false;
  g_standaloneActive = false;
  (void)api->stash_get(api->self, "standalone_active", &g_standaloneActive, sizeof(g_standaloneActive));
  if (!api->register_chat_command(api->self, ".prac", &OnPracChat, nullptr) ||
      !api->register_chat_command(api->self, ".tactics", &OnPracChat, nullptr)) {
    ru_logf(api, RU_LOG_WARN, "could not register .prac (another plugin owns it)");
  }
  for (const char* c : {".rethrow", ".rt", ".savepos", ".loadpos", ".back", ".clear", ".noflash", ".god", ".spawn",
                        ".ctspawn", ".tspawn", ".bot", ".cbot", ".crouchbot", ".boost", ".crouchboost", ".nobots"}) {
    if (!api->register_chat_command(api->self, c, &OnToolChat, nullptr)) ru_logf(api, RU_LOG_WARN, "could not register %s", c);
  }
  api->register_ru_subcommand(api->self, "practice", &OnRu, nullptr);
  for (const char* e : {"grenade_thrown", "player_blind", "player_spawn"}) {
    if (!api->subscribe_game_event(api->self, e, &OnGameEvent, nullptr)) ru_logf(api, RU_LOG_WARN, "could not subscribe to %s", e);
  }
  api->subscribe(api->self, RU_EVENT_MAP_START, &OnMapStart, nullptr);
  api->on_tick(api->self, &OnTick, nullptr);
  api->provide_interface(api->self, RU_PRACTICE_IFACE_NAME, RU_PRACTICE_IFACE_VERSION, const_cast<ru_practice_v1*>(&g_iface));
  api->provide_interface(api->self, RU_SELFTEST_IFACE_PREFIX "practice", RU_SELFTEST_IFACE_VERSION,
                         const_cast<ru_selftest_iface_v1*>(&g_selftestIface));
  ru_logf(api, RU_LOG_INFO, "loaded " PRACTICE_VERSION);
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  // Hot reload keeps a standalone practice mode on; toggles are restored on the pawns.
  g_api->stash_put(g_api->self, "standalone_active", &g_standaloneActive, sizeof(g_standaloneActive));
  ClearState(/*restorePawns=*/true);
  ru_logf(g_api, RU_LOG_INFO, "unloaded");
  g_api = nullptr;
}

}  // extern "C"
