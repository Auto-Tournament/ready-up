// Practice-mode tools (see practice_tools.h). Engine access through ru_api only: CS2 cheat
// commands on the server console (server_command) and schema reads / writes of the caller's pawn
// (schema_offset, entity_by_index, entity_from_handle, entity_mark_changed).
#include "readyup/practice_tools.h"

#include "readyup/engine.h"
#include "readyup/host.h"
#include "readyup/logging.h"
#include "readyup/match_events.h"
#include "readyup/modes.h"
#include "readyup/players.h"
#include "readyup/votes.h"
#include "readyup/webhook.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace readyup {
namespace {

const ru_api* A() { return host::Api(); }

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

int FirstOffset(std::initializer_list<const char*> classes, const char* field) {
  const ru_api* a = A();
  if (!a) return -1;
  for (const char* c : classes) {
    const int off = a->schema_offset(a->self, c, field);
    if (off >= 0) return off;
  }
  return -1;
}

const Offsets& Off() {
  static Offsets o;
  const ru_api* a = A();
  if (o.looked || !a || a->entity_system_status(a->self) != RU_ENTSYS_OK) return o;
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
  Print("practice: schema pawn=%d body=%d node=%d origin=%d life=%d eyes=%d flash=%d/%d takesdamage=%d spawn=%d/%d\n",
        o.ctrlPawn, o.bodyComponent, o.sceneNode, o.absOrigin, o.lifeState, o.eyeAngles, o.flashAlpha,
        o.flashDuration, o.takesDamage, o.spawnEnabled, o.spawnPriority);
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
  const ru_api* a = A();
  const Offsets& o = Off();
  if (!a || slot < 0 || o.ctrlPawn < 0) return nullptr;
  void* ctrl = a->entity_by_index(a->self, slot + 1);
  if (!ctrl) return nullptr;
  const char* cls = a->entity_classname(a->self, ctrl);
  if (!cls || std::strcmp(cls, "cs_player_controller") != 0) return nullptr;
  return a->entity_from_handle(a->self, Rd<uint32_t>(ctrl, o.ctrlPawn));
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

// CS2's setpos / setpos_player / ent_setpos need a client of their own: from the server console
// they do nothing (1.41.8.4). The move goes through ru_api entity_set_abs_origin (v1.3), the same
// CBaseEntity::SetAbsOrigin call those commands make.
bool Teleport(int slot, const Vec3& p) {
  const ru_api* a = A();
  if (!a || !RU_API_HAS(a, entity_set_abs_origin) || !a->entity_set_abs_origin) return false;
  void* pawn = PawnForSlot(slot);
  if (!Alive(pawn)) return false;
  const float xyz[3] = {p.x, p.y, p.z};
  return a->entity_set_abs_origin(a->self, pawn, xyz) == 1;
}

constexpr const char* kNoTeleport =
    "cannot move you: teleport is unavailable on this server build (ru selftest: CBaseEntity::SetAbsOrigin).";

std::string Fmt(const Vec3& p) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%.1f %.1f %.1f", p.x, p.y, p.z);
  return buf;
}

// ---- state (game thread only) ---------------------------------------------------------------------

std::string g_map;
std::map<uint64_t, std::map<std::string, Spot>> g_saved;  // player id -> name -> spot
std::unordered_map<uint64_t, Spot> g_lastThrow;          // grenade_thrown position
std::unordered_map<uint64_t, Spot> g_beforeTeleport;     // where .loadpos / .spawn took them from
std::unordered_set<int> g_noflash;                        // slots
std::unordered_set<int> g_god;                            // slots (m_bTakesDamage false)
bool g_buddhaOff = false;                                 // .god fallback toggled buddha off
bool g_active = false;                                    // any of the above set this practice session

struct PendingBot {
  int callerSlot = -1;
  Vec3 pos;
  std::set<int> before;  // bot userids when the bot was requested
  double deadline = 0;
};
std::vector<PendingBot> g_pendingBots;

uint64_t PlayerId(int slot, uint64_t steamid64) { return steamid64 ? steamid64 : DevBotIdForUserid(slot); }

void Reply(int slot, uint64_t steamid64, const std::string& msg) {
  Print("practice: slot %d: %s\n", slot, msg.c_str());
  if (steamid64 != 0 && !IsDevBotId(steamid64) && slot >= 0) {
    (void)ClientPrintChat(slot, (" \x04[ReadyUp]\x01 " + msg).c_str());
  }
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
        A()->entity_mark_changed(A()->self, pawn);
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
  g_active = false;
}

bool InPractice() { return GetMode() == ReadyUpMode::Practice; }

int TeamOfSlot(int slot) {
  const auto t = GetCsTeamNumForSlot(slot);
  return t ? *t : 0;
}

// ---- spawns -------------------------------------------------------------------------------------

struct SpawnPt {
  int index;
  int priority;
  Vec3 pos;
};

std::vector<SpawnPt> ListSpawns(int team) {
  std::vector<SpawnPt> out;
  const ru_api* a = A();
  const Offsets& o = Off();
  if (!a) return out;
  const char* want = team == 3 ? "info_player_counterterrorist" : "info_player_terrorist";
  int seen = 0, matched = 0, disabled = 0, noOrigin = 0;
  // Every entity slot: server-only entities (spawn points) sit above the 16384 networked ones.
  for (int i = 0; i < 32768; ++i) {
    void* e = a->entity_by_index(a->self, i);
    if (!e) continue;
    ++seen;
    const char* cls = a->entity_classname(a->self, e);
    if (!cls || std::strcmp(cls, want) != 0) continue;
    ++matched;
    if (o.spawnEnabled >= 0 && !Rd<bool>(e, o.spawnEnabled)) {
      ++disabled;
      continue;
    }
    SpawnPt s{i, o.spawnPriority >= 0 ? Rd<int32_t>(e, o.spawnPriority) : 0, {}};
    if (!OriginOf(e, &s.pos)) {
      ++noOrigin;
      continue;
    }
    out.push_back(s);
  }
  Debug("practice: spawns %s: %d entities, %d matched, %d disabled, %d without origin, %zu usable\n", want, seen,
        matched, disabled, noOrigin, out.size());
  std::sort(out.begin(), out.end(), [](const SpawnPt& x, const SpawnPt& y) {
    return x.priority != y.priority ? x.priority < y.priority : x.index < y.index;
  });
  return out;
}

// ---- commands ---------------------------------------------------------------------------------

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool IsPracticeCommand(const std::string& c) {
  static const std::set<std::string> k = {".rethrow", ".rt",    ".savepos", ".loadpos", ".back",   ".clear",
                                          ".noflash", ".god",   ".spawn",   ".ctspawn", ".tspawn"};
  return k.count(c) != 0;
}

bool IsBotCommand(const std::string& c) {
  return c == ".bot" || c == ".cbot" || c == ".crouchbot" || c == ".boost" || c == ".crouchboost" || c == ".nobots";
}

bool TeleportRemembering(int slot, uint64_t id, const Vec3& to) {
  Spot here;
  const bool had = SpotOfSlot(slot, &here);
  if (!Teleport(slot, to)) return false;
  if (had) g_beforeTeleport[id] = here;
  g_active = true;
  return true;
}

void RunPractice(int slot, uint64_t steamid64, const std::vector<std::string>& args) {
  const std::string cmd = Lower(args[0]);
  if (!InPractice()) {
    Reply(slot, steamid64, cmd + " only works in practice mode (.prac).");
    return;
  }
  const uint64_t id = PlayerId(slot, steamid64);
  const std::string name = args.size() > 1 ? args[1].substr(0, 32) : std::string("default");

  if (cmd == ".rethrow" || cmd == ".rt") {
    (void)EnqueueServerCommand("sv_rethrow_last_grenade");
    Reply(slot, steamid64, "rethrowing the last grenade thrown on the server.");
    return;
  }
  if (cmd == ".clear") {
    // ent_remove_all works from the server console; ent_fire needs a client of its own there.
    for (const char* c : {"ent_remove_all smokegrenade_projectile", "ent_remove_all molotov_projectile",
                          "ent_remove_all inferno", "ent_remove_all decoy_projectile"}) {
      (void)EnqueueServerCommand(c);
    }
    Reply(slot, steamid64, "smokes, molotovs and decoys cleared.");
    return;
  }
  if (cmd == ".savepos") {
    Spot s;
    if (!SpotOfSlot(slot, &s)) return Reply(slot, steamid64, "cannot read your position (are you alive?).");
    g_saved[id][name] = s;
    g_active = true;
    Reply(slot, steamid64, "position \"" + name + "\" saved (" + Fmt(s.pos) + ").");
    return;
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
    Reply(slot, steamid64, "loaded position \"" + name + "\" (" + Fmt(s.pos) + "; view angle not restored).");
    return;
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
    Reply(slot, steamid64, std::string("back to ") + what + " (" + Fmt(to) + ").");
    return;
  }
  if (cmd == ".noflash") {
    const Offsets& o = Off();
    void* pawn = PawnForSlot(slot);
    if (o.flashAlpha < 0 || !pawn) return Reply(slot, steamid64, "noflash is unavailable (pawn / schema field not found).");
    const bool on = !g_noflash.count(slot);
    if (on) g_noflash.insert(slot);
    else g_noflash.erase(slot);
    Wr<float>(pawn, o.flashAlpha, on ? 0.f : 255.f);
    A()->entity_mark_changed(A()->self, pawn);
    g_active = true;
    Reply(slot, steamid64, on ? "noflash on." : "noflash off.");
    return;
  }
  if (cmd == ".god") {
    const Offsets& o = Off();
    void* pawn = PawnForSlot(slot);
    if (o.takesDamage >= 0 && pawn) {
      const bool on = !g_god.count(slot);
      if (on) g_god.insert(slot);
      else g_god.erase(slot);
      Wr<bool>(pawn, o.takesDamage, !on);
      g_active = true;
      Reply(slot, steamid64, on ? "god mode on (you take no damage)." : "god mode off.");
      return;
    }
    // Fallback: buddha (server-wide, prac.cfg turns it on).
    g_buddhaOff = !g_buddhaOff;
    (void)EnqueueServerCommand(g_buddhaOff ? "buddha 0" : "buddha 1");
    g_active = true;
    Reply(slot, steamid64, g_buddhaOff ? "buddha off for everyone." : "buddha on for everyone (nobody dies).");
    return;
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
    Reply(slot, steamid64, std::string(side) + " spawn " + std::to_string(n) + "/" + std::to_string(spawns.size()) +
                               " (" + Fmt(to) + ").");
    return;
  }
}

// ---- engine callbacks ---------------------------------------------------------------------------

void OnChat(void*, const ru_command_ctx* c) {
  try {
    int slot = c->slot;
    if (slot < 0) {
      if (auto s = GameEventsSlotForSteam(c->steamid64)) slot = *s;
    }
    std::vector<std::string> args;
    for (int i = 0; i < c->argc; ++i) args.emplace_back(c->argv[i] ? c->argv[i] : "");
    if (args.empty()) return;
    if (slot < 0) {
      Print("practice: %s from %s: slot unknown\n", args[0].c_str(), c->name ? c->name : "?");
      return;
    }
    RunPractice(slot, c->steamid64, args);
  } catch (...) {
    Print("practice: chat command threw\n");
  }
}

// `ru as <slot> <chat line>`: the server console acts as that player (bot tests).
void OnRuAs(void*, const ru_command_ctx* c) {
  try {
    if (!c->is_console) {
      if (c->slot >= 0) (void)ClientPrintChat(c->slot, " \x04[ReadyUp]\x01 .ru as is console / RCON only.");
      return;
    }
    if (c->argc < 4) {
      PrintLine("usage: ru as <slot> <.command> [args] (practice tools, .bot, .gg, .stop as that player)");
      return;
    }
    const int slot = std::atoi(c->argv[2]);
    ru_player p{};
    p.struct_size = sizeof(p);
    uint64_t steamid64 = 0;
    std::string name;
    int csTeam = 0;
    bool found = false;
    struct Ctx {
      int slot;
      ru_player* out;
      bool* found;
    } fc{slot, &p, &found};
    A()->for_each_player(
        A()->self,
        [](void* u, const ru_player* pl) -> int {
          auto* f = static_cast<Ctx*>(u);
          const int s = pl->slot >= 0 ? pl->slot : pl->userid;
          if (s != f->slot) return 1;
          std::memcpy(f->out, pl, std::min<size_t>(sizeof(ru_player), pl->struct_size));
          *f->found = true;
          return 0;
        },
        &fc);
    if (!found) {
      Print("ru as: no player in slot %d\n", slot);
      return;
    }
    steamid64 = p.is_bot ? DevBotIdForUserid(slot) : p.steamid64;
    name = p.name;
    csTeam = TeamOfSlot(slot);
    if (csTeam != 2 && csTeam != 3) csTeam = p.team;
    std::vector<std::string> args;
    for (int i = 3; i < c->argc; ++i) args.emplace_back(c->argv[i] ? c->argv[i] : "");
    const std::string cmd = Lower(args[0]);
    Print("ru as: slot %d (%s%s) runs %s\n", slot, name.c_str(), p.is_bot ? ", bot" : "", cmd.c_str());
    if (IsPracticeCommand(cmd)) {
      RunPractice(slot, p.is_bot ? 0 : steamid64, args);
    } else if (IsBotCommand(cmd)) {
      PracticeToolsBotCommand(slot, p.is_bot ? 0 : steamid64, cmd);
    } else if (cmd == ".gg" || cmd == ".stop") {
      WebhookTeam team = WebhookTeam::Unknown;
      if (auto ctx = WebhookGetMatchContext()) {
        if (auto it = ctx->roster_team.find(steamid64); it != ctx->roster_team.end()) team = it->second;
      }
      if (team == WebhookTeam::Unknown) team = VotesTeamForSide(csTeam);
      if (team == WebhookTeam::Unknown) {
        Print("ru as: slot %d has no match team\n", slot);
        return;
      }
      if (cmd == ".gg") {
        if (!VotesGg(steamid64, team, name)) Print("ru as: .gg vote is off (gg_enabled=0)\n");
      } else {
        VotesStop(steamid64, team, name);
      }
    } else {
      Print("ru as: %s is not supported (practice tools, bot commands, .gg, .stop)\n", cmd.c_str());
    }
  } catch (...) {
    Print("ru as: threw\n");
  }
}

void OnGameEvent(void*, const char* evName, const ru_game_event* ev) {
  const ru_api* a = A();
  if (!a || !evName || !ev || !InPractice()) return;
  if (std::strcmp(evName, "grenade_thrown") == 0) {
    const int slot = a->ev_get_player_slot(a->self, ev, "userid");
    if (slot < 0) return;
    Spot s;
    if (!SpotOfSlot(slot, &s)) return;
    auto ident = GetSlotIdentity(slot);
    g_lastThrow[PlayerId(slot, ident ? ident->steamid64 : 0)] = s;
  } else if (std::strcmp(evName, "player_blind") == 0) {
    const int slot = a->ev_get_player_slot(a->self, ev, "userid");
    if (slot < 0 || !g_noflash.count(slot)) return;
    void* pawn = a->ev_get_player_pawn(a->self, ev, "userid");
    const Offsets& o = Off();
    if (!pawn) return;
    if (o.flashAlpha >= 0) Wr<float>(pawn, o.flashAlpha, 0.f);
    if (o.flashDuration >= 0) Wr<float>(pawn, o.flashDuration, 0.f);
    a->entity_mark_changed(a->self, pawn);
    Debug("practice: noflash: cleared the flash of slot %d\n", slot);
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

}  // namespace

const char* PracticeToolsHelp() {
  return "Ready Up practice: .bot .cbot .nobots | .savepos/.loadpos [name] .back | .spawn/.ctspawn/.tspawn N | "
         ".rethrow .clear .noflash .god | .prac to leave";
}

void PracticeToolsBotCommand(int slot, uint64_t id, const std::string& cmd) {
  (void)id;
  if (!InPractice()) {
    SendToChat("Ready Up: bot commands are only available in practice mode.");
    return;
  }
  if (cmd == ".nobots") {
    (void)EnqueueServerCommand("bot_kick");
    g_pendingBots.clear();
    SendToChat("Ready Up: bots removed.");
    return;
  }
  // One bot on the other team (CT player -> T bot, else CT bot), placed where the caller stands.
  const int tn = slot >= 0 ? TeamOfSlot(slot) : 0;
  const bool crouch = (cmd == ".cbot" || cmd == ".crouchbot" || cmd == ".crouchboost");
  if (crouch) (void)EnqueueServerCommand("bot_crouch 1");
  if (tn == 3) {
    (void)EnqueueServerCommand("bot_join_team T");
    (void)EnqueueServerCommand("bot_add_t");
  } else {
    (void)EnqueueServerCommand("bot_join_team CT");
    (void)EnqueueServerCommand("bot_add_ct");
  }
  (void)EnqueueServerCommand("bot_stop 1");
  (void)EnqueueServerCommand("bot_freeze 1");
  (void)EnqueueServerCommand("bot_zombie 1");
  if (crouch) (void)EnqueueServerCommand("bot_crouch 0");  // future bots are not forced to crouch
  Spot s;
  if (slot >= 0 && SpotOfSlot(slot, &s)) {
    PendingBot p;
    p.callerSlot = slot;
    p.pos = s.pos;
    for (const auto& b : ListBots()) p.before.insert(b.userid);
    p.deadline = host::NowSeconds() + 5.0;
    g_pendingBots.push_back(std::move(p));
    g_active = true;
  }
  SendToChat(crouch ? "Ready Up: crouch bot added." : "Ready Up: bot added.");
}

void PracticeToolsInstall(const ru_api* api) {
  for (const char* c : {".rethrow", ".rt", ".savepos", ".loadpos", ".back", ".clear", ".noflash", ".god", ".spawn",
                        ".ctspawn", ".tspawn"}) {
    if (!api->register_chat_command(api->self, c, &OnChat, nullptr)) Print("practice: could not register %s\n", c);
  }
  if (!api->register_ru_subcommand(api->self, "as", &OnRuAs, nullptr)) Print("practice: could not register `ru as`\n");
  for (const char* e : {"grenade_thrown", "player_blind", "player_spawn"}) {
    if (!api->subscribe_game_event(api->self, e, &OnGameEvent, nullptr)) {
      Print("practice: could not subscribe to %s\n", e);
    }
  }
}

void PracticeToolsTick(double now) {
  const ru_api* a = A();
  if (!a) return;
  const std::string map = a->current_map(a->self) ? a->current_map(a->self) : "";
  if (map != g_map) {
    g_map = map;
    ClearState(/*restorePawns=*/false);  // new map, new pawns
  }
  if (!InPractice()) {
    if (g_active) {
      ClearState(/*restorePawns=*/true);
      Print("practice: left practice mode; positions and toggles cleared\n");
    }
    return;
  }
  if (g_pendingBots.empty()) return;
  const auto bots = ListBots();
  for (auto it = g_pendingBots.begin(); it != g_pendingBots.end();) {
    bool done = false;
    for (const auto& b : bots) {
      if (it->before.count(b.userid)) continue;
      if (!Alive(PawnForSlot(b.userid))) continue;
      const bool moved = Teleport(b.userid, it->pos);
      Print("practice: bot %s (slot %d) %s slot %d's position (%s)\n", b.name.c_str(), b.userid,
            moved ? "placed at" : "could not be moved to", it->callerSlot, Fmt(it->pos).c_str());
      it->before.insert(b.userid);
      done = true;
      break;
    }
    if (done) {
      it = g_pendingBots.erase(it);
    } else if (now > it->deadline) {
      Print("practice: new bot did not spawn within 5s; left at its spawn\n");
      it = g_pendingBots.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace readyup
