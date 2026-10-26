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

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// ---- ticks and events ------------------------------------------------------------------------

void OnTick(void*, const ru_tick_info* t) {
  g_now = t->now;
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
  } else if (ev->type == RU_EVENT_PLAYER_CONNECT && ev->slot >= 0) {
    g_resendAt[ev->slot] = {g_now + 2.0, g_now + 5.0};
  }
}

// ---- `ru hud` ----------------------------------------------------------------------------------

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

// `ru hud kit <warmup|ready|knife|pause|damage|map|off>`: the at_kit card in a sample state for
// everyone, to check the addon and its animations on a server without a match.
void Kit(const ru_command_ctx* ctx, const std::string& state) {
  const int h = IfOpen("panorama/layout/custom_game/at_kit.xml");
  auto cls = [&](const char* panel, const char* c, bool on) { IfSetClass(h, RU_HUD_ALL, panel, c, on ? 1 : 0); };
  auto txt = [&](const char* panel, const std::string& v) { IfSetText(h, RU_HUD_ALL, panel, "text", v.c_str()); };
  const char* bodies[] = {"b-players", "b-dots", "b-dmg", "b-bar"};
  const char* colours[] = {"c-live", "c-warn", "c-info", "c-ban"};
  auto body = [&](const char* b) {
    for (const char* x : bodies) cls("at_card", x, b && std::strcmp(x, b) == 0);
  };
  auto colour = [&](const char* c) {
    for (const char* x : colours) cls("at_card", x, c && std::strcmp(x, c) == 0);
  };
  auto chip = [&](int i, const char* cmd, const char* lbl, const char* kind) {
    const std::string id = "at_c" + std::to_string(i);
    txt((id + "_a_cmd").c_str(), cmd ? cmd : "");
    txt((id + "_a_lbl").c_str(), lbl ? lbl : "");
    for (const char* k : {"main", "side-ct", "side-t", "gone"}) cls(id.c_str(), k, kind && std::strcmp(k, kind) == 0);
    if (!cmd) cls(id.c_str(), "gone", true);
  };
  auto head = [&](const char* kind, const char* text) {
    txt("at_kind_a", kind);
    txt("at_text_a", text);
    cls("at_kind_a", "cur", true);
    cls("at_text_a", "cur", true);
    cls("at_kind_b", "old", true);
    cls("at_text_b", "old", true);
  };
  cls("at_card", "nudge", false);
  cls("at_card", "go", false);
  if (state == "off") {
    cls("at_card", "on", false);
  } else if (state == "warmup") {
    head("Warmup", "6 / 10 ready");
    colour(nullptr);
    body("b-players");
    txt("at_team_a", "TEAM A");
    txt("at_team_b", "TEAM B");
    for (int i = 0; i < 5; ++i) {
      const std::string a = "at_pa" + std::to_string(i), b = "at_pb" + std::to_string(i);
      txt((a + "_name").c_str(), i == 0 ? "you" : "player " + std::to_string(i + 1));
      txt((b + "_name").c_str(), "player " + std::to_string(i + 6));
      cls(a.c_str(), "ok", i >= 1 && i <= 3);
      cls(a.c_str(), "me", i == 0);
      cls(b.c_str(), "ok", i <= 2);
    }
    cls("at_card", "cmds", true);
    chip(0, ".r", "ready", "main");
    chip(1, ".ur", "undo", nullptr);
    cls("at_card", "nudge", true);
    cls("at_card", "on", true);
  } else if (state == "ready") {
    head("Ready", "10 / 10 ready");
    colour("c-live");
    body("b-dots");
    for (int i = 0; i < 5; ++i) {
      cls(("at_da" + std::to_string(i)).c_str(), "ok", true);
      cls(("at_db" + std::to_string(i)).c_str(), "ok", true);
    }
    cls("at_card", "cmds", false);
    cls("at_card", "go", true);
    cls("at_card", "on", true);
  } else if (state == "knife") {
    head("Knife won", "Pick a side");
    colour("c-live");
    body(nullptr);
    cls("at_card", "cmds", true);
    chip(0, ".stay", "CT", "side-ct");
    chip(1, ".switch", "T", "side-t");
    cls("at_card", "on", true);
  } else if (state == "pause") {
    head("Tech pause", "Team B - 1 of 3");
    colour("c-warn");
    body(nullptr);
    cls("at_card", "cmds", true);
    chip(0, ".unpause", "resume", "main");
    chip(1, nullptr, nullptr, nullptr);
    cls("at_card", "on", true);
  } else if (state == "damage") {
    head("Round 14", "160 dmg given");
    colour(nullptr);
    body("b-dmg");
    const int rows[5][6] = {{100, 3, 0, 0, 0, 0}, {42, 2, 25, 1, 58, 0}, {0, 0, 75, 1, 100, 0}, {0, 0, 0, 0, 100, 0}, {18, 1, 0, 0, 0, 0}};
    for (int i = 0; i < 5; ++i) {
      const std::string r = "at_d" + std::to_string(i);
      txt((r + "_name").c_str(), "opponent " + std::to_string(i + 1));
      txt((r + "_given").c_str(), std::to_string(rows[i][0]) + " /" + std::to_string(rows[i][1]));
      txt((r + "_taken").c_str(), std::to_string(rows[i][2]) + " /" + std::to_string(rows[i][3]));
      txt((r + "_hp").c_str(), rows[i][4] ? std::to_string(rows[i][4]) : "DEAD");
      cls(r.c_str(), "hit", rows[i][0] > 0);
      cls(r.c_str(), "big", rows[i][0] >= 100);
      cls(r.c_str(), "took", rows[i][2] > 0);
      cls(r.c_str(), "dead", rows[i][4] == 0);
    }
    cls("at_card", "cmds", false);
    cls("at_card", "on", true);
  } else if (state == "map") {
    head("Map 2 of 3", "de_ancient");
    colour(nullptr);
    body("b-bar");
    for (int i = 0; i < 10; ++i) cls(("at_s" + std::to_string(i)).c_str(), "fill", i < 7);
    txt("at_pct", "70%");
    cls("at_card", "cmds", false);
    cls("at_card", "on", true);
  } else {
    Reply(ctx, "kit states: warmup ready knife pause damage map off");
    return;
  }
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
    Reply(ctx, g_eng.status + "; " + std::to_string(g_layouts.size()) + " layout(s)");
    for (size_t i = 0; i < g_layouts.size(); ++i) {
      auto& l = g_layouts[i];
      Reply(ctx, Fmt("#%zu ", i + 1) + l.path + (EntityOf(l) ? " (live)" : " (no entity)") +
                     (l.lastError.empty() ? "" : "; last error: " + l.lastError));
    }
  } else if (sub == "info") {
    Info(ctx);
  } else if (sub == "hello") {  // ru hud hello <text>: the P0 layout, for everyone
    const int h = IfOpen("panorama/layout/custom_game/at_hello.xml");
    const std::string text = ctx->argc > 3 ? Join(ctx, 3) : "Hello from Auto Tournament";
    Reply(ctx, IfSetText(h, RU_HUD_ALL, "at_hello_text", "text", text.c_str()) ? "at_hello: " + text : "failed: " + LayoutOf(h)->lastError);
  } else if (sub == "kit") {
    Kit(ctx, ctx->argc > 3 ? ctx->argv[3] : "");
  } else if (sub == "text" && ctx->argc >= 7) {  // ru hud text <hud#> <slot|all> <panel> <var> <value...>
    Reply(ctx, IfSetText(std::atoi(ctx->argv[3]), ParseSlot(ctx->argv[4]), ctx->argv[5], ctx->argv[6], Join(ctx, 7).c_str()) ? "ok" : "failed");
  } else if (sub == "class" && ctx->argc >= 8) {  // ru hud class <hud#> <slot|all> <panel> <class> <0|1>
    Reply(ctx, IfSetClass(std::atoi(ctx->argv[3]), ParseSlot(ctx->argv[4]), ctx->argv[5], ctx->argv[6], std::atoi(ctx->argv[7])) ? "ok" : "failed");
  } else {
    Reply(ctx, "ru hud [status|info|hello <text>|kit <state>|text <hud#> <slot|all> <panel> <var> <value>|class <hud#> <slot|all> <panel> <class> <0|1>]");
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
  g_straysCleared = false;
  g_mapStartedAt = -1;
  api->on_tick(api->self, &OnTick, nullptr);
  api->subscribe(api->self, RU_EVENT_ANY, &OnEvent, nullptr);
  api->register_ru_subcommand(api->self, "hud", &OnRu, nullptr);
  api->provide_interface(api->self, RU_HUD_IFACE_NAME, RU_HUD_IFACE_VERSION, const_cast<ru_hud_v1*>(&g_iface));
  api->provide_interface(api->self, RU_SELFTEST_IFACE_PREFIX "hud", RU_SELFTEST_IFACE_VERSION,
                         const_cast<ru_selftest_iface_v1*>(&g_selftestIface));
  ru_logf(api, RU_LOG_INFO, "loaded " HUD_VERSION " (ru hud for status)");
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
