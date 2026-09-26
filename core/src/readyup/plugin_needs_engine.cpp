// Real engine probes for plugin needs (plugin_needs.h): engine-surface resolution, schema
// fields, game event descriptors, and the in-game admin notice. Server build only; the offline
// plugin hosts (tests) never link this file, so they load every plugin as before.

#include "readyup/plugin_needs.h"

#include "readyup/admin_check.h"
#include "readyup/client_print.h"
#include "readyup/cs2_version.h"
#include "readyup/engine_surface.h"
#include "readyup/game_events.h"
#include "readyup/schema.h"
#include "readyup/slot_registry.h"

#include <map>
#include <mutex>
#include <set>
#include <string>

namespace readyup::plugins {
namespace {

// 1 resolved, 0 missing, -1 cannot tell. Functions and rtti are cached once decided (the image
// does not change while the process lives); vtable slots and layouts are re-read (verified later).
int SurfaceState(const std::string& name) {
  static std::mutex mu;
  static std::map<std::string, int> cache;
  {
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
  }
  const es::EngineSurface* s = GetEngineSurface();
  if (!s) return -1;
  int st = -2;  // -2: not a function / rtti entry
  for (const auto& f : s->functions) {
    if (f.name == name) st = EngineFunctionResolution(name.c_str()).ok ? 1 : 0;
  }
  if (st == -2) {
    for (const auto& rt : s->rtti) {
      if (rt.cls != name) continue;
      const es::Image* img = RealServerImage();
      if (rt.module != "server" || !img) return -1;  // other modules: checked on the object at use
      std::string detail;
      st = es::FindVtableByRtti(*img, rt.typeinfo_name, &detail) ? 1 : 0;
    }
  }
  if (st != -2) {
    std::lock_guard<std::mutex> lk(mu);
    cache[name] = st;
    return st;
  }
  for (const auto& v : EngineVtableReport()) {
    if (v.name == name) return !v.checked ? -1 : v.ok ? 1 : 0;
  }
  for (const auto& l : EngineLayoutReport()) {
    if (l.name == name) return l.ok ? 1 : 0;
  }
  return -1;  // not in this build's engine surface: nothing to hold against the plugin
}

int SchemaState(const std::string& cls, const std::string& field) {
  std::string detail;
  if (SchemaStatus(&detail) != 1) return -2;
  const auto off = SchemaFindOffset("server", cls, field);
  return off ? *off : -1;
}

int EventState(const std::string& name) { return GameEventDescriptorKnown(name.c_str()); }

NeedsProbe RealProbe() {
  NeedsProbe p;
  p.surface = &SurfaceState;
  p.schema = &SchemaState;
  p.event = &EventState;
  const Cs2VersionSnapshot v = GetCs2VersionSnapshot();
  p.cs2Build = v.build_id ? std::to_string(*v.build_id) : "?";
  return p;
}

// Game thread. Each admin in game hears every line once per map.
void NotifyAdmins(const std::vector<std::string>& lines, uint64_t mapGen) {
  static uint64_t gen = 0;
  static std::set<uint64_t> told;
  if (mapGen != gen) {
    gen = mapGen;
    told.clear();
  }
  for (const auto& h : ListHumans()) {
    if (h.slot < 0 || h.steamid64 == 0 || told.count(h.steamid64) || !IsReadyUpAdmin(h.steamid64)) continue;
    told.insert(h.steamid64);
    for (const auto& l : lines) ClientPrintChat(h.slot, ("[Ready Up] " + l).c_str());
  }
}

const bool kRegistered = [] {
  SetNeedsProbeProvider(&RealProbe);
  SetNeedsAdminNotifier(&NotifyAdmins);
  return true;
}();

}  // namespace
}  // namespace readyup::plugins
