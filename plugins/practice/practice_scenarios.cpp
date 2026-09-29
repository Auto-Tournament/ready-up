// readyup-practice scenarios ("pro round replay"), engine side. Usage and schema: docs/SCENARIOS.md;
// the pure parts (parsing, interpolation, scheduling) are scenario.{h,cpp}.
//
// How a replay runs:
//   load     bot_kick, the scenario cvars (bot_stop 1, bot_ignore_players 1, no respawn, buddha 0,
//            mp_ignore_round_win_conditions 1), one bot per recorded player alive at the start (minus
//            the one the player takes), then mp_restartgame 1 so everyone (the player too) is alive.
//   setup    once the bots are in: everyone to their recorded position (bots also their view angles),
//            health, armor, helmet, defuser, money and items as recorded at the start; smokes and fires
//            already up at the start are dropped where they landed.
//   replay   every tick each scripted bot is moved along its recorded path (entity_set_abs_origin +
//            m_angEyeAngles). Grenades are thrown at their recorded tick from their recorded spawn point
//            and velocity (ru_api grenade_spawn, 1.12; see SpawnGrenade). Recorded deaths kill the bot (bot_kill) unless the
//            player's pro was the killer. Bomb events go to chat.
//   handoff  a bot the player is spotted by (the player pawn's m_entitySpottedState mask, what the radar
//            uses) or that the player hurts stops following its path. The first handoff turns the bot AI
//            on for everyone (bot_stop 0, bot_ignore_players 0): handed-over bots fight like normal bots,
//            the rest keep following their paths (they may aim and shoot while doing so). Damage between
//            bots is undone while both still follow the script.
//   end      at the end of the recording every bot is handed over; `.scen r` replays, `.scen stop` ends.
#include "practice_scenarios.h"

#include "scenario.h"

#include <dirent.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace practice::scenarios {
namespace {

namespace sc = practice::scenario;

const ru_api* g_api = nullptr;
Host g_host{};
std::vector<ru_handle> g_handles;

void Log(const char* fmt, ...) RU_PRINTF(1, 2);
void Log(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (g_api) g_api->log_untagged(g_api->self, RU_LOG_INFO, buf);
}

void Cmd(const std::string& c) { g_api->server_command(g_api->self, c.c_str()); }
void ChatAll(const std::string& m) { g_api->chat_all(g_api->self, m.c_str(), 0); }

// Where a command's replies go: the console, or one player's chat.
struct Out {
  bool console = true;
  int slot = -1;
  void operator()(const std::string& m) const {
    if (console || slot < 0) Log("scenario: %s", m.c_str());
    else g_api->chat_to_slot(g_api->self, slot, (" \x04[ReadyUp]\x01 " + m).c_str());
  }
};

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// ---- schema -------------------------------------------------------------------------------------

struct Offsets {
  bool looked = false;
  int ctrlPawn = -1;       // CCSPlayerController::m_hPlayerPawn
  int money = -1;          // CCSPlayerController::m_pInGameMoneyServices (pointer)
  int account = -1;        // CCSPlayerController_InGameMoneyServices::m_iAccount
  int lifeState = -1;      // CBaseEntity::m_lifeState
  int health = -1;         // CBaseEntity::m_iHealth
  int owner = -1;          // CBaseEntity::m_hOwnerEntity
  int absVelocity = -1;    // CBaseEntity::m_vecAbsVelocity
  int bodyComponent = -1;  // CBaseEntity::m_CBodyComponent
  int sceneNode = -1;      // CBodyComponent::m_pSceneNode
  int absOrigin = -1;      // CGameSceneNode::m_vecAbsOrigin
  int eyeAngles = -1;      // CCSPlayerPawn::m_angEyeAngles
  int armor = -1;          // CCSPlayerPawn::m_ArmorValue
  int itemServices = -1;   // CBasePlayerPawn::m_pItemServices (pointer)
  int helmet = -1;         // CCSPlayer_ItemServices::m_bHasHelmet
  int defuser = -1;        // CCSPlayer_ItemServices::m_bHasDefuser
  int spotted = -1;        // CCSPlayerPawn::m_entitySpottedState
  int spottedMask = -1;    // EntitySpottedState_t::m_bSpottedByMask (uint32[2], bit = player slot)
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
  o.money = FirstOffset({"CCSPlayerController"}, "m_pInGameMoneyServices");
  o.account = FirstOffset({"CCSPlayerController_InGameMoneyServices"}, "m_iAccount");
  o.lifeState = FirstOffset({"CBaseEntity"}, "m_lifeState");
  o.health = FirstOffset({"CBaseEntity"}, "m_iHealth");
  o.owner = FirstOffset({"CBaseEntity"}, "m_hOwnerEntity");
  o.absVelocity = FirstOffset({"CBaseEntity"}, "m_vecAbsVelocity");
  o.bodyComponent = FirstOffset({"CBaseEntity"}, "m_CBodyComponent");
  o.sceneNode = FirstOffset({"CBodyComponent"}, "m_pSceneNode");
  o.absOrigin = FirstOffset({"CGameSceneNode"}, "m_vecAbsOrigin");
  o.eyeAngles = FirstOffset({"CCSPlayerPawn", "CCSPlayerPawnBase"}, "m_angEyeAngles");
  o.armor = FirstOffset({"CCSPlayerPawn", "CCSPlayerPawnBase"}, "m_ArmorValue");
  o.itemServices = FirstOffset({"CBasePlayerPawn"}, "m_pItemServices");
  o.helmet = FirstOffset({"CCSPlayer_ItemServices"}, "m_bHasHelmet");
  o.defuser = FirstOffset({"CCSPlayer_ItemServices"}, "m_bHasDefuser");
  o.spotted = FirstOffset({"CCSPlayerPawn", "CCSPlayerPawnBase"}, "m_entitySpottedState");
  o.spottedMask = FirstOffset({"EntitySpottedState_t"}, "m_bSpottedByMask");
  Log("practice: scenario schema pawn=%d money=%d/%d life=%d hp=%d owner=%d origin=%d/%d/%d eyes=%d armor=%d items=%d/%d/%d "
      "spotted=%d/%d",
      o.ctrlPawn, o.money, o.account, o.lifeState, o.health, o.owner, o.bodyComponent, o.sceneNode, o.absOrigin,
      o.eyeAngles, o.armor, o.itemServices, o.helmet, o.defuser, o.spotted, o.spottedMask);
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

void* Controller(int slot) {
  if (slot < 0 || slot >= 64) return nullptr;
  void* ctrl = g_api->entity_by_index(g_api->self, slot + 1);
  if (!ctrl) return nullptr;
  const char* cls = g_api->entity_classname(g_api->self, ctrl);
  return cls && std::strcmp(cls, "cs_player_controller") == 0 ? ctrl : nullptr;
}

void* PawnForSlot(int slot) {
  const Offsets& o = Off();
  void* ctrl = Controller(slot);
  if (!ctrl || o.ctrlPawn < 0) return nullptr;
  return g_api->entity_from_handle(g_api->self, Rd<uint32_t>(ctrl, o.ctrlPawn));
}

bool Alive(void* pawn) {
  const Offsets& o = Off();
  if (!pawn) return false;
  if (o.lifeState >= 0 && Rd<uint8_t>(pawn, o.lifeState) != 0) return false;
  return o.health < 0 || Rd<int32_t>(pawn, o.health) > 0;
}

bool OriginOf(void* ent, sc::V3* out) {
  const Offsets& o = Off();
  if (!ent || o.bodyComponent < 0 || o.sceneNode < 0 || o.absOrigin < 0) return false;
  void* body = Rd<void*>(ent, o.bodyComponent);
  void* node = body ? Rd<void*>(body, o.sceneNode) : nullptr;
  if (!node) return false;
  *out = Rd<sc::V3>(node, o.absOrigin);
  return std::isfinite(out->x) && std::isfinite(out->y) && std::isfinite(out->z);
}

bool Teleport(void* pawn, const sc::V3& p) {
  if (!pawn || !RU_API_HAS(g_api, entity_set_abs_origin) || !g_api->entity_set_abs_origin) return false;
  const float xyz[3] = {p.x, p.y, p.z};
  return g_api->entity_set_abs_origin(g_api->self, pawn, xyz) == 1;
}

// Horizontal path velocity (units/s). Also keeps gravity from building up while the pawn is moved
// every tick (without it a scripted bot eventually "lands" with fall damage).
void SetVelocity(void* pawn, float vx, float vy) {
  const Offsets& o = Off();
  if (!pawn || o.absVelocity < 0) return;
  const float v[3] = {vx, vy, 0.f};
  std::memcpy(static_cast<unsigned char*>(pawn) + o.absVelocity, v, sizeof(v));
}

void SetEyes(void* pawn, float pitch, float yaw) {
  const Offsets& o = Off();
  if (!pawn || o.eyeAngles < 0) return;
  const float ang[3] = {pitch, yaw, 0.f};
  std::memcpy(static_cast<unsigned char*>(pawn) + o.eyeAngles, ang, sizeof(ang));
}

struct PlayerInfo {
  int slot = -1;
  uint64_t steamid = 0;
  int team = 0;
  bool bot = false;
  std::string name;
};

std::vector<PlayerInfo> Players() {
  std::vector<PlayerInfo> out;
  g_api->for_each_player(
      g_api->self,
      [](void* u, const ru_player* p) -> int {
        PlayerInfo i;
        i.slot = p->slot >= 0 ? p->slot : p->userid;
        i.steamid = p->steamid64;
        i.team = p->team;
        i.bot = p->is_bot != 0;
        i.name = p->name;
        if (i.slot >= 0 && p->connected) static_cast<std::vector<PlayerInfo>*>(u)->push_back(i);
        return 1;
      },
      &out);
  return out;
}

bool PlayerBySlot(int slot, PlayerInfo* out) {
  for (const auto& p : Players()) {
    if (p.slot == slot) {
      *out = p;
      return true;
    }
  }
  return false;
}

// ---- grenades ----------------------------------------------------------------------------------------

// ru_api grenade_spawn (1.12): the projectile starts at `pos` with `vel`, thrown by the pawn of
// `ownerSlot` (-1: nobody). A core older than 1.12, or a CS2 build where that grenade's gamedata did
// not verify (grenade_spawn_available), throws nothing: the grenade is counted as skipped and logged.
bool SpawnGrenade(sc::GrenadeType type, const sc::V3& pos, const sc::V3& vel, int ownerSlot) {
  if (!RU_API_HAS(g_api, grenade_spawn_available) || !g_api->grenade_spawn) return false;
  uint32_t t = 0;
  switch (type) {
    case sc::GrenadeType::kSmoke: t = RU_GRENADE_SMOKE; break;
    case sc::GrenadeType::kFlash: t = RU_GRENADE_FLASH; break;
    case sc::GrenadeType::kHe: t = RU_GRENADE_HE; break;
    case sc::GrenadeType::kMolotov: t = RU_GRENADE_MOLOTOV; break;
    case sc::GrenadeType::kIncendiary: t = RU_GRENADE_INCENDIARY; break;
    case sc::GrenadeType::kDecoy: t = RU_GRENADE_DECOY; break;
  }
  if (!t || !g_api->grenade_spawn_available(g_api->self, t)) return false;
  ru_grenade_spawn s{};
  s.struct_size = sizeof(s);
  s.type = t;
  s.origin[0] = pos.x, s.origin[1] = pos.y, s.origin[2] = pos.z;
  s.velocity[0] = vel.x, s.velocity[1] = vel.y, s.velocity[2] = vel.z;
  s.owner_slot = ownerSlot;
  void* ent = g_api->grenade_spawn(g_api->self, &s);
  if (ent && g_host.spawned) g_host.spawned(ent);
  return ent != nullptr;
}

// ---- scenario library ---------------------------------------------------------------------------

std::string DataDir() {
  std::string d = g_api->data_dir(g_api->self);
  if (!d.empty() && d.back() != '/') d += '/';
  return d;
}
std::string ScenarioDir() { return DataDir() + "scenarios/"; }

struct Entry {
  std::string path;
  time_t mtime = 0;
  off_t size = 0;
  std::shared_ptr<sc::Scenario> sc;
  std::string error;
};
std::map<std::string, Entry> g_library;  // file name -> entry

// Rescans the scenario folder; parses new / changed files. Returns ids in name order.
std::vector<const Entry*> Library() {
  std::set<std::string> seen;
  const std::string dir = ScenarioDir();
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      const std::string f = e->d_name;
      if (f.size() < 6 || f.compare(f.size() - 5, 5, ".json") != 0 || f[0] == '.') continue;
      struct stat st {};
      const std::string path = dir + f;
      if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
      seen.insert(f);
      Entry& en = g_library[f];
      if (en.sc && en.mtime == st.st_mtime && en.size == st.st_size) continue;
      if (!en.error.empty() && en.mtime == st.st_mtime && en.size == st.st_size) continue;
      en = Entry{};
      en.path = path;
      en.mtime = st.st_mtime;
      en.size = st.st_size;
      auto parsed = std::make_shared<sc::Scenario>();
      if (sc::ParseFile(path, parsed.get(), &en.error)) {
        en.sc = parsed;
      } else {
        Log("practice: scenario file %s skipped: %s", f.c_str(), en.error.c_str());
      }
    }
    closedir(d);
  }
  for (auto it = g_library.begin(); it != g_library.end();) {
    it = seen.count(it->first) ? std::next(it) : g_library.erase(it);
  }
  std::vector<const Entry*> out;
  for (const auto& kv : g_library) {
    if (kv.second.sc) out.push_back(&kv.second);
  }
  std::sort(out.begin(), out.end(), [](const Entry* a, const Entry* b) { return a->sc->id < b->sc->id; });
  return out;
}

std::shared_ptr<sc::Scenario> FindScenario(const std::string& id, std::string* err) {
  const auto lib = Library();
  const std::string q = Lower(id);
  std::vector<const Entry*> hits;
  for (const Entry* e : lib) {
    if (e->sc->id == q) return e->sc;
    if (e->sc->id.find(q) != std::string::npos) hits.push_back(e);
  }
  if (hits.size() == 1) return hits[0]->sc;
  if (hits.empty()) *err = "no scenario \"" + id + "\" (.scen list)";
  else {
    *err = "\"" + id + "\" matches";
    for (size_t i = 0; i < hits.size() && i < 6; ++i) *err += (i ? ", " : " ") + hits[i]->sc->id;
  }
  return nullptr;
}

// ---- the replay ---------------------------------------------------------------------------------

enum class Phase { kIdle, kPreparing, kRunning, kDone };

struct Actor {
  int player = -1;  // scenario player index
  int slot = -1;    // bot slot
  std::string name;
  bool handed = false;  // bot AI owns it now
  bool dead = false;
};

struct Run {
  Phase phase = Phase::kIdle;
  std::shared_ptr<sc::Scenario> sc;
  int human = -1;  // scenario player the human plays; -1 = watch (bots only)
  int humanSlot = -1;
  uint64_t humanSteam = 0;
  int startT = 0;
  double readyAt = 0, deadline = 0;
  double t0 = 0;    // monotonic time of startT
  double lastT = 0;
  bool aiOn = false;
  bool humanDeadNoted = false;
  std::vector<Actor> actors;
  int nadesSpawned = 0, nadesSkipped = 0;
  Out out;  // who started it (errors while preparing)
};
Run g_run;
std::atomic<int> g_phaseAtomic{0};

void SetPhase(Phase p) {
  g_run.phase = p;
  g_phaseAtomic = static_cast<int>(p);
}

std::string PlayerName(int i) {
  return g_run.sc && i >= 0 && i < static_cast<int>(g_run.sc->players.size()) ? g_run.sc->players[i].name : "?";
}

Actor* ActorForPlayer(int player) {
  for (auto& a : g_run.actors) {
    if (a.player == player) return &a;
  }
  return nullptr;
}
Actor* ActorForSlot(int slot) {
  for (auto& a : g_run.actors) {
    if (a.slot == slot) return &a;
  }
  return nullptr;
}

void ScenarioCvars() {
  for (const char* c : {"bot_stop 1", "bot_freeze 0", "bot_zombie 0", "bot_crouch 0", "bot_ignore_players 1",
                        "bot_quota_mode normal", "buddha 0", "mp_respawn_on_death_ct 0", "mp_respawn_on_death_t 0",
                        "mp_ignore_round_win_conditions 1", "mp_autoteambalance 0", "mp_limitteams 0", "sv_infinite_ammo 0",
                        "mp_freezetime 0", "mp_death_drop_gun 1"}) {
    Cmd(c);
  }
}

void ClearUtility() {
  for (const char* c : {"ent_remove_all smokegrenade_projectile", "ent_remove_all molotov_projectile", "ent_remove_all inferno",
                        "ent_remove_all decoy_projectile", "ent_remove_all flashbang_projectile",
                        "ent_remove_all hegrenade_projectile"}) {
    Cmd(c);
  }
}

// Ends the replay. kNone: state only (map change, a new scenario takes over); kCleanup: bots out,
// the scenario cvars undone (practice mode was switched off); kPractice: also prac.cfg again.
enum class Restore { kNone, kCleanup, kPractice };
void Stop(Restore restore, const char* why) {
  if (g_run.phase == Phase::kIdle) return;
  Log("practice: scenario %s stopped (%s)", g_run.sc ? g_run.sc->id.c_str() : "?", why);
  if (restore != Restore::kNone) {
    Cmd("bot_kick");
    ClearUtility();
    Cmd("mp_ignore_round_win_conditions 0");
    Cmd("bot_stop 0");
    Cmd("bot_ignore_players 0");
  }
  if (restore == Restore::kPractice) Cmd("exec ReadyUp/prac.cfg");
  g_run = Run{};
  SetPhase(Phase::kIdle);
}

void HandOver(Actor& a, const char* why) {
  if (a.handed) return;
  a.handed = true;
  Log("practice: scenario bot %s (%s) handed to the bot AI: %s", a.name.c_str(), PlayerName(a.player).c_str(), why);
  if (!g_run.aiOn) {
    g_run.aiOn = true;
    Cmd("bot_stop 0");
    Cmd("bot_ignore_players 0");
  }
}

// Every weapon entity by owner (m_hOwnerEntity handle), knives left out. One pass over the entities.
std::map<uint32_t, std::vector<void*>> WeaponsByOwner() {
  std::map<uint32_t, std::vector<void*>> out;
  const Offsets& o = Off();
  if (o.owner < 0) return out;
  for (int i = 65; i < 16384; ++i) {
    void* e = g_api->entity_by_index(g_api->self, i);
    if (!e) continue;
    const char* cls = g_api->entity_classname(g_api->self, e);
    if (!cls || std::strncmp(cls, "weapon_", 7) != 0 || std::strstr(cls, "knife") || std::strstr(cls, "bayonet")) continue;
    out[Rd<uint32_t>(e, o.owner)].push_back(e);
  }
  return out;
}
std::map<uint32_t, std::vector<void*>> g_weapons;  // filled by Setup

// Strips a pawn's weapons (the knife stays) and gives the recorded items, health, armor and money.
void Equip(int slot, void* pawn, const sc::State& s) {
  const Offsets& o = Off();
  if (!pawn) return;
  if (RU_API_HAS(g_api, entity_remove) && g_api->entity_remove) {
    auto it = g_weapons.find(g_api->entity_handle_of(g_api->self, pawn));
    if (it != g_weapons.end()) {
      for (void* w : it->second) g_api->entity_remove(g_api->self, w);
    }
  }
  if (RU_API_HAS(g_api, player_give_item) && g_api->player_give_item) {
    if (s.helmet) g_api->player_give_item(g_api->self, slot, "item_assaultsuit");
    else if (s.armor > 0) g_api->player_give_item(g_api->self, slot, "item_kevlar");
    if (s.defuser) g_api->player_give_item(g_api->self, slot, "item_defuser");
    for (const auto& it : s.items) {
      if (it == "weapon_knife") continue;
      g_api->player_give_item(g_api->self, slot, it.c_str());
    }
  }
  if (o.health >= 0) Wr<int32_t>(pawn, o.health, s.hp);
  if (o.armor >= 0) Wr<int32_t>(pawn, o.armor, s.armor);
  if (o.itemServices >= 0) {
    if (void* svc = Rd<void*>(pawn, o.itemServices)) {
      if (o.helmet >= 0) Wr<bool>(svc, o.helmet, s.helmet);
      if (o.defuser >= 0) Wr<bool>(svc, o.defuser, s.defuser);
    }
  }
  g_api->entity_mark_changed(g_api->self, pawn);
  if (void* ctrl = Controller(slot); ctrl && o.money >= 0 && o.account >= 0) {
    if (void* ms = Rd<void*>(ctrl, o.money)) {
      Wr<int32_t>(ms, o.account, s.money);
      g_api->entity_mark_changed(g_api->self, ctrl);
    }
  }
}

// Places one participant at the start.
void PlaceAt(int slot, int player, bool bot) {
  const sc::Scenario& S = *g_run.sc;
  const sc::Player& p = S.players[static_cast<size_t>(player)];
  void* pawn = PawnForSlot(slot);
  sc::Pose pose;
  if (!pawn || !p.PoseAt(g_run.startT, S.sampleTicks, &pose)) return;
  Teleport(pawn, pose.pos);
  if (bot) SetEyes(pawn, pose.pitch, pose.yaw);
  SetVelocity(pawn, 0, 0);
  Equip(slot, pawn, p.StateAt(g_run.startT));
}

// Thrower for a grenade of `player`: their bot, else a live bot of the same team, else nobody.
// Returns that bot's slot, or -1.
int ThrowerFor(int player) {
  if (Actor* a = ActorForPlayer(player); a && !a->dead) {
    if (Alive(PawnForSlot(a->slot))) return a->slot;
  }
  const int team = g_run.sc->players[static_cast<size_t>(player)].team;
  for (const auto& a : g_run.actors) {
    if (a.dead || g_run.sc->players[static_cast<size_t>(a.player)].team != team) continue;
    if (Alive(PawnForSlot(a.slot))) return a.slot;
  }
  return -1;
}

double EffectSeconds(sc::GrenadeType t) {
  switch (t) {
    case sc::GrenadeType::kSmoke: return 20.0;
    case sc::GrenadeType::kMolotov:
    case sc::GrenadeType::kIncendiary: return 7.0;
    case sc::GrenadeType::kDecoy: return 15.0;
    default: return 0.0;
  }
}

void ThrowGrenade(const sc::Grenade& g, bool atLanding) {
  const sc::V3 zero{};
  const bool ok = SpawnGrenade(g.type, atLanding ? g.land : g.pos, atLanding ? zero : g.vel, ThrowerFor(g.player));
  (ok ? g_run.nadesSpawned : g_run.nadesSkipped)++;
  if (!ok) {
    Log("practice: scenario %s %s by %s (not thrown: grenade spawn not available on this CS2 build)", sc::Clock(g.t, g_run.sc->tickrate).c_str(),
        sc::GrenadeName(g.type), PlayerName(g.player).c_str());
  }
}

// Everyone in place, the clock starts.
void Setup() {
  const sc::Scenario& S = *g_run.sc;
  // Bots per team, in slot order, get the recorded players of that team in order.
  std::vector<PlayerInfo> bots[4];
  for (const auto& p : Players()) {
    if (p.bot && (p.team == 2 || p.team == 3) && Alive(PawnForSlot(p.slot))) bots[p.team].push_back(p);
  }
  for (auto& b : bots) std::sort(b.begin(), b.end(), [](const PlayerInfo& x, const PlayerInfo& y) { return x.slot < y.slot; });
  size_t next[4] = {0, 0, 0, 0};
  g_run.actors.clear();
  for (size_t i = 0; i < S.players.size(); ++i) {
    const sc::Player& p = S.players[i];
    if (static_cast<int>(i) == g_run.human || !p.AliveAt(g_run.startT)) continue;
    auto& pool = bots[p.team];
    if (next[p.team] >= pool.size()) continue;
    const PlayerInfo& b = pool[next[p.team]++];
    Actor a;
    a.player = static_cast<int>(i);
    a.slot = b.slot;
    a.name = b.name;
    g_run.actors.push_back(a);
  }
  // Extra bots (a restart left more than needed) sit out: kill them.
  for (int team : {2, 3}) {
    for (size_t k = next[team]; k < bots[team].size(); ++k) Cmd("bot_kill \"" + bots[team][k].name + "\"");
  }
  ClearUtility();
  g_weapons = WeaponsByOwner();
  for (const auto& a : g_run.actors) PlaceAt(a.slot, a.player, /*bot=*/true);
  if (g_run.human >= 0) PlaceAt(g_run.humanSlot, g_run.human, /*bot=*/false);
  g_weapons.clear();
  // Utility already out at the start: lingering smokes / fires / decoys where they landed, grenades
  // still in the air from where they were thrown.
  for (const auto& g : S.grenades) {
    if (g.t > g_run.startT || g.player == g_run.human) continue;
    const double left = (g.landT - g_run.startT) / static_cast<double>(S.tickrate) + EffectSeconds(g.type);
    if (g.landT > g_run.startT) ThrowGrenade(g, /*atLanding=*/false);
    else if (left > 1.0) ThrowGrenade(g, /*atLanding=*/true);
  }
  g_run.t0 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  g_run.lastT = g_run.startT;
  SetPhase(Phase::kRunning);

  const std::string at = sc::Clock(g_run.startT, S.tickrate);
  if (g_run.human >= 0) {
    const sc::Player& me = S.players[static_cast<size_t>(g_run.human)];
    sc::Pose pose;
    me.PoseAt(g_run.startT, S.sampleTicks, &pose);
    char look[96];
    std::snprintf(look, sizeof(look), "setang %.1f %.1f 0", pose.pitch, pose.yaw);
    g_run.out("you are " + me.name + (me.team == 2 ? " (T)" : " (CT)") + ", " + S.id + " from " + at +
              ". Recorded view: " + look + " in your console.");
  }
  ChatAll("Ready Up: scenario " + S.id + " (" + sc::Summary(S) + ") from " + at + ". Source: " + sc::Attribution(S));
  Log("practice: scenario %s running from t=%d: %zu bots, human %s", S.id.c_str(), g_run.startT, g_run.actors.size(),
      g_run.human >= 0 ? PlayerName(g_run.human).c_str() : "none (watch)");
}

double NowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void TickPreparing(double now) {
  if (now < g_run.readyAt) return;
  const sc::Scenario& S = *g_run.sc;
  int needT = 0, needCT = 0;
  sc::BotCounts(S, g_run.human, g_run.startT, &needT, &needCT);
  int haveT = 0, haveCT = 0;
  for (const auto& p : Players()) {
    if (!p.bot || !Alive(PawnForSlot(p.slot))) continue;
    if (p.team == 2) ++haveT;
    if (p.team == 3) ++haveCT;
  }
  bool humanOk = g_run.human < 0;
  if (!humanOk) {
    PlayerInfo me;
    humanOk = PlayerBySlot(g_run.humanSlot, &me) && me.steamid == g_run.humanSteam && Alive(PawnForSlot(g_run.humanSlot));
  }
  if (haveT >= needT && haveCT >= needCT && humanOk) return Setup();
  if (now > g_run.deadline) {
    g_run.out("scenario did not start: " + std::to_string(haveT) + "/" + std::to_string(needT) + " T bots, " +
              std::to_string(haveCT) + "/" + std::to_string(needCT) + " CT bots" + (humanOk ? "" : ", you are not alive") +
              " (server full? maxplayers must fit the bots).");
    Stop(Restore::kPractice, "bots did not join");
  }
}

void TickRunning(double now) {
  const sc::Scenario& S = *g_run.sc;
  const double t = g_run.startT + (now - g_run.t0) * S.tickrate;
  const int prev = static_cast<int>(std::floor(g_run.lastT));
  const int cur = static_cast<int>(std::floor(t));
  g_run.lastT = t;

  for (const sc::Due& d : sc::DueBetween(S, prev, cur)) {
    if (d.kind == sc::Due::kGrenade) {
      const sc::Grenade& g = S.grenades[static_cast<size_t>(d.index)];
      if (g.player != g_run.human) ThrowGrenade(g, /*atLanding=*/false);
    } else if (d.kind == sc::Due::kDeath) {
      const sc::Death& dd = S.deaths[static_cast<size_t>(d.index)];
      if (dd.player == g_run.human || (g_run.human >= 0 && dd.killer == g_run.human)) continue;
      Actor* a = ActorForPlayer(dd.player);
      if (!a || a->dead || a->handed) continue;
      Cmd("bot_kill \"" + a->name + "\"");
      a->dead = true;
      Log("practice: scenario %s: %s dies (recorded, by %s)", sc::Clock(dd.t, S.tickrate).c_str(),
          PlayerName(dd.player).c_str(), dd.killer >= 0 ? PlayerName(dd.killer).c_str() : "the world");
    } else {
      const sc::BombEvent& b = S.bomb[static_cast<size_t>(d.index)];
      ChatAll("Ready Up: " + sc::Clock(b.t, S.tickrate) + " bomb " + b.event +
              (b.player >= 0 ? " by " + PlayerName(b.player) : std::string()) + " (recorded; not simulated)");
    }
  }

  // Handoff: bots that spot the player.
  const Offsets& o = Off();
  void* me = g_run.human >= 0 ? PawnForSlot(g_run.humanSlot) : nullptr;
  const bool meAlive = me && Alive(me);
  uint32_t mask[2] = {0, 0};
  if (meAlive && o.spotted >= 0 && o.spottedMask >= 0) {
    std::memcpy(mask, static_cast<unsigned char*>(me) + o.spotted + o.spottedMask, sizeof(mask));
  }
  if (g_run.human >= 0 && !meAlive && !g_run.humanDeadNoted) {
    g_run.humanDeadNoted = true;
    g_run.out("you died. .scen r to try again, .scen stop to end.");
  }

  for (auto& a : g_run.actors) {
    if (a.dead) continue;
    void* pawn = PawnForSlot(a.slot);
    if (!Alive(pawn)) {
      a.dead = true;
      continue;
    }
    if (a.handed) continue;
    if (a.slot >= 0 && a.slot < 64 && (mask[a.slot / 32] >> (a.slot % 32)) & 1u) {
      HandOver(a, "spotted the player");
      continue;
    }
    sc::Pose pose;
    if (!S.players[static_cast<size_t>(a.player)].PoseAt(t, S.sampleTicks, &pose)) continue;  // path over: stays
    Teleport(pawn, pose.pos);
    SetEyes(pawn, pose.pitch, pose.yaw);
    sc::Pose ahead;
    if (S.players[static_cast<size_t>(a.player)].PoseAt(t + 1, S.sampleTicks, &ahead)) {
      SetVelocity(pawn, (ahead.pos.x - pose.pos.x) * S.tickrate, (ahead.pos.y - pose.pos.y) * S.tickrate);
    } else {
      SetVelocity(pawn, 0, 0);
    }
    g_api->entity_mark_changed(g_api->self, pawn);
  }

  if (t >= S.length) {
    for (auto& a : g_run.actors) {
      if (!a.dead) HandOver(a, "recording over");
    }
    SetPhase(Phase::kDone);
    ChatAll("Ready Up: scenario " + S.id + " recording over (" + sc::Clock(S.length, S.tickrate) +
            "); bots are on their own. .scen r replays, .scen stop ends.");
  }
}

void OnTick(void*, const ru_tick_info*) {
  if (g_run.phase == Phase::kIdle) return;
  if (!g_host.active()) return Stop(Restore::kCleanup, "practice mode off");
  const double now = NowSec();
  if (g_run.phase == Phase::kPreparing) TickPreparing(now);
  else if (g_run.phase == Phase::kRunning) TickRunning(now);
}

void OnHurt(void*, const char*, const ru_game_event* ev) {
  if (g_run.phase != Phase::kRunning) return;
  const ru_api* a = g_api;
  const int victim = a->ev_get_player_slot(a->self, ev, "userid");
  const int attacker = a->ev_get_player_slot(a->self, ev, "attacker");
  Actor* v = ActorForSlot(victim);
  if (!v || v->handed || v->dead) return;
  if (g_run.human >= 0 && attacker == g_run.humanSlot) return HandOver(*v, "hurt by the player");
  if (!ActorForSlot(attacker)) return;
  // Bot vs bot while the victim still follows its path: undo the damage (the recording decides deaths).
  const int left = a->ev_get_int(a->self, ev, "health", 0);
  const int dmg = a->ev_get_int(a->self, ev, "dmg_health", 0);
  void* pawn = a->ev_get_player_pawn(a->self, ev, "userid");
  if (left > 0 && dmg > 0 && pawn && Off().health >= 0) Wr<int32_t>(pawn, Off().health, std::min(100, left + dmg));
}

void OnMapStart(void*, const ru_event*) {
  g_off = Offsets{};
  if (g_run.phase != Phase::kIdle) Stop(Restore::kNone, "map change");
}

// ---- commands -----------------------------------------------------------------------------------

struct LoadArgs {
  std::string id, player, start;
};
LoadArgs g_lastLoad;
int g_lastSlot = -1;
uint64_t g_lastSteam = 0;

bool StartScenario(const LoadArgs& args, int slot, uint64_t steamid, const Out& out) {
  if (const std::string why = g_host.refusal(); !why.empty()) {
    out("scenarios " + why);
    return false;
  }
  std::string err;
  auto S = FindScenario(args.id, &err);
  if (!S) {
    out(err);
    return false;
  }
  const char* map = RU_API_HAS(g_api, current_map) && g_api->current_map ? g_api->current_map(g_api->self) : "";
  if (map && *map && S->map != map) {
    out(S->id + " is on " + S->map + " (this is " + map + "): change the map first.");
    return false;
  }
  int startT = S->defaultStart;
  if (!args.start.empty() && !sc::ParseStart(args.start, *S, &startT, &err)) {
    out(err);
    return false;
  }
  const std::string who = Lower(args.player);
  const bool watch = who == "watch" || who == "none" || who == "-" || who == "bots";
  int human = -1;
  int team = 0;
  if (!watch) {
    PlayerInfo me;
    if (slot < 0 || !PlayerBySlot(slot, &me) || me.bot) {
      out("no player to put in the scenario (use `watch` to replay with bots only).");
      return false;
    }
    team = me.team;
    if (team != 2 && team != 3) {
      out("join T or CT first (or .scen load " + S->id + " watch).");
      return false;
    }
    human = who.empty() ? sc::DefaultPlayer(*S, team, startT) : sc::FindPlayer(*S, args.player, &err);
    if (human < 0) {
      out(err.empty() ? "nobody on your team is alive at " + sc::Clock(startT, S->tickrate) : err);
      return false;
    }
    const sc::Player& p = S->players[static_cast<size_t>(human)];
    if (p.team != team) {
      out(p.name + " played " + (p.team == 2 ? "T" : "CT") + ": switch teams first, or pick a " + (team == 2 ? "T" : "CT") +
          " player (" + sc::PlayerList(*S) + ").");
      return false;
    }
    if (!p.AliveAt(startT)) {
      out(p.name + " is dead at " + sc::Clock(startT, S->tickrate) + " in the recording: pick an earlier start.");
      return false;
    }
  }
  int needT = 0, needCT = 0;
  sc::BotCounts(*S, human, startT, &needT, &needCT);

  const bool again = g_run.phase != Phase::kIdle && g_run.sc == S;
  if (g_run.phase != Phase::kIdle) Stop(Restore::kNone, "new scenario");
  g_run = Run{};
  g_run.sc = S;
  g_run.human = human;
  g_run.humanSlot = watch ? -1 : slot;
  g_run.humanSteam = steamid;
  g_run.startT = startT;
  g_run.out = out;
  g_lastLoad = args;
  g_lastSlot = slot;
  g_lastSteam = steamid;

  ScenarioCvars();
  // Same scenario, same bots on the right teams: keep them (a quicker restart).
  int haveT = 0, haveCT = 0;
  for (const auto& p : Players()) {
    if (p.bot && p.team == 2) ++haveT;
    if (p.bot && p.team == 3) ++haveCT;
  }
  if (!(again && haveT == needT && haveCT == needCT)) {
    Cmd("bot_kick");
    for (int i = 0; i < needT; ++i) Cmd("bot_add_t");
    for (int i = 0; i < needCT; ++i) Cmd("bot_add_ct");
  }
  ClearUtility();
  Cmd("mp_restartgame 1");
  const double now = NowSec();
  g_run.readyAt = now + 1.6;
  g_run.deadline = now + 15.0;
  SetPhase(Phase::kPreparing);
  out("loading " + S->id + " (" + sc::Summary(*S) + ") from " + sc::Clock(startT, S->tickrate) + ": " +
      std::to_string(needT) + " T + " + std::to_string(needCT) + " CT bots" +
      (human >= 0 ? ", you play " + S->players[static_cast<size_t>(human)].name : std::string(", watch mode")) + ".");
  return true;
}

void CmdList(const Out& out, bool all) {
  const auto lib = Library();
  const char* map = RU_API_HAS(g_api, current_map) && g_api->current_map ? g_api->current_map(g_api->self) : "";
  int shown = 0, other = 0;
  for (const Entry* e : lib) {
    if (!all && map && *map && e->sc->map != map) {
      ++other;
      continue;
    }
    if (++shown > 12 && !out.console) continue;
    out(e->sc->id + ": " + sc::Summary(*e->sc));
  }
  if (shown > 12 && !out.console) out("... " + std::to_string(shown - 12) + " more (ru scenario list in the console).");
  if (shown == 0) {
    out(std::string("no scenarios") + (all || !map || !*map ? "" : std::string(" for ") + map) + " in " + ScenarioDir() +
        (other ? " (" + std::to_string(other) + " for other maps: .scen list all)" : std::string()));
  } else {
    out(".scen load <id> [player|watch] [start: 25, 1:05, t12345]");
  }
}

void CmdInfo(const Out& out, const std::string& id) {
  std::string err;
  auto S = FindScenario(id, &err);
  if (!S) return out(err);
  out(S->id + ": " + (S->title.empty() ? sc::Summary(*S) : S->title + " - " + sc::Summary(*S)));
  out("players: " + sc::PlayerList(*S));
  out("start " + sc::Clock(S->defaultStart, S->tickrate) + ", " + std::to_string(S->grenades.size()) + " grenades, " +
      std::to_string(S->deaths.size()) + " deaths" + (S->bomb.empty() ? "" : ", bomb " + S->bomb.front().event + " at " +
                                                                            sc::Clock(S->bomb.front().t, S->tickrate)));
  out("source: " + sc::Attribution(*S));
}

void CmdStatus(const Out& out) {
  static const char* kNames[] = {"idle", "preparing", "running", "over"};
  if (g_run.phase == Phase::kIdle) return out("no scenario running.");
  const sc::Scenario& S = *g_run.sc;
  int scripted = 0, handed = 0, dead = 0;
  for (const auto& a : g_run.actors) {
    if (a.dead) ++dead;
    else if (a.handed) ++handed;
    else ++scripted;
  }
  out(S.id + " " + kNames[static_cast<int>(g_run.phase)] + " at " + sc::Clock(static_cast<int>(g_run.lastT), S.tickrate) +
      "/" + sc::Clock(S.length, S.tickrate) + ", player " + (g_run.human >= 0 ? PlayerName(g_run.human) : "none (watch)") +
      "; bots: " + std::to_string(scripted) + " scripted, " + std::to_string(handed) + " AI, " + std::to_string(dead) +
      " dead; grenades " + std::to_string(g_run.nadesSpawned) + " spawned, " + std::to_string(g_run.nadesSkipped) +
      " skipped");
  if (out.console) {
    for (const auto& a : g_run.actors) {
      sc::V3 at{};
      sc::Pose want;
      const bool has = OriginOf(PawnForSlot(a.slot), &at);
      const bool w = S.players[static_cast<size_t>(a.player)].PoseAt(g_run.lastT, S.sampleTicks, &want);
      char b[256];
      std::snprintf(b, sizeof(b), "  slot %d %s as %s: %s at %.0f %.0f %.0f (path %.0f %.0f %.0f)", a.slot, a.name.c_str(),
                    PlayerName(a.player).c_str(), a.dead ? "dead" : a.handed ? "AI" : "scripted", has ? at.x : 0.f,
                    has ? at.y : 0.f, has ? at.z : 0.f, w ? want.pos.x : 0.f, w ? want.pos.y : 0.f, w ? want.pos.z : 0.f);
      out(b);
    }
  }
}

// ---- server-side conversion (tools/scenario/ru_scenario.py) ------------------------------------------

struct Job {
  std::mutex mu;
  std::thread th;
  std::atomic<bool> running{false};
  pid_t pid = -1;
  std::vector<std::string> lines;
  int rc = -1;
  Out out;
};
Job g_job;

std::string Config(const char* key, const std::string& def) {
  char buf[512] = {};
  return g_api->config_get(g_api->self, key, buf, sizeof(buf)) > 0 ? std::string(buf) : def;
}

// csgo/ (the game dir): data_dir is csgo/readyup/plugins/practice/.
std::string GameDir() {
  std::string d = DataDir();
  for (int i = 0; i < 4 && !d.empty(); ++i) {
    d.pop_back();
    const size_t s = d.rfind('/');
    if (s == std::string::npos) return {};
    d.resize(s + 1);
  }
  return d;
}

bool SafeRelPath(const std::string& p) {
  if (p.empty() || p.size() > 200 || p[0] == '/' || p.find("..") != std::string::npos) return false;
  for (char c : p) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '/')) return false;
  }
  return true;
}

bool FileExists(const std::string& p) {
  struct stat st {};
  return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void JobDone(void*) {
  std::vector<std::string> lines;
  int rc;
  Out out;
  {
    std::lock_guard<std::mutex> lk(g_job.mu);
    lines.swap(g_job.lines);
    rc = g_job.rc;
    out = g_job.out;
  }
  if (g_job.th.joinable()) g_job.th.join();
  g_job.running = false;
  for (const auto& l : lines) out(l);
  out(rc == 0 ? "conversion finished (.scen list)." : "conversion failed (exit " + std::to_string(rc) + ").");
  g_library.clear();  // rescan
}

void RunJob(std::vector<std::string> argv, std::string pythonpath) {
  int fds[2];
  int rc = -1;
  auto add = [](const std::string& l) {
    std::lock_guard<std::mutex> lk(g_job.mu);
    if (g_job.lines.size() < 60) g_job.lines.push_back(l.substr(0, 400));
  };
  if (pipe(fds) == 0) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
      if (std::strncmp(*e, "PYTHONPATH=", 11) != 0 && std::strncmp(*e, "LD_PRELOAD=", 11) != 0 &&
          std::strncmp(*e, "LD_LIBRARY_PATH=", 16) != 0) {
        env.emplace_back(*e);
      }
    }
    if (!pythonpath.empty()) env.push_back("PYTHONPATH=" + pythonpath);
    std::vector<char*> envp, av;
    for (auto& s : env) envp.push_back(s.data());
    envp.push_back(nullptr);
    for (auto& s : argv) av.push_back(s.data());
    av.push_back(nullptr);
    pid_t pid = -1;
    const int err = posix_spawnp(&pid, av[0], &fa, nullptr, av.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (err != 0) {
      add(std::string("cannot run ") + argv[0] + ": " + std::strerror(err));
    } else {
      {
        std::lock_guard<std::mutex> lk(g_job.mu);
        g_job.pid = pid;
      }
      std::string buf;
      char tmp[4096];
      ssize_t n;
      while ((n = read(fds[0], tmp, sizeof(tmp))) > 0) {
        buf.append(tmp, static_cast<size_t>(n));
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
          add(buf.substr(0, nl));
          buf.erase(0, nl + 1);
        }
      }
      if (!buf.empty()) add(buf);
      int status = 0;
      waitpid(pid, &status, 0);
      rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
      std::lock_guard<std::mutex> lk(g_job.mu);
      g_job.pid = -1;
    }
    close(fds[0]);
  } else {
    add("pipe() failed");
  }
  {
    std::lock_guard<std::mutex> lk(g_job.mu);
    g_job.rc = rc;
  }
  g_api->post_to_game_thread(g_api->self, &JobDone, nullptr);
}

// `ru scenario convert <demo> <round[,round]> [start] [id]` and `ru scenario rounds <demo>`.
void CmdConvert(const Out& out, const std::vector<std::string>& a, bool roundsOnly) {
  if (g_job.running) return out("a conversion is already running.");
  if (a.empty() || (!roundsOnly && a.size() < 2)) {
    return out(roundsOnly ? "usage: ru scenario rounds <demo.dem>"
                          : "usage: ru scenario convert <demo.dem> <round[,round]> [start] [id]");
  }
  const std::string demo = a[0];
  if (!SafeRelPath(demo) || demo.size() < 5 || demo.compare(demo.size() - 4, 4, ".dem") != 0) {
    return out("demo must be a .dem path relative to " + DataDir() + "demos/ or csgo/ (letters, digits, _ - . /).");
  }
  std::string path;
  for (const std::string& base : {DataDir() + "demos/", GameDir()}) {
    if (!base.empty() && FileExists(base + demo)) {
      path = base + demo;
      break;
    }
  }
  if (path.empty()) return out("no " + demo + " in " + DataDir() + "demos/ or " + GameDir());
  std::string converter = Config("scenario_converter", "");
  if (converter.empty()) {
    const std::string core = g_api->config_dir(g_api->self);  // csgo/readyup/bin/linuxsteamrt64
    converter = core + "/../../tools/scenario/ru_scenario.py";
  }
  if (!FileExists(converter)) return out("converter not found: " + converter + " (scenario_converter in practice.cfg)");
  std::vector<std::string> argv = {Config("scenario_python", "python3"), converter, roundsOnly ? "rounds" : "convert", path};
  if (!roundsOnly) {
    const std::string& rounds = a[1];
    if (rounds.empty() || rounds.size() > 64 ||
        !std::all_of(rounds.begin(), rounds.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == ','; })) {
      return out("rounds: 7 or 7,9,12");
    }
    argv.insert(argv.end(), {"--round", rounds, "--out", ScenarioDir()});
    if (a.size() > 2 && a[2] != "-") {
      const std::string& st = a[2];
      if (st.size() > 16 || !std::all_of(st.begin(), st.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == ':' || c == '.'; })) {
        return out("start: 25, 1:05 or t12345");
      }
      argv.insert(argv.end(), {"--start", st});
    }
    if (a.size() > 3) {
      if (!sc::IsValidId(a[3])) return out("id: [a-z0-9_-], up to 64 chars");
      argv.insert(argv.end(), {"--id", a[3]});
    }
    mkdir(ScenarioDir().c_str(), 0755);
  }
  if (g_job.th.joinable()) g_job.th.join();
  {
    std::lock_guard<std::mutex> lk(g_job.mu);
    g_job.lines.clear();
    g_job.rc = -1;
    g_job.out = out;
  }
  g_job.running = true;
  g_job.th = std::thread(&RunJob, argv, Config("scenario_pythonpath", ""));
  out(std::string(roundsOnly ? "reading rounds of " : "converting ") + demo + " in the background (needs python3 + demoparser2)...");
}

std::vector<std::string> Args(const ru_command_ctx* c, int from) {
  std::vector<std::string> out;
  for (int i = from; i < c->argc; ++i) out.emplace_back(c->argv[i] ? c->argv[i] : "");
  return out;
}

void Help(const Out& out, bool console) {
  out(".scen list [all] | .scen load <id> [player|watch] [start] | .scen r | .scen stop | .scen info <id>");
  if (console) out("ru scenario status | rounds <demo.dem> | convert <demo.dem> <round[,round]> [start|-] [id]");
}

// Shared by chat and `ru scenario`. a[0] is the subcommand.
void Dispatch(const Out& out, int slot, uint64_t steamid, bool console, std::vector<std::string> a) {
  const std::string sub = a.empty() ? "" : Lower(a[0]);
  auto rest = [&](size_t i) { return a.size() > i ? a[i] : std::string(); };
  if (sub.empty() || sub == "help") return Help(out, console);
  if (sub == "list" || sub == "ls") return CmdList(out, Lower(rest(1)) == "all");
  if (sub == "info") return rest(1).empty() ? out("usage: .scen info <id>") : CmdInfo(out, rest(1));
  if (sub == "status") return CmdStatus(out);
  if (sub == "stop" || sub == "end") {
    if (g_run.phase == Phase::kIdle) return out("no scenario running.");
    Stop(Restore::kPractice, "stopped by command");
    return ChatAll("Ready Up: scenario stopped.");
  }
  if (sub == "restart" || sub == "r" || sub == "again") {
    if (g_lastLoad.id.empty()) return out("nothing to restart (.scen load <id> first).");
    return (void)StartScenario(g_lastLoad, console ? g_lastSlot : slot, console ? g_lastSteam : steamid, out);
  }
  if (sub == "convert" || sub == "rounds") {
    if (!console && g_api->is_admin(g_api->self, steamid) != 1) return out("not authorized (admins / console only).");
    return CmdConvert(out, std::vector<std::string>(a.begin() + 1, a.end()), sub == "rounds");
  }
  LoadArgs la;
  size_t i = 1;
  if (sub != "load" && sub != "play") i = 0;  // `.scen <id> ...`
  la.id = rest(i);
  la.player = rest(i + 1);
  la.start = rest(i + 2);
  if (la.id.empty()) return Help(out, console);
  // `.scen load <id> 1:05` (start without a player).
  if (la.start.empty() && !la.player.empty()) {
    int dummy;
    std::string e;
    sc::Scenario probe;
    probe.tickrate = 64;
    probe.length = 1 << 30;
    const std::string p = la.player;
    const bool looksTime = p.find(':') != std::string::npos || (p.size() > 1 && (p.front() == 't' || p.back() == 't') &&
                                                               std::isdigit(static_cast<unsigned char>(p[p.size() / 2]))) ||
                           (p.size() > 1 && p.back() == 's' && std::isdigit(static_cast<unsigned char>(p[0])));
    if (looksTime && sc::ParseStart(p, probe, &dummy, &e)) {
      la.start = p;
      la.player.clear();
    }
  }
  int who = slot;
  uint64_t sid = steamid;
  if (console && Lower(la.player) != "watch") {
    // The console has no player: the only human on the server plays, otherwise watch mode.
    std::vector<PlayerInfo> humans;
    for (const auto& p : Players()) {
      if (!p.bot && p.steamid) humans.push_back(p);
    }
    if (humans.size() == 1) {
      who = humans[0].slot;
      sid = humans[0].steamid;
    } else if (la.player.empty()) {
      la.player = "watch";
    }
  }
  (void)StartScenario(la, who, sid, out);
}

void OnChat(void*, const ru_command_ctx* c) {
  const int slot = c->slot >= 0 ? c->slot : (c->steamid64 ? g_api->slot_for_steamid(g_api->self, c->steamid64) : -1);
  Out out;
  out.console = false;
  out.slot = slot;
  auto a = Args(c, 1);
  const std::string cmd = Lower(c->argc > 0 && c->argv[0] ? c->argv[0] : "");
  if (cmd == ".scenarios") a.insert(a.begin(), "list");
  Dispatch(out, slot, c->steamid64, false, a);
}

// `ru scenario ...` / `.ru scenario ...`: argv[0] "ru", argv[1] "scenario".
void OnRu(void*, const ru_command_ctx* c) {
  Out out;
  out.console = c->is_console != 0;
  out.slot = c->slot;
  Dispatch(out, c->slot, c->steamid64, c->is_console != 0, Args(c, 2));
}

void SelftestRun(ru_selftest_add_fn add, void* ctx) {
  static const char* kNames[] = {"idle", "preparing", "running", "over"};
  add(ctx, "INFO", "practice scenario", kNames[std::clamp(g_phaseAtomic.load(), 0, 3)]);
}

}  // namespace

void Load(const ru_api* api, const Host& host) {
  g_api = api;
  g_host = host;
  g_off = Offsets{};
  g_run = Run{};
  SetPhase(Phase::kIdle);
  g_library.clear();
  g_lastLoad = LoadArgs{};
  for (const char* c : {".scen", ".scenario", ".scenarios"}) {
    if (ru_handle h = api->register_chat_command(api->self, c, &OnChat, nullptr)) g_handles.push_back(h);
    else ru_logf(api, RU_LOG_WARN, "could not register %s", c);
  }
  if (ru_handle h = api->register_ru_subcommand(api->self, "scenario", &OnRu, nullptr)) g_handles.push_back(h);
  if (ru_handle h = api->subscribe_game_event(api->self, "player_hurt", &OnHurt, nullptr)) g_handles.push_back(h);
  if (ru_handle h = api->subscribe(api->self, RU_EVENT_MAP_START, &OnMapStart, nullptr)) g_handles.push_back(h);
  if (ru_handle h = api->on_tick(api->self, &OnTick, nullptr)) g_handles.push_back(h);
}

void Unload() {
  if (g_job.running) {
    pid_t pid;
    {
      std::lock_guard<std::mutex> lk(g_job.mu);
      pid = g_job.pid;
    }
    if (pid > 0) kill(pid, SIGTERM);
  }
  if (g_job.th.joinable()) g_job.th.join();
  g_job.running = false;
  if (g_run.phase != Phase::kIdle) Stop(Restore::kPractice, "plugin unload");
  g_handles.clear();
  g_library.clear();
  g_api = nullptr;
}

void Selftest(ru_selftest_add_fn add, void* ctx) { SelftestRun(add, ctx); }

}  // namespace practice::scenarios
