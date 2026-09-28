// readyup-midas: a fun plugin. Every weapon a Midas player picks up (buys, gets at spawn, picks
// off the floor) turns gold. It stays gold when someone else picks it up (Midas touch).
//
// Gold, two ways (`finish`):
//   - a gold paint kit (`paint_kit`, default 1025 "Gold Brick") through the skins plugin's
//     readyup.skins.v1 paint_weapon, when skins.so is loaded and active (finish=auto, default);
//   - else, and for knives / grenades / C4, the render colour: m_clrRender = `color`
//     (default 255,200,40). The server can't send clients custom textures.
//
// Who is Midas: the players in `midas_steamids`, plus with best_player=1 the best player of the
// map (top ADR or kills, `best_player_stat`) from the match plugin's stats (readyup.match.v1
// map_stats), picked a few ticks after a round start (every round once
// `best_player_min_rounds` are played, or at each new half: `best_player_when`). Scrims only
// unless best_player_in_matches=1.
//
// StatTrak (`stattrak=1`, default): the Midas weapons a Midas player holds (guns and knives) show
// a StatTrak counter = the player's kills on this map (the match plugin's stats while they record,
// else Midas's own player_death count since the map started), rewritten when it changes.
//
//   cfg/ReadyUp/midas.cfg (or readyup.cfg [midas]), re-read every 5 s: see that file.
//
// Never active under the valve ruleset (the match plugin's ruleset, else readyup.cfg's): going
// inert puts every weapon it touched back (white / handed back to skins.so), and so does
// unloading. `ru plugin reload midas` therefore restores, then the new image paints again on the
// next pickup / spawn. Engine access through ru_api only (schema offsets, entity handles,
// entity_mark_changed); paint kits only through skins.so.
#include "midas_rules.h"

#include "readyup/match_iface.h"
#include "readyup/plugin_api.h"
#include "readyup/selftest_iface.h"
#include "readyup/skins_iface.h"

#include <cstdio>
#include <cctype>
#include <cstring>
#include <iterator>
#include <map>
#include <sstream>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#ifndef MIDAS_VERSION
#define MIDAS_VERSION "dev"
#endif

namespace midas {
namespace {

const ru_api* g_api = nullptr;

struct Offsets {
  bool resolved = false;
  int ctrl_playerPawn = -1;      // CCSPlayerController::m_hPlayerPawn (CHandle)
  int ctrl_steamId = -1;         // CBasePlayerController::m_steamID
  int pawn_weaponServices = -1;  // CBasePlayerPawn::m_pWeaponServices (ptr)
  int ws_myWeapons = -1;         // CPlayer_WeaponServices::m_hMyWeapons (CNetworkUtlVectorBase<CHandle>)
  int clrRender = -1;            // CBaseModelEntity::m_clrRender (Color: r, g, b, a bytes)
  int teamNum = -1;              // CBaseEntity::m_iTeamNum (optional: team kills don't count for StatTrak)
  bool Ok() const {
    return ctrl_playerPawn >= 0 && ctrl_steamId >= 0 && pawn_weaponServices >= 0 && ws_myWeapons >= 0 && clrRender >= 0;
  }
} g_off;

// Config (refreshed every 5 s) and state.
bool g_enabled = false;
std::set<uint64_t> g_midas;         // midas_steamids
Rgba g_color = kGold;
Finish g_finish = Finish::kAuto;
int g_paintKit = kGoldPaintKit;
float g_paintWear = 0.0f;
int g_paintSeed = 0;
bool g_bestOn = false;              // best_player
bool g_bestInMatches = false;       // best_player_in_matches
BestStat g_bestStat = BestStat::kAdr;
BestWhen g_bestWhen = BestWhen::kRound;
int g_bestMinRounds = 3;
std::string g_ruleset = "default";
bool g_active = false;
double g_lastConfig = -1e9;
int g_pending[65] = {};             // ticks left to (re)check a slot's weapons after an event
std::set<uint32_t> g_tinted;        // weapon handles this image tinted (restored when inert / unloading)
std::set<uint32_t> g_painted;       // weapon handles painted through skins.so (handed back likewise)
uint64_t g_best = 0;                // the best player's SteamID64 while they are Midas, else 0
std::string g_bestLine;             // why (selftest)
int g_lastPickHalf = 0;             // best_player_when=half: the half the last pick was for
int g_pickIn = 0;                   // ticks until the pick after a round start (0 = none pending)
// StatTrak (`stattrak`, default 1): Midas weapons count the Midas player's kills on this map.
bool g_stattrakOn = true;
bool g_debugBots = false;           // debug_midas_bots=1 with debug on (dev): bots are Midas too
std::map<uint64_t, int> g_ownKills; // player_death kills since the map started (KillKey)
std::map<uint32_t, int> g_stattrak; // weapon handle -> the count written on it
int g_stattrakDue[65] = {};         // ticks until a slot's counter is refreshed after a kill

std::string ConfigValue(const char* key) {
  char buf[512] = {};
  return g_api->config_get(g_api->self, key, buf, sizeof(buf)) > 0 ? std::string(buf) : std::string();
}

void ResolveOffsets() {
  if (g_off.resolved) return;
  if (g_api->entity_system_status(g_api->self) != RU_ENTSYS_OK) return;
  auto F = [](const char* c, const char* f) { return g_api->schema_offset(g_api->self, c, f); };
  g_off.ctrl_playerPawn = F("CCSPlayerController", "m_hPlayerPawn");
  g_off.ctrl_steamId = F("CBasePlayerController", "m_steamID");
  g_off.pawn_weaponServices = F("CBasePlayerPawn", "m_pWeaponServices");
  g_off.ws_myWeapons = F("CPlayer_WeaponServices", "m_hMyWeapons");
  g_off.clrRender = F("CBaseModelEntity", "m_clrRender");
  g_off.teamNum = F("CBaseEntity", "m_iTeamNum");
  g_off.resolved = true;
  if (!g_off.Ok()) {
    ru_logf(g_api, RU_LOG_WARN, "schema fields missing (m_clrRender=%d, weapons=%d/%d); no tint", g_off.clrRender,
            g_off.pawn_weaponServices, g_off.ws_myWeapons);
  }
}

template <typename T>
T Rd(const void* base, int off) {
  T v{};
  std::memcpy(&v, static_cast<const unsigned char*>(base) + off, sizeof(T));
  return v;
}

// Writes the colour when it differs. True if written.
bool SetColor(void* ent, const Rgba& c) {
  unsigned char* p = static_cast<unsigned char*>(ent) + g_off.clrRender;
  const unsigned char want[4] = {c.r, c.g, c.b, c.a};
  if (std::memcmp(p, want, 4) == 0) return false;
  std::memcpy(p, want, 4);
  g_api->entity_mark_changed(g_api->self, ent);
  return true;
}

// skins.so's readyup.skins.v1 with paint_weapon, or NULL (not loaded / reloading). Look it up in
// every callback: the pointer is only valid until skins.so can unload.
const ru_skins_v1* Skins() {
  const auto* s = static_cast<const ru_skins_v1*>(g_api->get_interface(g_api->self, RU_SKINS_IFACE_NAME, 1));
  return s && RU_API_HAS(s, paint_weapon) && s->active && s->paint_weapon ? s : nullptr;
}

bool IsMidas(uint64_t sid) { return ShouldTint(g_active, g_midas, sid) || (g_active && sid != 0 && sid == g_best); }
// A player slot is Midas: its SteamID64, or (debug_midas_bots, dev) any bot (SteamID64 0).
bool IsMidasSlot(uint64_t sid) { return IsMidas(sid) || (g_active && g_debugBots && sid == 0); }

void SwapOutStatTrak(const std::set<uint32_t>& handles, const char* why);

// Puts one weapon back: white, and handed back to skins.so if it painted it. True if it was ours.
// Its StatTrak counter can't be taken off (the core has no attribute remove): callers swap the
// weapons that had one for new ones (SwapOutStatTrak).
bool RestoreWeapon(uint32_t h, const ru_skins_v1* skins) {
  bool ours = false;
  if (g_stattrak.erase(h)) ours = true;
  void* w = g_api->entity_from_handle(g_api->self, h);
  if (g_tinted.erase(h)) {
    ours = true;
    if (w) SetColor(w, kWhite);
  }
  if (g_painted.erase(h)) {
    ours = true;
    if (w && skins) skins->paint_weapon(h, 0, 0, 0.0f, 0);
  }
  return ours;
}

// swapStatTrak: weapons that carry a Midas StatTrak counter are swapped for new ones (skins.so
// refresh_weapons); not when the caller swaps Midas players' weapons itself (finish changed).
void RestoreAll(const char* why, bool swapStatTrak = true) {
  if (g_tinted.empty() && g_painted.empty() && g_stattrak.empty()) return;
  int n = 0;
  std::set<uint32_t> counted;
  for (const auto& kv : g_stattrak) counted.insert(kv.first);
  if (g_off.Ok() && g_api->entity_system_status(g_api->self) == RU_ENTSYS_OK) {
    const ru_skins_v1* skins = Skins();
    std::set<uint32_t> all = g_tinted;
    all.insert(g_painted.begin(), g_painted.end());
    all.insert(counted.begin(), counted.end());
    for (uint32_t h : all) n += RestoreWeapon(h, skins) ? 1 : 0;
    if (swapStatTrak) SwapOutStatTrak(counted, why);
  }
  ru_logf(g_api, RU_LOG_INFO, "restored %d weapon(s) to their normal look (%s)", n, why);
  g_tinted.clear();
  g_painted.clear();
  g_stattrak.clear();
}

// skins.so's set_player_paint: Midas players' new weapons get the gold paint kit when they are
// created, before they are networked, instead of their loadout skin (a weapon painted after
// that keeps its old wear on clients, and bought weapons were not gold at all). Sent again on
// every config read (5 s) so a reloaded skins.so gets it back; `clear`: take it all back.
std::set<uint64_t> g_overrideSent;
// `.midas` trial settings (runtime only; `.midas reset` or a plugin reload goes back to the cfg).
int g_trialKit = 0;          // > 0: used instead of paint_kit
std::set<uint32_t> g_modelFailed, g_noModel;  // logged once per weapon handle
std::set<uint32_t> g_modelled;  // weapon handles given a Midas model (model_<classname>)

// plugins/midas/models.txt (the readyup_midas addon's midas_models.txt): "<defindex> <classname> <model>"
// per line. Items sharing a classname (M4A4 / M4A1-S, USP-S / P2000, knives, ...) are told apart by
// the item definition index. Re-read with the config.
std::map<int, std::string> g_modelByDef;
std::string g_modelsPath;
void LoadModelList() {
  std::map<int, std::string> m;
  std::ifstream f(g_modelsPath);
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream ss(line);
    int def = 0;
    std::string cls, model;
    if (line.empty() || line[0] == '#' || !(ss >> def >> cls >> model) || def <= 0) continue;
    m[def] = model;
  }
  if (m != g_modelByDef) {
    g_modelByDef = std::move(m);
    ru_logf(g_api, RU_LOG_INFO, "models.txt: %zu weapon model(s)", g_modelByDef.size());
  }
}

int ItemDefIndex(void* w) {
  const int mgr = g_api->schema_offset(g_api->self, "CEconEntity", "m_AttributeManager");
  const int item = g_api->schema_offset(g_api->self, "CAttributeContainer", "m_Item");
  const int def = g_api->schema_offset(g_api->self, "CEconItemView", "m_iItemDefinitionIndex");
  if (mgr < 0 || item < 0 || def < 0) return 0;
  uint16_t v = 0;
  std::memcpy(&v, static_cast<unsigned char*>(w) + mgr + item + def, sizeof(v));
  return v;
}

// model_def_<n> (midas.cfg) > models.txt > model_<classname> (midas.cfg).
std::string ModelFor(void* w, const char* classname) {
  const int def = ItemDefIndex(w);
  if (def > 0) {
    const std::string byCfg = ConfigValue(("model_def_" + std::to_string(def)).c_str());
    if (!byCfg.empty()) return byCfg;
    const auto it = g_modelByDef.find(def);
    if (it != g_modelByDef.end()) return it->second;
  }
  return ConfigValue((std::string("model_") + classname).c_str());
}

// A Midas model is drawn with its normal (hd) mesh. The gold paint kit through skins.so may be a
// legacy one, which switches the weapon to the legacy mesh (m_MeshGroupMask 2, body 1): on the Midas
// model that put the charm on the wrong attachment. Mesh group mask: m_CBodyComponent ->
// CBodyComponentSkeletonInstance::m_skeletonInstance -> m_modelState.m_MeshGroupMask.
// CUtlStringToken of a name: MurmurHash2 of the lower-cased bytes, seed 0x31415926 (Source 2).
uint32_t StringToken(const std::string& name) {
  const uint32_t m = 0x5bd1e995;
  const int r = 24;
  uint32_t h = 0x31415926u ^ static_cast<uint32_t>(name.size());
  std::string low = name;
  for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const unsigned char* d = reinterpret_cast<const unsigned char*>(low.data());
  size_t len = low.size();
  while (len >= 4) {
    uint32_t k = static_cast<uint32_t>(d[0]) | static_cast<uint32_t>(d[1]) << 8 | static_cast<uint32_t>(d[2]) << 16 |
                 static_cast<uint32_t>(d[3]) << 24;
    k *= m;
    k ^= k >> r;
    k *= m;
    h *= m;
    h ^= k;
    d += 4;
    len -= 4;
  }
  switch (len) {
    case 3: h ^= static_cast<uint32_t>(d[2]) << 16; [[fallthrough]];
    case 2: h ^= static_cast<uint32_t>(d[1]) << 8; [[fallthrough]];
    case 1: h ^= d[0]; h *= m;
  }
  h ^= h >> 13;
  h *= m;
  h ^= h >> 15;
  return h;
}

// model_group=<name>: one of the Midas model's material groups (the readyup_midas addon: brushed,
// matte, painted, silver; empty = its default, polished gold). CSkeletonInstance::m_materialGroup
// (on 1.41.8.5) holds the group name's string token.
void UseMaterialGroup(void* w, const std::string& group) {
  const int body = g_api->schema_offset(g_api->self, "CBaseEntity", "m_CBodyComponent");
  const int skel = g_api->schema_offset(g_api->self, "CBodyComponentSkeletonInstance", "m_skeletonInstance");
  const int state = g_api->schema_offset(g_api->self, "CSkeletonInstance", "m_modelState");
  if (body < 0 || skel < 0 || state < 0) return;
  void* bc = Rd<void*>(w, body);
  if (!bc) return;
  if (group.empty()) return;  // the model's default group; a wrong token would drop every remap
  const uint32_t token = StringToken(group);
  // Where m_materialGroup lives differs between builds: look it up once and log where it was found.
  static int where = -2, off = -1;
  static const char* const kClasses[] = {"CSkeletonInstance", "CModelState", "CBodyComponentSkeletonInstance",
                                         "CBaseModelEntity", "CGameSceneNode", "CBodyComponent"};
  if (where == -2) {
    where = -1;
    for (int i = 0; i < 6 && where < 0; ++i) {
      off = g_api->schema_offset(g_api->self, kClasses[i], "m_materialGroup");
      if (off >= 0) where = i;
    }
    ru_logf(g_api, RU_LOG_INFO, "m_materialGroup: %s", where >= 0 ? kClasses[where] : "not found");
  }
  unsigned char* base = nullptr;
  switch (where) {
    case 0: base = static_cast<unsigned char*>(bc) + skel; break;
    case 1: base = static_cast<unsigned char*>(bc) + skel + state; break;
    case 2: case 5: base = static_cast<unsigned char*>(bc); break;
    case 3: base = static_cast<unsigned char*>(w); break;
    default: return;  // CGameSceneNode / not found: not handled
  }
  std::memcpy(base + off, &token, sizeof(token));
}

// A charm can't be placed on a Midas model: CS2 positions charms from the model's compiled
// KeychainMarkup, which the public Workshop Tools can't build, so it floated with the viewmodel.
// keep_charm=0 (default) clears the player's charm on Midas-model weapons: "keychain slot 0 id" = 0
// in both attribute lists of the weapon's CEconItemView.
void RemoveCharm(void* w) {
  if (!RU_API_HAS(g_api, econ_attr_set_by_name)) return;
  const int mgr = g_api->schema_offset(g_api->self, "CEconEntity", "m_AttributeManager");
  const int item = g_api->schema_offset(g_api->self, "CAttributeContainer", "m_Item");
  const int dyn = g_api->schema_offset(g_api->self, "CEconItemView", "m_NetworkedDynamicAttributes");
  const int lst = g_api->schema_offset(g_api->self, "CEconItemView", "m_AttributeList");
  if (mgr < 0 || item < 0) return;
  unsigned char* view = static_cast<unsigned char*>(w) + mgr + item;
  for (int off : {dyn, lst}) {
    if (off >= 0) g_api->econ_attr_set_by_name(g_api->self, view + off, "keychain slot 0 id", 0.0);
  }
}

void UseNormalMesh(void* w) {
  const int body = g_api->schema_offset(g_api->self, "CBaseEntity", "m_CBodyComponent");
  const int skel = g_api->schema_offset(g_api->self, "CBodyComponentSkeletonInstance", "m_skeletonInstance");
  const int state = g_api->schema_offset(g_api->self, "CSkeletonInstance", "m_modelState");
  const int mask = g_api->schema_offset(g_api->self, "CModelState", "m_MeshGroupMask");
  if (body >= 0 && skel >= 0 && state >= 0 && mask >= 0) {
    void* bc = Rd<void*>(w, body);
    if (bc) {
      const uint64_t one = 1;
      std::memcpy(static_cast<unsigned char*>(bc) + skel + state + mask, &one, sizeof(one));
    }
  }
  if (RU_API_HAS(g_api, entity_set_bodygroup_by_name)) g_api->entity_set_bodygroup_by_name(g_api->self, w, "body", 0);
  g_api->entity_mark_changed(g_api->self, w);
}
bool g_configRead = false;   // midas.cfg read once since load
int g_lastFileKit = 0;       // paint_kit as last read from midas.cfg
bool g_refreshHeld = false;  // the finish changed: refresh held weapons after the next sync

// Swaps every Midas player's held weapons for new ones (skins.so refresh_weapons), so a new finish
// shows at once: the weapons are painted when they are created.
void RefreshMidasWeapons() {
  const ru_skins_v1* s = Skins();
  if (!s || !RU_API_HAS(s, refresh_weapons) || !s->refresh_weapons) return;
  for (uint64_t sid : g_overrideSent) {
    const int slot = g_api->slot_for_steamid(g_api->self, sid);
    if (slot < 0 || slot >= 64) continue;
    s->refresh_weapons(slot);
    g_api->chat_to_slot(g_api->self, slot, ("Midas: midas.cfg applied: finish " + std::to_string(g_paintKit) + ", wear " +
                                            std::to_string(g_paintWear).substr(0, 4) + ", seed " + std::to_string(g_paintSeed)).c_str());
  }
}
bool g_tintPainted = false;  // also tint weapons that got the paint kit
void SyncPaintOverrides(bool clear) {
  const auto* s = static_cast<const ru_skins_v1*>(g_api->get_interface(g_api->self, RU_SKINS_IFACE_NAME, 1));
  if (!s || !RU_API_HAS(s, set_player_paint) || !s->set_player_paint) {
    g_overrideSent.clear();
    return;
  }
  const std::set<uint64_t> want = clear ? std::set<uint64_t>{} : PaintOverrideSet(g_active, g_finish, g_midas, g_best);
  const bool tags = RU_API_HAS(s, set_player_name_tag) && s->set_player_name_tag;
  // name_tag (default none): a name Midas weapons show instead of the skin's own name.
  const std::string tag = ConfigValue("name_tag");
  for (uint64_t sid : g_overrideSent) {
    if (want.count(sid)) continue;
    s->set_player_paint(sid, 0, 0.0f, 0);
    if (tags) s->set_player_name_tag(sid, "");
  }
  for (uint64_t sid : want) {
    s->set_player_paint(sid, g_paintKit, g_paintWear, g_paintSeed);
    if (tags) s->set_player_name_tag(sid, tag == "off" ? "" : tag.c_str());  // "" = no name tag
  }
  g_overrideSent = want;
}

// Calls fn(handle, weapon) for each weapon the player in `slot` holds; returns their SteamID64
// (0: no controller / pawn).
template <typename Fn>
uint64_t ForHeldWeapons(int slot, Fn&& fn) {
  void* ctrl = g_api->entity_by_index(g_api->self, slot + 1);
  if (!ctrl) return 0;
  const uint64_t sid = Rd<uint64_t>(ctrl, g_off.ctrl_steamId);
  void* pawn = g_api->entity_from_handle(g_api->self, Rd<uint32_t>(ctrl, g_off.ctrl_playerPawn));
  if (!pawn) return sid;
  void* ws = Rd<void*>(pawn, g_off.pawn_weaponServices);
  if (!ws) return sid;
  // CNetworkUtlVectorBase<CHandle<CBasePlayerWeapon>>: { int m_Size; <pad>; CHandle* m_pElements; }
  const int count = Rd<int32_t>(ws, g_off.ws_myWeapons);
  const uint32_t* handles = Rd<const uint32_t*>(ws, g_off.ws_myWeapons + 8);
  if (!handles || count <= 0 || count > 64) return sid;
  for (int i = 0; i < count; ++i) {
    if (void* w = g_api->entity_from_handle(g_api->self, handles[i])) fn(handles[i], w);
  }
  return sid;
}

// Weapons that lost their Midas StatTrak counter: the "kill eater" attribute can't be removed
// through ru_api, so every player holding one gets their weapons swapped for new ones (skins.so
// refresh_weapons: removed now, given back two ticks later with their ammo). Without skins.so the
// counter stays (frozen) until the weapon is replaced; logged.
void SwapOutStatTrak(const std::set<uint32_t>& handles, const char* why) {
  if (handles.empty() || !g_off.Ok() || g_api->entity_system_status(g_api->self) != RU_ENTSYS_OK) return;
  const ru_skins_v1* s = Skins();
  const bool canSwap = s && RU_API_HAS(s, refresh_weapons) && s->refresh_weapons;
  int swapped = 0, stuck = 0, held = 0;
  for (int slot = 0; slot < 64; ++slot) {
    int n = 0;
    ForHeldWeapons(slot, [&](uint32_t h, void*) { n += handles.count(h) ? 1 : 0; });
    if (n == 0) continue;
    held += n;
    if (canSwap && s->refresh_weapons(slot) == 1) ++swapped;
    else ++stuck;
  }
  if (swapped || stuck) {
    ru_logf(g_api, stuck ? RU_LOG_WARN : RU_LOG_INFO, "stattrak off %d held weapon(s) (%s): %d player(s) get new weapons%s",
            held, why, swapped, stuck ? ", the counter stays on the others' (no skins.so refresh_weapons)" : "");
  }
}

void SetBest(uint64_t sid, const std::string& why);
void CheckBestAllowed();

void RefreshConfig(double now) {
  if (now - g_lastConfig < 5.0) return;
  g_lastConfig = now;
  g_enabled = ParseBool(ConfigValue("enabled"), false);
  LoadModelList();
  int bad = 0;
  const std::string ids = ConfigValue("midas_steamids");
  auto list = ParseSteamIds(ids, &bad);
  if (list != g_midas) {
    ru_logf(g_api, RU_LOG_INFO, "midas_steamids: %zu player(s)%s", list.size(), bad ? " (some entries are not SteamID64s)" : "");
    g_midas = std::move(list);
    for (int& p : g_pending) p = 16;  // check everyone's weapons again
  }
  Rgba c = kGold;
  const std::string cs = ConfigValue("color");
  if (!cs.empty() && !ParseColor(cs, &c)) ru_logf(g_api, RU_LOG_WARN, "color \"%s\" is not r,g,b[,a]; using gold", cs.c_str());
  if (c != g_color) {
    g_color = c;
    for (int& p : g_pending) p = 16;
  }
  // finish / paint kit: a change puts everything back, then paints again.
  Finish finish = Finish::kAuto;
  const std::string fs = ConfigValue("finish");
  if (!fs.empty() && !ParseFinish(fs, &finish)) ru_logf(g_api, RU_LOG_WARN, "finish \"%s\" is not auto|tint; using auto", fs.c_str());
  // paint_kit=skin: the player's own skin stays under a Midas model (its mesh, legacy or new, follows).
  const std::string pk = ConfigValue("paint_kit");
  const int fileKit = pk == "skin" ? -1 : ParseInt(pk, kGoldPaintKit, 1, 100000);
  if (g_configRead && fileKit != g_lastFileKit) g_trialKit = 0;  // a saved paint_kit wins over .midas
  g_lastFileKit = fileKit;
  const int kit = g_trialKit > 0 ? g_trialKit : fileKit;
  const float wear = ParseFloat(ConfigValue("paint_wear"), 0.0f, 0.0f, 1.0f);
  const int seed = ParseInt(ConfigValue("paint_seed"), 0, 0, 1000);
  if (finish != g_finish || kit != g_paintKit || wear != g_paintWear || seed != g_paintSeed) {
    g_finish = finish;
    g_paintKit = kit;
    g_paintWear = wear;
    g_paintSeed = seed;
    RestoreAll("finish changed", false);  // no-op on the first read; Midas weapons are swapped below
    g_refreshHeld = g_configRead;   // midas.cfg saved: swap Midas players' held weapons for new ones
    ru_logf(g_api, RU_LOG_INFO, "finish %s, paint kit %d (wear %.2f, seed %d)", finish == Finish::kTint ? "tint" : "auto",
            kit, static_cast<double>(wear), seed);
    for (int& p : g_pending) p = 16;
  }
  // StatTrak counter on Midas weapons (default on). Switched off: the weapons that have one are swapped.
  const bool stattrak = ParseBool(ConfigValue("stattrak"), true);
  if (stattrak != g_stattrakOn) {
    g_stattrakOn = stattrak;
    ru_logf(g_api, RU_LOG_INFO, "stattrak %s", stattrak ? "on" : "off");
    if (!stattrak) {
      std::set<uint32_t> counted;
      for (const auto& kv : g_stattrak) counted.insert(kv.first);
      g_stattrak.clear();
      SwapOutStatTrak(counted, "stattrak=0");
    } else {
      for (int& p : g_pending) p = 16;
    }
  }
  const bool debugBots = ParseBool(ConfigValue("debug_midas_bots"), false) && g_api->debug_enabled(g_api->self);
  if (debugBots != g_debugBots) {
    g_debugBots = debugBots;
    ru_logf(g_api, RU_LOG_INFO, "debug_midas_bots: bots are %sMidas", debugBots ? "" : "no longer ");
    for (int& p : g_pending) p = 16;
  }
  // Best player.
  g_bestOn = ParseBool(ConfigValue("best_player"), false);
  g_bestInMatches = ParseBool(ConfigValue("best_player_in_matches"), false);
  BestStat stat = BestStat::kAdr;
  const std::string ss = ConfigValue("best_player_stat");
  if (!ss.empty() && !ParseBestStat(ss, &stat)) ru_logf(g_api, RU_LOG_WARN, "best_player_stat \"%s\" is not adr|kills; using adr", ss.c_str());
  BestWhen when = BestWhen::kRound;
  const std::string ws = ConfigValue("best_player_when");
  if (!ws.empty() && !ParseBestWhen(ws, &when)) ru_logf(g_api, RU_LOG_WARN, "best_player_when \"%s\" is not round|half; using round", ws.c_str());
  g_bestStat = stat;
  g_bestWhen = when;
  g_bestMinRounds = ParseInt(ConfigValue("best_player_min_rounds"), 3, 0, 30);
  std::string ruleset = "default";
  const auto* m = static_cast<const ru_match_v1*>(g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1));
  if (m && RU_API_HAS(m, ruleset) && m->ruleset && m->ruleset()) ruleset = m->ruleset();
  else if (!ConfigValue("ruleset").empty()) ruleset = ConfigValue("ruleset");
  g_ruleset = ruleset;
  const bool active = Active(g_enabled, g_ruleset);
  if (active != g_active) {
    g_active = active;
    if (active) {
      ru_logf(g_api, RU_LOG_INFO, "active: %zu Midas player(s), colour %d,%d,%d,%d", g_midas.size(), g_color.r,
              g_color.g, g_color.b, g_color.a);
      for (int& p : g_pending) p = 16;
    } else {
      g_best = 0;
      g_bestLine.clear();
      RestoreAll(g_enabled ? "valve ruleset" : "disabled");
      ru_logf(g_api, RU_LOG_INFO, "inert (%s)", g_enabled ? "valve ruleset" : "enabled=0");
    }
  }
  CheckBestAllowed();
  SyncPaintOverrides(false);
  g_configRead = true;
  if (g_refreshHeld) {
    g_refreshHeld = false;
    RefreshMidasWeapons();
  }
}

// ---- best player ------------------------------------------------------------------------------

const ru_match_v1* Match() {
  const auto* m = static_cast<const ru_match_v1*>(g_api->get_interface(g_api->self, RU_MATCH_IFACE_NAME, 1));
  return m && RU_API_HAS(m, map_stats) && m->map_stats ? m : nullptr;
}

bool MapInfo(const ru_match_v1* m, ru_match_map_info* info, std::vector<PlayerTotals>* players) {
  *info = ru_match_map_info{};
  info->struct_size = sizeof(*info);
  auto collect = [](void* user, const ru_match_player_stats* p) {
    if (!p || p->struct_size < sizeof(ru_match_player_stats)) return;
    PlayerTotals t;
    t.steamid64 = p->steamid64;
    t.kills = p->kills;
    t.deaths = p->deaths;
    t.damage = p->damage;
    t.rounds = p->rounds_played;
    static_cast<std::vector<PlayerTotals>*>(user)->push_back(t);
  };
  return m->map_stats(info, players ? +collect : nullptr, players) == 1;
}

bool AllowedNow(const ru_match_map_info& info) {
  return g_active && BestPlayerAllowed(g_bestOn, g_bestInMatches, info.live != 0, info.scrim != 0, g_ruleset);
}

// Every 5 s: the rule switched off, the map stopped recording (postgame, idle) or a real match
// without best_player_in_matches -> no best-player Midas.
void CheckBestAllowed() {
  if (g_best == 0) return;
  const ru_match_v1* m = Match();
  ru_match_map_info info{};
  if (!m || !MapInfo(m, &info, nullptr) || !AllowedNow(info)) SetBest(0, "");
}

std::string PlayerName(uint64_t sid) {
  ru_player p{};
  p.struct_size = sizeof(p);
  if (g_api->get_player_by_steamid(g_api->self, sid, &p) == 1 && p.name[0]) return p.name;
  return std::to_string(sid);
}

// A new best-player Midas (0 = none). The previous one's held weapons go back to normal (unless
// they are on midas_steamids); weapons they dropped stay gold.
void SetBest(uint64_t sid, const std::string& why) {
  if (sid == g_best) return;
  const uint64_t old = g_best;
  g_best = sid;
  g_bestLine = sid ? PlayerName(sid) + " (" + why + ")" : std::string();
  SyncPaintOverrides(false);
  if (old != 0 && !g_midas.count(old) && g_off.Ok()) {
    const int slot = g_api->slot_for_steamid(g_api->self, old);
    if (slot >= 0 && slot < 64) {
      const ru_skins_v1* skins = Skins();
      std::set<uint32_t> counted;
      ForHeldWeapons(slot, [&](uint32_t h, void*) {
        if (g_stattrak.count(h)) counted.insert(h);
        RestoreWeapon(h, skins);
      });
      SwapOutStatTrak(counted, "no longer Midas");
    }
  }
  if (sid == 0) {
    if (old != 0) ru_logf(g_api, RU_LOG_INFO, "best player: no Midas now");
    return;
  }
  const int slot = g_api->slot_for_steamid(g_api->self, sid);
  if (slot >= 0 && slot < 64) g_pending[slot] = 16;
  const std::string name = PlayerName(sid);
  ru_logf(g_api, RU_LOG_INFO, "best player: %s (%llu) is Midas: %s", name.c_str(), static_cast<unsigned long long>(sid),
          why.c_str());
  g_api->chat_all(g_api->self, ("Midas: " + name + " has the golden touch (" + why + ").").c_str(), 0);
}

// A few ticks after a round start (the match plugin handles round_start on its tick first).
void PickBestPlayer() {
  const ru_match_v1* m = Match();
  std::vector<PlayerTotals> all;
  ru_match_map_info info{};
  if (!m || !MapInfo(m, &info, &all) || !AllowedNow(info)) {
    SetBest(0, "");
    return;
  }
  if (!PickNow(g_bestWhen, info.rounds, g_bestMinRounds, info.half, g_lastPickHalf)) return;
  if (g_bestWhen == BestWhen::kHalf) g_lastPickHalf = info.half;
  std::vector<PlayerTotals> connected;
  for (const auto& p : all) {
    if (g_api->slot_for_steamid(g_api->self, p.steamid64) >= 0) connected.push_back(p);
  }
  const uint64_t best = midas::PickBest(connected, g_bestStat, g_best);
  std::string why;
  for (const auto& p : connected) {
    if (p.steamid64 != best) continue;
    char buf[96];
    if (g_bestStat == BestStat::kAdr) std::snprintf(buf, sizeof(buf), "best ADR: %.0f", Adr(p));
    else std::snprintf(buf, sizeof(buf), "most kills: %d", p.kills);
    why = buf;
  }
  SetBest(best, why);
}

// ---- StatTrak ---------------------------------------------------------------------------------

// g_ownKills key: the SteamID64; bots (0) by slot.
uint64_t KillKey(int slot, uint64_t sid) { return sid ? sid : (1ull << 63) | static_cast<uint64_t>(slot); }

// The player's kills on this map: the match plugin's stats while they record and list the player,
// else the player_death count since the map started.
int KillsOf(int slot, uint64_t sid) {
  const auto own = g_ownKills.find(KillKey(slot, sid));
  bool live = false, listed = false;
  int statsKills = 0;
  if (sid != 0) {
    if (const ru_match_v1* m = Match()) {
      ru_match_map_info info{};
      std::vector<PlayerTotals> all;
      if (MapInfo(m, &info, &all) && info.live) {
        live = true;
        for (const auto& p : all) {
          if (p.steamid64 != sid) continue;
          listed = true;
          statsKills = p.kills;
        }
      }
    }
  }
  return StatTrakKills(live, listed, statsKills, own == g_ownKills.end() ? 0 : own->second);
}

uint64_t g_nextItemId = 0x4D1DA5000000ull;  // made-up item IDs for weapons that have none

// Writes the counter on one weapon: "kill eater" (the count in the float's bits) and "kill eater
// score type" 0 (kills) in both attribute lists of its CEconItemView, as skins.so writes a
// loadout's StatTrak, plus CEconEntity::m_nFallbackStatTrak. An item without an ID (a stock weapon
// skins.so never painted) gets a made-up one, as skins.so gives every weapon it paints: clients
// read an item's networked attributes only when it has one. Then marks the entity changed so a
// weapon clients already have is sent again with the new count.
bool WriteStatTrak(void* w, uint64_t owner, int kills) {
  if (!RU_API_HAS(g_api, econ_attr_set_by_name)) return false;
  auto F = [](const char* c, const char* f) { return g_api->schema_offset(g_api->self, c, f); };
  const int mgr = F("CEconEntity", "m_AttributeManager");
  const int item = F("CAttributeContainer", "m_Item");
  const int dyn = F("CEconItemView", "m_NetworkedDynamicAttributes");
  const int lst = F("CEconItemView", "m_AttributeList");
  if (mgr < 0 || item < 0 || (dyn < 0 && lst < 0)) return false;
  unsigned char* view = static_cast<unsigned char*>(w) + mgr + item;
  const int idOff = F("CEconItemView", "m_iItemID");
  const int idLow = F("CEconItemView", "m_iItemIDLow");
  const int idHigh = F("CEconItemView", "m_iItemIDHigh");
  const int account = F("CEconItemView", "m_iAccountID");
  if (idOff >= 0 && idLow >= 0 && idHigh >= 0 && Rd<uint64_t>(view, idOff) == 0) {
    const uint64_t id = ++g_nextItemId;
    const uint32_t lo = static_cast<uint32_t>(id & 0xFFFFFFFFu), hi = static_cast<uint32_t>(id >> 32);
    std::memcpy(view + idOff, &id, sizeof(id));
    std::memcpy(view + idLow, &lo, sizeof(lo));
    std::memcpy(view + idHigh, &hi, sizeof(hi));
    if (account >= 0 && owner != 0) {
      const uint32_t acc = static_cast<uint32_t>(owner & 0xFFFFFFFFu);
      std::memcpy(view + account, &acc, sizeof(acc));
    }
  }
  bool ok = false;
  for (int off : {dyn, lst}) {
    if (off < 0) continue;
    ok = g_api->econ_attr_set_by_name(g_api->self, view + off, "kill eater", KillEaterBits(kills)) == 1 || ok;
    g_api->econ_attr_set_by_name(g_api->self, view + off, "kill eater score type", 0.0);
  }
  const int fb = F("CEconEntity", "m_nFallbackStatTrak");
  if (fb >= 0) {
    const int32_t v = kills < 0 ? 0 : kills;
    std::memcpy(static_cast<unsigned char*>(w) + fb, &v, sizeof(v));
  }
  g_api->entity_mark_changed(g_api->self, w);
  return ok;
}

// Puts the Midas player's map kills on the Midas weapons they hold (the ones Midas gilded; guns and
// knives). Only writes when the count changed.
void ApplyStatTrak(int slot) {
  if (!g_stattrakOn || !g_active || !g_off.Ok()) return;
  void* ctrl = g_api->entity_by_index(g_api->self, slot + 1);
  if (!ctrl) return;
  const uint64_t sid = Rd<uint64_t>(ctrl, g_off.ctrl_steamId);
  if (!IsMidasSlot(sid)) return;
  if (g_stattrak.size() > 256) {  // weapons removed since (dead players' drops, round ends)
    for (auto it = g_stattrak.begin(); it != g_stattrak.end();) {
      it = g_api->entity_from_handle(g_api->self, it->first) ? std::next(it) : g_stattrak.erase(it);
    }
  }
  int kills = -1;  // looked up once, when a weapon needs it
  ForHeldWeapons(slot, [&](uint32_t h, void* w) {
    if (!g_tinted.count(h) && !g_painted.count(h) && !g_modelled.count(h)) return;  // not gilded (yet)
    const char* cn = g_api->entity_classname(g_api->self, w);
    if (!cn || !StatTrakable(cn)) return;
    if (kills < 0) kills = KillsOf(slot, sid);
    const auto it = g_stattrak.find(h);
    if (it != g_stattrak.end() && it->second == kills) return;
    if (WriteStatTrak(w, sid, kills)) {
      g_stattrak[h] = kills;
      if (g_api->debug_enabled(g_api->self)) ru_logf(g_api, RU_LOG_DEBUG, "stattrak %d on %s of slot %d", kills, cn, slot);
    }
  });
}

// player_death: the attacker's own count; their counter is refreshed two ticks later (after the
// match plugin has counted the kill too).
void OnPlayerDeath(void*, const char*, const ru_game_event* ev) {
  if (!g_off.Ok()) return;
  const int attacker = g_api->ev_get_player_slot(g_api->self, ev, "attacker");
  const int victim = g_api->ev_get_player_slot(g_api->self, ev, "userid");
  if (attacker < 0 || attacker >= 64) return;
  void* actrl = g_api->entity_by_index(g_api->self, attacker + 1);
  void* vctrl = victim >= 0 && victim < 64 ? g_api->entity_by_index(g_api->self, victim + 1) : nullptr;
  if (!actrl) return;
  const int at = g_off.teamNum >= 0 ? Rd<uint8_t>(actrl, g_off.teamNum) : 0;
  const int vt = g_off.teamNum >= 0 && vctrl ? Rd<uint8_t>(vctrl, g_off.teamNum) : 0;
  if (!CountsAsKill(attacker, victim, at, vt)) return;
  ++g_ownKills[KillKey(attacker, Rd<uint64_t>(actrl, g_off.ctrl_steamId))];
  if (g_stattrakOn) g_stattrakDue[attacker] = 2;
}

// Gilds the weapons a Midas player holds now: the paint kit through skins.so where it can,
// else the tint.
void TintSlot(int slot) {
  void* ctrl = g_api->entity_by_index(g_api->self, slot + 1);
  if (!ctrl || !IsMidasSlot(Rd<uint64_t>(ctrl, g_off.ctrl_steamId))) return;
  const ru_skins_v1* skins = g_finish == Finish::kAuto ? Skins() : nullptr;
  // debug_midas_bots: bots (no SteamID64, no loadout) only get the tint / model.
  const bool paint = skins && skins->active() == 1 && Rd<uint64_t>(ctrl, g_off.ctrl_steamId) != 0;
  // Painted weapons get the tint on top only with `.midas tint`.
  auto paintTint = [&](uint32_t h, void* w) {
    if (g_tintPainted) {
      if (SetColor(w, g_color)) g_tinted.insert(h);
    } else if (g_tinted.erase(h)) {
      SetColor(w, kWhite);
    }
  };
  ForHeldWeapons(slot, [&](uint32_t h, void* w) {
    // model_<classname>=<vmdl>: a Midas model (readyup_midas addon: white metal, tinted by `color`).
    if (const char* mcn = g_api->entity_classname(g_api->self, w)) {
      const std::string model = ModelFor(w, mcn);
      if (model.empty() && !g_modelByDef.empty() && g_noModel.insert(h).second && g_api->debug_enabled(g_api->self)) {
        ru_logf(g_api, RU_LOG_DEBUG, "no model for %s (item %d) of slot %d", mcn, ItemDefIndex(w), slot);
      }
      if (!model.empty() && RU_API_HAS(g_api, entity_set_model)) {
        if (!g_modelled.count(h) && g_api->entity_set_model(g_api->self, w, model.c_str()) == 1) {
          g_modelled.insert(h);
          UseMaterialGroup(w, ConfigValue("model_group"));
          if (!ParseBool(ConfigValue("keep_charm"), false)) RemoveCharm(w);
          // The readyup_midas models ship only the new (hd) mesh: always show it, also under a legacy skin
          // (paint_kit=skin), which would otherwise pick the legacy mesh the model doesn't have.
          UseNormalMesh(w);  // also marks the entity changed
          ru_logf(g_api, RU_LOG_INFO, "model %s on %s of slot %d", model.c_str(), mcn, slot);
        } else if (!g_modelled.count(h) && g_modelFailed.insert(h).second) {
          ru_logf(g_api, RU_LOG_WARN, "model %s on %s (item %d) of slot %d: SetModel refused", model.c_str(), mcn,
                  ItemDefIndex(w), slot);
        }
        if (g_modelled.count(h)) {
          if (SetColor(w, g_color)) g_tinted.insert(h);
          return;
        }
      }
    }
    if (g_painted.count(h)) {
      paintTint(h, w);
      return;
    }
    const char* cn = g_api->entity_classname(g_api->self, w);
    const uint64_t sid = Rd<uint64_t>(ctrl, g_off.ctrl_steamId);
    if (paint && cn && Paintable(cn) && g_overrideSent.count(sid)) {
      // skins.so already painted it gold when it was created (set_player_paint). Painting it again
      // here would claim it before skins.so sees it (no knife model, no gold on clients); only
      // remember it so it is handed back if this player stops being Midas.
      g_painted.insert(h);
      paintTint(h, w);
      return;
    }
    if (paint && cn && Paintable(cn) && skins->paint_weapon(h, sid, g_paintKit, g_paintWear, g_paintSeed) == 1) {
      g_painted.insert(h);
      paintTint(h, w);
      if (g_api->debug_enabled(g_api->self)) ru_logf(g_api, RU_LOG_DEBUG, "painted %s of slot %d", cn, slot);
      return;
    }
    if (SetColor(w, g_color)) {
      g_tinted.insert(h);
      if (g_api->debug_enabled(g_api->self)) ru_logf(g_api, RU_LOG_DEBUG, "tinted %s of slot %d", cn ? cn : "?", slot);
    }
  });
  ApplyStatTrak(slot);
}

// `.midas`: try paint kits live (admins, or a Midas player). A finish only shows on a weapon
// created with it, so the reply says to buy / pick up a new one (the knife: respawn).
struct TrialKit {
  int kit;
  const char* name;
};
constexpr TrialKit kTrialKits[] = {
    {159, "Brass"},           {409, "Tiger Tooth"},     {32, "Silver"},          {1025, "Gold Brick"},
    {413, "Marble Fade"},     {38, "Fade"},             {44, "Case Hardened"},   {98, "Ultraviolet"},
    {42, "Blue Steel"},       {410, "Damascus Steel"},  {578, "Bright Water"},   {252, "Silver Quartz"},
    {407, "Quicksilver"},     {210, "Anodized Gunmetal"}, {921, "Gold Arabesque"}, {185, "Golden Koi"},
    {497, "Golden Coil"},     {990, "Gold Bismuth"},    {1294, "Gold Leaf"},     {129, "Gold Toof"},
    {1170, "Chrome Cannon"},
};
constexpr int kTrialCount = static_cast<int>(sizeof(kTrialKits) / sizeof(kTrialKits[0]));

std::string KitLabel(int kit) {
  for (int i = 0; i < kTrialCount; ++i) {
    if (kTrialKits[i].kit == kit) {
      return std::to_string(kit) + " " + kTrialKits[i].name + " (" + std::to_string(i + 1) + "/" +
             std::to_string(kTrialCount) + ")";
    }
  }
  return std::to_string(kit);
}

void OnMidasChat(void*, const ru_command_ctx* ctx) {
  auto reply = [&](const std::string& msg) {
    if (ctx->slot >= 0) g_api->chat_to_slot(g_api->self, ctx->slot, ("Midas: " + msg).c_str());
    else ru_logf(g_api, RU_LOG_INFO, "%s", msg.c_str());
  };
  const bool admin = ctx->is_console || (RU_API_HAS(g_api, is_admin) && g_api->is_admin(g_api->self, ctx->steamid64) == 1);
  if (!admin && !IsMidas(ctx->steamid64)) {
    reply("only admins and Midas players can use .midas");
    return;
  }
  std::string sub = ctx->argc >= 2 && ctx->argv[1] ? ctx->argv[1] : "";
  for (char& c : sub) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  int cur = 0;
  for (int i = 0; i < kTrialCount; ++i) {
    if (kTrialKits[i].kit == g_paintKit) cur = i;
  }
  int kit = 0;
  if (sub == "next") {
    kit = kTrialKits[(cur + 1) % kTrialCount].kit;
  } else if (sub == "prev") {
    kit = kTrialKits[(cur + kTrialCount - 1) % kTrialCount].kit;
  } else if (sub == "kit" && ctx->argc >= 3) {
    kit = ParseInt(ctx->argv[2], 0, 1, 100000);
    if (kit <= 0) {
      reply("kit needs a paint kit id, e.g. .midas kit 409");
      return;
    }
  } else if (sub == "tint") {
    g_tintPainted = !g_tintPainted;
    for (int& p : g_pending) p = 16;
    reply(std::string("gold tint on painted weapons ") + (g_tintPainted ? "ON" : "OFF") + " (updates live)");
    return;
  } else if (sub == "reset") {
    g_trialKit = 0;
    g_tintPainted = false;
    g_lastConfig = -1e9;  // re-read midas.cfg on the next tick
    for (int& p : g_pending) p = 16;
    reply("back to midas.cfg (new weapons)");
    return;
  } else {
    reply("finish " + KitLabel(g_paintKit) + (g_tintPainted ? " + tint" : "") +
          ". .midas next | prev | kit <id> | tint | reset");
    return;
  }
  g_trialKit = kit;
  g_paintKit = kit;
  SyncPaintOverrides(false);
  ru_logf(g_api, RU_LOG_INFO, ".midas: finish %s by %s", KitLabel(kit).c_str(), ctx->name ? ctx->name : "?");
  const ru_skins_v1* s = Skins();
  const bool swapped = s && RU_API_HAS(s, refresh_weapons) && s->refresh_weapons && ctx->slot >= 0 &&
                       s->refresh_weapons(ctx->slot) == 1;
  reply("finish " + KitLabel(kit) + (swapped ? ": your weapons are being swapped for new ones."
                                             : ". Buy or pick up a new weapon to see it (knife: respawn)."));
}

void OnTick(void*, const ru_tick_info* t) {
  RefreshConfig(t->now);
  ResolveOffsets();
  if (g_pickIn > 0 && --g_pickIn == 0 && g_active && g_bestOn) PickBestPlayer();
  if (!g_active || !g_off.Ok() || (g_midas.empty() && g_best == 0 && !g_debugBots)) return;
  for (int s = 0; s < 64; ++s) {
    if (g_stattrakDue[s] > 0 && --g_stattrakDue[s] == 0) ApplyStatTrak(s);
    if (g_pending[s] <= 0) continue;
    // Right after the event and a few ticks later (the weapon entity can show up a tick late).
    if (g_pending[s] == 16 || g_pending[s] == 12 || g_pending[s] == 4 || g_pending[s] == 1) TintSlot(s);
    --g_pending[s];
  }
}

// item_pickup / item_equip / player_spawn: check that player's weapons over the next ticks.
void OnItemEvent(void*, const char*, const ru_game_event* ev) {
  const int slot = g_api->ev_get_player_slot(g_api->self, ev, "userid");
  if (slot >= 0 && slot < 64) g_pending[slot] = 16;
}

// round_start: pick the best player a few ticks from now.
void OnRoundStart(void*, const char*, const ru_game_event*) { g_pickIn = 8; }

void OnMap(void*, const ru_event*) {
  // New map: new entities; old handles mean nothing. New stats: no best player yet.
  g_tinted.clear();
  g_painted.clear();
  g_stattrak.clear();
  g_ownKills.clear();  // StatTrak counts this map's kills
  for (int& d : g_stattrakDue) d = 0;
  g_best = 0;
  g_bestLine.clear();
  g_lastPickHalf = 0;
  g_pickIn = 0;
  g_off.resolved = false;
}

void RunSelftest(ru_selftest_add_fn add, void* ctx) {
  char line[160];
  if (!g_enabled) {
    add(ctx, "INFO", "midas", "off (enabled=0)");
  } else if (!g_active) {
    add(ctx, "INFO", "midas", "inert (valve ruleset)");
  } else {
    std::snprintf(line, sizeof(line), "active: %zu player(s), %zu weapon(s) tinted, %zu painted (%s)", g_midas.size(),
                  g_tinted.size(), g_painted.size(),
                  g_finish == Finish::kTint ? "finish tint" : Skins() ? "paint kit via skins.so" : "no skins.so: tint");
    add(ctx, "INFO", "midas", line);
    if (g_bestOn) {
      std::snprintf(line, sizeof(line), "best player (%s, per %s): %s", g_bestStat == BestStat::kAdr ? "ADR" : "kills",
                    g_bestWhen == BestWhen::kHalf ? "half" : "round", g_best ? g_bestLine.c_str() : "nobody yet");
      add(ctx, "INFO", "midas best", line);
    }
    std::snprintf(line, sizeof(line), "stattrak %s: %zu weapon(s) counting%s", g_stattrakOn ? "on" : "off",
                  g_stattrak.size(), g_debugBots ? " (debug_midas_bots)" : "");
    add(ctx, "INFO", "midas stattrak", line);
  }
  if (g_off.resolved) {
    std::snprintf(line, sizeof(line), "CBaseModelEntity::m_clrRender offset %d", g_off.clrRender);
    add(ctx, g_off.clrRender >= 0 ? "OK" : "WARN", "midas schema", line);
  }
}
const ru_selftest_iface_v1 g_selftestIface = {sizeof(ru_selftest_iface_v1), &RunSelftest};

}  // namespace
}  // namespace midas

using namespace midas;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 1u,  // needs API 1.1 (entities, schema, raw events, config, interfaces)
      "midas",
      MIDAS_VERSION,
      "Ready Up",
      "fun: Midas players' weapons turn gold (midas_steamids, or the best player); off by default",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, get_interface)) return 1;
  g_api = api;
  g_off = Offsets{};
  g_enabled = g_active = false;
  g_midas.clear();
  g_tinted.clear();
  g_painted.clear();
  g_overrideSent.clear();
  g_stattrakOn = true;
  g_debugBots = false;
  g_stattrak.clear();
  g_ownKills.clear();
  for (int& d : g_stattrakDue) d = 0;
  g_modelByDef.clear();
  g_modelsPath = api->data_dir(api->self) ? std::string(api->data_dir(api->self)) + "/models.txt" : std::string();
  g_trialKit = 0;
  g_modelled.clear();
  g_tintPainted = false;
  g_configRead = g_refreshHeld = false;
  g_color = kGold;
  g_finish = Finish::kAuto;
  g_paintKit = kGoldPaintKit;
  g_paintWear = 0.0f;
  g_paintSeed = 0;
  g_bestOn = false;
  g_best = 0;
  g_bestLine.clear();
  g_lastPickHalf = 0;
  g_pickIn = 0;
  g_lastConfig = -1e9;
  for (int& p : g_pending) p = 0;
  api->on_tick(api->self, OnTick, nullptr);
  api->subscribe(api->self, RU_EVENT_MAP_START, OnMap, nullptr);
  for (const char* e : {"item_pickup", "item_equip", "player_spawn"}) {
    if (!api->subscribe_game_event(api->self, e, OnItemEvent, nullptr)) ru_logf(api, RU_LOG_WARN, "could not subscribe to %s", e);
  }
  if (!api->subscribe_game_event(api->self, "player_death", OnPlayerDeath, nullptr)) {
    ru_logf(api, RU_LOG_WARN, "could not subscribe to player_death (StatTrak counts only the match plugin's stats)");
  }
  if (RU_API_HAS(api, register_chat_command)) api->register_chat_command(api->self, ".midas", OnMidasChat, nullptr);
  if (!api->subscribe_game_event(api->self, "round_start", OnRoundStart, nullptr)) {
    ru_logf(api, RU_LOG_WARN, "could not subscribe to round_start (no best player)");
  }
  if (RU_API_HAS(api, provide_interface)) {
    api->provide_interface(api->self, RU_SELFTEST_IFACE_PREFIX "midas", RU_SELFTEST_IFACE_VERSION,
                           const_cast<ru_selftest_iface_v1*>(&g_selftestIface));
  }
  ru_logf(api, RU_LOG_INFO, "loaded " MIDAS_VERSION " (enable with enabled=1 and midas_steamids in cfg/ReadyUp/midas.cfg)");
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  // Hot reload / unload: weapons go back to their normal colour; the next image tints again.
  SyncPaintOverrides(true);
  RestoreAll("unload");
  ru_logf(g_api, RU_LOG_INFO, "unloaded");
  g_api = nullptr;
}

}  // extern "C"
