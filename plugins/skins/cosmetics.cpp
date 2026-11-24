#include "skins.h"

#include "apply_internal.h"
#include "knife_swap.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

// Spawn cosmetics: agent model + gloves. Called from GameFrameTick (game thread) on the frame a
// pawn becomes alive, and once more on the next frame.

namespace skins {
namespace {

std::unordered_set<uint64_t> g_warned;
// The gloves we last put on each player's pawn (knife_swap.h PaintStamp): the same gloves on the
// same item are not written again, so the client rebuilds the glove model once per spawn, not on
// every cosmetics pass.
std::unordered_map<uint64_t, PaintStamp> g_gloveStamps;

void WarnOnce(uint64_t steamid64, const char* what) {
  if (!DebugOn() || steamid64 == 0) return;
  if (!g_warned.insert(steamid64 ^ std::hash<std::string>{}(what)).second) return;
  Log(RU_LOG_DEBUG, "%llu: %s", static_cast<unsigned long long>(steamid64), what);
}

struct PawnOffsets {
  bool resolved = false;
  int econGloves = -1;         // CCSPlayerPawn::m_EconGloves (CEconItemView, embedded)
  int econGlovesChanged = -1;  // CCSPlayerPawn::m_nEconGlovesChanged (uint8)
};

PawnOffsets& POff() {
  static PawnOffsets o;
  if (!o.resolved) {
    o.resolved = true;
    o.econGloves = SchemaOffset("CCSPlayerPawn", "m_EconGloves");
    o.econGlovesChanged = SchemaOffset("CCSPlayerPawn", "m_nEconGlovesChanged");
  }
  return o;
}

// A loadout holds either a full model path ("agents/models/ctm_fbi/ctm_fbi_variantb.vmdl") or the short
// WeaponPaints-style form ("ctm_fbi/ctm_fbi_variantb"). CS2 1.41 ships agent models under
// agents/models/ (the old characters/models/ entries are small stubs).
std::string NormalizeAgentModel(const std::string& raw) {
  if (raw.empty()) return raw;
  std::string m = raw;
  if (m.find('/') == std::string::npos) return {};  // not a path
  if (m.rfind("agents/", 0) != 0 && m.rfind("characters/", 0) != 0) m = "agents/models/" + m;
  if (m.size() < 5 || m.compare(m.size() - 5, 5, ".vmdl") != 0) m += ".vmdl";
  return m;
}

// The pawn's model path (CModelState::m_ModelName, a CUtlSymbolLarge: a char pointer), "" unknown.
std::string PawnModelName(void* pawn) {
  static int body = -2, skel = -1, state = -1, name = -1;
  if (body == -2) {
    body = SchemaOffset("CBaseEntity", "m_CBodyComponent");
    skel = SchemaOffset("CBodyComponentSkeletonInstance", "m_skeletonInstance");
    state = SchemaOffset("CSkeletonInstance", "m_modelState");
    name = SchemaOffset("CModelState", "m_ModelName");
  }
  if (!pawn || body < 0 || skel < 0 || state < 0 || name < 0) return {};
  void* bc = nullptr;
  std::memcpy(&bc, static_cast<unsigned char*>(pawn) + body, sizeof(bc));
  if (!bc) return {};
  const char* s = nullptr;
  std::memcpy(&s, static_cast<unsigned char*>(bc) + skel + state + name, sizeof(s));
  if (!s) return {};
  std::string out;
  for (size_t i = 0; i < 260 && s[i]; ++i) {
    if (s[i] < 0x20 || s[i] > 0x7e) return {};  // not a path
    out.push_back(s[i]);
  }
  return out;
}

// Hides the model's built-in gloves (bodygroup default_gloves) under a glove skin; logs the model
// when it has none (#395: agents/models/tm_professional/tm_professional_varf4.vmdl, 2026-11-24).
int HideDefaultGloves(void* pawn, uint64_t steamid64) {
  const int bg = SetBodygroupByName(pawn, "default_gloves", 1);
  if (bg == RU_BODYGROUP_NO_GROUP) {
    const std::string cur = PawnModelName(pawn);
    WarnOnce(steamid64, ("no default_gloves bodygroup on " + (cur.empty() ? std::string("an unknown model") : cur)).c_str());
  }
  return bg;
}

// The WeaponPaints way to make the client rebuild the hands: set another model and the pawn's own
// one again, in the same frame. Without it a player with no gloves of their own in CS2 kept the
// stock gloves over the glove skin (#395); with gloves of their own the client rebuilt them anyway.
void BounceModel(void* pawn, uint64_t steamid64) {
  const std::string cur = PawnModelName(pawn);
  if (cur.empty() || cur.find("tm_jumpsuit") != std::string::npos) return;
  if (SetModel(pawn, "characters/models/tm_jumpsuit/tm_jumpsuit_varianta.vmdl") && SetModel(pawn, cur.c_str()))
    WarnOnce(steamid64, ("model bounced for the gloves: " + cur).c_str());
}

}  // namespace

bool ApplySpawnCosmetics(void* pawn, uint64_t steamid64, int team, bool late) {
  if (!pawn || steamid64 == 0 || (team != 2 && team != 3)) return true;
  if (!IsLoaded(steamid64)) return false;
  bool changed = false;

  // 1) Agent model (only when the player picked one; otherwise leave the game's choice alone).
  if (const auto agents = FindAgents(steamid64)) {
    const std::string model = NormalizeAgentModel(team == 3 ? agents->agent_ct : agents->agent_t);
    if (!model.empty()) {
      if (SetModel(pawn, model.c_str())) changed = true;
      else WarnOnce(steamid64, "SetModel unavailable; agent not applied");
    }
  }

  // 2) Gloves: fill the pawn's glove econ item, then hide the model's default gloves.
  if (const auto gloveDef = FindGloveDefindex(steamid64, team)) {
    auto& po = POff();
    if (po.econGloves < 0 || !detail::EconOffsetsReady()) {
      WarnOnce(steamid64, "glove schema fields missing; gloves not applied");
    } else {
      void* item = static_cast<unsigned char*>(pawn) + po.econGloves;
      WeaponSkinEntry skin;
      if (const auto s = FindWeaponSkin(steamid64, team, *gloveDef)) skin = *s;
      if (skin.paint_id <= 0) {
        WarnOnce(steamid64, "glove row has no paint in readyup_weapon_skins; gloves need a paint kit");
      } else if (StampMatches(g_gloveStamps[steamid64], detail::ItemIdOf(item), *gloveDef, skin.paint_id, skin.seed,
                              skin.wear)) {
        // Already on: only the default-gloves bodygroup (the model may have loaded since).
        (void)HideDefaultGloves(pawn, steamid64);
      } else {
        BounceModel(pawn, steamid64);
        const uint16_t def = static_cast<uint16_t>(*gloveDef);
        std::memcpy(static_cast<unsigned char*>(item) + detail::ItemDefIndexOffset(), &def, sizeof(def));
        const uint64_t id = detail::WritePaint(nullptr, item, steamid64, skin);
        g_gloveStamps[steamid64] = PaintStamp{id, *gloveDef, skin.paint_id, skin.seed, skin.wear};
        if (detail::ItemInitializedOffset() >= 0) {
          const bool init = true;
          std::memcpy(static_cast<unsigned char*>(item) + detail::ItemInitializedOffset(), &init, sizeof(init));
        }
        if (po.econGlovesChanged >= 0) {
          // Networked counter; bumping it tells clients to rebuild the glove model.
          auto* counter = static_cast<unsigned char*>(pawn) + po.econGlovesChanged;
          *counter = static_cast<unsigned char>(*counter + 1);
        }
        // Hides the agent model's built-in gloves. Retried on every cosmetics pass: on the spawn
        // frame the (new) agent model is often not loaded yet (GetModel == null).
        const auto bg = HideDefaultGloves(pawn, steamid64);
        if (bg == RU_BODYGROUP_OK) {
          WarnOnce(steamid64, "default_gloves bodygroup set");
        } else {
          const std::string why = std::string("SetBodygroup(default_gloves,1) not applied yet: ") +
                                  BodygroupResultName(bg);
          WarnOnce(steamid64, why.c_str());
        }
        changed = true;
      }
    }
  }

  // Pawns persist across rounds, so their glove/model fields were networked long ago. Force a
  // full re-send (cheap: once per spawn, only for players with cosmetics).
  if (changed) {
    if (!MarkEntityFullyChanged(pawn)) WarnOnce(steamid64, "NetworkStateChanged unavailable");
  }
  (void)late;
  return true;
}

}  // namespace skins
