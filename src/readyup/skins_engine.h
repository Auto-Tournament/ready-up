#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Engine surface used by weapon skins / knives / gloves / agents.
//
// Everything here must be called from the server GameFrame thread (see game_frame_hook.cpp).
// Every function is best-effort: if its signature did not resolve to exactly one match in the
// real libserver.so, it returns false/nullptr and does nothing. See docs/skins-engine-surface.md
// for how each item was verified against CS2 1.41.8.3.
namespace readyup::skins {

// Resolves all signatures once (idempotent). Cheap after the first call.
void ResolveEngine();

// ---- Entity system -------------------------------------------------------------------------
// The entity system global comes from the verified UTIL_Remove signature; its layout is only
// trusted after a fault-safe round-trip (identity -> entity -> m_pEntity -> identity, handle index
// bits, entity 0 = "worldent"), with the identity stride and field offsets taken from schema.
// 0 = not known yet (no map / schema not ready), 1 = verified, 2 = failed. Until verified every
// accessor below returns nullptr / 0xFFFFFFFF.
int EntitySystemStatus(std::string* detail);
bool EntitySystemReady();
void* EntityByIndex(int index);
void* EntityFromHandle(uint32_t handle);
uint32_t EntityHandleOf(void* entity);            // 0xFFFFFFFF if unknown
const char* EntityDesignerName(void* entity);     // nullptr if unknown

// ---- Calls into libserver ------------------------------------------------------------------
// CAttributeList::SetOrAddAttributeValueByName(this, name, value).
bool AttrSetOrAddByName(void* attributeList, const char* name, float value);
// CBaseEntity::ChangeSubclass(this, subclassName). Knives use the item defindex ("507").
bool ChangeSubclass(void* entity, const char* subclass);
// CBaseModelEntity::SetModel(this, "agents/models/....vmdl").
bool SetModel(void* entity, const char* model);
// Model lookup + FindBodygroupByName + CBaseModelEntity::SetBodygroup(group, value).
enum class BodygroupResult {
  kOk,
  kUnavailable,  // one of the three functions did not resolve
  kNoModel,      // entity has no (loaded) model yet, e.g. right after SetModel
  kNoGroup,      // model has no bodygroup with that name
};
BodygroupResult SetBodygroupByName(void* entity, const char* group, int value);
const char* BodygroupResultName(BodygroupResult r);
// CEntityInstance::NetworkStateChanged (vtbl[29]) with an empty change list => mark the whole
// entity dirty so fields written after its first snapshot are re-sent.
bool MarkEntityFullyChanged(void* entity);

// Human-readable status of every engine item (for debug logs / `ru` diagnostics).
struct ItemStatus {
  std::string name;
  bool ok = false;
  std::string detail;
};
std::vector<ItemStatus> EngineStatus();

// Prints EngineStatus() once (always, not debug-gated) so operators can see what is missing.
void LogEngineStatusOnce();

}  // namespace readyup::skins
