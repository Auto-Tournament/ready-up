// readyup-hud (plugins/hud, hud.so): custom in-game HUDs through CS2's custom_hud_layout entity.
//
// Clients render a Panorama layout from the Auto Tournament HUD workshop addon (repo hud/); the
// server only says which layout (m_strLayout, the layout's SOURCE path) and, per player or for
// everyone, which text each panel's dialog variables hold and which CSS classes each panel has.
// Other plugins drive it through readyup.hud.v1 (core/include/readyup/hud_iface.h).
//
// How the entity is driven (CS2 1.41.9.x, all field offsets by name from the schema):
//  - Three string tables on the entity: m_vecPanelIds, m_vecClassNames, m_vecDialogVariableNames
//    (CUtlVector<CUtlString>). Everything below refers to a panel / class / variable by its index
//    there.
//  - A CCSCustomHudLayoutState per player in m_vecPlayerLayoutStates (m_playerSlot says whose) and
//    one for everyone in m_globalLayoutState. Each holds m_vecHasClasses (HUDPanelHasClass_t:
//    panel index, class index, status -1 / 0 / 1) and m_vecDialogVariableStrings
//    (HUDPanelDialogVariableString_t: vtable, panel index, variable index, CUtlString value,
//    is-set flag). The vtable is the class's own, found by RTTI (engine-surface.hud.json,
//    ru_api surface_rtti_vtable).
//  - Vectors grow with tier0's allocator (MemAlloc_*Func, exported by libtier0.so), the one the
//    engine frees them with. After writing, the entity is marked changed so it is re-sent.
//
// The entity is ours alone: strays (a reload, a crash) are removed before the first one is made,
// it is recreated after a map change, and the desired state is pushed again then and a few seconds
// after a player takes a slot (the engine clears a slot's is-set flags when a player joins).
#include "hud_vec.h"

#include "readyup/hud_iface.h"
#include "readyup/plugin_api.h"
#include "readyup/selftest_iface.h"

#include <dlfcn.h>
#include <link.h>
#include <climits>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hud {
namespace {

const ru_api* g_api = nullptr;
constexpr const char* kClass = "custom_hud_layout";
constexpr uint32_t kNoHandle = 0xFFFFFFFFu;

// ---- engine access ---------------------------------------------------------------------------

struct Offsets {
  int strLayout = -1, playerStates = -1, globalState = -1, panelIds = -1, classNames = -1, varNames = -1;
  int observable = -1;
  int stSlot = -1, stCapture = -1, stClasses = -1, stTexts = -1;
  int hcPanel = -1, hcClass = -1, hcStatus = -1;
  int dvPanel = -1, dvVar = -1, dvValue = -1, dvSet = -1;
};

struct Engine {
  bool resolved = false;
  bool ok = false;
  std::string status = "not resolved";
  Offsets o;
  Allocator a;
  const void* dvVtable = nullptr;
} g_eng;

// Sizes of the two element types, from their field offsets (checked in Resolve).
constexpr size_t kHasClassSize = 8;   // u16 panel, u16 class, int status
constexpr size_t kDialogVarSize = 32; // vtable, u16 panel, u16 var, (pad), CUtlString, bool, (pad)

std::string Fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return buf;
}

int Off(const char* cls, const char* field) { return g_api->schema_offset(g_api->self, cls, field); }

void Resolve() {
  if (g_eng.resolved && g_eng.ok) return;
  g_eng.resolved = true;
  g_eng.ok = false;
  if (g_api->entity_system_status(g_api->self) != RU_ENTSYS_OK) {
    g_eng.resolved = false;  // try again once a map is up
    g_eng.status = "entity system not ready";
    return;
  }
  Offsets& o = g_eng.o;
  o.strLayout = Off("CCSCustomHudLayout", "m_strLayout");
  o.playerStates = Off("CCSCustomHudLayout", "m_vecPlayerLayoutStates");
  o.globalState = Off("CCSCustomHudLayout", "m_globalLayoutState");
  o.panelIds = Off("CCSCustomHudLayout", "m_vecPanelIds");
  o.classNames = Off("CCSCustomHudLayout", "m_vecClassNames");
  o.varNames = Off("CCSCustomHudLayout", "m_vecDialogVariableNames");
  o.observable = Off("CCSCustomHudLayout", "m_bObservable");
  o.stSlot = Off("CCSCustomHudLayoutState", "m_playerSlot");
  o.stCapture = Off("CCSCustomHudLayoutState", "m_bInputCaptureEnabled");
  o.stClasses = Off("CCSCustomHudLayoutState", "m_vecHasClasses");
  o.stTexts = Off("CCSCustomHudLayoutState", "m_vecDialogVariableStrings");
  o.hcPanel = Off("HUDPanelHasClass_t", "m_nPanelIdIndex");
  o.hcClass = Off("HUDPanelHasClass_t", "m_nClassNameIndex");
  o.hcStatus = Off("HUDPanelHasClass_t", "m_eClassStatus");
  o.dvPanel = Off("HUDPanelDialogVariableString_t", "m_nPanelIdIndex");
  o.dvVar = Off("HUDPanelDialogVariableString_t", "m_nDialogVariableIndex");
  o.dvValue = Off("HUDPanelDialogVariableString_t", "m_sValue");
  o.dvSet = Off("HUDPanelDialogVariableString_t", "m_bIsSet");
  const int must[] = {o.strLayout, o.playerStates, o.globalState, o.panelIds, o.classNames, o.varNames,
                      o.stSlot,    o.stClasses,    o.stTexts,     o.hcPanel,  o.hcClass,    o.hcStatus,
                      o.dvPanel,   o.dvVar,        o.dvValue,     o.dvSet};
  for (int v : must) {
    if (v < 0) {
      g_eng.status = "schema: a CCSCustomHudLayout field is missing (CS2 too old, or the schema changed)";
      return;
    }
  }
  // The element layouts this plugin writes. Anything else and it stays off.
  if (o.hcPanel != 0 || o.hcClass != 2 || o.hcStatus != 4) {
    g_eng.status = "schema: HUDPanelHasClass_t layout changed";
    return;
  }
  if (o.dvPanel != 8 || o.dvVar != 10 || o.dvValue != 16 || o.dvSet != 24) {
    g_eng.status = "schema: HUDPanelDialogVariableString_t layout changed";
    return;
  }
  void* t0 = dlopen("libtier0.so", RTLD_NOW | RTLD_NOLOAD);
  if (!t0) {
    g_eng.status = "libtier0.so not loaded";
    return;
  }
  g_eng.a.alloc = reinterpret_cast<void* (*)(size_t)>(dlsym(t0, "MemAlloc_AllocFunc"));
  g_eng.a.realloc = reinterpret_cast<void* (*)(void*, size_t)>(dlsym(t0, "MemAlloc_ReallocFunc"));
  g_eng.a.free = reinterpret_cast<void (*)(void*)>(dlsym(t0, "MemAlloc_FreeFunc"));
  g_eng.a.strdup = reinterpret_cast<char* (*)(const char*)>(dlsym(t0, "MemAlloc_StrDupFunc"));
  if (!g_eng.a.ok()) {
    g_eng.status = "tier0 MemAlloc_* exports missing";
    return;
  }
  g_eng.dvVtable = RU_API_HAS(g_api, surface_rtti_vtable)
                       ? g_api->surface_rtti_vtable(g_api->self, "HUDPanelDialogVariableString_t")
                       : nullptr;
  if (!g_eng.dvVtable) {
    g_eng.status = "HUDPanelDialogVariableString_t vtable not found (core older than API 1.17, or RTTI changed)";
    return;
  }
  g_eng.ok = true;
  g_eng.status = "ok";
}

RawVec* VecAtOff(void* base, int off) { return reinterpret_cast<RawVec*>(static_cast<uint8_t*>(base) + off); }

// ---- desired state ---------------------------------------------------------------------------

using Key = std::pair<std::string, std::string>;  // (panel id, variable / class)

struct SlotState {
  std::map<Key, std::string> texts;
  std::map<Key, bool> classes;
};

struct Layout {
  std::string path;
  char* pathCopy = nullptr;  // tier0 copy m_strLayout points at; kept for the process (tiny)
  uint32_t handle = kNoHandle;
  std::map<int, SlotState> slots;  // RU_HUD_ALL (-1) = global
  bool changed = false;            // entity needs entity_mark_changed this frame
  std::string lastError;
};

std::vector<Layout> g_layouts;  // handle id = index + 1
bool g_straysCleared = false;
double g_now = 0;
double g_mapStartedAt = -1;
std::map<int, std::vector<double>> g_resendAt;  // slot -> times to push its state again
std::vector<std::pair<double, std::function<void()>>> g_later;  // work to run at a time (game thread)
std::mutex g_statusMu;                           // guards g_selftestLine for `ru selftest`
std::string g_selftestLine = "not resolved";

void* EntityOf(Layout& l) {
  if (l.handle == kNoHandle) return nullptr;
  void* e = g_api->entity_from_handle(g_api->self, l.handle);
  const char* cn = e ? g_api->entity_classname(g_api->self, e) : nullptr;
  if (!cn || std::strcmp(cn, kClass) != 0) {
    l.handle = kNoHandle;
    return nullptr;
  }
  return e;
}

void RemoveStrays() {
  int removed = 0;
  for (int i = 65; i < 8192; ++i) {
    void* e = g_api->entity_by_index(g_api->self, i);
    const char* cn = e ? g_api->entity_classname(g_api->self, e) : nullptr;
    if (cn && std::strcmp(cn, kClass) == 0 && g_api->entity_remove(g_api->self, e)) ++removed;
  }
  if (removed) ru_logf(g_api, RU_LOG_INFO, "removed %d stray %s entit%s", removed, kClass, removed == 1 ? "y" : "ies");
}

// The layout state for `slot` on entity `e` (RU_HUD_ALL = global), or nullptr.
void* StateOf(void* e, int slot, std::string* why) {
  const Offsets& o = g_eng.o;
  if (slot == RU_HUD_ALL) return static_cast<uint8_t*>(e) + o.globalState;
  RawVec* v = VecAtOff(e, o.playerStates);
  if (!VecLooksValid(v, why)) return nullptr;
  if (v->size == 0) {
    if (why) *why = "no per-player layout states on the entity";
    return nullptr;
  }
  // The element size is not in the schema API: find the stride at which element i has
  // m_playerSlot == i (the engine keeps one state per slot, in slot order).
  static size_t stride = 0;
  if (!stride && v->size >= 2) {
    for (size_t s = 16; s <= 1024 && !stride; s += 8) {
      bool okAll = true;
      for (int i = 0; i < v->size && i < 8 && okAll; ++i) {
        const auto* el = static_cast<const uint8_t*>(v->mem) + s * static_cast<size_t>(i);
        okAll = *reinterpret_cast<const int32_t*>(el + o.stSlot) == i;
      }
      if (okAll) stride = s;
    }
  }
  if (!stride) {
    if (why) *why = "could not work out the per-player state size";
    return nullptr;
  }
  if (slot < 0 || slot >= v->size) {
    if (why) *why = "slot " + std::to_string(slot) + " outside the " + std::to_string(v->size) + " player states";
    return nullptr;
  }
  auto* st = static_cast<uint8_t*>(v->mem) + stride * static_cast<size_t>(slot);
  if (*reinterpret_cast<int32_t*>(st + o.stSlot) != slot) {
    if (why) *why = "player state " + std::to_string(slot) + " belongs to another slot";
    return nullptr;
  }
  return st;
}

bool WriteText(void* e, void* st, const Key& k, const std::string& value, std::string* why) {
  const Offsets& o = g_eng.o;
  const int panel = StringIndexOrAdd(VecAtOff(e, o.panelIds), k.first.c_str(), g_eng.a, why);
  const int var = StringIndexOrAdd(VecAtOff(e, o.varNames), k.second.c_str(), g_eng.a, why);
  if (panel < 0 || var < 0) return false;
  RawVec* v = VecAtOff(st, o.stTexts);
  if (!VecLooksValid(v, why)) return false;
  uint8_t* el = nullptr;
  for (int i = 0; i < v->size && !el; ++i) {
    auto* c = static_cast<uint8_t*>(VecAt(v, i, kDialogVarSize));
    if (*reinterpret_cast<uint16_t*>(c + o.dvPanel) == panel && *reinterpret_cast<uint16_t*>(c + o.dvVar) == var) el = c;
  }
  if (!el) {
    el = static_cast<uint8_t*>(VecAppend(v, kDialogVarSize, g_eng.a, why));
    if (!el) return false;
    *reinterpret_cast<const void**>(el) = g_eng.dvVtable;
    *reinterpret_cast<uint16_t*>(el + o.dvPanel) = static_cast<uint16_t>(panel);
    *reinterpret_cast<uint16_t*>(el + o.dvVar) = static_cast<uint16_t>(var);
  }
  if (!SetCUtlString(reinterpret_cast<char**>(el + o.dvValue), value.c_str(), g_eng.a)) {
    if (why) *why = "strdup failed";
    return false;
  }
  *reinterpret_cast<bool*>(el + o.dvSet) = true;
  return true;
}

bool WriteClass(void* e, void* st, const Key& k, bool on, std::string* why) {
  const Offsets& o = g_eng.o;
  const int panel = StringIndexOrAdd(VecAtOff(e, o.panelIds), k.first.c_str(), g_eng.a, why);
  const int cls = StringIndexOrAdd(VecAtOff(e, o.classNames), k.second.c_str(), g_eng.a, why);
  if (panel < 0 || cls < 0) return false;
  RawVec* v = VecAtOff(st, o.stClasses);
  if (!VecLooksValid(v, why)) return false;
  uint8_t* el = nullptr;
  for (int i = 0; i < v->size && !el; ++i) {
    auto* c = static_cast<uint8_t*>(VecAt(v, i, kHasClassSize));
    if (*reinterpret_cast<uint16_t*>(c + o.hcPanel) == panel && *reinterpret_cast<uint16_t*>(c + o.hcClass) == cls) el = c;
  }
  if (!el) {
    el = static_cast<uint8_t*>(VecAppend(v, kHasClassSize, g_eng.a, why));
    if (!el) return false;
    *reinterpret_cast<uint16_t*>(el + o.hcPanel) = static_cast<uint16_t>(panel);
    *reinterpret_cast<uint16_t*>(el + o.hcClass) = static_cast<uint16_t>(cls);
  }
  *reinterpret_cast<int32_t*>(el + o.hcStatus) = on ? 1 : 0;
  return true;
}

// Writes everything wanted for `slot` (or every slot) onto the entity.
bool Push(Layout& l, void* e, int onlySlot) {
  bool all = true;
  for (auto& [slot, s] : l.slots) {
    if (onlySlot != -2 && slot != onlySlot) continue;
    std::string why;
    void* st = StateOf(e, slot, &why);
    if (!st) {
      l.lastError = why;
      all = false;
      continue;
    }
    for (const auto& [k, v] : s.texts) all &= WriteText(e, st, k, v, &l.lastError);
    for (const auto& [k, on] : s.classes) all &= WriteClass(e, st, k, on, &l.lastError);
  }
  l.changed = true;
  return all;
}

// The entity for `l`, created (and filled) when missing. nullptr while it cannot exist yet.
void* Ensure(Layout& l) {
  if (void* e = EntityOf(l)) return e;
  Resolve();
  if (!g_eng.ok) return nullptr;
  if (g_mapStartedAt >= 0 && g_now - g_mapStartedAt < 1.0) return nullptr;  // let the map settle
  if (!g_straysCleared) {
    RemoveStrays();
    g_straysCleared = true;
  }
  void* e = g_api->entity_create(g_api->self, kClass);
  if (!e) {
    l.lastError = "entity_create refused (see the log)";
    return nullptr;
  }
  if (!l.pathCopy) l.pathCopy = g_eng.a.strdup(l.path.c_str());
  *reinterpret_cast<const char**>(static_cast<uint8_t*>(e) + g_eng.o.strLayout) = l.pathCopy;  // CUtlSymbolLarge
  if (!g_api->entity_spawn(g_api->self, e)) {
    g_api->entity_remove(g_api->self, e);
    l.lastError = "entity_spawn refused (see the log)";
    return nullptr;
  }
  l.handle = g_api->entity_handle_of(g_api->self, e);
  ru_logf(g_api, RU_LOG_INFO, "%s for %s created", kClass, l.path.c_str());
  Push(l, e, -2);
  return e;
}

Layout* LayoutOf(int hud) {
  if (hud < 1 || hud > static_cast<int>(g_layouts.size())) return nullptr;
  return &g_layouts[static_cast<size_t>(hud - 1)];
}

// ---- readyup.hud.v1 --------------------------------------------------------------------------

int IfAvailable() {
  Resolve();
  return g_eng.ok ? 1 : 0;
}

int IfOpen(const char* path) {
  if (!path || !*path) return 0;
  for (size_t i = 0; i < g_layouts.size(); ++i)
    if (g_layouts[i].path == path) return static_cast<int>(i) + 1;
  Layout l;
  l.path = path;
  g_layouts.push_back(std::move(l));
  Ensure(g_layouts.back());
  return static_cast<int>(g_layouts.size());
}

int IfSetText(int hud, int slot, const char* panel, const char* var, const char* value) {
  Layout* l = LayoutOf(hud);
  if (!l || !panel || !var || slot < RU_HUD_ALL || slot > 63) return 0;
  const Key k{panel, var};
  std::string& cur = l->slots[slot].texts[k];
  const bool same = cur == (value ? value : "") && !cur.empty();
  cur = value ? value : "";
  if (same) return 1;
  void* e = Ensure(*l);
  if (!e) return 1;  // kept; pushed when the entity exists
  std::string why;
  void* st = StateOf(e, slot, &why);
  if (!st || !WriteText(e, st, k, cur, &why)) {
    l->lastError = why;
    return 0;
  }
  l->changed = true;
  return 1;
}

int IfSetClass(int hud, int slot, const char* panel, const char* cls, int on) {
  Layout* l = LayoutOf(hud);
  if (!l || !panel || !cls || slot < RU_HUD_ALL || slot > 63) return 0;
  const Key k{panel, cls};
  auto& m = l->slots[slot].classes;
  auto it = m.find(k);
  if (it != m.end() && it->second == (on != 0)) return 1;
  m[k] = on != 0;
  void* e = Ensure(*l);
  if (!e) return 1;
  std::string why;
  void* st = StateOf(e, slot, &why);
  if (!st || !WriteClass(e, st, k, on != 0, &why)) {
    l->lastError = why;
    return 0;
  }
  l->changed = true;
  return 1;
}

int IfClearSlot(int hud, int slot) {
  Layout* l = LayoutOf(hud);
  if (!l || slot < 0 || slot > 63) return 0;
  auto it = l->slots.find(slot);
  if (it == l->slots.end()) return 1;
  // Turn its classes off and empty its texts on the entity, then forget them.
  SlotState off;
  for (const auto& [k, on] : it->second.classes) off.classes[k] = false;
  for (const auto& [k, v] : it->second.texts) off.texts[k] = "";
  it->second = off;
  if (void* e = EntityOf(*l)) Push(*l, e, slot);
  l->slots.erase(slot);
  return 1;
}

const ru_hud_v1 g_iface = {sizeof(ru_hud_v1), &IfAvailable, &IfOpen, &IfSetText, &IfSetClass, &IfClearSlot};

// ---- clicks (proof of concept) ----------------------------------------------------------------
//
// A layout's Button panels become clickable while the server captures that player's cursor
// (CCSCustomHudLayoutState::m_bInputCaptureEnabled). Each click comes back as user message 390
// (CS_UM_CustomHudClicked: field 1 the layout's packed entity handle, field 2 the Button's id),
// which the engine hands to IServerGameClients::ClientSvcUserMessage (vtable slot 38 on CS2
// 1.41.9.x, verified through engine-surface.hud.json before it is patched). Never capture during a
// live round: the player can't aim or move while the cursor is out.

constexpr int kCustomHudClicked = 390;

struct Click {
  int slot;
  uint32_t handle;
  std::string button;
};
std::mutex g_clickMu;
std::vector<Click> g_clicks;  // filled by the hook, drained on the game thread
using SvcUserMessageFn = void (*)(void*, int, int, uint32_t, const void*);
void* g_origSvcUm = nullptr;
std::string g_clickHookStatus = "not installed";

// Minimal protobuf reader for CCSUsrMsg_CustomHudClicked.
bool ParseClick(const uint8_t* p, uint32_t n, uint32_t* handle, std::string* button) {
  uint32_t i = 0;
  auto varint = [&](uint64_t* out) {
    uint64_t v = 0;
    for (int shift = 0; i < n && shift < 64; shift += 7) {
      const uint8_t b = p[i++];
      v |= static_cast<uint64_t>(b & 0x7f) << shift;
      if (!(b & 0x80)) {
        *out = v;
        return true;
      }
    }
    return false;
  };
  while (i < n) {
    uint64_t key = 0, v = 0;
    if (!varint(&key)) return false;
    const uint32_t field = static_cast<uint32_t>(key >> 3), wire = static_cast<uint32_t>(key & 7);
    if (wire == 0) {
      if (!varint(&v)) return false;
      if (field == 1) *handle = static_cast<uint32_t>(v);
    } else if (wire == 2) {
      if (!varint(&v) || v > n - i) return false;
      if (field == 2) button->assign(reinterpret_cast<const char*>(p + i), static_cast<size_t>(v));
      i += static_cast<uint32_t>(v);
    } else {
      return false;  // nothing else is in this message
    }
  }
  return true;
}

void OnSvcUserMessage(void* self, int slot, int umType, uint32_t size, const void* buf) {
  if (umType == kCustomHudClicked && buf && size < 4096) {
    Click c{slot, kNoHandle, {}};
    if (ParseClick(static_cast<const uint8_t*>(buf), size, &c.handle, &c.button) && !c.button.empty()) {
      std::lock_guard<std::mutex> lk(g_clickMu);
      if (g_clicks.size() < 256) g_clicks.push_back(std::move(c));
    }
  }
  reinterpret_cast<SvcUserMessageFn>(g_origSvcUm)(self, slot, umType, size, buf);
}

// Valve's libserver.so, not Ready Up's (the core opens it through a relative path such as
// .../readyup/bin/linuxsteamrt64/../../../bin/linuxsteamrt64/libserver.so, so resolve it first).
int FindRealServer(struct dl_phdr_info* info, size_t, void* out) {
  const char* name = info->dlpi_name;
  if (!name || !std::strstr(name, "libserver.so")) return 0;
  char resolved[PATH_MAX];
  if (!realpath(name, resolved)) return 0;
  if (std::strstr(resolved, "/csgo/bin/linuxsteamrt64/libserver.so") && !std::strstr(resolved, "/readyup/")) {
    *static_cast<std::string*>(out) = name;
    return 1;
  }
  return 0;
}

void InstallClickHook() {
  if (g_origSvcUm || !RU_API_HAS(g_api, hook_vtable)) return;
  std::string path;
  dl_iterate_phdr(&FindRealServer, &path);
  void* lib = path.empty() ? nullptr : dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
  using CreateInterfaceFn = void* (*)(const char*, int*);
  auto ci = lib ? reinterpret_cast<CreateInterfaceFn>(dlsym(lib, "CreateInterface")) : nullptr;
  void* clients = ci ? ci("Source2GameClients001", nullptr) : nullptr;
  if (!clients) {
    g_clickHookStatus = "Source2GameClients001 not found";
    return;
  }
  void* orig = nullptr;
  if (g_api->hook_vtable(g_api->self, "ISource2GameClients::ClientSvcUserMessage", clients,
                         reinterpret_cast<void*>(&OnSvcUserMessage), &orig) && orig) {
    g_origSvcUm = orig;
    g_clickHookStatus = "ok";
    ru_logf(g_api, RU_LOG_INFO, "click hook installed (ClientSvcUserMessage)");
  } else {
    g_clickHookStatus = "refused (see the log)";
  }
}

int IfSetCapture(int hud, int slot, int on) {
  Layout* l = LayoutOf(hud);
  if (!l || slot < 0 || slot > 63 || g_eng.o.stCapture < 0) return 0;
  void* e = Ensure(*l);
  if (!e) return 0;
  std::string why;
  void* st = StateOf(e, slot, &why);
  if (!st) {
    l->lastError = why;
    return 0;
  }
  *(static_cast<uint8_t*>(st) + g_eng.o.stCapture) = on ? 1 : 0;
  l->changed = true;
  return 1;
}

// `ru hudkit click [slot]`: the CT / T proof of concept for one player, or every human.
const char* kClickLayout = "panorama/layout/custom_game/at_click.xml";
std::set<int> g_clickOpen;  // slots that have it open
std::set<int> g_bMode;      // slots whose open card waits for B (see ClickOpenB)

void ClickOpen(int slot) {
  const int h = IfOpen(kClickLayout);
  IfSetText(h, slot, "ac_title", "text", "Which side do you want?");
  IfSetText(h, slot, "ac_status", "text", "Click a side");
  IfSetClass(h, slot, "at_btn_ct", "picked", 0);
  IfSetClass(h, slot, "at_btn_t", "picked", 0);
  IfSetClass(h, slot, "ac_card", "on", 1);
  IfSetCapture(h, slot, 1);
  g_clickOpen.insert(slot);
  const double opened = g_now;
  g_later.emplace_back(g_now + 30.0, [slot, opened] {  // never leave a cursor captured
    if (g_clickOpen.count(slot)) {
      const int hh = IfOpen(kClickLayout);
      IfSetCapture(hh, slot, 0);
      IfSetClass(hh, slot, "ac_card", "on", 0);
      g_clickOpen.erase(slot);
      ru_logf(g_api, RU_LOG_INFO, "click POC: slot %d timed out after %.0f s", slot, g_now - opened);
    }
  });
}

void ClickClose(int slot) {
  g_bMode.erase(slot);
  const int h = IfOpen(kClickLayout);
  IfSetCapture(h, slot, 0);
  IfSetClass(h, slot, "ac_card", "on", 0);
  g_clickOpen.erase(slot);
}

void DrainClicks() {
  std::vector<Click> clicks;
  {
    std::lock_guard<std::mutex> lk(g_clickMu);
    clicks.swap(g_clicks);
  }
  for (const Click& c : clicks) {
    ru_logf(g_api, RU_LOG_INFO, "click: slot %d button \"%s\" (layout handle 0x%x)", c.slot, c.button.c_str(), c.handle);
    if (!g_clickOpen.count(c.slot)) continue;
    const int h = IfOpen(kClickLayout);
    if (c.button == "at_btn_close") {
      ClickClose(c.slot);
    } else if (c.button == "at_btn_ct" || c.button == "at_btn_t") {
      const bool ct = c.button == "at_btn_ct";
      IfSetClass(h, c.slot, "at_btn_ct", "picked", ct ? 1 : 0);
      IfSetClass(h, c.slot, "at_btn_t", "picked", ct ? 0 : 1);
      IfSetText(h, c.slot, "ac_status", "text", ct ? "You picked CT" : "You picked T");
      IfSetCapture(h, c.slot, 0);  // the choice is made: give the mouse back
      const int slot = c.slot;
      g_later.emplace_back(g_now + 1.5, [slot] { ClickClose(slot); });
    }
  }
}

// B-key mode: the player decides when the mouse is out. The card shows without a capture; the
// stock buy key B sends open_buymenu (the client does that by itself while buying is allowed), which
// captures the cursor, and close_buymenu gives it back. No binds, no chat.

void ClickOpenB(int slot) {
  const int h = IfOpen(kClickLayout);
  IfSetText(h, slot, "ac_title", "text", "Which side do you want?");
  IfSetText(h, slot, "ac_status", "text", "Press B for your mouse");
  IfSetClass(h, slot, "at_btn_ct", "picked", 0);
  IfSetClass(h, slot, "at_btn_t", "picked", 0);
  IfSetClass(h, slot, "ac_card", "on", 1);
  IfSetCapture(h, slot, 0);
  g_clickOpen.insert(slot);
  g_bMode.insert(slot);
}

int OnBuyMenu(void*, int slot, uint64_t, const char* cmd, int, const char* const*) {
  if (slot < 0 || !g_bMode.count(slot) || !g_clickOpen.count(slot)) return 0;  // never block the buy menu
  const bool open = cmd && std::strcmp(cmd, "open_buymenu") == 0;
  const int h = IfOpen(kClickLayout);
  IfSetCapture(h, slot, open ? 1 : 0);
  IfSetText(h, slot, "ac_status", "text", open ? "Click a side" : "Press B for your mouse");
  ru_logf(g_api, RU_LOG_INFO, "click POC: slot %d %s the buy menu", slot, open ? "opened" : "closed");
  return 0;
}

int CollectHuman(void* user, const ru_player* p) {
  if (!p->is_bot && p->connected && p->slot >= 0) static_cast<std::vector<int>*>(user)->push_back(p->slot);
  return 1;
}

// ---- ticks and events ------------------------------------------------------------------------

void OnTick(void*, const ru_tick_info* t) {
  g_now = t->now;
  if (!g_origSvcUm && g_eng.ok) InstallClickHook();
  DrainClicks();
  for (size_t i = 0; i < g_later.size();) {
    if (g_later[i].first <= g_now) {
      auto fn = std::move(g_later[i].second);
      g_later.erase(g_later.begin() + static_cast<long>(i));
      fn();
    } else {
      ++i;
    }
  }
  for (auto it = g_resendAt.begin(); it != g_resendAt.end();) {
    auto& times = it->second;
    bool due = false;
    for (size_t i = 0; i < times.size();) {
      if (times[i] <= g_now) {
        due = true;
        times.erase(times.begin() + static_cast<long>(i));
      } else {
        ++i;
      }
    }
    if (due) {
      for (auto& l : g_layouts) {
        if (void* e = EntityOf(l)) {
          Push(l, e, RU_HUD_ALL);
          Push(l, e, it->first);
        }
      }
    }
    it = times.empty() ? g_resendAt.erase(it) : std::next(it);
  }
  for (auto& l : g_layouts) {
    void* e = EntityOf(l);
    if (!e && !l.slots.empty()) e = Ensure(l);  // recreate after a map change
    if (e && l.changed) {
      g_api->entity_mark_changed(g_api->self, e);
      l.changed = false;
    }
  }
  std::lock_guard<std::mutex> lk(g_statusMu);
  g_selftestLine = g_eng.status + "; " + std::to_string(g_layouts.size()) + " layout(s)";
}

void OnEvent(void*, const ru_event* ev) {
  if (ev->type == RU_EVENT_MAP_START) {
    g_mapStartedAt = g_now;
    for (auto& l : g_layouts) l.handle = kNoHandle;  // the map took the entity with it
  } else if (ev->type == RU_EVENT_PLAYER_DISCONNECT && ev->slot >= 0) {
    g_clickOpen.erase(ev->slot);
  } else if (ev->type == RU_EVENT_PLAYER_CONNECT && ev->slot >= 0) {
    g_resendAt[ev->slot] = {g_now + 2.0, g_now + 5.0};
  }
}

// ---- `ru hudkit` (`ru hud` is the match plugin's center-HTML HUD) ----------------------------------------------------------------------------------

void Reply(const ru_command_ctx* ctx, const std::string& msg) {
  const std::string line = "Ready Up hud: " + msg;
  if (ctx->slot >= 0) g_api->chat_to_slot(g_api->self, ctx->slot, line.c_str());
  else ru_logf(g_api, RU_LOG_INFO, "%s", msg.c_str());
}

std::string Join(const ru_command_ctx* ctx, int from) {
  std::string s;
  for (int i = from; i < ctx->argc; ++i) s += (s.empty() ? "" : " ") + std::string(ctx->argv[i]);
  return s;
}

int ParseSlot(const char* s) { return std::strcmp(s, "all") == 0 ? RU_HUD_ALL : std::atoi(s); }

void Info(const ru_command_ctx* ctx) {
  Resolve();
  const Offsets& o = g_eng.o;
  Reply(ctx, "engine " + g_eng.status + Fmt("; vtable %p", g_eng.dvVtable));
  Reply(ctx, Fmt("offsets layout=%d states=%d global=%d panels=%d classes=%d vars=%d observable=%d", o.strLayout,
                 o.playerStates, o.globalState, o.panelIds, o.classNames, o.varNames, o.observable));
  Reply(ctx, Fmt("state slot=%d capture=%d classes=%d texts=%d; hasclass %d/%d/%d; dialogvar %d/%d/%d/%d", o.stSlot,
                 o.stCapture, o.stClasses, o.stTexts, o.hcPanel, o.hcClass, o.hcStatus, o.dvPanel, o.dvVar, o.dvValue,
                 o.dvSet));
  for (auto& l : g_layouts) {
    void* e = EntityOf(l);
    Reply(ctx, l.path + (e ? Fmt(" entity %p handle 0x%x", e, l.handle) : std::string(" (no entity)")) +
                   (l.lastError.empty() ? "" : "; last error: " + l.lastError));
    if (!e || !g_eng.ok) continue;
    auto* b = static_cast<uint8_t*>(e);
    auto vec = [&](const char* name, int off) {
      const RawVec* v = VecAtOff(e, off);
      Reply(ctx, Fmt("  %s size=%d alloc=%d grow=%d mem=%p", name, v->size, v->alloc, v->grow, v->mem));
    };
    vec("panelIds", o.panelIds);
    vec("classNames", o.classNames);
    vec("varNames", o.varNames);
    vec("playerStates", o.playerStates);
    Reply(ctx, "  global state bytes:\n" + HexDump(b + o.globalState, 64));
    const RawVec* ps = VecAtOff(e, o.playerStates);
    if (ps->size > 0 && ps->mem) Reply(ctx, "  player state[0..1] bytes:\n" + HexDump(ps->mem, 192));
  }
}

// `ru hudkit kit <state>`: the at_kit HUD in a sample state for everyone, to check the addon and its
// animations on a server without a match. States follow the design canvas (hud/README.md).
bool KitApply(const std::string& state) {
  const int h = IfOpen("panorama/layout/custom_game/at_kit.xml");
  auto cls = [&](const std::string& panel, const char* c, bool on) { IfSetClass(h, RU_HUD_ALL, panel.c_str(), c, on ? 1 : 0); };
  auto txt = [&](const std::string& panel, const std::string& v) { IfSetText(h, RU_HUD_ALL, panel.c_str(), "text", v.c_str()); };
  auto only = [&](const std::string& panel, std::initializer_list<const char*> all, const char* keep) {
    for (const char* c : all) cls(panel, c, keep && std::strcmp(c, keep) == 0);
  };
  const auto colours = {"c-live", "c-warn", "c-info", "c-ban", "c-muted", "c-acc"};
  // Sentence parts (prefix "at_p" on the card, "at_mp" in the centre): up to five, "" hides one.
  struct Part {
    const char* text;
    const char* kind;  // nullptr = text, "bold", "bold c-live", "cmd main", "cmd warn", "cmd side-ct", "cmd side-t", "cmd"
  };
  auto sentence = [&](const char* prefix, std::initializer_list<Part> parts) {
    int n = 0;
    for (const Part& p : parts) {
      const std::string id = std::string(prefix) + std::to_string(n++);
      txt(id + "_t", p.text);
      const std::string k = p.kind ? p.kind : "";
      cls(id, "hide", false);
      cls(id, "bold", k.find("bold") != std::string::npos);
      cls(id, "cmd", k.find("cmd") != std::string::npos);
      for (const char* x : {"main", "warn", "side-ct", "side-t", "c-live", "c-ban", "tight"}) cls(id, x, k.find(x) != std::string::npos);
    }
    for (; n < 5; ++n) cls(std::string(prefix) + std::to_string(n), "hide", true);
  };
  auto off = [&]() {
    cls("at_card", "on", false);
    cls("at_mid", "on", false);
    cls("at_end", "on", false);
  };
  auto card = [&](const char* kind, const std::string& text, const char* colour, const char* body, bool chat) {
    off();
    // The header rolls: the new text goes into the layer that is not showing, then they swap.
    static bool useB = false;
    useB = !useB;
    const char* cur = useB ? "b" : "a";
    const char* old = useB ? "a" : "b";
    txt(std::string("at_kind_") + cur, kind);
    txt(std::string("at_text_") + cur, text);
    for (const char* f : {"at_kind_", "at_text_"}) {
      cls(std::string(f) + cur, "cur", true);
      cls(std::string(f) + cur, "old", false);
      cls(std::string(f) + old, "cur", false);
      cls(std::string(f) + old, "old", true);
    }
    only("at_card", colours, colour);
    only("at_card", {"b-rules", "b-line", "b-dmg", "b-bar"}, body);
    cls("at_card", "chat", chat);
    cls("at_card", "nudge", false);
    cls("at_card", "on", true);
  };
  auto mid = [&](const char* kind, const std::string& text, const char* sub, const char* colour) {
    off();
    txt("at_mkind", kind ? kind : "");
    txt("at_mtext", text);
    txt("at_msub", sub ? sub : "");
    only("at_mid", colours, colour);
    cls("at_mid", "has-kind", kind != nullptr);
    cls("at_mid", "has-sub", sub != nullptr);
    for (const char* x : {"has-confirm", "has-parts", "has-cmds", "chat", "has-timer", "has-counter", "go"}) cls("at_mid", x, false);
  };
  if (state == "off") {
    off();
  } else if (state == "welcome") {
    card("Team A vs Team B", "Welcome, Sivert", nullptr, "b-rules", false);
    const char* rules[3][2] = {{"SERIES", "Best of 3"}, {"ROUNDS", "24 (first to 13)"}, {"OVERTIME", "Up to 3 (first to 4)"}};
    for (int i = 0; i < 3; ++i) {
      txt("at_r" + std::to_string(i) + "_k", rules[i][0]);
      txt("at_r" + std::to_string(i) + "_v", rules[i][1]);
    }
  } else if (state == "notready") {
    card("Warmup", "6 / 10 ready", nullptr, "b-line", true);
    sentence("at_p", {{"You are", nullptr}, {"not ready", "bold tight"}, {", type", nullptr}, {".r", "cmd main"}, {"to ready up", nullptr}});
    cls("at_card", "nudge", true);
  } else if (state == "ready") {
    card("Ready", "7 / 10 ready", "c-live", "b-line", true);
    sentence("at_p", {{"You are", nullptr}, {"ready", "bold c-live tight"}, {", type", nullptr}, {".ur", "cmd"}, {"to undo", nullptr}});
  } else if (state == "everyone") {
    mid("Everyone is ready", "Knife round (5)", nullptr, "c-live");
    cls("at_mid", "go", true);
    cls("at_mid", "on", true);
  } else if (state == "knife") {
    mid("You won the knife", "Which side do you want?", nullptr, "c-live");
    txt("at_mtimer", "20");
    sentence("at_mp", {{"Write", nullptr}, {".ct", "cmd side-ct"}, {"or", nullptr}, {".t", "cmd side-t"}});
    for (const char* x : {"has-timer", "has-parts", "chat", "on"}) cls("at_mid", x, true);
  } else if (state == "live") {
    mid("Match live", "Good luck, have fun", nullptr, "c-live");
    const char* cmds[2][2] = {{"Timeout", ".tac"}, {"Tech pause", ".p"}};
    for (int i = 0; i < 4; ++i) {
      const std::string id = "at_mk" + std::to_string(i);
      cls(id, "gone", i >= 2);
      if (i < 2) {
        txt(id + "_l", cmds[i][0]);
        txt(id + "_c", cmds[i][1]);
      }
    }
    for (const char* x : {"has-cmds", "chat", "on"}) cls("at_mid", x, true);
  } else if (state == "pause" || state == "pause2") {
    mid("Tech pause", "Team B used a tech pause", "Unpauses in (252)", "c-warn");
    txt("at_mc0_team", "Team A");
    txt("at_mc1_team", "Team B");
    txt("at_mc0_state", state == "pause2" ? "READY" : "WAITING");
    txt("at_mc1_state", "WAITING");
    cls("at_mc0", "ok", state == "pause2");
    cls("at_mc1", "ok", false);
    sentence("at_mp", {{"Write", nullptr}, {".up", "cmd warn"}, {"to unpause", nullptr}});
    txt("at_mcounter", "1 / 3 used");
    for (const char* x : {"has-confirm", "has-parts", "chat", "has-counter", "on"}) cls("at_mid", x, true);
  } else if (state == "missing") {
    mid(nullptr, "Team B is missing", "You win by forfeit if they are not back in (165)", "c-ban");
    cls("at_mid", "on", true);
  } else if (state == "damage") {
    card("Round 14", "160 damage", nullptr, "b-dmg", false);
    const int rows[5][5] = {{100, 3, 0, 0, 0}, {42, 2, 25, 1, 58}, {0, 0, 75, 1, 100}, {0, 0, 0, 0, 100}, {18, 1, 0, 0, 0}};
    for (int i = 0; i < 5; ++i) {
      const std::string r = "at_d" + std::to_string(i);
      txt(r + "_name", "opponent " + std::to_string(i + 1));
      txt(r + "_given", std::to_string(rows[i][0]) + " /" + std::to_string(rows[i][1]));
      txt(r + "_taken", std::to_string(rows[i][2]) + " /" + std::to_string(rows[i][3]));
      txt(r + "_hp", rows[i][4] ? std::to_string(rows[i][4]) : "DEAD");
      cls(r, "hit", rows[i][0] > 0);
      cls(r, "big", rows[i][0] >= 100);
      cls(r, "took", rows[i][2] > 0);
      cls(r, "dead", rows[i][4] == 0);
    }
  } else if (state == "end" || state == "endlost") {
    off();
    const bool lost = state == "endlost";
    txt("at_e_head", lost ? "You lost" : "You won!");
    txt("at_e_how", "By rounds");
    txt("at_e_ta", "Team A");
    txt("at_e_tb", "Team B");
    txt("at_e_sa", lost ? "9" : "13");
    txt("at_e_sb", lost ? "13" : "9");
    txt("at_e_where", "MAP 2 OF 3");
    txt("at_e_next", "Next map: Inferno (45)");
    txt("at_ea_name", "TEAM A");
    txt("at_eb_name", "TEAM B");
    const int a[5][7] = {{27, 14, 6, 2464, 112, 82, 54}, {21, 15, 4, 2002, 91, 77, 41}, {18, 16, 9, 1848, 84, 73, 38}, {15, 17, 5, 1562, 71, 68, 47}, {11, 18, 7, 1276, 58, 64, 29}};
    const int b[5][7] = {{22, 18, 3, 2112, 96, 68, 45}, {19, 19, 5, 1826, 83, 64, 52}, {16, 19, 6, 1628, 74, 59, 36}, {13, 20, 4, 1452, 66, 55, 31}, {10, 21, 6, 1144, 52, 50, 28}};
    auto rows = [&](char side, const int (*t)[7], int nameFrom) {
      for (int i = 0; i < 5; ++i) {
        const std::string r = std::string("at_e") + side + std::to_string(i);
        txt(r + "_n", side == 'a' && i == 1 ? "Sivert" : "player " + std::to_string(nameFrom + i));
        const char cols[] = "kdagrsh";
        for (int c = 0; c < 7; ++c) {
          std::string v = std::to_string(t[i][c]);
          if (c == 5 || c == 6) v += "%";
          txt(r + "_" + cols[c], v);
        }
        cls(r, "me", side == 'a' && i == 1);
        cls(r, "mvp", i == 0);
      }
    };
    rows('a', a, 1);
    rows('b', b, 6);
    only("at_end", {"won", "lost", "draw"}, lost ? "lost" : "won");
    cls("at_end", "how", true);
    cls("at_end", "on", true);
  } else if (state == "next") {
    card("Next map", "Inferno (14)", nullptr, nullptr, false);
  } else {
    return false;
  }
  return true;
}

// Every element leaves first; the new state enters 0.35 s later, so both animations are seen.
void Kit(const ru_command_ctx* ctx, const std::string& state) {
  static const std::set<std::string> kStates = {"welcome", "notready", "ready", "everyone", "knife", "live", "damage",
                                                "pause", "pause2", "missing", "end", "endlost", "next", "off"};
  if (!kStates.count(state)) {
    Reply(ctx, "kit states: welcome notready ready everyone knife live damage pause pause2 missing end endlost next off");
    return;
  }
  const int h = IfOpen("panorama/layout/custom_game/at_kit.xml");
  for (const char* el : {"at_card", "at_mid", "at_end"}) IfSetClass(h, RU_HUD_ALL, el, "on", 0);
  if (state != "off") g_later.emplace_back(g_now + 0.35, [state] { KitApply(state); });
  Reply(ctx, "at_kit: " + state);
}

void OnRu(void*, const ru_command_ctx* ctx) {
  if (ctx->slot >= 0 && !g_api->is_admin(g_api->self, ctx->steamid64)) {
    Reply(ctx, "admins only");
    return;
  }
  const std::string sub = ctx->argc > 2 ? ctx->argv[2] : "status";
  if (sub == "status") {
    Resolve();
    Reply(ctx, g_eng.status + "; " + std::to_string(g_layouts.size()) + " layout(s); click hook " + g_clickHookStatus);
    for (size_t i = 0; i < g_layouts.size(); ++i) {
      auto& l = g_layouts[i];
      Reply(ctx, Fmt("#%zu ", i + 1) + l.path + (EntityOf(l) ? " (live)" : " (no entity)") +
                     (l.lastError.empty() ? "" : "; last error: " + l.lastError));
    }
  } else if (sub == "info") {
    Info(ctx);
  } else if (sub == "hello") {  // ru hudkit hello <text>: the P0 layout, for everyone
    const int h = IfOpen("panorama/layout/custom_game/at_hello.xml");
    const std::string text = ctx->argc > 3 ? Join(ctx, 3) : "Hello from Auto Tournament";
    Reply(ctx, IfSetText(h, RU_HUD_ALL, "at_hello_text", "text", text.c_str()) ? "at_hello: " + text : "failed: " + LayoutOf(h)->lastError);
  } else if (sub == "click") {  // ru hudkit click [slot]: the CT / T click proof of concept
    std::vector<int> slots;
    if (ctx->argc > 3) slots.push_back(std::atoi(ctx->argv[3]));
    else g_api->for_each_player(g_api->self, &CollectHuman, &slots);
    for (int slot : slots) ClickOpen(slot);
    Reply(ctx, "click POC open for " + std::to_string(slots.size()) + " player(s); click hook " + g_clickHookStatus);
  } else if (sub == "bclick") {  // ru hudkit bclick [slot]: the same card, mouse on the B key
    std::vector<int> slots;
    if (ctx->argc > 3) slots.push_back(std::atoi(ctx->argv[3]));
    else g_api->for_each_player(g_api->self, &CollectHuman, &slots);
    for (int slot : slots) ClickOpenB(slot);
    Reply(ctx, "click POC (B key) open for " + std::to_string(slots.size()) + " player(s); click hook " + g_clickHookStatus);
  } else if (sub == "kit") {
    Kit(ctx, ctx->argc > 3 ? ctx->argv[3] : "");
  } else if (sub == "text" && ctx->argc >= 7) {  // ru hudkit text <hud#> <slot|all> <panel> <var> <value...>
    Reply(ctx, IfSetText(std::atoi(ctx->argv[3]), ParseSlot(ctx->argv[4]), ctx->argv[5], ctx->argv[6], Join(ctx, 7).c_str()) ? "ok" : "failed");
  } else if (sub == "class" && ctx->argc >= 8) {  // ru hudkit class <hud#> <slot|all> <panel> <class> <0|1>
    Reply(ctx, IfSetClass(std::atoi(ctx->argv[3]), ParseSlot(ctx->argv[4]), ctx->argv[5], ctx->argv[6], std::atoi(ctx->argv[7])) ? "ok" : "failed");
  } else {
    Reply(ctx, "ru hudkit [status|info|click [slot]|bclick [slot]|hello <text>|kit <state>|text <hud#> <slot|all> <panel> <var> <value>|class <hud#> <slot|all> <panel> <class> <0|1>]");
  }
}

void RunSelftest(ru_selftest_add_fn add, void* ctx) {
  std::lock_guard<std::mutex> lk(g_statusMu);
  add(ctx, "INFO", "hud", g_selftestLine.c_str());
}
const ru_selftest_iface_v1 g_selftestIface = {sizeof(ru_selftest_iface_v1), &RunSelftest};

}  // namespace
}  // namespace hud

using namespace hud;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 17u,  // needs API 1.17 (surface_rtti_vtable)
      "hud",
      HUD_VERSION,
      "Ready Up",
      "Custom in-game HUDs (custom_hud_layout) for the Auto Tournament HUD addon",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, surface_rtti_vtable)) return 1;
  g_api = api;
  g_eng = Engine{};
  g_layouts.clear();
  g_resendAt.clear();
  g_later.clear();
  g_bMode.clear();
  g_clickOpen.clear();
  g_straysCleared = false;
  g_mapStartedAt = -1;
  api->on_tick(api->self, &OnTick, nullptr);
  api->subscribe(api->self, RU_EVENT_ANY, &OnEvent, nullptr);
  api->register_ru_subcommand(api->self, "hudkit", &OnRu, nullptr);
  if (RU_API_HAS(api, filter_client_command)) {
    api->filter_client_command(api->self, "open_buymenu", &OnBuyMenu, nullptr);
    api->filter_client_command(api->self, "close_buymenu", &OnBuyMenu, nullptr);
  }
  api->provide_interface(api->self, RU_HUD_IFACE_NAME, RU_HUD_IFACE_VERSION, const_cast<ru_hud_v1*>(&g_iface));
  api->provide_interface(api->self, RU_SELFTEST_IFACE_PREFIX "hud", RU_SELFTEST_IFACE_VERSION,
                         const_cast<ru_selftest_iface_v1*>(&g_selftestIface));
  ru_logf(api, RU_LOG_INFO, "loaded " HUD_VERSION " (ru hudkit for status)");
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  // Remove our entities: a reload would otherwise leave them behind with their state.
  if (g_api) {
    for (auto& l : g_layouts)
      if (void* e = EntityOf(l)) g_api->entity_remove(g_api->self, e);
    ru_logf(g_api, RU_LOG_INFO, "unloaded");
  }
  g_api = nullptr;
}

}  // extern "C"
