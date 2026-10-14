#include "skins.h"

#include "apply_internal.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

// Spawn cosmetics: agent model + gloves. Called from GameFrameTick (game thread) on the frame a
// pawn becomes alive, and once more on the next frame.

namespace skins {
namespace {

std::unordered_set<uint64_t> g_warned;

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

}  // namespace

void SettleGloves(void* pawn) {
  if (pawn) (void)SetBodygroupByName(pawn, "first_or_third_person", 1);
}

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
      } else {
        const uint16_t def = static_cast<uint16_t>(*gloveDef);
        std::memcpy(static_cast<unsigned char*>(item) + detail::ItemDefIndexOffset(), &def, sizeof(def));
        detail::WritePaint(nullptr, item, steamid64, skin);
        if (detail::ItemInitializedOffset() >= 0) {
          const bool init = true;
          std::memcpy(static_cast<unsigned char*>(item) + detail::ItemInitializedOffset(), &init, sizeof(init));
        }
        // As WeaponPaints does it: no glove counter bump and no full re-send of the pawn (both
        // crashed clients when CS2 restarted the round, 2026-10-14). The model is redrawn by
        // switching its first_or_third_person bodygroup off here and on again ~0.2 s later
        // (SettleGloves, after the last pass).
        (void)SetBodygroupByName(pawn, "first_or_third_person", 0);
        // Hides the agent model's built-in gloves. Retried on every cosmetics pass: on the spawn
        // frame the (new) agent model is often not loaded yet (GetModel == null).
        const auto bg = SetBodygroupByName(pawn, "default_gloves", 1);
        if (bg == RU_BODYGROUP_OK) {
          WarnOnce(steamid64, "default_gloves bodygroup set");
        } else {
          const std::string why = std::string("SetBodygroup(default_gloves,1) not applied yet: ") +
                                  BodygroupResultName(bg);
          WarnOnce(steamid64, why.c_str());
        }
      }
    }
  }

  // Pawns persist across rounds, so a new agent model was networked long ago: force a full
  // re-send. Gloves do not (above).
  if (changed) {
    if (!MarkEntityFullyChanged(pawn)) WarnOnce(steamid64, "NetworkStateChanged unavailable");
  }
  (void)late;
  return true;
}

}  // namespace skins
