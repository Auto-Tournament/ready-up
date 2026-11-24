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
#include "hud_addon.h"
#include "hud_screen.h"
#include "hud_vec.h"

#include "readyup/hud_iface.h"
#include "readyup/plugin_api.h"
#include "readyup/i18n.h"
#include "readyup/i18n_api.h"
#include "readyup/selftest_iface.h"

#include <dlfcn.h>
#include <link.h>
#include <algorithm>
#include <bitset>
#include <climits>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

extern const readyup::i18n::Locale kReadyUpLocales[];  // locales/ (readyup_embed_locales)
extern const size_t kReadyUpLocaleCount;

namespace hud {
namespace {

readyup::i18n::Catalog g_text;  // player-facing text (plugins/hud/locales)

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
// A round restart (round_prestart, then CleanUpMap) deletes map entities, ours included. Marking
// a dying entity changed puts it in the next snapshot after it is freed, and the engine dies
// ("WriteEnterPVS: GetEntServerClass failed"). So the entity is left alone for a moment; state
// set meanwhile is kept and pushed again afterwards.
double g_pauseUntil = -1;
bool g_repushAfterPause = false;
double g_beginMatchAt = -100;      // the last BeginMatch
bool g_restartAfterBeginMatch = false;  // a round restart began within its pause
bool Paused() { return g_now < g_pauseUntil; }
std::map<int, std::vector<double>> g_resendAt;  // slot -> times to push its state again
std::vector<std::pair<double, std::function<void()>>> g_later;  // work to run at a time (game thread)
std::mutex g_statusMu;                           // guards g_selftestLine for `ru selftest`
std::string g_selftestLine = "not resolved";

void* EntityOf(Layout& l) {
  if (l.handle == kNoHandle || Paused()) return nullptr;
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
  if (Paused()) return nullptr;
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

// ---- players who play without the custom HUD (`.hud off`, or the platform's player setting) ----
// Their slots get nothing from screen / menu / set_text / set_class: the callers send chat and the
// centre panel instead (ru_hud_v1 player_off, v1.4). Kept in <data_dir>/hud_off.txt, one SteamID64
// a line (Sivert, 2026-11-24: chat as the fallback, a choice per player).
std::set<uint64_t> g_hudOff;
std::bitset<64> g_slotOff;  // the slots of g_hudOff's players: every tick and on a change

std::string HudOffPath() {
  if (!RU_API_HAS(g_api, data_dir) || !g_api->data_dir) return {};
  const char* d = g_api->data_dir(g_api->self);
  return d && *d ? std::string(d) + "/hud_off.txt" : std::string();
}

void HudOffLoad() {
  g_hudOff.clear();
  const std::string p = HudOffPath();
  if (p.empty()) return;
  std::ifstream f(p);
  unsigned long long id = 0;
  while (f >> id)
    if (id) g_hudOff.insert(static_cast<uint64_t>(id));
}

void HudOffSave() {
  const std::string p = HudOffPath();
  if (p.empty()) return;
  std::ofstream f(p, std::ios::trunc);
  for (uint64_t id : g_hudOff) f << static_cast<unsigned long long>(id) << "\n";
}

int SlotOfSteamId(uint64_t id) {
  for (int slot = 0; slot <= 63; ++slot) {
    ru_player p{};
    p.struct_size = sizeof(p);
    if (g_api->get_player(g_api->self, slot, &p) && p.connected && !p.is_bot && p.steamid64 == id) return slot;
  }
  return -1;
}

void SlotOffRefresh() {
  std::bitset<64> next;
  if (!g_hudOff.empty()) {
    for (int slot = 0; slot <= 63; ++slot) {
      ru_player p{};
      p.struct_size = sizeof(p);
      if (g_api->get_player(g_api->self, slot, &p) && p.connected && !p.is_bot && p.steamid64 && g_hudOff.count(p.steamid64))
        next.set(static_cast<size_t>(slot));
    }
  }
  g_slotOff = next;
}

bool SlotOff(int slot) { return slot >= 0 && slot <= 63 && g_slotOff.test(static_cast<size_t>(slot)); }

int IfSetText(int hud, int slot, const char* panel, const char* var, const char* value) {
  if (SlotOff(slot)) return 1;  // HUD off for this player
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
  // HUD off for this player: nothing new is shown, but what was up can still be taken away (the
  // Hold E chip stayed on screen when the off write was blocked too, 2026-11-24).
  if (SlotOff(slot) && on) return 1;
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
  uint8_t* flag = static_cast<uint8_t*>(st) + g_eng.o.stCapture;
  if (*flag != (on ? 1 : 0)) {  // the entity is only re-sent when something changed
    *flag = on ? 1 : 0;
    l->changed = true;
  }
  return 1;
}

// ---- screens (readyup.hud.v1 `screen`, at_ui.xml) ---------------------------------------------
//
// One screen per player at a time, in the card or the centre panel. A new id leaves first and
// enters 0.3 s later so both animations play; the same id is rewritten in place. The cursor rules
// live here and nowhere else: mouse=on captures at once, mouse=b only while the buy menu is open
// (B), chips=1 never; a click, a new id, a hide or a disconnect gives the cursor back.

const char* kUiLayout = "panorama/layout/custom_game/at_ui.xml";
constexpr double kLeaveSeconds = 0.3;

struct ScreenSlot {
  std::string spec;          // last spec asked for
  Screen cur;                // what is (or is about to be) drawn
  bool shown = false;        // a panel is on
  bool clicked = false;      // a button of this id was clicked: no cursor again until the id changes
  bool dismissed = false;    // closed with a plain close button: hidden until the id changes
  std::set<std::pair<std::string, std::string>> utils;  // utility classes applied (element, class)
  bool captured = false;
  int sel = 0;               // ui_left: the chosen row (0..buttons-1, then the close row), W S move it
  std::string selId;         // the row that has `sel` now
  unsigned gen = 0;          // bumps on every new id, so a stale delayed enter does nothing
  ru_hud_click_fn onClick = nullptr;
  void* user = nullptr;
};
std::map<int, ScreenSlot> g_screens;
std::set<int> g_buyOpen;  // slots whose buy menu is open (open_buymenu .. close_buymenu)

std::string PanelOf(const Screen& s) { return s.where == "card" ? "ui_card" : s.where == "left" ? "ui_left" : "ui_mid"; }
const char* const kPanels[] = {"ui_card", "ui_mid", "ui_left"};

void UpdateCapture(int slot, bool force = false) {
  auto it = g_screens.find(slot);
  bool want = false;
  if (it != g_screens.end()) {
    const ScreenSlot& ss = it->second;
    const Mouse m = ss.cur.mouse;
    want = ss.shown && !ss.dismissed && !ss.clicked && !ss.cur.chips &&
           (m == Mouse::On || (m == Mouse::B && g_buyOpen.count(slot)));
  }
  const bool had = it != g_screens.end() && it->second.captured;
  if (want == had && !force) return;
  if (IfSetCapture(IfOpen(kUiLayout), slot, want ? 1 : 0) && it != g_screens.end()) it->second.captured = want;
}

// Once a second: write the wanted capture again. The flag lives on the entity, so a map change
// (new entity) or a player taking the slot would otherwise drop it.
void CaptureSweep() {
  static double next = 0;
  if (g_now < next) return;
  next = g_now + 1.0;
  for (auto& kv : g_screens) UpdateCapture(kv.first, true);
}

// ---- the menu panel (ui_left): W S choose a row, holding E picks it (HoldAction) ----------------
// Rows are the buttons that are a choice (a label and a command, not off), then the close row.
std::vector<std::string> NavRows(const ScreenSlot& ss) {
  std::vector<std::string> rows;
  for (size_t i = 0; i < ss.cur.buttons.size() && i < static_cast<size_t>(kMaxButtons); ++i) {
    const Button& b = ss.cur.buttons[i];
    if (!b.label.empty() && !b.cmd.empty() && b.state != "off") rows.push_back("ui_left_b" + std::to_string(i));
  }
  if (ss.cur.hasClose) rows.push_back("ui_left_close");
  return rows;
}

// `sel` on the chosen row only (clamped: the rows can change under it).
void NavDraw(int slot, ScreenSlot& ss) {
  const std::vector<std::string> rows = NavRows(ss);
  std::string want;
  if (!rows.empty()) {
    const int n = static_cast<int>(rows.size());
    ss.sel = ((ss.sel % n) + n) % n;
    want = rows[static_cast<size_t>(ss.sel)];
  }
  if (want == ss.selId) return;
  const int h = IfOpen(kUiLayout);
  if (!ss.selId.empty()) IfSetClass(h, slot, ss.selId.c_str(), "sel", 0);
  if (!want.empty()) IfSetClass(h, slot, want.c_str(), "sel", 1);
  ss.selId = want;
}

// A left panel always ends with a close row and says how to use it.
void LeftDefaults(int slot, Screen& s) {
  const std::string lang = readyup::i18n::LanguageOf(g_api, slot);
  if (!s.hasClose && !s.noCloseRow) {
    s.hasClose = true;
    s.closeLabel = g_text.Tr(lang, "hud.hold_close");
    s.closeCmd.clear();
  }
  s.hint = g_text.Tr(lang, "hud.menu_hint");
  // A captured cursor takes the keyboard too, and W S must reach the server: the cursor only
  // with the buy menu open (B), as on the cards.
  if (s.mouse == Mouse::On) s.mouse = Mouse::B;
}

void DrawScreen(int slot, ScreenSlot& ss) {
  const int h = IfOpen(kUiLayout);
  if (ss.cur.where == "chip") {
    // No panel: the hold chip above the logo carries it (HoldAction), so every panel is off.
    for (const char* panel : kPanels) IfSetClass(h, slot, panel, "on", 0);
    ss.shown = !ss.dismissed && !ss.cur.empty();
    UpdateCapture(slot);
    return;
  }
  const std::string p = PanelOf(ss.cur);
  const bool wasShown = ss.shown;
  if (!wasShown) {
    // A fresh entrance: drop the last exit so the panel snaps (unseen) to this screen's enter pose.
    IfSetClass(h, slot, p.c_str(), "leaving", 0);
    for (const auto& c : ExitClasses()) IfSetClass(h, slot, p.c_str(), c.c_str(), 0);
  }
  for (const Write& w : RenderScreen(ss.cur, p)) {
    if (w.isText) IfSetText(h, slot, w.panel.c_str(), w.name.c_str(), w.value.c_str());
    else IfSetClass(h, slot, w.panel.c_str(), w.name.c_str(), w.on ? 1 : 0);
  }
  // hold=1: the chip above the logo carries the first button, so the card hides its buttons.
  IfSetClass(h, slot, p.c_str(), "chiphold", ss.cur.hold && p != "ui_left" ? 1 : 0);
  // Utilities (cls= / cls.<part>=): off the ones this screen no longer has, on the new ones.
  std::set<std::pair<std::string, std::string>> want;
  for (auto& u : Utilities(ss.cur, p)) want.insert(std::move(u));
  for (const auto& u : ss.utils)
    if (!want.count(u)) IfSetClass(h, slot, u.first.c_str(), u.second.c_str(), 0);
  for (const auto& u : want) IfSetClass(h, slot, u.first.c_str(), u.second.c_str(), 1);
  ss.utils.swap(want);
  const bool show = !ss.dismissed && !ss.cur.empty();
  for (const char* other : kPanels)
    if (p != other) IfSetClass(h, slot, other, "on", 0);
  ss.shown = show;
  if (show && !wasShown) {
    // `on` a moment later, so the client has the enter pose before it animates out of it.
    const unsigned gen = ss.gen;
    const std::string panel = p;
    // The menu list gets longer: it is laid out for the first time when it opens, and with 0.06 s
    // the client took the enter pose and `on` in one go, so it appeared without sliding (2026-11-24).
    const double delay = p == "ui_left" ? 0.15 : 0.06;
    g_later.emplace_back(g_now + delay, [slot, gen, panel] {
      auto it = g_screens.find(slot);
      if (it != g_screens.end() && it->second.gen == gen && it->second.shown)
        IfSetClass(IfOpen(kUiLayout), slot, panel.c_str(), "on", 1);
    });
  } else {
    IfSetClass(h, slot, p.c_str(), "on", show ? 1 : 0);
  }
  UpdateCapture(slot);
  if (p == "ui_left") NavDraw(slot, ss);
}

std::string ButtonsKey(const Screen& s) {
  std::string k;
  for (const auto& b : s.buttons) k += b.label + '' + b.cmd + '';
  return k;
}

bool DemoActive(int slot);
void TourTick();
void DemoForget(int slot);
bool DemoDefer(int slot, const std::string& spec, ru_hud_click_fn onClick, void* user);

int IfScreen(int slot, const char* specIn, ru_hud_click_fn onClick, void* user) {
  if (slot < 0 || slot > 63) return 0;
  const std::string spec = SlotOff(slot) ? std::string() : std::string(specIn ? specIn : "");  // HUD off: hidden
  if (DemoDefer(slot, spec, onClick, user)) return 1;
  ScreenSlot& ss = g_screens[slot];
  ss.onClick = onClick;
  ss.user = user;
  if (spec == ss.spec) return 1;
  ss.spec = spec;
  Screen next = ParseScreen(spec);
  if (next.where == "left") LeftDefaults(slot, next);
  const int h = IfOpen(kUiLayout);
  const bool sameId = !next.empty() && !ss.cur.empty() && next.id == ss.cur.id && next.where == ss.cur.where;
  if (sameId) {
    // New buttons (Ready became Unready) can be clicked again.
    if (ButtonsKey(next) != ButtonsKey(ss.cur)) ss.clicked = false;
    const bool bump = next.counter != ss.cur.counter || (next.mini && next.kind != ss.cur.kind) ||
                      next.teamReady[0] != ss.cur.teamReady[0] || next.teamReady[1] != ss.cur.teamReady[1];
    ss.cur = std::move(next);
    if (!ss.dismissed) DrawScreen(slot, ss);
    if (bump && !ss.dismissed) {
      // The count changed: pop it (CS2's money change). Keyframes on transform never run in a
      // custom HUD, so `bump` jumps to 1.6x with no transition and dropping it eases back.
      const std::string target = PanelOf(ss.cur) + (ss.cur.mini ? "_kind" : "_counter");
      IfSetClass(h, slot, target.c_str(), "bump", 1);
      const unsigned gen = ss.gen;
      g_later.emplace_back(g_now + 0.12, [slot, gen, target] {
        auto it = g_screens.find(slot);
        if (it != g_screens.end() && it->second.gen == gen) IfSetClass(IfOpen(kUiLayout), slot, target.c_str(), "bump", 0);
      });
    }
    else UpdateCapture(slot);
    return 1;
  }
  // A new screen (or none): the old one leaves and gives the cursor back.
  const bool wasShown = ss.shown;
  if (wasShown) {
    // Leave with the screen's exit: the exit pose plus leaving, then drop on.
    const std::string old = PanelOf(ss.cur);
    IfSetClass(h, slot, old.c_str(), ExitOf(ss.cur, old).c_str(), 1);
    IfSetClass(h, slot, old.c_str(), "leaving", 1);
    IfSetClass(h, slot, old.c_str(), "on", 0);
  }
  ss.shown = false;
  ss.clicked = false;
  ss.dismissed = false;
  if (!ss.selId.empty()) IfSetClass(h, slot, ss.selId.c_str(), "sel", 0);
  ss.sel = 0;
  ss.selId.clear();
  ss.cur = std::move(next);
  UpdateCapture(slot);
  if (ss.cur.empty()) return 1;
  const unsigned gen = ++ss.gen;
  if (!wasShown) {
    DrawScreen(slot, ss);
    return 1;
  }
  g_later.emplace_back(g_now + kLeaveSeconds, [slot, gen] {
    auto it = g_screens.find(slot);
    if (it != g_screens.end() && it->second.gen == gen) DrawScreen(slot, it->second);
  });
  return 1;
}

bool OpenProviderMenu(int slot, const std::string& owner);

// A click on one of our at_ui buttons. True when it was one (handled or ignored).
bool ScreenClick(int slot, const std::string& button) {
  std::string panel;
  int index = 0;
  if (!ParseButtonId(button, &panel, &index)) return false;
  auto it = g_screens.find(slot);
  if (it == g_screens.end() || !it->second.shown || PanelOf(it->second.cur) != panel) return true;
  ScreenSlot& ss = it->second;
  std::string cmd;
  if (index < 0) {
    cmd = ss.cur.closeCmd;
    if (cmd.empty()) {  // a plain close: hide until the caller shows another id
      ss.dismissed = true;
      IfSetClass(IfOpen(kUiLayout), slot, panel.c_str(), "on", 0);
      ss.shown = false;
    }
  } else {
    if (static_cast<size_t>(index) >= ss.cur.buttons.size()) return true;
    const Button& b = ss.cur.buttons[static_cast<size_t>(index)];
    if (b.state == "off" || b.cmd.empty()) return true;  // a label, not a choice
    cmd = b.cmd;
    IfSetClass(IfOpen(kUiLayout), slot, button.c_str(), "picked", 1);  // feedback until the caller redraws
  }
  ss.clicked = true;
  UpdateCapture(slot);
  if (cmd.empty()) return true;
  // `#open <owner>`: another plugin's menu (the movement menu's Maps button opens the map vote's).
  if (cmd.rfind("#open ", 0) == 0 && OpenProviderMenu(slot, cmd.substr(6))) return true;
  if (ss.onClick) ss.onClick(ss.user, slot, cmd.c_str());
  else if (RU_API_HAS(g_api, run_chat_command)) g_api->run_chat_command(g_api->self, slot, cmd.c_str());  // as if typed
  return true;
}

void MenuForget(int slot);
int IfMenu(int slot, const char* owner, const char* spec, ru_hud_click_fn onClick, void* user);
int IfRegisterMenu(const char* owner, int priority, ru_hud_menu_active_fn active, ru_hud_menu_open_fn open, void* user);

void ScreenForget(int slot) {
  g_screens.erase(slot);
  DemoForget(slot);
  MenuForget(slot);
  g_buyOpen.erase(slot);
  if (!g_layouts.empty()) {
    for (size_t i = 0; i < g_layouts.size(); ++i)
      if (g_layouts[i].path == kUiLayout) IfClearSlot(static_cast<int>(i) + 1, slot);
  }
}

// Turns the custom HUD off (or back on) for one player and saves the choice. Off: whatever is shown
// goes away now; on: the callers' next screen / card draws again.
bool SetPlayerOff(uint64_t id, bool off) {
  if (!id || (g_hudOff.count(id) != 0) == off) return false;
  const int slot = SlotOfSteamId(id);
  if (off && slot >= 0) {
    IfScreen(slot, "", nullptr, nullptr);
    MenuForget(slot);
  }
  if (off)
    g_hudOff.insert(id);
  else
    g_hudOff.erase(id);
  HudOffSave();
  SlotOffRefresh();
  if (!off && slot >= 0) ScreenForget(slot);
  return true;
}

int IfPlayerOff(uint64_t id) { return id && g_hudOff.count(id) ? 1 : 0; }
int IfSetPlayerOff(uint64_t id, int off) {
  SetPlayerOff(id, off != 0);
  return id ? 1 : 0;
}

const ru_hud_v1 g_iface = {sizeof(ru_hud_v1), &IfAvailable, &IfOpen, &IfSetText, &IfSetClass, &IfClearSlot, &IfScreen, &IfMenu, &IfRegisterMenu,
                            &IfSetCapture, &IfPlayerOff, &IfSetPlayerOff};

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
    if (ScreenClick(c.slot, c.button)) continue;
    // at_kit's leaderboard Close: match.so's `.lb` closes it and gives the cursor back.
    if (c.button == "at_e_close") {
      if (RU_API_HAS(g_api, run_chat_command)) g_api->run_chat_command(g_api->self, c.slot, ".lb");
      continue;
    }
    if (!g_clickOpen.count(c.slot)) continue;
    const int h = IfOpen(kClickLayout);
    if (c.button == "at_btn_close") {
      continue;  // no way out without picking a side (the button goes in the next layout)
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
  if (slot < 0) return 0;
  const bool open = cmd && std::strcmp(cmd, "open_buymenu") == 0;
  if (open) g_buyOpen.insert(slot);
  else g_buyOpen.erase(slot);
  UpdateCapture(slot);  // a mouse=b screen follows the buy menu
  if (!g_bMode.count(slot) || !g_clickOpen.count(slot)) return 0;  // never block the buy menu
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

// `ru hudkit buttons [slot] [seconds]`: which keys reach the server. Reads the pawn's
// CPlayer_MovementServices::m_nButtons (CInButtonState: m_pButtonStates[0] = held buttons) every
// tick and logs each change, so a tester can press Tab / E / R / F / Shift and see what arrives.
struct ButtonProbe {
  int slot = -1;
  double until = 0;
  uint64_t last = ~0ull;
} g_probe;

const char* ButtonName(int bit) {
  static const char* names[] = {"attack", "jump", "duck", "forward", "back", "use", "cancel", "left", "right",
                                "moveleft", "moveright", "attack2", "run", "reload", "alt1", "alt2", "score",
                                "speed", "walk", "zoom", "weapon1", "weapon2", "bullrush", "grenade1", "grenade2",
                                "lookatweapon"};
  return bit < static_cast<int>(sizeof(names) / sizeof(names[0])) ? names[bit] : "?";
}

void ProbeTick() {
  if (g_probe.slot < 0) return;
  if (g_now > g_probe.until) {
    ru_logf(g_api, RU_LOG_INFO, "buttons: probe for slot %d done", g_probe.slot);
    g_probe.slot = -1;
    return;
  }
  static int ctrlPawn = -2, pawnMove = -2, moveButtons = -2, btnStates = -2;
  if (ctrlPawn == -2) {
    ctrlPawn = g_api->schema_offset(g_api->self, "CCSPlayerController", "m_hPlayerPawn");
    pawnMove = g_api->schema_offset(g_api->self, "CBasePlayerPawn", "m_pMovementServices");
    moveButtons = g_api->schema_offset(g_api->self, "CPlayer_MovementServices", "m_nButtons");
    btnStates = g_api->schema_offset(g_api->self, "CInButtonState", "m_pButtonStates");
    ru_logf(g_api, RU_LOG_INFO, "buttons: offsets pawn=%d move=%d buttons=%d states=%d", ctrlPawn, pawnMove, moveButtons, btnStates);
  }
  if (ctrlPawn < 0 || pawnMove < 0 || moveButtons < 0 || btnStates < 0) return;
  void* ctrl = g_api->entity_by_index(g_api->self, g_probe.slot + 1);
  if (!ctrl) return;
  uint32_t h = 0;
  std::memcpy(&h, static_cast<uint8_t*>(ctrl) + ctrlPawn, sizeof(h));
  void* pawn = g_api->entity_from_handle(g_api->self, h);
  if (!pawn) return;
  void* move = nullptr;
  std::memcpy(&move, static_cast<uint8_t*>(pawn) + pawnMove, sizeof(move));
  if (!move) return;
  uint64_t held = 0;
  std::memcpy(&held, static_cast<uint8_t*>(move) + moveButtons + btnStates, sizeof(held));
  if (held == g_probe.last) return;
  std::string on;
  for (int b = 0; b < 64; ++b)
    if (held & (1ull << b)) on += std::string(on.empty() ? "" : " ") + ButtonName(b) + "(" + std::to_string(b) + ")";
  ru_logf(g_api, RU_LOG_INFO, "buttons: slot %d held 0x%llx [%s]", g_probe.slot, static_cast<unsigned long long>(held), on.c_str());
  g_probe.last = held;
}

void HoldTick();  // hold Use: defined with the menu providers below

// ---- rank test ---------------------------------------------------------------------------------
// `ru hudkit rank <slot|all> <type> <value> [wins]`: writes CCSPlayerController's competitive rank
// (m_iCompetitiveRankType, m_iCompetitiveRanking, m_iCompetitiveWins) and keeps writing it every
// tick, to see what the Tab scoreboard shows for each type (the platform rating as a CS Rating,
// 2026-11-20). `ru hudkit rank off` stops (the values stay until the game overwrites them).
struct RankTest {
  int type = 0, value = 0, wins = 0;
};
std::map<int, RankTest> g_rankTest;  // slot (RU_HUD_ALL = everyone) -> what to write
double g_rankNext = 0;

void RankWrite(int slot, const RankTest& r) {
  static int offType = -2, offRank = -2, offWins = -2;
  if (offType == -2) {
    offType = g_api->schema_offset(g_api->self, "CCSPlayerController", "m_iCompetitiveRankType");
    offRank = g_api->schema_offset(g_api->self, "CCSPlayerController", "m_iCompetitiveRanking");
    offWins = g_api->schema_offset(g_api->self, "CCSPlayerController", "m_iCompetitiveWins");
    ru_logf(g_api, RU_LOG_INFO, "rank test: offsets type=%d ranking=%d wins=%d", offType, offRank, offWins);
  }
  if (offType < 0 || offRank < 0) return;
  void* ctrl = g_api->entity_by_index(g_api->self, slot + 1);
  if (!ctrl) return;
  auto* b = static_cast<uint8_t*>(ctrl);
  const int8_t type = static_cast<int8_t>(r.type);
  const int32_t value = r.value, wins = r.wins;
  if (*reinterpret_cast<int8_t*>(b + offType) == type && *reinterpret_cast<int32_t*>(b + offRank) == value &&
      (offWins < 0 || *reinterpret_cast<int32_t*>(b + offWins) == wins))
    return;
  std::memcpy(b + offType, &type, sizeof(type));
  std::memcpy(b + offRank, &value, sizeof(value));
  if (offWins >= 0) std::memcpy(b + offWins, &wins, sizeof(wins));
  g_api->entity_mark_changed(g_api->self, ctrl);
}

void RankTick() {
  // Every tick: CS2 writes its own values back between our writes, and once a second flickered
  // (2026-11-20). RankWrite only marks the controller changed when a value differs.
  if (g_rankTest.empty()) return;
  for (int slot = 0; slot <= 63; ++slot) {
    auto it = g_rankTest.find(slot);
    if (it == g_rankTest.end()) it = g_rankTest.find(RU_HUD_ALL);
    if (it == g_rankTest.end()) continue;
    ru_player p{};
    p.struct_size = sizeof(p);
    if (!g_api->get_player(g_api->self, slot, &p) || !p.connected) continue;
    RankWrite(slot, it->second);
  }
}

// ---- ticks and events ------------------------------------------------------------------------

// HUD scale (at_ui's wraps, scale-70 .. scale-100): `scale=` in the plugin's cfg, `ru hudkit scale` (default 75).
int g_scale = 75;
void ApplyScale() {
  const int h = IfOpen(kUiLayout);
  for (int pct = 70; pct <= 100; pct += 5) {
    const std::string c = "scale-" + std::to_string(pct);
    for (const char* wrap : {"ui_cardwrap", "ui_midwrap", "ui_leftwrap"}) IfSetClass(h, RU_HUD_ALL, wrap, c.c_str(), pct == g_scale ? 1 : 0);
  }
}
int ParseScale(const std::string& s) {
  const int v = std::atoi(s.c_str());
  if (v < 70 || v > 100) return -1;
  return (v + 2) / 5 * 5;  // the nearest step of 5
}

// "HUD out of date" (#ui_ver in at_ui, hud_addon.h): `check` and `v-<kAddonVersion>` for everyone,
// the text in each player's language. Once a second, so a cleared slot (ScreenForget) or a new
// language gets its text back. An addon that knows the version hides the notice itself. While
// another plugin puts `hold` on #ui_ver (match.so: knife round to the end of the map), `check` is
// off: nobody needs telling mid-match.
double g_verNextAt = 0;
void AddonVersionTick() {
  if (g_now < g_verNextAt) return;
  g_verNextAt = g_now + 1.0;
  const int h = IfOpen(kUiLayout);
  bool hold = false;
  if (Layout* l = LayoutOf(h)) {
    const auto& cls = l->slots[RU_HUD_ALL].classes;
    const auto it = cls.find(Key{"ui_ver", "hold"});
    hold = it != cls.end() && it->second;
  }
  IfSetClass(h, RU_HUD_ALL, "ui_ver", "check", hold ? 0 : 1);
  const std::string ver = "v-" + std::to_string(kAddonVersion);
  IfSetClass(h, RU_HUD_ALL, "ui_ver", ver.c_str(), 1);
  IfSetText(h, RU_HUD_ALL, "ui_ver_t", "text", g_text.Tr("en", "hud.outdated").c_str());
  for (int slot = 0; slot <= 63; ++slot) {
    ru_player p{};
    p.struct_size = sizeof(p);
    if (!g_api->get_player(g_api->self, slot, &p) || !p.connected || p.is_bot || !p.steamid64) continue;
    const std::string text = g_text.Tr(readyup::i18n::LanguageOf(g_api, slot), "hud.outdated");
    IfSetText(h, slot, "ui_ver_t", "text", text.c_str());
  }
}

void OnTick(void*, const ru_tick_info* t) {
  g_now = t->now;
  SlotOffRefresh();
  if (!g_origSvcUm && g_eng.ok) InstallClickHook();
  if (g_layouts.empty() || std::none_of(g_layouts.begin(), g_layouts.end(), [](const Layout& l) { return l.path == kUiLayout; }))
    IfOpen(kUiLayout);
  AddonVersionTick();
  DrainClicks();
  CaptureSweep();
  ProbeTick();
  HoldTick();
  RankTick();
  TourTick();
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
  if (g_repushAfterPause && !Paused()) {
    g_repushAfterPause = false;
    for (auto& l : g_layouts)
      if (void* e = EntityOf(l)) Push(l, e, -2);  // it survived the restart: catch up
  }
  for (auto& l : g_layouts) {
    void* e = EntityOf(l);
    // Recreate after a map change. at_ui always exists: an entity created after a player's first
    // full update, followed by CS2's BeginMatch round restart, crashes the server
    // ("WriteEnterPVS: GetEntServerClass failed").
    if (!e && (!l.slots.empty() || l.path == kUiLayout)) e = Ensure(l);
    if (e && l.changed) {
      g_api->entity_mark_changed(g_api->self, e);
      l.changed = false;
    }
  }
  std::lock_guard<std::mutex> lk(g_statusMu);
  g_selftestLine = g_eng.status + "; " + std::to_string(g_layouts.size()) + " layout(s)";
}

void OnRoundPrestart(void*, const char*, const ru_game_event*) {
  g_pauseUntil = std::max(g_pauseUntil, g_now + 1.0);
  g_repushAfterPause = true;
  if (g_now - g_beginMatchAt < 4.0) g_restartAfterBeginMatch = true;
}

// The map cleanup is done by round_poststart: the pause ends a few frames later instead of a
// full second, so cards sent at round_start (the damage report) show at once (2026-10-27: ~1 s
// late). A longer pause (BeginMatch's) is left alone.
void OnRoundPoststart(void*, const char*, const ru_game_event*) {
  if (g_pauseUntil > g_now && g_pauseUntil <= g_now + 1.0) g_pauseUntil = g_now + 0.1;
  // BeginMatch's pause also ends once the restart after it is done (the first player on a team
  // waited the full 4 s for their ready card, 2026-11-06). With no restart after it, it runs out.
  if (g_restartAfterBeginMatch && g_pauseUntil > g_now + 0.1) g_pauseUntil = g_now + 0.1;
  g_restartAfterBeginMatch = false;
}

// A player's first full update (entering the game) clears their slot's is-set flags: push their
// state again right after it, however long their map load took. The resends at connect alone came
// before the full update for a slow loader, whose card then waited for the next change.
void OnPlayerActivate(void*, const char*, const ru_game_event* ev) {
  const int slot = g_api->ev_get_player_slot(g_api->self, ev, "userid");
  if (slot >= 0 && slot <= 63) g_resendAt[slot] = {g_now + 0.2, g_now + 1.0, g_now + 3.0};
}

// Long enough for match.so's fallback restart (3 s, modes.h ModesObserveBeginMatch), whose
// round_prestart pauses again.
void OnBeginMatch(void*, const char*, const ru_game_event*) {
  g_pauseUntil = g_now + 4.0;
  g_repushAfterPause = true;
  g_beginMatchAt = g_now;
  g_restartAfterBeginMatch = false;
}

// The AT ram as both teams' logo: CS2 draws the team logo in the circle between health and ammo.
// The addon ships it at panorama/images/tournaments/teams/atram (hud/build.ps1; CS2 cuts a logo name at 7 characters, "autotournament" looked for "autotou"), a path CS2
// has never loaded, so it shows every time; replacing icons/ui/ct_logo_1c only worked right after a
// download, because CS2 keeps the emblem it loaded at startup (2026-11-24, found by setting
// mp_teamlogo_1 navi). A match config's own mp_teamlogo_* goes out after this and wins.
void SetTeamLogos() {
  if (!g_api || !g_api->server_command) return;
  g_api->server_command(g_api->self, "mp_teamlogo_1 atram");
  g_api->server_command(g_api->self, "mp_teamlogo_2 atram");
}

void OnEvent(void*, const ru_event* ev) {
  if (ev->type == RU_EVENT_MAP_START) {
    g_mapStartedAt = g_now;
    SetTeamLogos();
    for (auto& l : g_layouts) l.handle = kNoHandle;  // the map took the entity with it
    for (auto& [slot, ss] : g_screens) ss.captured = false;
    g_buyOpen.clear();
  } else if (ev->type == RU_EVENT_PLAYER_DISCONNECT && ev->slot >= 0) {
    g_clickOpen.erase(ev->slot);
    ScreenForget(ev->slot);
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
    mid("Everyone is ready", "Knife round in 5s", nullptr, "c-live");
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
    mid("Tech pause", "Team B used a tech pause", "Unpauses in 252s", "c-warn");
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
    mid(nullptr, "Team B is missing", "You win by forfeit if they are not back in 165s", "c-ban");
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
  } else if (state == "next") {
    card("Next map", "Inferno in 14s", nullptr, nullptr, false);
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

// `ru hudkit demo <screen> [slot]` and `ru hudkit tour`: every screen of the design canvas (page
// In game (v3)) with sample data, in match order, to review the addon without a match. They copy
// what the match plugin sends today. Buttons that lead to another screen carry "#demo <name>"; any
// other click is echoed in chat.
const std::vector<std::pair<std::string, std::string>>& DemoScreens() {
  static const std::vector<std::pair<std::string, std::string>> k = {
      {"welcome", "id=warm\nwhere=card\nkind=Welcome, Simpert\ntitle=You're not ready\nbtn=Ready|green|pulse|#demo ready\nbw=l\ncounter=0 / 1\nhint=Press B to click\nmouse=b"},
      {"notready", "id=warm\nwhere=card\nkind=Scrim\ntitle=You're not ready\nteams=CT|3|5|T|3|5\nbtn=Ready|green|pulse|#demo ready\nbw=l\ncounter=6 / 10\nhint=Press B to click\nmouse=b"},
      {"ready", "id=ready\nwhere=card\ncolor=live\nkind=Ready \xC2\xB7 7 / 10\nsize=mini\ntitle=You're ready\nteams=CT|4|5|T|3|5\nbtn=Unready|ghost||#demo notready\nbw=s\nmouse=b"},
      {"everyone", "id=countdown\ncolor=live\nkind=Everyone is ready\ntitle=Knife round in 5s"},
      {"knife", "id=knife\ncolor=live\nkind=Knife round\ntitle=Winners pick a side"},
      {"pick", "id=pick\ncolor=live\nkind=You won the knife\ntitle=Which side do you want?\ntimer=20\nbtn=CT|ct||#demo pickedct\nbtn=T|t||#demo pickedt\nmouse=on"},
      {"pickurgent", "id=pick\ncolor=live\nkind=You won the knife\ntitle=Which side do you want?\ntimer=4\nbtn=CT|ct||#demo pickedct\nbtn=T|t||#demo pickedt\nmouse=on"},
      {"pickedct", "id=pick\ncolor=live\nkind=You won the knife\ntitle=Simpert picked CT\nsub=Match starts in 5s\nbtn=CT|ct|picked|\nbtn=T|t|off|"},
      {"lost", "id=lost\nkind=You lost the knife\ntitle=Team A is picking a side\ntimer=20"},
      {"golive", "id=golive\ncolor=live\nkind=Live\ntitle=Match is live\nsub=Team A (CT) vs Team B (T)\ncmd=Type|.menu|in chat to open the match menu and view commands"},
      {"pause", "id=pause\ncolor=warn\nkind=Tech pause\ntitle=Team B paused\nsub=Auto unpause in 252s\nconfirm=Team A|0|Team B|0\nbtn=Unpause|warn|pulse|#demo waiting\nbw=l\nmouse=on"},
      {"waiting", "id=pause\ncolor=warn\nkind=Tech pause\ntitle=Team B paused\nsub=Auto unpause in 245s\nconfirm=Team A|1|Team B|0\nbtn=Waiting for Team B|ghost|off|\nbw=xl"},
      {"timeout", "id=tac\nkind=Timeout\ntitle=Team A timeout"},
      {"adminpause", "id=adminpause\ncolor=warn\nkind=Admin pause\ntitle=Paused by an admin"},
      {"vote", "id=gg\ncolor=ban\nkind=Give up vote\ntitle=Give up this map?\ntimer=15\ncounter=2 / 5 yes\nbw=s\nbtn=Yes|danger||#demo voted\nbtn=No|ghost||#demo off\nmouse=on"},
      {"voted", "id=gg\ncolor=ban\nkind=Give up vote\ntitle=Give up this map?\ntimer=11\ncounter=3 / 5 yes\nbw=s\nbtn=Yes|danger|picked|\nbtn=No|ghost|off|"},
      {"chips", "id=gg\ncolor=ban\nkind=Give up vote\ntitle=Give up this map?\ntimer=15\ncounter=2 / 5 yes\nbw=s\nbtn=Yes|danger||.gg\nbtn=No|ghost||\nchips=1\nmouse=on"},
      {"replay", "id=stop\nkind=Replay this round?\ntitle=Team A wants to replay it\ntimer=20\nbw=m\nbtn=Accept|green||#demo off\nbtn=Decline|ghost||#demo off\nmouse=on"},
      {"missing", "id=missing\ncolor=ban\ntitle=Team B is missing\nsub=Forfeit in 165s"},
      {"admincall", "id=admincall\nwhere=card\ncolor=ban\nkind=Admin called\ntitle=Oli (Team B) needs an admin\nsub=my game crashed"},
      {"help", "id=help\nkind=Help\ntitle=What do you need?\nbtn=Ready|green||#demo ready\nbtn=Call an admin|ghost||\nbtn=Match info|ghost||#demo info\nbtn=Commands|ghost||#demo cmds\nbtn=Admin|ghost||#demo admin\ncols=3\nbw=m\nclose=Close|\nmouse=on"},
      {"helplive", "id=help\nkind=Help\ntitle=What do you need?\nbtn=Tech pause|ghost||#demo pause\nbtn=Votes|ghost||#demo votes\nbtn=Call an admin|ghost||\nbtn=Match info|ghost||#demo info\nbtn=Commands|ghost||#demo cmds\nbtn=Admin|ghost||#demo admin\ncols=3\nbw=m\nclose=Close|\nmouse=on"},
      {"votes", "id=votes\nkind=Help \xC2\xB7 votes\ntitle=Start a vote\nbtn=Give up|danger||#demo vote\nbtn=Replay round|ghost||#demo replay\nbtn=Timeout|ghost||#demo timeout\ncols=3\nbw=m\nclose=Back|#demo helplive\nmouse=on"},
      {"info", "id=info\nkind=Help \xC2\xB7 match\ntitle=Team A vs Team B\nline=Format|Best of 3 \xC2\xB7 24 rounds\nline=Maps|Dust2 \xE2\x86\x92 Mirage \xE2\x86\x92 Inferno\nline=Map|1 of 3\nline=Score|7 - 5\nclose=Back|#demo help\nmouse=on"},
      {"cmds", "id=cmds\nkind=Help \xC2\xB7 commands\ntitle=Prefer typing?\nline=Ready|.r\nline=Tech pause|.p\nline=Timeout|.tac\nline=Give up|.gg\nline=Call an admin|.admin\nclose=Back|#demo help\nmouse=on"},
      {"admin", "id=admin\nkind=Admin\ntitle=Match controls\nbtn=Pause|ghost||#demo adminpause\nbtn=Unpause|ghost||#demo off\nbtn=Replay a round|ghost||#demo rounds\nbtn=Change map|ghost||#demo maps\nbtn=Swap teams|ghost||\nbtn=Force ready|ghost||#demo everyone\nbtn=Restart map|ghost||\nbtn=End match|danger||#demo endwho\ncols=4\nbw=m\nclose=Close|\nmouse=on"},
      {"maps", "id=maps\nkind=Admin \xC2\xB7 change map\ntitle=Pick a map\nbtn=Mirage|ghost||\nbtn=Inferno|ghost||\nbtn=Nuke|ghost||\nbtn=Ancient|ghost||\nbtn=Anubis|ghost||\nbtn=Dust2|ghost||\nbtn=Train|ghost||\nbtn=Overpass|ghost||\ncmd=Any other map or workshop id|.map <name or id>\ncols=4\nbw=s\nclose=Back|#demo admin\nmouse=on"},
      {"rounds", "id=rounds\nkind=Admin \xC2\xB7 replay\ntitle=Back to which round?\nbtn=Round 14|ghost|||7-6\nbtn=Round 13|ghost|||6-6\nbtn=Round 12|ghost|||6-5\nbtn=Round 11|ghost|||5-5\nbtn=Round 10|ghost|||4-5\nbtn=Round 9|ghost|||4-4\ncols=3\nbw=m\nclose=Back|#demo admin\nmouse=on"},
      {"endwho", "id=endwho\ncolor=ban\nkind=Admin \xC2\xB7 end match\ntitle=Who won?\nbtn=Team A|ghost||#demo endconfirm\nbtn=Team B|ghost||#demo endconfirm\nbtn=No winner|ghost||#demo endconfirm\ncols=3\nbw=m\nclose=Back|#demo admin\nmouse=on"},
      {"endconfirm", "id=end\ncolor=ban\nkind=Admin \xC2\xB7 end match\ntitle=Team A wins?\nsub=Ends the match now. No undo.\nbtn=End match|danger||#demo seriesresult\nbtn=Cancel|ghost||#demo endwho\nbw=m\nmouse=on"},
      {"mapresult", "id=mapresult\ncolor=live\nkind=Map 1 \xC2\xB7 Dust2\ntitle=Team A wins the map\nsub=13 - 7\nhint=Next: Mirage"},
      {"seriesresult", "id=seriesresult\ncolor=live\nkind=Series\ntitle=Team A wins the series\nsub=2 - 1"},
      {"mapvote", "id=mapvote\nkind=Map vote\ntitle=Next map?\ntimer=25\nbtn=Mirage|ghost||#demo off|3\nbtn=Inferno|ghost||#demo off|2\nbtn=Nuke|ghost||#demo off|1\nbtn=Ancient|ghost||#demo off|0\nbtn=Anubis|ghost||#demo off|2\nbtn=Dust2|ghost||#demo off|1\ncols=3\nbw=m\nmouse=on"},
      // The rest of the drafts (v1 "Every event" board): screens the match does not send yet.
      {"teamready", "id=warm\nwhere=card\ncolor=live\nkind=Ready\ntitle=Your team is ready\nsub=Waiting for Team B (3 / 5)\nteams=Team A|5|5|Team B|3|5"},
      {"joinateam", "id=warm\nwhere=card\nkind=Scrim\ntitle=Join CT or T to play"},
      {"spectating", "id=warm\nwhere=card\ncolor=muted\nkind=Spectating\ntitle=Not in this match"},
      {"coach", "id=warm\nwhere=card\ncolor=ct\nkind=Coach\ntitle=Coaching Team A"},
      {"sidechosen", "id=sides\ncolor=ct\nkind=Sides chosen\ntitle=You start as CT"},
      {"timeup", "id=sides\ncolor=muted\nkind=Time is up\ntitle=Team A stays CT"},
      {"halftime", "id=half\nkind=Halftime\ntitle=Sides switch\nsub=7 - 5"},
      {"overtime", "id=ot\nkind=Overtime 1\ntitle=First to 4"},
      {"pausepending", "id=notice\nwhere=card\ncolor=warn\nkind=Tech pause\ntitle=Pausing at the end of this round"},
      {"notimeouts", "id=notice\nwhere=card\ncolor=ban\nkind=Timeout\ntitle=No timeouts left\nsub=Team A used 3 / 3"},
      {"notech", "id=notice\nwhere=card\ncolor=ban\nkind=Tech pause\ntitle=No tech pauses left\nsub=Team A used 3 / 3"},
      {"dropped", "id=dropped\ncolor=warn\nkind=Paused\ntitle=Oli disconnected"},
      {"missingours", "id=missing\ncolor=ban\ntitle=Your team is missing players\nsub=Forfeit in 165s"},
      {"unpausing", "id=unpause\ncolor=live\nkind=Both teams ready\ntitle=Unpausing in 3s"},
      {"admincalled", "id=notice\nwhere=card\ncolor=warn\nkind=Admin\ntitle=An admin is on the way"},
      {"toosoon", "id=notice\nwhere=card\ncolor=muted\nkind=Admin\ntitle=Already called\nsub=Again in 42s"},
      {"notnow", "id=notice\nwhere=card\ncolor=muted\nkind=Not now\ntitle=Works once the match is live\ncmd=Timeout|.tac"},
      {"asay", "id=asay\nwhere=card\ncolor=ct\nkind=Admin\ntitle=Server restarts after this map"},
      {"tournament", "id=asay\nwhere=card\nkind=Tournament news\ntitle=Semi-finals start at 20:00"},
      {"restored", "id=restored\ncolor=ct\nkind=Round restored\ntitle=Back to round 14\nsub=7 - 6"},
      {"forcestart", "id=countdown\ncolor=ct\nkind=Admin\ntitle=Match starts in 5s"},
      {"adminended", "id=ended\ncolor=ban\nkind=Match ended\ntitle=An admin ended the match"},
      {"nextmap", "id=next\nwhere=card\nkind=Next map\ntitle=Inferno in 14s"},
      {"gg", "id=next\nwhere=card\ncolor=muted\nkind=GG\ntitle=Server closes in 38s"},
      {"download", "id=next\nwhere=card\nkind=Map 2 of 3\ntitle=Downloading the map\nsub=70%"},
      // The end screen and the leaderboard live in the other layout (at_kit, v1 board "Match end").
      {"kit:damage", ""},
      {"kit:end", ""},
      {"kit:endlost", ""},
      {"kit:next", ""},
      {"off", ""},
  };
  return k;
}

const std::string* DemoSpec(const std::string& name) {
  for (const auto& kv : DemoScreens())
    if (kv.first == name) return &kv.second;
  return nullptr;
}

// A slot showing demos: the match plugin's screens wait (DemoDefer) until the demo ends.
struct Demo {
  double until = 0;     // a single `demo` keeps the slot for a while
  int tour = -1;        // index into DemoScreens() while a tour runs
  double next = 0;      // when the tour moves on (0 = paused)
  double every = 0;     // seconds per screen
};
std::map<int, Demo> g_demos;
struct Deferred {
  std::string spec;
  ru_hud_click_fn onClick = nullptr;
  void* user = nullptr;
};
std::map<int, Deferred> g_deferred;

void OnDemoClick(void*, int slot, const char* command);

void DemoForget(int slot) {
  g_demos.erase(slot);
  g_deferred.erase(slot);
}

bool DemoActive(int slot) {
  auto it = g_demos.find(slot);
  return it != g_demos.end() && (it->second.tour >= 0 || g_now < it->second.until);
}

void DemoShow(int slot, const std::string& name) {
  if (const std::string* spec = DemoSpec(name)) IfScreen(slot, spec->c_str(), &OnDemoClick, nullptr);
}

void MenuRefresh(int slot);

void DemoEnd(int slot) {
  g_demos.erase(slot);
  MenuRefresh(slot);  // a menu if one is up, else what the match wants now
}

// The at_kit end screens (leaderboard) are for everyone at once, so a demo of one shows to all.
bool g_kitDemo = false;
void KitDemoOff() {
  if (!g_kitDemo) return;
  g_kitDemo = false;
  const int h = IfOpen("panorama/layout/custom_game/at_kit.xml");
  for (const char* el : {"at_card", "at_mid", "at_end"}) IfSetClass(h, RU_HUD_ALL, el, "on", 0);
}

// Shows demo screen `i` (index into DemoScreens) to `slot`: at_ui screens to the slot, "kit:" ones
// through at_kit.
void DemoShowIndex(int slot, size_t i) {
  const auto& kv = DemoScreens()[i];
  if (kv.first.rfind("kit:", 0) == 0) {
    IfScreen(slot, "", &OnDemoClick, nullptr);
    KitDemoOff();
    const std::string state = kv.first.substr(4);
    // The leaderboard is the match plugin's real end screen with made-up numbers (.enddemo,
    // end_screen.cpp): it fills the toolkit layout, holds the cursor and closes itself.
    if (state == "end" || state == "endlost") {
      if (RU_API_HAS(g_api, run_chat_command)) g_api->run_chat_command(g_api->self, slot, state == "end" ? ".enddemo" : ".enddemo lost");
      return;
    }
    g_kitDemo = true;
    const int h = IfOpen("panorama/layout/custom_game/at_kit.xml");
    for (const char* el : {"at_card", "at_mid", "at_end"}) IfSetClass(h, RU_HUD_ALL, el, "on", 0);
    g_later.emplace_back(g_now + 0.35, [state] { KitApply(state); });
    return;
  }
  KitDemoOff();
  IfScreen(slot, kv.second.c_str(), &OnDemoClick, nullptr);
}

// "28" (1-based, as the tour numbers them) or a screen name; -1 when neither.
int DemoFind(const std::string& arg) {
  const auto& list = DemoScreens();
  const int n = static_cast<int>(list.size()) - 1;
  if (!arg.empty() && arg.find_first_not_of("0123456789") == std::string::npos) {
    const int i = std::atoi(arg.c_str()) - 1;
    return i >= 0 && i < n ? i : -1;
  }
  for (int i = 0; i < n; ++i)
    if (list[static_cast<size_t>(i)].first == arg) return i;
  return -1;
}

void TourStep(int slot, Demo& t) {
  const auto& list = DemoScreens();
  const int n = static_cast<int>(list.size()) - 1;  // "off" is not part of the tour
  if (t.tour >= n) {
    g_api->chat_to_slot(g_api->self, slot, "Ready Up hud: tour done.");
    KitDemoOff();
    DemoEnd(slot);
    return;
  }
  DemoShowIndex(slot, static_cast<size_t>(t.tour));
  const std::string line = Fmt("Ready Up hud: tour %d/%d: ", t.tour + 1, n) + list[static_cast<size_t>(t.tour)].first;
  g_api->chat_to_slot(g_api->self, slot, line.c_str());
  if (t.next > 0) t.next = g_now + t.every;
}

// `.dev`: a clickable list of every demo screen, five a page, plus Prev / Next / Tour.
constexpr int kDevPerPage = 20;  // + Prev, Next, Tour all = 23 of the 24 button slots
// What a demo screen is, for the .dev buttons (its id, e.g. "everyone", says little).
std::string DevLabel(const std::string& id) {
  static const std::map<std::string, std::string> kLabels = {
      {"welcome", "Welcome"}, {"notready", "Warmup: not ready"}, {"ready", "Warmup: ready pill"},
      {"everyone", "Everyone ready"}, {"knife", "Knife round"}, {"pick", "Knife: pick a side"},
      {"pickurgent", "Knife: pick, hurry"}, {"pickedct", "Knife: picked CT"}, {"lost", "Knife: lost"},
      {"golive", "Going live"}, {"pause", "Tech pause"}, {"waiting", "Tech pause: waiting"},
      {"timeout", "Timeout"}, {"adminpause", "Admin pause"}, {"vote", "Surrender vote"},
      {"voted", "Surrender: voted"}, {"chips", "Vote mid-round"}, {"replay", "Replay request"},
      {"missing", "Team missing"}, {"admincall", "Admin called (admin)"}, {"help", "Help: warmup"},
      {"helplive", "Help: live"}, {"votes", "Help: votes"}, {"info", "Help: match info"},
      {"cmds", "Help: commands"}, {"admin", "Admin menu"}, {"maps", "Admin: change map"},
      {"rounds", "Admin: replay round"}, {"endwho", "Admin: end, who won"}, {"endconfirm", "Admin: end, confirm"},
      {"mapresult", "Map result card"}, {"seriesresult", "Series result card"}, {"mapvote", "Map veto"},
      {"teamready", "Warmup: team ready"}, {"joinateam", "Warmup: join a team"}, {"spectating", "Warmup: spectating"},
      {"coach", "Warmup: coach"}, {"sidechosen", "Sides chosen"}, {"timeup", "Side pick timed out"},
      {"halftime", "Halftime"}, {"overtime", "Overtime"},
      {"pausepending", "Pause next freeze"}, {"notimeouts", "No timeouts left"}, {"notech", "No tech pauses left"},
      {"dropped", "Player dropped"}, {"missingours", "Our team missing"}, {"unpausing", "Unpausing"},
      {"admincalled", "Admin on the way"}, {"toosoon", "Admin: called already"}, {"notnow", "Not now (warmup)"},
      {"asay", "Admin message"}, {"tournament", "Tournament message"}, {"restored", "Round restored"},
      {"forcestart", "Admin force start"}, {"adminended", "Match ended (admin)"}, {"nextmap", "Next map"},
      {"gg", "Server closing"}, {"download", "Map downloading"}, {"kit:damage", "Round damage"},
      {"kit:end", "Leaderboard: won"}, {"kit:endlost", "Leaderboard: lost"}, {"kit:next", "Old: next map card"},
  };
  const auto it = kLabels.find(id);
  return it != kLabels.end() ? it->second : id;
}

std::string DevMenuSpec(int page) {
  const auto& list = DemoScreens();
  const int n = static_cast<int>(list.size()) - 1;
  const int pages = (n + kDevPerPage - 1) / kDevPerPage;
  page = std::clamp(page, 0, pages - 1);
  const int from = page * kDevPerPage;
  const int to = std::min(n, from + kDevPerPage);
  std::string s = "id=dev\nkind=Dev \xC2\xB7 " + std::to_string(from + 1) + "-" + std::to_string(to) + " of " + std::to_string(n) +
                  "\ntitle=Show a screen\n";
  for (int i = from; i < to; ++i)
    s += "btn=" + DevLabel(list[static_cast<size_t>(i)].first) + "|ghost||#devshow " + std::to_string(i) + "|" +
         std::to_string(i + 1) + "\n";
  for (int i = to; i < from + kDevPerPage; ++i) s += "btn= |ghost|off|\n";  // keep the nav row in place
  s += "btn=Prev|ghost|" + std::string(page == 0 ? "off" : "") + "|#devpage " + std::to_string(page - 1) + "\n";
  s += "btn=Next|ghost|" + std::string(page + 1 >= pages ? "off" : "") + "|#devpage " + std::to_string(page + 1) + "\n";
  s += "btn=Tour all|green||#devtour\n";
  return s + "cols=5\nbw=xs\nclose=Close|#devclose\nmouse=on\nenter=fade\n";
}

void DevOpen(int slot, int page) {
  g_demos[slot] = Demo{g_now + 120.0, -1, 0, 0};
  KitDemoOff();
  IfScreen(slot, DevMenuSpec(page).c_str(), &OnDemoClick, nullptr);
}

void TourTick() {
  for (auto it = g_demos.begin(); it != g_demos.end();) {
    const int slot = it->first;
    Demo& t = it->second;
    ++it;  // TourStep may end (erase) this slot's entry
    if (t.tour < 0) {
      if (g_now >= t.until) {
        KitDemoOff();
        DemoEnd(slot);
      }
      continue;
    }
    if (t.next > 0 && g_now >= t.next) {
      ++t.tour;
      TourStep(slot, t);
    }
  }
}

// ---- menus (readyup.hud.v1.2 `menu`, `register_menu`) -------------------------------------------
//
// A plugin's menu sits over the base screen (`screen`, the match's): the latest menu from any owner
// is drawn, the base waits, and comes back when the last menu closes. A demo sits over both.

struct MenuEntry {
  std::string owner, spec;
  ru_hud_click_fn onClick = nullptr;
  void* user = nullptr;
};
std::map<int, std::vector<MenuEntry>> g_menus;  // slot -> open menus, the top one last
bool g_drawingMenu = false;                       // IfScreen is drawing a menu, not a base screen

void MenuForget(int slot) { g_menus.erase(slot); }

bool HasMenu(int slot) {
  auto it = g_menus.find(slot);
  return it != g_menus.end() && !it->second.empty();
}

bool DemoDefer(int slot, const std::string& spec, ru_hud_click_fn onClick, void* user) {
  if (g_drawingMenu || onClick == &OnDemoClick) return false;
  if (!DemoActive(slot) && !HasMenu(slot)) return false;
  g_deferred[slot] = Deferred{spec, onClick, user};  // shown again when the demo / menus end
  return true;
}

// Draws what `slot` should see now: nothing while a demo owns it, else the top menu, else the base.
void MenuRefresh(int slot) {
  if (DemoActive(slot)) return;
  if (HasMenu(slot)) {
    const MenuEntry top = g_menus[slot].back();
    g_drawingMenu = true;
    // A menu with buttons is the list on the left (v4, Hold E): W S choose, holding E picks, a close
    // row last. One with no buttons (retakes' "Retake site B!" banner) keeps its own place: as a list
    // it was a bare Close row (2026-11-24).
    const bool list = top.spec.find("btn=") != std::string::npos;
    const std::string spec = list ? top.spec + "\nwhere=left" : top.spec;
    IfScreen(slot, spec.c_str(), top.onClick, top.user);
    g_drawingMenu = false;
    return;
  }
  g_menus.erase(slot);
  auto d = g_deferred.find(slot);
  if (d == g_deferred.end()) {
    IfScreen(slot, "", nullptr, nullptr);
    return;
  }
  const Deferred back = d->second;
  g_deferred.erase(d);
  IfScreen(slot, back.spec.c_str(), back.onClick, back.user);
}

int IfMenu(int slot, const char* ownerIn, const char* specIn, ru_hud_click_fn onClick, void* user) {
  if (!ownerIn || !*ownerIn) return 0;
  if (SlotOff(slot) && specIn && *specIn) return 0;  // HUD off for this player: the caller uses chat
  const std::string owner = ownerIn, spec = specIn ? specIn : "";
  std::vector<int> slots;
  if (slot == RU_HUD_ALL) {
    for (const auto& kv : g_menus) slots.push_back(kv.first);
  } else if (slot >= 0 && slot <= 63) {
    slots.push_back(slot);
  } else {
    return 0;
  }
  for (int s : slots) {
    auto& v = g_menus[s];
    const bool wasTop = !v.empty() && v.back().owner == owner;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const MenuEntry& m) { return m.owner == owner; }), v.end());
    if (!spec.empty() && slot != RU_HUD_ALL) v.push_back(MenuEntry{owner, spec, onClick, user});
    if (!spec.empty() || wasTop) MenuRefresh(s);
  }
  return 1;
}

// `.menu` opens the menu that fits the server right now: the highest-priority provider that says
// it is active (practice over movement over map vote over the match).
struct MenuProvider {
  std::string owner;
  int priority = 0;
  ru_hud_menu_active_fn active = nullptr;
  ru_hud_menu_open_fn open = nullptr;
  void* user = nullptr;
};
std::vector<MenuProvider> g_providers;

int IfRegisterMenu(const char* ownerIn, int priority, ru_hud_menu_active_fn active, ru_hud_menu_open_fn open, void* user) {
  if (!ownerIn || !*ownerIn) return 0;
  const std::string owner = ownerIn;
  g_providers.erase(std::remove_if(g_providers.begin(), g_providers.end(), [&](const MenuProvider& p) { return p.owner == owner; }),
                    g_providers.end());
  if (!active || !open) {
    IfMenu(RU_HUD_ALL, ownerIn, "", nullptr, nullptr);  // unregistering: its menus close too
    return 1;
  }
  g_providers.push_back(MenuProvider{owner, priority, active, open, user});
  std::stable_sort(g_providers.begin(), g_providers.end(), [](const MenuProvider& a, const MenuProvider& b) { return a.priority > b.priority; });
  return 1;
}

bool OpenProviderMenu(int slot, const std::string& owner) {
  ru_player p{};
  if (!g_api->get_player(g_api->self, slot, &p)) return false;
  for (const auto& m : g_providers) {
    if (m.owner != owner || !m.active(m.user, slot)) continue;
    m.open(m.user, slot, p.steamid64);
    return true;
  }
  return false;
}

// ---- hold Use --------------------------------------------------------------------------------
// Holding the key bound to +use (E) for kHoldSeconds does the obvious thing without the mouse (see
// HoldAction): close the leaderboard or a menu, press the open card's first button (Ready), or
// open `.menu`. The card button shows the player's own key and fills while held; otherwise #ui_hold
// in at_ui does. Never while defusing, and the key must come up before it can fire again.

constexpr double kHoldSeconds = 0.6;  // = the 0.6s animations in at_ui.css (.ui-hold.hold)
constexpr uint64_t kInUse = 1ull << 5;  // CS2's IN_USE in m_pButtonStates[0] (see ButtonName)

struct HoldSlot {
  bool down = false;     // the key is held and counting
  bool fired = false;    // it did its thing: wait for the key to come up
  double since = 0;
  std::string label;     // what #ui_hold says now ("" = hidden)
  std::string button;    // the card button that has `holdable` ("" = none)
  bool holdClass = false;
  uint64_t prev = 0;     // last tick's buttons: W S move the menu row on the press, not while held
  bool frozen = false;   // we stopped the pawn moving while the menu panel is up (FreezeFor)
  bool live = false;     // `c-live` on the chip
};
std::map<int, HoldSlot> g_hold;
// The key the hold chip and buttons show. {g:csgo_key:k} in at_ui gives the value back as it is
// ("+use" and "use" both showed as text, 2026-11-20), so this is the key itself: E, CS2's default
// for +use. `ru hudkit holdkey <text>` changes it live.
std::string g_holdKey = "E";

// The held buttons of a slot's pawn and whether it is defusing. False if it has none (dead, spectating).
bool HeldButtons(int slot, uint64_t* held, bool* defusing) {
  static int ctrlPawn = -2, pawnMove = -2, moveButtons = -2, btnStates = -2, isDefusing = -2;
  if (ctrlPawn == -2) {
    ctrlPawn = g_api->schema_offset(g_api->self, "CCSPlayerController", "m_hPlayerPawn");
    pawnMove = g_api->schema_offset(g_api->self, "CBasePlayerPawn", "m_pMovementServices");
    moveButtons = g_api->schema_offset(g_api->self, "CPlayer_MovementServices", "m_nButtons");
    btnStates = g_api->schema_offset(g_api->self, "CInButtonState", "m_pButtonStates");
    isDefusing = g_api->schema_offset(g_api->self, "CCSPlayerPawn", "m_bIsDefusing");
  }
  if (ctrlPawn < 0 || pawnMove < 0 || moveButtons < 0 || btnStates < 0) return false;
  void* ctrl = g_api->entity_by_index(g_api->self, slot + 1);
  if (!ctrl) return false;
  uint32_t h = 0;
  std::memcpy(&h, static_cast<uint8_t*>(ctrl) + ctrlPawn, sizeof(h));
  void* pawn = g_api->entity_from_handle(g_api->self, h);
  if (!pawn) return false;
  void* move = nullptr;
  std::memcpy(&move, static_cast<uint8_t*>(pawn) + pawnMove, sizeof(move));
  if (!move) return false;
  std::memcpy(held, static_cast<uint8_t*>(move) + moveButtons + btnStates, sizeof(*held));
  *defusing = isDefusing >= 0 && *(static_cast<uint8_t*>(pawn) + isDefusing) != 0;
  return true;
}

// A class's state as `slot` sees it in a layout we already have (never opens one): the slot's own
// value if it set one, else everyone's.
bool ClassOn(const char* path, int slot, const char* panel, const char* cls) {
  for (const Layout& l : g_layouts) {
    if (l.path != path) continue;
    const Key k{panel, cls};
    auto s = l.slots.find(slot);
    if (s != l.slots.end()) {
      auto c = s->second.classes.find(k);
      if (c != s->second.classes.end()) return c->second;
    }
    auto a = l.slots.find(RU_HUD_ALL);
    if (a != l.slots.end()) {
      auto c = a->second.classes.find(k);
      if (c != a->second.classes.end()) return c->second;
    }
    return false;
  }
  return false;
}

const char* kKitLayout = "panorama/layout/custom_game/at_kit.xml";

// What holding Use does for `slot` now, in order:
//  - the end-of-map leaderboard is up: close it (`.lb`), on the chip;
//  - a menu is open: close it (its close button), on the chip, never one of its buttons;
//  - a card that asks for it (`hold=1`) is up and its first button is live (Ready, Unready,
//    Scoreboard): press it, shown on that button itself (`holdable`, `hold`), not on the chip.
// False: nothing to do, chip and button stay plain.
struct HoldTarget {
  std::string label;   // the chip's text ("" when the button shows it)
  std::string action;  // "#cmd <chat>", a button id for ScreenClick, or "" for `.menu`
  std::string button;  // the button that shows the hold ("" = the chip)
  bool live = false;   // the chip in green (a where=chip screen with color=live: Ready)
};

bool HoldAction(int slot, HoldTarget* t) {
  *t = HoldTarget{};
  const std::string lang = readyup::i18n::LanguageOf(g_api, slot);
  if (ClassOn(kKitLayout, slot, "at_end", "on") && ClassOn(kKitLayout, slot, "at_end", "can-close")) {
    t->label = g_text.Tr(lang, "hud.hold_close");
    t->action = "#cmd .lb";
    return true;
  }
  auto it = g_screens.find(slot);
  const bool card = it != g_screens.end() && it->second.shown && !it->second.dismissed && !DemoActive(slot);
  // The chip above the logo (v4): a where=chip screen (Ready up: no card at all) or a card that asks
  // for it with hold=1 (pause, votes: the card says what happened, its buttons are hidden). The chip
  // names the first button (and a chip screen's count); holding E presses it.
  const bool chipScreen = it != g_screens.end() && (it->second.cur.where == "chip" || it->second.cur.hold) &&
                          it->second.cur.where != "left";
  if (card && chipScreen && !it->second.clicked && !it->second.cur.buttons.empty()) {
    const Button& b = it->second.cur.buttons[0];
    if (b.state != "off" && !b.cmd.empty() && !b.label.empty()) {
      t->label = b.label + (it->second.cur.counter.empty() ? "" : "  " + it->second.cur.counter);
      t->action = "#cmd " + b.cmd;
      t->live = it->second.cur.color == "live";
      return true;
    }
  }
  // The menu panel: holding E picks the chosen row (W S choose it); the row itself fills.
  if (card && PanelOf(it->second.cur) == "ui_left" && !it->second.selId.empty() && !it->second.clicked) {
    t->button = it->second.selId;
    t->action = it->second.selId;
    return true;
  }
  if (HasMenu(slot)) {
    if (!card) return false;
    t->label = g_text.Tr(lang, "hud.hold_close");
    t->action = PanelOf(it->second.cur) + "_close";
    return true;
  }
  // No "hold for .menu": a chip on screen all the time, live rounds included, was too much
  // (Sivert, 2026-11-20). `.menu` is still typed.
  return false;
}

// Draws the hold: the chip (#ui_hold) or a card button, and `hold` on whichever shows it.
void HoldShow(int slot, HoldSlot& hs, const HoldTarget& t, bool has, bool hold) {
  const int h = IfOpen(kUiLayout);
  const std::string chip = has && t.button.empty() ? t.label : "";
  if (chip != hs.label) {
    if (!chip.empty()) {
      IfSetText(h, slot, "ui_hold_t", "text", chip.c_str());
      IfSetText(h, slot, "ui_hold_k", "k", g_holdKey.c_str());
    }
    IfSetClass(h, slot, "ui_hold", "on", chip.empty() ? 0 : 1);
    hs.label = chip;
  }
  const bool live = has && t.live;
  if (live != hs.live) {
    IfSetClass(h, slot, "ui_hold", "c-live", live ? 1 : 0);
    hs.live = live;
  }
  const std::string button = has ? t.button : "";
  if (button != hs.button) {
    std::string panel;
    int index = 0;
    if (!hs.button.empty()) {
      IfSetClass(h, slot, hs.button.c_str(), "hold", 0);
      if (ParseButtonId(hs.button, &panel, &index) && panel != "ui_left") {
        IfSetClass(h, slot, hs.button.c_str(), "holdable", 0);
        IfSetClass(h, slot, panel.c_str(), "holdable", 0);
      }
    }
    // Menu rows (ui_left) show the hold as their fill only: no key, no `holdable`.
    if (!button.empty() && ParseButtonId(button, &panel, &index) && panel != "ui_left") {
      IfSetText(h, slot, (button + "_k").c_str(), "k", g_holdKey.c_str());
      IfSetClass(h, slot, button.c_str(), "holdable", 1);
      IfSetClass(h, slot, panel.c_str(), "holdable", 1);
    }
    hs.button = button;
    hs.holdClass = false;
    IfSetClass(h, slot, "ui_hold", "hold", 0);
  }
  if (hold != hs.holdClass) {
    IfSetClass(h, slot, hs.button.empty() ? "ui_hold" : hs.button.c_str(), "hold", hold ? 1 : 0);
    hs.holdClass = hold;
  }
}

// ---- the menu panel holds the player still --------------------------------------------------
// While ui_left is up W S choose rows, so in warmup (where players can walk) the pawn is held in
// place, the way CS2 plugins freeze players (MOVETYPE_OBSOLETE in m_MoveType and m_nActualMoveType,
// MOVETYPE_WALK to let go). Freeze time already holds players, and a live round is never frozen:
// a menu left open into a round only moves its row.
constexpr uint64_t kInForward = 1ull << 3;  // W (IN_FORWARD)
constexpr uint64_t kInBack = 1ull << 4;     // S (IN_BACK)
constexpr uint8_t kMoveObsolete = 1, kMoveWalk = 2;

void* PawnOf(int slot) {
  static int ctrlPawn = -2;
  if (ctrlPawn == -2) ctrlPawn = g_api->schema_offset(g_api->self, "CCSPlayerController", "m_hPlayerPawn");
  if (ctrlPawn < 0) return nullptr;
  void* ctrl = g_api->entity_by_index(g_api->self, slot + 1);
  if (!ctrl) return nullptr;
  uint32_t h = 0;
  std::memcpy(&h, static_cast<uint8_t*>(ctrl) + ctrlPawn, sizeof(h));
  return g_api->entity_from_handle(g_api->self, h);
}

// CCSGameRules::m_bWarmupPeriod through the cs_gamerules proxy (looked up once, checked each time).
bool InWarmup() {
  static int proxyIndex = -1, rulesPtr = -2, warmup = -2;
  if (rulesPtr == -2) {
    rulesPtr = g_api->schema_offset(g_api->self, "CCSGameRulesProxy", "m_pGameRules");
    warmup = g_api->schema_offset(g_api->self, "CCSGameRules", "m_bWarmupPeriod");
  }
  if (rulesPtr < 0 || warmup < 0) return false;
  auto isProxy = [](int i) {
    void* e = g_api->entity_by_index(g_api->self, i);
    const char* cls = e ? g_api->entity_classname(g_api->self, e) : nullptr;
    return cls && std::strcmp(cls, "cs_gamerules") == 0;
  };
  if (proxyIndex < 0 || !isProxy(proxyIndex)) {
    proxyIndex = -1;
    for (int i = 65; i < 2048 && proxyIndex < 0; ++i)
      if (isProxy(i)) proxyIndex = i;
    if (proxyIndex < 0) return false;
  }
  void* proxy = g_api->entity_by_index(g_api->self, proxyIndex);
  void* rules = nullptr;
  std::memcpy(&rules, static_cast<uint8_t*>(proxy) + rulesPtr, sizeof(rules));
  return rules && *(static_cast<uint8_t*>(rules) + warmup) != 0;
}

bool FreezeFor(int slot, bool on) {
  static int moveType = -2, actual = -2;
  if (moveType == -2) {
    moveType = g_api->schema_offset(g_api->self, "CBaseEntity", "m_MoveType");
    actual = g_api->schema_offset(g_api->self, "CBaseEntity", "m_nActualMoveType");
  }
  void* pawn = PawnOf(slot);
  if (moveType < 0 || actual < 0 || !pawn) return false;
  const uint8_t v = on ? kMoveObsolete : kMoveWalk;
  *(static_cast<uint8_t*>(pawn) + moveType) = v;
  *(static_cast<uint8_t*>(pawn) + actual) = v;
  g_api->entity_mark_changed(g_api->self, pawn);
  return true;
}

// W S move the chosen row (on the press, wrapping); the pawn is held still in warmup.
void NavTick(int slot, HoldSlot& hs, bool gotButtons, uint64_t held) {
  auto it = g_screens.find(slot);
  const bool left = it != g_screens.end() && it->second.shown && !it->second.dismissed &&
                    PanelOf(it->second.cur) == "ui_left" && !DemoActive(slot);
  if (left && gotButtons) {
    const uint64_t pressed = held & ~hs.prev;
    int step = 0;
    if (pressed & kInForward) --step;
    if (pressed & kInBack) ++step;
    if (step) {
      it->second.sel += step;
      NavDraw(slot, it->second);
      hs.down = false;  // a hold on the old row does not carry over
    }
  }
  hs.prev = gotButtons ? held : 0;
  const bool freeze = left && InWarmup();
  if (freeze != hs.frozen && FreezeFor(slot, freeze)) hs.frozen = freeze;
}

void HoldTick() {
  for (int slot = 0; slot <= 63; ++slot) {
    ru_player p{};
    p.struct_size = sizeof(p);
    const bool human = g_api->get_player(g_api->self, slot, &p) && p.connected && !p.is_bot && p.steamid64;
    if (!human) {
      g_hold.erase(slot);
      continue;
    }
    HoldSlot& hs = g_hold[slot];
    uint64_t held = 0;
    bool defusing = false;
    const bool got = HeldButtons(slot, &held, &defusing);
    NavTick(slot, hs, got, held);
    HoldTarget t;
    const bool has = HoldAction(slot, &t);
    const bool use = got && (held & kInUse) && !defusing;
    if (!use) {
      hs.down = false;
      hs.fired = false;
    } else if (has && !hs.down && !hs.fired) {
      hs.down = true;
      hs.since = g_now;
    }
    if (!has) hs.down = false;
    if (hs.down && !hs.fired && g_now - hs.since >= kHoldSeconds) {
      hs.down = false;
      hs.fired = true;
      ru_logf(g_api, RU_LOG_INFO, "hold: slot %d %s", slot, t.action.empty() ? "opens .menu" : t.action.c_str());
      HoldShow(slot, hs, t, has, false);
      if (t.action.rfind("#cmd ", 0) == 0) {
        if (RU_API_HAS(g_api, run_chat_command)) g_api->run_chat_command(g_api->self, slot, t.action.substr(5).c_str());
      } else if (!t.action.empty()) {
        ScreenClick(slot, t.action);
      } else {
        for (const auto& pr : g_providers) {
          if (pr.active(pr.user, slot)) {
            pr.open(pr.user, slot, p.steamid64);
            break;
          }
        }
      }
      continue;
    }
    HoldShow(slot, hs, t, has, hs.down);
  }
}

void OnMenuChat(void*, const ru_command_ctx* ctx) {
  if (ctx->slot < 0) return;
  if (SlotOff(ctx->slot)) {
    // HUD off for this player: the commands in chat instead of a menu.
    g_api->chat_to_slot(g_api->self, ctx->slot, g_text.Tr(readyup::i18n::LanguageOf(g_api, ctx->slot), "menu.chat_help").c_str());
    return;
  }
  for (const auto& p : g_providers) {
    if (p.active(p.user, ctx->slot)) {
      p.open(p.user, ctx->slot, ctx->steamid64);
      return;
    }
  }
  g_api->chat_to_slot(g_api->self, ctx->slot, g_text.Tr(readyup::i18n::LanguageOf(g_api, ctx->slot), "menu.none").c_str());
}

// `.hud` (toggle), `.hud off`, `.hud on`: the custom HUD for this player. Off: Ready Up uses the
// chat and the centre panel for them (saved; the platform's player setting sets the same).
void OnHudChat(void*, const ru_command_ctx* ctx) {
  if (ctx->slot < 0 || !ctx->steamid64) return;
  const std::string a = ctx->argc > 1 && ctx->argv[1] ? ctx->argv[1] : "";
  const bool off = a == "off" || a == "0" ? true : a == "on" || a == "1" ? false : !g_hudOff.count(ctx->steamid64);
  SetPlayerOff(ctx->steamid64, off);
  g_api->chat_to_slot(g_api->self, ctx->slot,
                      g_text.Tr(readyup::i18n::LanguageOf(g_api, ctx->slot), off ? "hud.now_off" : "hud.now_on").c_str());
}

void OnDemoClick(void*, int slot, const char* command) {
  const std::string c = command ? command : "";
  if (c.rfind("#demo ", 0) == 0) {
    auto t = g_demos.find(slot);
    if (t != g_demos.end() && t->second.tour < 0) t->second.until = g_now + 60.0;
    DemoShow(slot, c.substr(6));
    return;
  }
  if (c.rfind("#devpage ", 0) == 0) return DevOpen(slot, std::atoi(c.c_str() + 9));
  if (c == "#devclose") {
    KitDemoOff();
    return DemoEnd(slot);
  }
  if (c == "#devtour") {
    g_demos[slot] = Demo{0, 0, g_now + 6.0, 6.0};
    return TourStep(slot, g_demos[slot]);
  }
  if (c.rfind("#devshow ", 0) == 0) {
    const int i = std::atoi(c.c_str() + 9);
    if (i < 0 || i >= static_cast<int>(DemoScreens().size()) - 1) return;
    g_demos[slot] = Demo{g_now + 60.0, -1, 0, 0};
    DemoShowIndex(slot, static_cast<size_t>(i));
    const std::string line = Fmt("Ready Up hud: %d ", i + 1) + DemoScreens()[static_cast<size_t>(i)].first + " (.dev for the list)";
    g_api->chat_to_slot(g_api->self, slot, line.c_str());
    return;
  }
  const std::string line = "Ready Up hud: you clicked " + c;
  g_api->chat_to_slot(g_api->self, slot, line.c_str());
}

// `.dev` (admins): the clickable list of every HUD screen.
void OnDevChat(void*, const ru_command_ctx* ctx) {
  if (ctx->slot < 0) return;
  if (!g_api->is_admin(g_api->self, ctx->steamid64)) {
    g_api->chat_to_slot(g_api->self, ctx->slot, "Ready Up hud: .dev is for admins");
    return;
  }
  // The leaderboard preview belongs to the match plugin (.enddemo): `.dev` always closes it.
  if (RU_API_HAS(g_api, run_chat_command)) g_api->run_chat_command(g_api->self, ctx->slot, ".enddemo off");
  // `.dev` again closes the menu (or the demo it showed); else it opens.
  if (DemoActive(ctx->slot)) {
    KitDemoOff();
    return DemoEnd(ctx->slot);
  }
  DevOpen(ctx->slot, 0);
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
  } else if (sub == "buttons") {  // ru hudkit buttons [slot] [seconds]
    std::vector<int> slots;
    if (ctx->argc > 3) slots.push_back(std::atoi(ctx->argv[3]));
    else g_api->for_each_player(g_api->self, &CollectHuman, &slots);
    if (slots.empty()) return Reply(ctx, "nobody to probe");
    g_probe = ButtonProbe{slots[0], g_now + (ctx->argc > 4 ? std::atof(ctx->argv[4]) : 30.0), ~0ull};
    Reply(ctx, "logging held buttons of slot " + std::to_string(slots[0]));
  } else if (sub == "bclick") {  // ru hudkit bclick [slot]: the same card, mouse on the B key
    std::vector<int> slots;
    if (ctx->argc > 3) slots.push_back(std::atoi(ctx->argv[3]));
    else g_api->for_each_player(g_api->self, &CollectHuman, &slots);
    for (int slot : slots) ClickOpenB(slot);
    Reply(ctx, "click POC (B key) open for " + std::to_string(slots.size()) + " player(s); click hook " + g_clickHookStatus);
  } else if (sub == "kit") {
    Kit(ctx, ctx->argc > 3 ? ctx->argv[3] : "");
  } else if (sub == "demo") {  // ru hudkit demo <screen> [slot]
    const std::string name = ctx->argc > 3 ? ctx->argv[3] : "";
    if (!DemoSpec(name)) {
      std::string names;
      for (const auto& kv : DemoScreens()) names += " " + kv.first;
      return Reply(ctx, "demo screens:" + names);
    }
    std::vector<int> slots;
    if (ctx->argc > 4) slots.push_back(std::atoi(ctx->argv[4]));
    else if (ctx->slot >= 0) slots.push_back(ctx->slot);
    else g_api->for_each_player(g_api->self, &CollectHuman, &slots);
    for (int slot : slots) {
      g_demos[slot] = Demo{g_now + 60.0, -1, 0, 0};
      DemoShow(slot, name);
    }
    Reply(ctx, "demo " + name + " for " + std::to_string(slots.size()) + " player(s) (60 s); click hook " + g_clickHookStatus);
  } else if (sub == "rank") {  // ru hudkit rank <slot|all> <type> <value> [wins] | off
    if (ctx->argc >= 4 && std::strcmp(ctx->argv[3], "off") == 0) {
      g_rankTest.clear();
      return Reply(ctx, "rank test: off");
    }
    if (ctx->argc < 6) return Reply(ctx, "usage: ru hudkit rank <slot|all> <type> <value> [wins] | off");
    RankTest r;
    r.type = std::atoi(ctx->argv[4]);
    r.value = std::atoi(ctx->argv[5]);
    r.wins = ctx->argc > 6 ? std::atoi(ctx->argv[6]) : 100;
    g_rankTest[ParseSlot(ctx->argv[3])] = r;
    g_rankNext = 0;
    Reply(ctx, "rank test: slot " + std::string(ctx->argv[3]) + " type " + std::to_string(r.type) + " value " +
                   std::to_string(r.value) + " wins " + std::to_string(r.wins));
  } else if (sub == "holdkey") {  // ru hudkit holdkey <binding>: what the hold chip's key lookup gets
    if (ctx->argc < 4) return Reply(ctx, "holdkey: " + g_holdKey);
    g_holdKey = ctx->argv[3];
    for (auto& kv : g_hold) kv.second.label.clear();  // rewrite every chip next tick
    Reply(ctx, "holdkey: " + g_holdKey);
  } else if (sub == "scale") {  // ru hudkit scale [70..100]: the HUD's size (until a restart; cfg scale= keeps it)
    if (ctx->argc < 4) return Reply(ctx, "HUD scale: " + std::to_string(g_scale) + "% (70 to 100, steps of 5)");
    const int v = ParseScale(ctx->argv[3]);
    if (v < 0) return Reply(ctx, "usage: ru hudkit scale <70..100>");
    g_scale = v;
    ApplyScale();
    return Reply(ctx, "HUD scale: " + std::to_string(g_scale) + "%");
  } else if (sub == "dev") {  // ru hudkit dev: the clickable screen list (also .dev)
    if (ctx->slot < 0) return Reply(ctx, "dev: in game only");
    DevOpen(ctx->slot, 0);
  } else if (sub == "tour") {  // ru hudkit tour [<n>|<name>|every <s>|next|back|pause|stop] [slot]
    std::string arg = ctx->argc > 3 ? ctx->argv[3] : "";
    double every = 6.0;
    int argSlot = 4;
    if (arg == "every") {
      every = std::clamp(ctx->argc > 4 ? std::atof(ctx->argv[4]) : 6.0, 2.0, 60.0);
      arg.clear();
      argSlot = 5;
    }
    int slot = ctx->argc > argSlot ? std::atoi(ctx->argv[argSlot]) : ctx->slot;
    if (slot < 0) {
      std::vector<int> humans;
      g_api->for_each_player(g_api->self, &CollectHuman, &humans);
      if (humans.empty()) return Reply(ctx, "nobody to show the tour to");
      slot = humans[0];
    }
    auto it = g_demos.find(slot);
    const bool running = it != g_demos.end() && it->second.tour >= 0;
    if (arg == "stop") {
      KitDemoOff();
      if (running) DemoEnd(slot);
      return Reply(ctx, "tour stopped");
    }
    if (!arg.empty() && arg != "next" && arg != "back" && arg != "pause") {
      // One screen, held: by its tour number or its name; next / back step from there.
      const int i = DemoFind(arg);
      if (i < 0) return Reply(ctx, "no screen \"" + arg + "\" (.dev lists them)");
      g_demos[slot] = Demo{0, i, 0, every};
      TourStep(slot, g_demos[slot]);
      return Reply(ctx, "holding; .ru hudkit tour next / back / pause / stop");
    }
    if (running && (arg == "next" || arg == "back" || arg == "pause")) {
      Demo& t = it->second;
      if (arg == "pause") {
        t.next = t.next > 0 ? 0 : g_now + t.every;
        return Reply(ctx, t.next > 0 ? "tour playing" : "tour paused (next / back to step)");
      }
      t.tour = std::max(0, t.tour + (arg == "next" ? 1 : -1));
      TourStep(slot, t);
      return;
    }
    g_demos[slot] = Demo{0, 0, g_now + every, every};
    TourStep(slot, g_demos[slot]);
    Reply(ctx, Fmt("tour: %zu screens, %.0f s each (.ru hudkit tour pause / next / back / stop)", DemoScreens().size() - 1, every));
  } else if (sub == "screen" && ctx->argc >= 5) {  // ru hudkit screen <slot> <spec, lines split by ;>
    std::string spec = Join(ctx, 4);
    for (char& ch : spec)
      if (ch == ';') ch = '\n';
    Reply(ctx, IfScreen(std::atoi(ctx->argv[3]), spec.c_str(), &OnDemoClick, nullptr) ? "ok" : "failed");
  } else if (sub == "text" && ctx->argc >= 7) {  // ru hudkit text <hud#> <slot|all> <panel> <var> <value...>
    Reply(ctx, IfSetText(std::atoi(ctx->argv[3]), ParseSlot(ctx->argv[4]), ctx->argv[5], ctx->argv[6], Join(ctx, 7).c_str()) ? "ok" : "failed");
  } else if (sub == "class" && ctx->argc >= 8) {  // ru hudkit class <hud#> <slot|all> <panel> <class> <0|1>
    Reply(ctx, IfSetClass(std::atoi(ctx->argv[3]), ParseSlot(ctx->argv[4]), ctx->argv[5], ctx->argv[6], std::atoi(ctx->argv[7])) ? "ok" : "failed");
  } else {
    Reply(ctx, "ru hudkit [status|info|demo <screen> [slot]|dev|tour [<n>|<name>|every <s>|pause|next|back|stop]|screen <slot> <k=v;k=v>|click [slot]|bclick [slot]|hello <text>|kit <state>|text <hud#> <slot|all> <panel> <var> <value>|class <hud#> <slot|all> <panel> <class> <0|1>]");
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
  g_text = {};
  if (const std::string err = g_text.Load(kReadyUpLocales, kReadyUpLocaleCount); !err.empty())
    g_api->log(g_api->self, RU_LOG_WARN, ("locales: " + err).c_str());
  g_eng = Engine{};
  g_layouts.clear();
  g_resendAt.clear();
  g_verNextAt = 0;
  g_later.clear();
  g_bMode.clear();
  g_clickOpen.clear();
  g_screens.clear();
  g_demos.clear();
  g_deferred.clear();
  g_menus.clear();
  g_providers.clear();
  g_buyOpen.clear();
  g_straysCleared = false;
  g_mapStartedAt = -1;
  g_pauseUntil = -1;
  g_repushAfterPause = false;
  g_scale = 75;
  HudOffLoad();
  g_slotOff.reset();
  if (RU_API_HAS(api, config_get)) {
    char buf[16] = {};
    if (api->config_get(api->self, "scale", buf, sizeof(buf)) > 0 && ParseScale(buf) > 0) g_scale = ParseScale(buf);
  }
  ApplyScale();
  SetTeamLogos();
  api->on_tick(api->self, &OnTick, nullptr);
  if (!api->subscribe_game_event(api->self, "round_prestart", &OnRoundPrestart, nullptr))
    ru_logf(api, RU_LOG_WARN, "round_prestart not available: a round restart may crash the server while a HUD is shown");
  if (!api->subscribe_game_event(api->self, "round_poststart", &OnRoundPoststart, nullptr))
    ru_logf(api, RU_LOG_WARN, "round_poststart not available: cards wait 1 s after a round restart");
  // CS2's BeginMatch (a warmup ending) cleans up the map too, sometimes with no round_prestart
  // after it (surf_utopia_njv, 2026-10-27: "WriteEnterPVS" on a team change). Same pause.
  if (!api->subscribe_game_event(api->self, "begin_new_match", &OnBeginMatch, nullptr))
    ru_logf(api, RU_LOG_WARN, "begin_new_match not available");
  if (!api->subscribe_game_event(api->self, "player_activate", &OnPlayerActivate, nullptr))
    ru_logf(api, RU_LOG_WARN, "player_activate not available: a joining player's HUD is pushed again 2 and 5 s after connect only");
  api->subscribe(api->self, RU_EVENT_ANY, &OnEvent, nullptr);
  api->register_ru_subcommand(api->self, "hudkit", &OnRu, nullptr);
  api->register_chat_command(api->self, ".dev", &OnDevChat, nullptr);
  api->register_chat_command(api->self, ".menu", &OnMenuChat, nullptr);
  api->register_chat_command(api->self, ".hud", &OnHudChat, nullptr);
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
