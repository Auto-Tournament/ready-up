#include "readyup/skins_engine.h"

#include "readyup/config.h"
#include "readyup/engine_surface.h"
#include "readyup/features.h"
#include "readyup/logging.h"
#include "readyup/schema.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace readyup::skins {
namespace {

// Every function comes from the repo-owned engine surface (gamedata/engine-surface.json) via
// EngineFunction(): exactly one signature match AND all identity anchors pass, or nullptr.
// Verify after a CS2 update with: build/readyup_sigcheck <libserver.so> gamedata/engine-surface.json
struct FnSpec {
  const char* label;  // human-readable
  const char* key;    // engine-surface.json "functions" key
};

constexpr FnSpec kAttrSet{"CAttributeList::SetOrAddAttributeValueByName", "CAttributeList_SetOrAddAttributeValueByName"};
constexpr FnSpec kChangeSubclass{"CBaseEntity::ChangeSubclass", "CBaseEntity_ChangeSubclass"};
constexpr FnSpec kSetModel{"CBaseModelEntity::SetModel", "CBaseModelEntity_SetModel"};
constexpr FnSpec kGetModel{"CBaseModelEntity::GetModel", "CBaseModelEntity_GetModel"};
constexpr FnSpec kFindBodygroup{"CModel::FindBodygroupByName", "CModel_FindBodygroupByName"};
constexpr FnSpec kSetBodygroup{"CBaseModelEntity::SetBodygroup", "CBaseModelEntity_SetBodygroup"};
constexpr FnSpec kStateChanged{"CEntityInstance::NetworkStateChanged (CBaseEntity impl)", "CBaseEntity_NetworkStateChanged"};
// UTIL_Remove is only used as an anchor: its body does `lea rax, [rip+g_pGameEntitySystem]`
// (those bytes are part of its verified signature).
constexpr FnSpec kUtilRemove{"UTIL_Remove (entity system anchor)", "UTIL_Remove"};
constexpr const char* kStateChangedSlot = "CEntityInstance::NetworkStateChanged";

// Engine constants of CEntitySystem's identity list (not layout guesses): 512 identities per
// chunk, 15-bit entity index in a handle. Everything that is a layout -- the identity stride
// (schema sizeof CEntityIdentity), CEntityInstance::m_pEntity / CEntityIdentity::m_designerName
// (schema), the handle at identity+0x10 and the chunk array at entitysystem+0x10 -- is
// round-trip verified by VerifyEntitySystem() before any fast-path read happens.
constexpr int kChunkShift = 9;
constexpr int kChunkMask = 0x1FF;
constexpr uint32_t kIndexMask = 0x7FFF;
constexpr int kMaxEntityIndex = 0x7FFE;
constexpr int kIdentityHandle = 0x10;      // CEntityIdentity::m_EHandle (not in schema; verified)
constexpr int kEntitySystemChunks = 0x10;  // CEntitySystem::m_EntityList.m_pIdentityChunks (verified)

struct Resolved {
  void* addr = nullptr;
  std::string detail;
};

std::once_flag g_once;
Resolved g_attrSet, g_changeSubclass, g_setModel, g_getModel, g_findBodygroup, g_setBodygroup, g_stateChanged,
    g_utilRemove;
void** g_entitySystemGlobal = nullptr;  // &g_pGameEntitySystem inside libserver
std::string g_entitySystemDetail;
int g_vtStateChanged = -1;  // only set once the slot verified

// Layout (valid only when g_state == 1).
int g_offDesigner = -1;
int g_offInstanceEntity = -1;
int g_identitySize = -1;

std::mutex g_stateMu;
std::atomic<int> g_state{0};  // 0 unknown (pending), 1 verified, 2 failed
std::string g_stateDetail = "not checked yet (needs a loaded map)";

Resolved ResolveFn(const FnSpec& spec) {
  Resolved r;
  r.addr = EngineFunction(spec.key);
  const es::Resolution res = EngineFunctionResolution(spec.key);
  r.detail = res.detail.empty() ? (r.addr ? "ok" : "unresolved") : res.detail;
  return r;
}

void ResolveEntitySystem() {
  g_utilRemove = ResolveFn(kUtilRemove);
  if (!g_utilRemove.addr) {
    g_entitySystemDetail = "UTIL_Remove anchor " + g_utilRemove.detail;
    return;
  }
  // Signature: 48 89 FE | 48 85 FF | 74 xx | 48 8D 05 <disp32> : lea rax, [rip + disp32]
  const auto* p = static_cast<const unsigned char*>(g_utilRemove.addr);
  int32_t disp = 0;
  std::memcpy(&disp, p + 11, sizeof(disp));
  g_entitySystemGlobal = reinterpret_cast<void**>(const_cast<unsigned char*>(p + 15) + disp);
  g_entitySystemDetail = "g_pGameEntitySystem via UTIL_Remove";
}

void ResolveAll() {
  g_attrSet = ResolveFn(kAttrSet);
  g_changeSubclass = ResolveFn(kChangeSubclass);
  g_setModel = ResolveFn(kSetModel);
  g_getModel = ResolveFn(kGetModel);
  g_findBodygroup = ResolveFn(kFindBodygroup);
  g_setBodygroup = ResolveFn(kSetBodygroup);
  g_stateChanged = ResolveFn(kStateChanged);
  int idx = -1;
  std::string why;
  if (VerifyEngineVtableSlot(kStateChangedSlot, nullptr, &idx, &why)) g_vtStateChanged = idx;
  ResolveEntitySystem();
}

void* EntitySystem() {
  if (!g_entitySystemGlobal) return nullptr;
  return *g_entitySystemGlobal;
}

template <typename T>
bool SafeGet(uintptr_t addr, T* out) {
  return SafeReadMemory(addr, out, sizeof(T));
}

// Fault-safe identity lookup used only during verification.
uintptr_t SafeIdentityForIndex(uintptr_t es, int index) {
  uintptr_t chunk = 0;
  if (!SafeGet(es + kEntitySystemChunks + static_cast<uintptr_t>(index >> kChunkShift) * sizeof(void*), &chunk)) return 0;
  if (!chunk) return 0;
  return chunk + static_cast<uintptr_t>(index & kChunkMask) * static_cast<uintptr_t>(g_identitySize);
}

// identity -> entity -> m_pEntity must lead back to the same identity, and the handle's index
// bits must equal the index. 1 = ok, 0 = empty slot, -1 = layout mismatch.
int RoundTrip(uintptr_t es, int index) {
  const uintptr_t ident = SafeIdentityForIndex(es, index);
  if (!ident) return 0;
  uintptr_t ent = 0;
  uint32_t handle = 0;
  if (!SafeGet(ident, &ent) || !SafeGet(ident + kIdentityHandle, &handle)) return -1;
  if (!ent) return 0;
  uintptr_t back = 0;
  if (!SafeGet(ent + static_cast<uintptr_t>(g_offInstanceEntity), &back)) return -1;
  if (back != ident) return -1;
  if ((handle & kIndexMask) != static_cast<uint32_t>(index)) return -1;
  return 1;
}

// Returns 0 pending / 1 ok / 2 failed and sets *why.
int VerifyEntitySystem(std::string* why) {
  if (!g_entitySystemGlobal) {
    *why = g_entitySystemDetail.empty() ? "entity system global unresolved" : g_entitySystemDetail;
    return 2;
  }
  std::string schemaWhy;
  const int schema = SchemaStatus(&schemaWhy);
  if (schema != 1) {
    *why = "schema: " + schemaWhy;
    return schema == 2 ? 2 : 0;
  }
  const auto designer = SchemaFindOffset("server", "CEntityIdentity", "m_designerName");
  const auto instEnt = SchemaFindOffset("server", "CEntityInstance", "m_pEntity");
  const auto size = SchemaClassSize("CEntityIdentity");
  if (!designer || !instEnt || !size || *size <= kIdentityHandle + 4) {
    *why = "schema lacks CEntityIdentity::m_designerName / CEntityInstance::m_pEntity / sizeof(CEntityIdentity)";
    return 2;
  }
  g_offDesigner = *designer;
  g_offInstanceEntity = *instEnt;
  g_identitySize = *size;

  uintptr_t es = 0;
  if (!SafeGet(reinterpret_cast<uintptr_t>(g_entitySystemGlobal), &es) || !es) {
    *why = "entity system not created yet (no map loaded)";
    return 0;
  }
  // Entity 0 is the world ("worldent") once a map is loaded.
  const int w = RoundTrip(es, 0);
  if (w == 0) {
    *why = "world entity not spawned yet";
    return 0;
  }
  if (w < 0) {
    *why = "entity 0 identity/handle/m_pEntity round-trip failed (entity list layout changed)";
    return 2;
  }
  uintptr_t ident0 = SafeIdentityForIndex(es, 0), namePtr = 0;
  const bool nameRead = SafeGet(ident0 + static_cast<uintptr_t>(g_offDesigner), &namePtr);
  const auto dn = nameRead ? SafeReadCString(namePtr, 64) : std::nullopt;
  if (!dn || *dn != "worldent") {
    *why = "entity 0 is '" + (dn ? *dn : std::string("?")) + "', expected 'worldent'";
    return 2;
  }
  // A few more live entities (exercise the identity stride and a second chunk index).
  int checked = 0;
  for (int i = 1; i < 2048 && checked < 8; ++i) {
    const int r = RoundTrip(es, i);
    if (r < 0) {
      *why = "entity " + std::to_string(i) + " round-trip failed (identity stride/handle layout changed)";
      return 2;
    }
    checked += r;
  }
  *why = "verified (worldent + " + std::to_string(checked) + " entities round-trip; identity stride " +
         std::to_string(g_identitySize) + ")";
  return 1;
}

unsigned char* IdentityForIndex(int index) {
  void* es = EntitySystem();
  if (!es || index < 0 || index > kMaxEntityIndex) return nullptr;
  auto** chunks = reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(es) + kEntitySystemChunks);
  unsigned char* chunk = chunks[index >> kChunkShift];
  if (!chunk) return nullptr;
  return chunk + (index & kChunkMask) * g_identitySize;
}

}  // namespace

void ResolveEngine() {
  std::call_once(g_once, ResolveAll);
}

int EntitySystemStatus(std::string* detail) {
  ResolveEngine();
  int st = g_state.load(std::memory_order_acquire);
  if (st == 0) {
    std::lock_guard<std::mutex> lk(g_stateMu);
    st = g_state.load(std::memory_order_acquire);
    if (st == 0) {
      std::string why;
      st = VerifyEntitySystem(&why);
      g_stateDetail = why;
      if (st != 0) g_state.store(st, std::memory_order_release);
    }
  }
  if (detail) {
    std::lock_guard<std::mutex> lk(g_stateMu);
    *detail = g_stateDetail;
  }
  return st;
}

bool EntitySystemReady() {
  return EntitySystemStatus(nullptr) == 1;
}

void* EntityByIndex(int index) {
  if (g_state.load(std::memory_order_acquire) != 1) return nullptr;
  unsigned char* ident = IdentityForIndex(index);
  if (!ident) return nullptr;
  uint32_t h = 0;
  std::memcpy(&h, ident + kIdentityHandle, sizeof(h));
  if (static_cast<int>(h & kIndexMask) != index) return nullptr;
  return *reinterpret_cast<void**>(ident);
}

void* EntityFromHandle(uint32_t handle) {
  if (handle == 0xFFFFFFFFu || g_state.load(std::memory_order_acquire) != 1) return nullptr;
  const int index = static_cast<int>(handle & kIndexMask);
  unsigned char* ident = IdentityForIndex(index);
  if (!ident) return nullptr;
  uint32_t h = 0;
  std::memcpy(&h, ident + kIdentityHandle, sizeof(h));
  if (h != handle) return nullptr;  // stale handle (different serial)
  return *reinterpret_cast<void**>(ident);
}

uint32_t EntityHandleOf(void* entity) {
  if (!entity || g_state.load(std::memory_order_acquire) != 1) return 0xFFFFFFFFu;
  auto* ident = *reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(entity) + g_offInstanceEntity);
  if (!ident) return 0xFFFFFFFFu;
  uint32_t h = 0;
  std::memcpy(&h, ident + kIdentityHandle, sizeof(h));
  return h;
}

const char* EntityDesignerName(void* entity) {
  if (!entity || g_state.load(std::memory_order_acquire) != 1) return nullptr;
  auto* ident = *reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(entity) + g_offInstanceEntity);
  if (!ident) return nullptr;
  return *reinterpret_cast<const char**>(ident + g_offDesigner);
}

bool AttrSetOrAddByName(void* attributeList, const char* name, float value) {
  ResolveEngine();
  if (!g_attrSet.addr || !attributeList || !name) return false;
  using Fn = void (*)(void*, const char*, float);
  reinterpret_cast<Fn>(g_attrSet.addr)(attributeList, name, value);
  return true;
}

bool ChangeSubclass(void* entity, const char* subclass) {
  ResolveEngine();
  if (!g_changeSubclass.addr || !entity || !subclass || !*subclass) return false;
  using Fn = void (*)(void*, const char*);
  reinterpret_cast<Fn>(g_changeSubclass.addr)(entity, subclass);
  return true;
}

bool SetModel(void* entity, const char* model) {
  ResolveEngine();
  if (!g_setModel.addr || !entity || !model || !*model) return false;
  using Fn = void (*)(void*, const char*);
  reinterpret_cast<Fn>(g_setModel.addr)(entity, model);
  return true;
}

const char* BodygroupResultName(BodygroupResult r) {
  switch (r) {
    case BodygroupResult::kOk: return "ok";
    case BodygroupResult::kUnavailable: return "functions unresolved";
    case BodygroupResult::kNoModel: return "entity has no loaded model";
    case BodygroupResult::kNoGroup: return "model has no such bodygroup";
  }
  return "?";
}

BodygroupResult SetBodygroupByName(void* entity, const char* group, int value) {
  ResolveEngine();
  if (!FeatureEnabled(Feature::SkinsBodygroups) || !g_getModel.addr || !g_findBodygroup.addr || !g_setBodygroup.addr ||
      !entity || !group) {
    return BodygroupResult::kUnavailable;
  }
  // Mirrors CBaseModelEntity::InputSetBodyGroup: model = GetModel(this);
  // idx = FindBodygroupByName(model, name); if (idx >= 0) SetBodygroup(this, idx, value).
  using GetModelFn = void* (*)(void*);
  using FindFn = int (*)(void*, const char*);
  using SetFn = void (*)(void*, int, int);
  // GetModel returns null until the model resource behind the entity's model handle is loaded,
  // which is the case on the same frame as a SetModel to a not-yet-resident agent model.
  void* model = reinterpret_cast<GetModelFn>(g_getModel.addr)(entity);
  if (!model) return BodygroupResult::kNoModel;
  const int idx = reinterpret_cast<FindFn>(g_findBodygroup.addr)(model, group);
  if (idx < 0) return BodygroupResult::kNoGroup;
  reinterpret_cast<SetFn>(g_setBodygroup.addr)(entity, idx, value);
  return BodygroupResult::kOk;
}

bool MarkEntityFullyChanged(void* entity) {
  ResolveEngine();
  if (!g_stateChanged.addr || g_vtStateChanged < 0 || !entity) return false;
  void** vt = *reinterpret_cast<void***>(entity);
  if (!vt || vt[g_vtStateChanged] != g_stateChanged.addr) return false;  // unexpected override
  // NetworkStateChangedData: first dword = number of local offsets. Zero => whole entity changed
  // (the verified signature covers `mov eax,[rsi]; test eax,eax; jne`: with a zero count the
  // implementation reads nothing else from the data). Zeroed and oversized on purpose.
  alignas(16) unsigned char data[0x80] = {};
  using Fn = void (*)(void*, void*);
  reinterpret_cast<Fn>(vt[g_vtStateChanged])(entity, data);
  return true;
}

std::vector<ItemStatus> EngineStatus() {
  ResolveEngine();
  std::vector<ItemStatus> out;
  auto add = [&](const FnSpec& s, const Resolved& r) { out.push_back({s.label, r.addr != nullptr, r.detail}); };
  add(kAttrSet, g_attrSet);
  add(kChangeSubclass, g_changeSubclass);
  add(kSetModel, g_setModel);
  add(kGetModel, g_getModel);
  add(kFindBodygroup, g_findBodygroup);
  add(kSetBodygroup, g_setBodygroup);
  add(kStateChanged, g_stateChanged);
  out.push_back({"CEntityInstance::NetworkStateChanged vtable slot", g_vtStateChanged >= 0,
                 g_vtStateChanged >= 0 ? "verified" : "unverified"});
  std::string entDetail;
  const int ent = EntitySystemStatus(&entDetail);
  out.push_back({"CGameEntitySystem", ent == 1, g_entitySystemDetail + "; " + entDetail});
  return out;
}

void LogEngineStatusOnce() {
  static std::once_flag once;
  std::call_once(once, [] {
    for (const auto& s : EngineStatus()) {
      Print("skins: %-62s %s  %s\n", s.name.c_str(), s.ok ? "OK     " : "MISSING", s.detail.c_str());
    }
  });
}

}  // namespace readyup::skins
