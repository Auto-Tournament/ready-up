// readyup-movement: surf, bhop and kz on community servers: the mode's physics, a run timer
// between the map's start and end zones, personal bests and the map's top times.
//
//   .restart / .rs       back to the start zone (the timer resets)
//   .pb                  your best time on this map
//   .wr / .maptop        the map's top 10 (this mode)
//   .cp / .tp            kz: save a checkpoint / teleport to it (the run counts its teleports)
//   .timerhud            your timer panel on / off (.timer is practice's)
//
// The mode comes from cfg/ReadyUp/movement.cfg `mode=` (auto: from the map prefix surf_ / bhop_ /
// kz_). Zones are the map's own triggers and buttons by name (map_start / map_end,
// timer_startzone / timer_endzone, climb_startbutton / climb_endbutton, ...): their boxes are read
// on map start, players are checked against them every tick. Off while a match is loaded.
//
// Records: csgo/readyup/plugins/movement/records.json (best time per player per map and mode).
#include "movement_rules.h"

#include "readyup/json_store.h"
#include "readyup/match_iface.h"
#include "readyup/plugin_api.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef MOVEMENT_VERSION
#define MOVEMENT_VERSION "dev"
#endif

namespace movement {
namespace {

const ru_api* g_api = nullptr;

struct Off {
  bool resolved = false;
  int ctrlPawn = -1, lifeState = -1, bodyComponent = -1, sceneNode = -1, absOrigin = -1, absVelocity = -1;
  int collision = -1, colMins = -1, colMaxs = -1, entityIdentity = -1, identityName = -1;
  bool ok() const {
    return ctrlPawn >= 0 && lifeState >= 0 && bodyComponent >= 0 && sceneNode >= 0 && absOrigin >= 0 &&
           collision >= 0 && colMins >= 0 && colMaxs >= 0 && entityIdentity >= 0 && identityName >= 0;
  }
};
Off g_off;

struct Zone {
  ZoneKind kind = ZoneKind::None;
  Box box;
  std::string name;
};

struct Run {
  bool inStart = false, running = false;
  double startedAt = 0;
  int teleports = 0;
  bool hasCheckpoint = false;
  Vec3 cp;
  float cpAngles[3] = {0, 0, 0};
  bool hud = true;
  double nextHud = 0;
};

struct State {
  Mode configured = Mode::Off;  // from movement.cfg; Off = auto
  bool autoMode = true;
  Mode mode = Mode::Off;        // this map's
  std::string map;
  std::vector<Zone> zones;
  double scanAt = 0;            // zones are read a moment after the map starts (entities spawn)
  Run runs[64];
  Records records;
  std::string path;
  bool dirty = false;
  double nextSave = 0;
};
State g;

double Now() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

template <typename T>
T Rd(const void* base, int off) {
  T v{};
  std::memcpy(&v, static_cast<const unsigned char*>(base) + off, sizeof(T));
  return v;
}

void Reply(const ru_command_ctx* c, const std::string& m) {
  if (c->is_console || c->slot < 0) ru_logf(g_api, RU_LOG_INFO, "%s", m.c_str());
  else g_api->chat_to_slot(g_api->self, c->slot, m.c_str());
}

bool MatchLoaded() {
  const auto* m = static_cast<const ru_match_v1*>(g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1));
  if (!m || !RU_API_HAS(m, mode) || !m->mode) return false;
  const char* mode = m->mode();
  const std::string s = mode ? mode : "";
  return s.rfind("match", 0) == 0 || s.rfind("knife", 0) == 0 || s.rfind("scrim", 0) == 0;
}

bool Active() { return g.mode != Mode::Off && !MatchLoaded(); }

bool Resolve() {
  if (g_off.resolved) return g_off.ok();
  g_off.resolved = true;
  auto so = [](const char* c, const char* f) { return g_api->schema_offset(g_api->self, c, f); };
  g_off.ctrlPawn = so("CCSPlayerController", "m_hPlayerPawn");
  g_off.lifeState = so("CBaseEntity", "m_lifeState");
  g_off.bodyComponent = so("CBaseEntity", "m_CBodyComponent");
  g_off.sceneNode = so("CBodyComponent", "m_pSceneNode");
  g_off.absOrigin = so("CGameSceneNode", "m_vecAbsOrigin");
  g_off.absVelocity = so("CBaseEntity", "m_vecAbsVelocity");
  g_off.collision = so("CBaseModelEntity", "m_Collision");
  g_off.colMins = so("CCollisionProperty", "m_vecMins");
  g_off.colMaxs = so("CCollisionProperty", "m_vecMaxs");
  g_off.entityIdentity = so("CEntityInstance", "m_pEntity");
  g_off.identityName = so("CEntityIdentity", "m_name");
  if (!g_off.ok()) ru_logf(g_api, RU_LOG_WARN, "schema fields missing: zones and timers off on this build");
  return g_off.ok();
}

bool Origin(void* ent, Vec3* out) {
  void* body = Rd<void*>(ent, g_off.bodyComponent);
  if (!body) return false;
  void* node = Rd<void*>(body, g_off.sceneNode);
  if (!node) return false;
  const float* v = reinterpret_cast<const float*>(static_cast<const unsigned char*>(node) + g_off.absOrigin);
  *out = {v[0], v[1], v[2]};
  return true;
}

std::string TargetName(void* ent) {
  void* ident = Rd<void*>(ent, g_off.entityIdentity);
  if (!ident) return {};
  const char* name = Rd<const char*>(ident, g_off.identityName);
  return name ? std::string(name) : std::string();
}

// The map's start and end zones from its triggers / buttons (by targetname).
void ScanZones() {
  g.zones.clear();
  if (!Resolve()) return;
  int scanned = 0;
  for (int i = 65; i < 8192; ++i) {
    void* ent = g_api->entity_by_index(g_api->self, i);
    if (!ent) continue;
    ++scanned;
    const char* cls = g_api->entity_classname(g_api->self, ent);
    if (!cls) continue;
    const bool trigger = std::strncmp(cls, "trigger_", 8) == 0;
    const bool button = std::strcmp(cls, "func_button") == 0;
    if (!trigger && !button) continue;
    const std::string name = TargetName(ent);
    const ZoneKind kind = ClassifyZone(name);
    if (kind == ZoneKind::None) continue;
    Vec3 o;
    if (!Origin(ent, &o)) continue;
    const unsigned char* col = static_cast<const unsigned char*>(ent) + g_off.collision;
    const float* mn = reinterpret_cast<const float*>(col + g_off.colMins);
    const float* mx = reinterpret_cast<const float*>(col + g_off.colMaxs);
    Zone z;
    z.kind = kind;
    z.name = name;
    z.box.mins = {o.x + mn[0], o.y + mn[1], o.z + mn[2]};
    z.box.maxs = {o.x + mx[0], o.y + mx[1], o.z + mx[2]};
    g.zones.push_back(z);
  }
  int starts = 0, ends = 0;
  for (const auto& z : g.zones) (z.kind == ZoneKind::Start ? starts : ends)++;
  ru_logf(g_api, RU_LOG_INFO, "%s on %s: %d start zone(s), %d end zone(s) (%d entities scanned)", ModeName(g.mode),
          g.map.c_str(), starts, ends, scanned);
}

const Zone* ZoneAt(const Vec3& p, ZoneKind kind) {
  // Buttons are small: a player next to one counts.
  for (const auto& z : g.zones) {
    if (z.kind == kind && Inside(z.box, p, z.name.find("button") != std::string::npos ? 48.0f : 2.0f)) return &z;
  }
  return nullptr;
}

const Zone* FirstZone(ZoneKind kind) {
  for (const auto& z : g.zones) {
    if (z.kind == kind) return &z;
  }
  return nullptr;
}

void ApplyModeCvars() {
  for (const auto& c : ModeCvars(g.mode)) (void)g_api->server_command(g_api->self, c.c_str());
}

void LoadSettings() {
  char buf[64] = {};
  g.autoMode = true;
  g.configured = Mode::Off;
  if (g_api->config_get(g_api->self, "mode", buf, sizeof(buf)) >= 0 && std::string(buf) != "auto") {
    bool ok = false;
    g.configured = ParseMode(buf, &ok);
    g.autoMode = !ok;
    if (!ok) ru_logf(g_api, RU_LOG_WARN, "movement.cfg: mode=%s is not auto, surf, bhop, kz or off", buf);
  }
}

void LoadRecords() {
  std::ifstream f(g.path);
  if (!f) return;
  std::stringstream ss;
  ss << f.rdbuf();
  if (!Deserialize(ss.str(), &g.records)) ru_logf(g_api, RU_LOG_WARN, "records.json could not be read; starting empty");
}

void SaveRecords() {
  if (!g.dirty || g.path.empty()) return;
  const std::string tmp = g.path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return;
    f << Serialize(g.records) << "\n";
  }
  if (std::rename(tmp.c_str(), g.path.c_str()) == 0) g.dirty = false;
}

// ---- runs ---------------------------------------------------------------------------------------

void FinishRun(int slot, double now) {
  Run& r = g.runs[slot];
  r.running = false;
  ru_player p{};
  p.struct_size = sizeof(p);
  if (!g_api->get_player(g_api->self, slot, &p) || p.steamid64 == 0) return;
  const int64_t ms = static_cast<int64_t>((now - r.startedAt) * 1000.0 + 0.5);
  const Finish f = AddRun(&g.records, g.map, ModeName(g.mode), p.steamid64, p.name, ms, static_cast<int64_t>(std::time(nullptr)));
  g.dirty = true;
  std::string line = std::string(p.name) + " finished " + g.map + " in " + FormatTime(ms);
  if (g.mode == Mode::Kz) line += r.teleports > 0 ? " (" + std::to_string(r.teleports) + " TP)" : " (pro)";
  if (f.serverRecord) line += ". New server record!";
  else if (f.personalBest && f.previousMs > 0) line += ". New personal best (-" + FormatTime(f.previousMs - ms) + "), rank " + std::to_string(f.rank) + ".";
  else if (f.personalBest) line += ", rank " + std::to_string(f.rank) + ".";
  else line += " (best " + FormatTime(f.previousMs) + ").";
  g_api->chat_all(g_api->self, line.c_str(), 0);
}

void HudFor(int slot, double now) {
  Run& r = g.runs[slot];
  if (!r.hud || now < r.nextHud) return;
  r.nextHud = now + 0.1;
  std::string h;
  if (r.running) {
    const int64_t ms = static_cast<int64_t>((now - r.startedAt) * 1000.0);
    h = "<font class='fontSize-l' color='#FFD34D'>" + FormatTime(ms) + "</font>";
    if (g.mode == Mode::Kz) h += " <font color='#9AA4B2'>" + std::to_string(r.teleports) + " TP</font>";
  } else if (r.inStart) {
    h = "<font color='#7FD7FF'>In the start zone</font>";
  } else {
    return;
  }
  (void)g_api->center_html_to_slot_prio(g_api->self, slot, h.c_str(), 1, RU_HTML_PRIO_HUD);
}

void OnTick(void*, const ru_tick_info* t) {
  const double now = t->now;
  if (g.scanAt > 0 && now >= g.scanAt) {
    g.scanAt = 0;
    ScanZones();
    ApplyModeCvars();
  }
  if (now >= g.nextSave) {
    g.nextSave = now + 30.0;
    SaveRecords();
  }
  if (!Active() || g.zones.empty() || !Resolve()) return;
  for (int slot = 0; slot < 64; ++slot) {
    Run& r = g.runs[slot];
    void* ctrl = g_api->entity_by_index(g_api->self, slot + 1);
    if (!ctrl) {
      r = Run{};
      continue;
    }
    void* pawn = g_api->entity_from_handle(g_api->self, Rd<uint32_t>(ctrl, g_off.ctrlPawn));
    if (!pawn || Rd<uint8_t>(pawn, g_off.lifeState) != 0) {
      r.running = false;
      r.inStart = false;
      continue;
    }
    Vec3 p;
    if (!Origin(pawn, &p)) continue;
    const bool inStart = ZoneAt(p, ZoneKind::Start) != nullptr;
    if (inStart) {
      r.running = false;
      r.teleports = 0;
    } else if (r.inStart) {
      r.running = true;  // left the start zone: go
      r.startedAt = now;
    }
    r.inStart = inStart;
    if (r.running && ZoneAt(p, ZoneKind::End)) FinishRun(slot, now);
    HudFor(slot, now);
  }
}

void OnMapStart(void*, const ru_event* e) {
  SaveRecords();
  const char* cm = g_api->current_map(g_api->self);
  g.map = cm ? cm : (e && e->map ? e->map : "");
  LoadSettings();
  g.mode = g.autoMode ? ModeForMap(g.map) : g.configured;
  g.zones.clear();
  for (auto& r : g.runs) r = Run{};
  g.scanAt = g.mode != Mode::Off ? Now() + 3.0 : 0;
}

// ---- commands -----------------------------------------------------------------------------------

bool CheckActive(const ru_command_ctx* c) {
  if (Active()) return true;
  Reply(c, MatchLoaded() ? "Movement commands are off during a match." : "This map has no movement mode (surf_, bhop_, kz_).");
  return false;
}

void TeleportTo(int slot, const Vec3& p, const float* angles) {
  const float origin[3] = {p.x, p.y, p.z};
  const float zero[3] = {0, 0, 0};
  (void)g_api->player_teleport(g_api->self, slot, origin, angles, zero);
}

void OnRestart(void*, const ru_command_ctx* c) {
  if (!CheckActive(c) || c->slot < 0) return;
  const Zone* z = FirstZone(ZoneKind::Start);
  if (!z) return Reply(c, "This map has no start zone the timer knows.");
  g.runs[c->slot].running = false;
  TeleportTo(c->slot, Center(z->box), nullptr);
}

void OnPb(void*, const ru_command_ctx* c) {
  if (!CheckActive(c) || c->steamid64 == 0) return;
  const auto all = Top(g.records, g.map, ModeName(g.mode), 100000);
  for (size_t i = 0; i < all.size(); ++i) {
    if (all[i].steamid64 == c->steamid64) {
      return Reply(c, "Your best on " + g.map + ": " + FormatTime(all[i].rec.ms) + ", rank " + std::to_string(i + 1) + " of " +
                          std::to_string(all.size()) + ".");
    }
  }
  Reply(c, "No time on " + g.map + " yet.");
}

void OnTop(void*, const ru_command_ctx* c) {
  if (!CheckActive(c)) return;
  const auto top = Top(g.records, g.map, ModeName(g.mode), 10);
  if (top.empty()) return Reply(c, "No times on " + g.map + " yet.");
  Reply(c, "Top times on " + g.map + " (" + ModeName(g.mode) + "):");
  for (size_t i = 0; i < top.size(); ++i) {
    Reply(c, std::to_string(i + 1) + ". " + FormatTime(top[i].rec.ms) + "  " + top[i].rec.name);
  }
}

void OnCheckpoint(void*, const ru_command_ctx* c) {
  if (!CheckActive(c) || c->slot < 0) return;
  if (g.mode != Mode::Kz) return Reply(c, "Checkpoints are for kz maps.");
  void* ctrl = g_api->entity_by_index(g_api->self, c->slot + 1);
  void* pawn = ctrl ? g_api->entity_from_handle(g_api->self, Rd<uint32_t>(ctrl, g_off.ctrlPawn)) : nullptr;
  Vec3 p;
  if (!pawn || !Origin(pawn, &p)) return;
  Run& r = g.runs[c->slot];
  r.cp = p;
  r.hasCheckpoint = true;
  Reply(c, "Checkpoint saved.");
}

void OnTeleport(void*, const ru_command_ctx* c) {
  if (!CheckActive(c) || c->slot < 0) return;
  Run& r = g.runs[c->slot];
  if (!r.hasCheckpoint) return Reply(c, "No checkpoint yet: .cp saves one.");
  if (r.running) ++r.teleports;
  TeleportTo(c->slot, r.cp, nullptr);
}

void OnTimer(void*, const ru_command_ctx* c) {
  if (c->slot < 0) return;
  Run& r = g.runs[c->slot];
  r.hud = !r.hud;
  if (!r.hud) g_api->center_html_release(g_api->self, c->slot);
  Reply(c, r.hud ? "Timer panel on." : "Timer panel off.");
}

}  // namespace
}  // namespace movement

using namespace movement;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 13u,  // needs API 1.13 (player_teleport)
      "movement",
      MOVEMENT_VERSION,
      "Ready Up",
      "surf, bhop, kz: the mode's physics, a run timer between the map's zones, personal bests and top times",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, player_teleport) || !RU_API_HAS(api, data_dir)) return 1;
  g_api = api;
  g = State{};
  g_off = Off{};
  const char* dir = api->data_dir(api->self);
  const std::string data = dir && *dir ? dir : ".";
  std::string err;
  (void)readyup::json_store::MakeDirs(data, &err);
  g.path = data + "/records.json";
  LoadRecords();
  LoadSettings();
  const char* cm = api->current_map(api->self);
  g.map = cm ? cm : "";
  g.mode = g.autoMode ? ModeForMap(g.map) : g.configured;
  g.scanAt = g.mode != Mode::Off ? Now() + 1.0 : 0;
  struct Cmd {
    const char* name;
    ru_command_fn fn;
  };
  for (const Cmd& c : {Cmd{".restart", &OnRestart}, Cmd{".rs", &OnRestart}, Cmd{".pb", &OnPb}, Cmd{".wr", &OnTop},
                       Cmd{".maptop", &OnTop}, Cmd{".cp", &OnCheckpoint}, Cmd{".tp", &OnTeleport}, Cmd{".timerhud", &OnTimer}}) {
    if (!api->register_chat_command(api->self, c.name, c.fn, nullptr)) {
      ru_logf(api, RU_LOG_WARN, "could not register `%s` (another plugin owns it)", c.name);
    }
  }
  api->subscribe(api->self, RU_EVENT_MAP_START, &OnMapStart, nullptr);
  api->on_tick(api->self, &OnTick, nullptr);
  ru_logf(api, RU_LOG_INFO, "loaded " MOVEMENT_VERSION ": %s on %s", ModeName(g.mode), g.map.c_str());
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  SaveRecords();
  if (g_api) ru_logf(g_api, RU_LOG_INFO, "unloaded");
  g_api = nullptr;
}

}  // extern "C"
