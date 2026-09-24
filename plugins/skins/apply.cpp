#include "skins.h"

#include "apply_internal.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

// Weapon skins + knives, driven from the plugin's on_tick (the core's GameFrame hook).
//
// Why polling instead of events/detours: engine events are currently disabled on the live build
// (CGameEventManager_Init capture crashed) and there is no detour on GiveNamedItem yet. Walking the
// 64 player controllers each tick is cheap, runs on the game thread, and — because our hook runs
// after the original GameFrame but before the snapshot for that frame is sent — a weapon created
// this frame (buy, round-start give, pickup) gets its skin before any client ever sees it.
// Weapons we can only decorate later (loadout still loading) are re-networked with
// MarkEntityFullyChanged.

namespace skins {
namespace {

struct Offsets {
  bool ok = false;
  // Controller / pawn.
  int ctrl_steamId = -1;     // CBasePlayerController::m_steamID (uint64)
  int ctrl_playerPawn = -1;  // CCSPlayerController::m_hPlayerPawn (CHandle)
  int ent_teamNum = -1;      // CBaseEntity::m_iTeamNum (uint8)
  int ent_lifeState = -1;    // CBaseEntity::m_lifeState (uint8, 0 = alive)
  int pawn_weaponServices = -1;  // CBasePlayerPawn::m_pWeaponServices (ptr)
  int ws_myWeapons = -1;         // CPlayer_WeaponServices::m_hMyWeapons (CNetworkUtlVectorBase<CHandle>)
  // Weapon (CEconEntity).
  int econ_attrMgr = -1;     // m_AttributeManager (CAttributeContainer, embedded)
  int econ_fbPaint = -1;     // m_nFallbackPaintKit
  int econ_fbSeed = -1;      // m_nFallbackSeed
  int econ_fbWear = -1;      // m_flFallbackWear
  int econ_fbStatTrak = -1;  // m_nFallbackStatTrak
  int econ_xuidLow = -1;     // m_OriginalOwnerXuidLow
  int econ_xuidHigh = -1;    // m_OriginalOwnerXuidHigh
  int cont_item = -1;        // CAttributeContainer::m_Item (CEconItemView, embedded)
  // CEconItemView.
  int item_defIndex = -1;    // uint16
  int item_quality = -1;     // int32
  int item_id = -1;          // uint64
  int item_idHigh = -1;      // uint32
  int item_idLow = -1;       // uint32
  int item_accountId = -1;   // uint32
  int item_initialized = -1; // bool
  int item_attrList = -1;    // CAttributeList (embedded)
  int item_netAttrs = -1;    // CAttributeList (embedded)
  int item_customName = -1;  // char[161]
};

Offsets& Off() {
  static Offsets o;
  return o;
}

int F(const char* cls, const char* field) { return SchemaOffset(cls, field); }

bool ResolveOffsets() {
  auto& o = Off();
  if (o.ok) return true;
  o.ctrl_steamId = F("CBasePlayerController", "m_steamID");
  o.ctrl_playerPawn = F("CCSPlayerController", "m_hPlayerPawn");
  o.ent_teamNum = F("CBaseEntity", "m_iTeamNum");
  o.ent_lifeState = F("CBaseEntity", "m_lifeState");
  o.pawn_weaponServices = F("CBasePlayerPawn", "m_pWeaponServices");
  o.ws_myWeapons = F("CPlayer_WeaponServices", "m_hMyWeapons");
  o.econ_attrMgr = F("CEconEntity", "m_AttributeManager");
  o.econ_fbPaint = F("CEconEntity", "m_nFallbackPaintKit");
  o.econ_fbSeed = F("CEconEntity", "m_nFallbackSeed");
  o.econ_fbWear = F("CEconEntity", "m_flFallbackWear");
  o.econ_fbStatTrak = F("CEconEntity", "m_nFallbackStatTrak");
  o.econ_xuidLow = F("CEconEntity", "m_OriginalOwnerXuidLow");
  o.econ_xuidHigh = F("CEconEntity", "m_OriginalOwnerXuidHigh");
  o.cont_item = F("CAttributeContainer", "m_Item");
  o.item_defIndex = F("CEconItemView", "m_iItemDefinitionIndex");
  o.item_quality = F("CEconItemView", "m_iEntityQuality");
  o.item_id = F("CEconItemView", "m_iItemID");
  o.item_idHigh = F("CEconItemView", "m_iItemIDHigh");
  o.item_idLow = F("CEconItemView", "m_iItemIDLow");
  o.item_accountId = F("CEconItemView", "m_iAccountID");
  o.item_initialized = F("CEconItemView", "m_bInitialized");
  o.item_attrList = F("CEconItemView", "m_AttributeList");
  o.item_netAttrs = F("CEconItemView", "m_NetworkedDynamicAttributes");
  o.item_customName = F("CEconItemView", "m_szCustomName");

  const int required[] = {o.ctrl_steamId,  o.ctrl_playerPawn, o.ent_teamNum,  o.ent_lifeState, o.pawn_weaponServices,
                          o.ws_myWeapons,  o.econ_attrMgr,    o.econ_fbPaint, o.econ_fbSeed,   o.econ_fbWear,
                          o.cont_item,     o.item_defIndex,   o.item_idHigh,  o.item_idLow,    o.item_netAttrs,
                          o.item_attrList};
  o.ok = std::all_of(std::begin(required), std::end(required), [](int v) { return v >= 0; });
  return o.ok;
}

template <typename T>
T Rd(const void* base, int off) {
  T v{};
  std::memcpy(&v, static_cast<const unsigned char*>(base) + off, sizeof(T));
  return v;
}

template <typename T>
void Wr(void* base, int off, T v) {
  if (off < 0) return;
  std::memcpy(static_cast<unsigned char*>(base) + off, &v, sizeof(T));
}

float BitsAsFloat(uint32_t u) {
  float f = 0.0f;
  std::memcpy(&f, &u, sizeof(f));
  return f;
}

// Fake, per-server-unique item IDs. A non-zero item ID makes the client treat the item as a
// "real" econ item and read the paint attributes instead of rendering the stock weapon.
uint64_t NextItemId() {
  static uint64_t next = 0x10000;
  return ++next;
}

bool IsKnifeDesigner(const char* dn) {
  if (!dn) return false;
  return std::strncmp(dn, "weapon_knife", 12) == 0 || std::strcmp(dn, "weapon_bayonet") == 0;
}

}  // namespace

namespace detail {

bool EconOffsetsReady() { return ResolveOffsets(); }
void ResetOffsets() { Off() = Offsets{}; }
int ItemDefIndexOffset() { return Off().item_defIndex; }
int ItemInitializedOffset() { return Off().item_initialized; }

// Writes paint/seed/wear/StatTrak/nametag onto an econ item view + the owning entity's fallback
// fields. Used for weapons (owner = the weapon entity) and gloves (owner = nullptr).
void WritePaint(void* ownerEconEntity, void* itemView, uint64_t steamid64, const WeaponSkinEntry& skin) {
  auto& o = Off();
  const uint64_t itemId = NextItemId();
  Wr<uint64_t>(itemView, o.item_id, itemId);
  Wr<uint32_t>(itemView, o.item_idLow, static_cast<uint32_t>(itemId & 0xFFFFFFFFu));
  Wr<uint32_t>(itemView, o.item_idHigh, static_cast<uint32_t>(itemId >> 32));
  Wr<uint32_t>(itemView, o.item_accountId, static_cast<uint32_t>(steamid64 & 0xFFFFFFFFu));

  if (ownerEconEntity) {
    Wr<int32_t>(ownerEconEntity, o.econ_fbPaint, skin.paint_id);
    Wr<int32_t>(ownerEconEntity, o.econ_fbSeed, skin.seed);
    Wr<float>(ownerEconEntity, o.econ_fbWear, skin.wear);
    if (skin.stattrak_enabled) Wr<int32_t>(ownerEconEntity, o.econ_fbStatTrak, std::max(0, skin.stattrak_count));
  }

  void* lists[2] = {static_cast<unsigned char*>(itemView) + o.item_netAttrs,
                    static_cast<unsigned char*>(itemView) + o.item_attrList};
  for (void* list : lists) {
    AttrSetOrAddByName(list, "set item texture prefab", static_cast<float>(skin.paint_id));
    AttrSetOrAddByName(list, "set item texture seed", static_cast<float>(skin.seed));
    AttrSetOrAddByName(list, "set item texture wear", skin.wear);
    if (skin.stattrak_enabled) {
      AttrSetOrAddByName(list, "kill eater", BitsAsFloat(static_cast<uint32_t>(std::max(0, skin.stattrak_count))));
      AttrSetOrAddByName(list, "kill eater score type", 0.0f);
    }
  }

  if (o.item_customName >= 0 && !skin.nametag.empty()) {
    char* dst = reinterpret_cast<char*>(itemView) + o.item_customName;
    const size_t n = std::min<size_t>(skin.nametag.size(), 160);
    std::memcpy(dst, skin.nametag.data(), n);
    dst[n] = '\0';
  }
}

void* EconItemViewOfWeapon(void* weapon) {
  auto& o = Off();
  return static_cast<unsigned char*>(weapon) + o.econ_attrMgr + o.cont_item;
}

}  // namespace detail

using detail::EconItemViewOfWeapon;
using detail::WritePaint;

namespace {

enum class WeaponResult { kDone, kRetry };

// Paint kits with items_game `use_legacy_model 1` (CS:GO-era UVs). In CS2 they only render
// correctly on the weapon's legacy model: bodygroup "body" = 1 (what WeaponPaints does with
// AcceptInput("SetBodygroup", "body,1")). Regenerate with scripts/gen_legacy_paint_kits.py.
constexpr int kLegacyPaintKits[] = {
#include "legacy_paint_kits.inc"
};

bool IsLegacyPaintKit(int paint) {
  return std::binary_search(std::begin(kLegacyPaintKits), std::end(kLegacyPaintKits), paint);
}

// Returns true when done (applied or definitively impossible), false to retry on a later tick.
bool ApplyLegacyBody(void* weapon, uint64_t steamid64) {
  const auto r = SetBodygroupByName(weapon, "body", 1);
  if (r == RU_BODYGROUP_NO_MODEL) return false;
  if (DebugOn()) {
    Log(RU_LOG_DEBUG, "legacy model (body=1) for %s owner=%llu: %s", EntityDesignerName(weapon),
          static_cast<unsigned long long>(steamid64), BodygroupResultName(r));
  }
  return true;
}

long long g_applied = 0;  // weapons painted / knives swapped this session (for skins_status)

WeaponResult ProcessWeapon(void* weapon, uint64_t steamid64, int team, bool late, bool* legacyPending) {
  auto& o = Off();
  if (!IsLoaded(steamid64)) return WeaponResult::kRetry;

  // Weapons picked up from someone else keep their original owner's look.
  const uint64_t xuid = (static_cast<uint64_t>(Rd<uint32_t>(weapon, o.econ_xuidHigh)) << 32) |
                        Rd<uint32_t>(weapon, o.econ_xuidLow);
  if (xuid != 0 && xuid != steamid64) return WeaponResult::kDone;
  if (xuid == 0) {
    Wr<uint32_t>(weapon, o.econ_xuidLow, static_cast<uint32_t>(steamid64 & 0xFFFFFFFFu));
    Wr<uint32_t>(weapon, o.econ_xuidHigh, static_cast<uint32_t>(steamid64 >> 32));
  }

  void* item = EconItemViewOfWeapon(weapon);
  int defindex = Rd<uint16_t>(item, o.item_defIndex);
  if (defindex <= 0) return WeaponResult::kDone;

  bool changed = false;
  const bool isKnife = IsKnifeDesigner(EntityDesignerName(weapon));
  if (isKnife) {
    if (const auto want = FindKnifeClassname(steamid64, team)) {
      if (const auto wantDef = KnifeClassnameToDefindex(*want)) {
        if (*wantDef != defindex) {
          // Same as the "ChangeSubclass" input / `subclass_change <defindex>`: swaps weapon VData
          // (model, animations) to the chosen knife.
          if (ChangeSubclass(weapon, std::to_string(*wantDef).c_str())) {
            Wr<uint16_t>(item, o.item_defIndex, static_cast<uint16_t>(*wantDef));
            defindex = *wantDef;
            changed = true;
          } else if (DebugOn()) {
            Log(RU_LOG_DEBUG, "ChangeSubclass unavailable; keeping default knife for %llu",
                  static_cast<unsigned long long>(steamid64));
          }
        }
        // Quality 3 = "unusual" (the ★ prefix); non-default knives always carry it.
        Wr<int32_t>(item, o.item_quality, 3);
      }
    }
  }

  if (const auto skin = FindWeaponSkin(steamid64, team, defindex)) {
    if (skin->paint_id > 0) {
      WritePaint(weapon, item, steamid64, *skin);
      changed = true;
      if (IsLegacyPaintKit(skin->paint_id) && !ApplyLegacyBody(weapon, steamid64)) *legacyPending = true;
    }
  }

  if (changed) ++g_applied;
  if (changed && late) MarkEntityFullyChanged(weapon);
  if (changed && DebugOn()) {
    Log(RU_LOG_DEBUG, "applied to %s def=%d owner=%llu%s", EntityDesignerName(weapon), defindex,
          static_cast<unsigned long long>(steamid64), late ? " (late)" : "");
  }
  return WeaponResult::kDone;
}

struct PlayerState {
  uint64_t steamid64 = 0;
  uint32_t pawnHandle = 0xFFFFFFFFu;
  bool wasAlive = false;
  int cosmeticsPass = -1;      // next index into kCosmeticPassTicks, -1 = none pending
  long long spawnTick = 0;
};

struct WeaponState {
  long long firstSeen = 0;
  long long lastSeen = 0;
  bool done = false;
  bool legacyPending = false;  // legacy "body" bodygroup still to set (model not loaded yet)
};

// Cosmetics (agent model, gloves, default_gloves bodygroup) are applied on these ticks after the
// spawn frame. The game can re-apply its own model/gloves shortly after spawning, and a freshly
// SetModel'd agent model may not be loaded (no bodygroups) on the first frames.
constexpr long long kCosmeticPassTicks[] = {0, 1, 8, 32};
constexpr int kCosmeticPassCount = static_cast<int>(sizeof(kCosmeticPassTicks) / sizeof(kCosmeticPassTicks[0]));

PlayerState g_players[65];
// Dev only (skins_debug_as, debug=1): a bot slot decorated with a real player's loadout, so the
// whole apply path can be exercised without a human client.
uint64_t g_debugAs[65] = {};
std::unordered_map<uint32_t, WeaponState> g_weapons;  // by entity handle (index + serial)
long long g_tick = 0;
long long g_activeSince = 0;  // first tick of the current map/session we saw entities
void* g_lastEntitySystemWorld = nullptr;

// Loadout loads are async; give up decorating a weapon/spawn after this many ticks (~10s @64).
constexpr long long kRetryTicks = 640;

bool DisabledByEnv() {  // READYUP_DISABLE_SKINS=1: keep the plugin loaded but idle
  static const bool disabled = [] {
    const char* v = std::getenv("READYUP_DISABLE_SKINS");
    return v && *v && std::strcmp(v, "0") != 0;
  }();
  return disabled;
}

void ProcessPlayer(int slot, void* controller) {
  auto& o = Off();
  PlayerState& ps = g_players[slot];

  uint64_t steamid64 = Rd<uint64_t>(controller, o.ctrl_steamId);
  if (steamid64 == 0 && g_debugAs[slot] != 0 && DebugOn()) steamid64 = g_debugAs[slot];
  if (steamid64 == 0) {  // bots / not yet authenticated
    ps = PlayerState{};
    return;
  }
  if (ps.steamid64 != steamid64) ps = PlayerState{steamid64};

  // Prefetch roughly once a second; MaybeRefreshAsync is TTL-gated anyway.
  if (((g_tick + slot) & 63) == 0) MaybeRefreshAsync(steamid64);

  const uint32_t pawnHandle = Rd<uint32_t>(controller, o.ctrl_playerPawn);
  void* pawn = EntityFromHandle(pawnHandle);
  if (!pawn) {
    ps.wasAlive = false;
    return;
  }
  const int team = Rd<uint8_t>(pawn, o.ent_teamNum);
  const bool alive = Rd<uint8_t>(pawn, o.ent_lifeState) == 0;
  if (team != 2 && team != 3) {
    ps.wasAlive = false;
    return;
  }

  if (alive && (!ps.wasAlive || ps.pawnHandle != pawnHandle)) {
    // Fresh spawn: apply now (same frame as the spawn) and on the follow-up passes.
    ps.cosmeticsPass = 0;
    ps.spawnTick = g_tick;
  }
  ps.wasAlive = alive;
  ps.pawnHandle = pawnHandle;
  if (!alive) return;

  if (ps.cosmeticsPass >= 0 && ps.cosmeticsPass < kCosmeticPassCount &&
      g_tick - ps.spawnTick >= kCosmeticPassTicks[ps.cosmeticsPass]) {
    // Anything first seen on our first active tick may already have been networked.
    const bool late = g_tick > ps.spawnTick + 1 || ps.spawnTick == g_activeSince;
    if (ApplySpawnCosmetics(pawn, steamid64, team, late)) {
      if (++ps.cosmeticsPass >= kCosmeticPassCount) ps.cosmeticsPass = -1;
    } else if (g_tick - ps.spawnTick > kRetryTicks) {
      ps.cosmeticsPass = -1;  // loadout never loaded (DB down?) — give up for this spawn
    }
  }

  void* ws = Rd<void*>(pawn, o.pawn_weaponServices);
  if (!ws) return;
  // CNetworkUtlVectorBase<CHandle<CBasePlayerWeapon>>: { int m_Size; <pad>; CHandle* m_pElements; }
  const int count = Rd<int32_t>(ws, o.ws_myWeapons);
  const uint32_t* handles = Rd<const uint32_t*>(ws, o.ws_myWeapons + 8);
  if (!handles || count <= 0 || count > 64) return;

  for (int i = 0; i < count; ++i) {
    const uint32_t h = handles[i];
    WeaponState& st = g_weapons[h];
    if (st.firstSeen == 0) st.firstSeen = g_tick;
    st.lastSeen = g_tick;
    if (st.done && !st.legacyPending) continue;
    void* weapon = EntityFromHandle(h);
    if (!weapon) continue;
    if (st.done) {
      // Legacy bodygroup retry (the weapon model wasn't loaded when we painted it).
      if (ApplyLegacyBody(weapon, steamid64)) {
        st.legacyPending = false;
        MarkEntityFullyChanged(weapon);
      } else if (g_tick - st.firstSeen > kRetryTicks) {
        st.legacyPending = false;
      }
      continue;
    }
    const bool late = g_tick > st.firstSeen || st.firstSeen == g_activeSince;
    if (ProcessWeapon(weapon, steamid64, team, late, &st.legacyPending) == WeaponResult::kDone ||
        g_tick - st.firstSeen > kRetryTicks) {
      st.done = true;
    }
  }
}

void PruneWeapons() {
  for (auto it = g_weapons.begin(); it != g_weapons.end();) {
    if (g_tick - it->second.lastSeen > 64 * 60) it = g_weapons.erase(it);
    else ++it;
  }
}

}  // namespace

void GameFrameTick() {
  if (DisabledByEnv()) return;
  ++g_tick;

  if (g_api->entity_system_status(g_api->self) != RU_ENTSYS_OK) return;
  if (!ResolveOffsets()) {
    static bool s_warned = false;
    if (!s_warned) {
      s_warned = true;
      Log(RU_LOG_WARN, "required schema fields missing; weapon skins disabled for this session");
    }
    return;
  }

  // New map => entity handles are reused from scratch; forget per-entity state.
  void* world = EntityByIndex(0);
  if (world != g_lastEntitySystemWorld) {
    g_lastEntitySystemWorld = world;
    g_activeSince = g_tick;
    g_weapons.clear();
    for (auto& p : g_players) p = PlayerState{};
  }

  for (int slot = 0; slot < 64; ++slot) {
    void* controller = EntityByIndex(slot + 1);
    if (!controller) {
      g_players[slot] = PlayerState{};
      continue;
    }
    const char* dn = EntityDesignerName(controller);
    if (!dn || std::strcmp(dn, "cs_player_controller") != 0) continue;
    ProcessPlayer(slot, controller);
  }

  if ((g_tick & 1023) == 0) PruneWeapons();
}

bool SetDebugAs(int slot, uint64_t steamid64) {
  if (slot < 0 || slot >= 64) return false;
  g_debugAs[slot] = steamid64;
  g_players[slot] = PlayerState{};
  return true;
}

std::string ApplyStatus() {
  int players = 0;
  for (const auto& p : g_players) players += p.steamid64 != 0;
  return std::to_string(players) + " player(s) tracked, " + std::to_string(g_weapons.size()) + " weapon(s) seen, " +
         std::to_string(g_applied) + " applied" + (Off().ok ? "" : ", schema offsets not resolved yet") +
         (DisabledByEnv() ? ", DISABLED by READYUP_DISABLE_SKINS" : "");
}

}  // namespace skins
