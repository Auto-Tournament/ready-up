#pragma once

// What a plugin needs from the engine (plugins/<name>/needs.json, shipped next to the plugin as
// csgo/readyup/plugins/<name>.needs.json; docs/CS2-COMPAT.md "Plugin needs"), and the load
// decision the core takes from it on the running CS2 build:
//
//   - a needed engine-surface entry (function, hook, rtti, vtable slot, layout) that did not
//     resolve, or a required schema field (every alternative missing)  -> do not load the plugin
//   - a missing optional schema field or an unknown game event          -> load, with a warning
//   - something that cannot be checked yet (schema system or game event manager not up)
//                                                                         -> load (pending)
//   - no needs.json                                                       -> load, as before
//
// Engine-free (the probes are injected) so the decision logic is unit tested
// (tests/plugin_needs_test.cpp); plugin_needs_engine.cpp wires the real probes.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace readyup::plugins {

struct PluginNeeds {
  std::string plugin;
  std::vector<std::string> surface;          // engine-surface entry names
  std::vector<std::string> schema;           // "Class.m_field" or "A.m_x|B.m_x" (any one resolves)
  std::vector<std::string> schemaOptional;   // same syntax; missing only warns
  std::vector<std::string> events;           // game event names; missing only warns
};

// nullopt + *err set on malformed JSON / wrong shape.
std::optional<PluginNeeds> ParseNeeds(const std::string& json, std::string* err);
// nullopt with an empty *err when the file does not exist (backward compatible: no manifest).
std::optional<PluginNeeds> LoadNeedsFile(const std::string& path, std::string* err);

struct NeedsProbe {
  // 1 resolved, 0 missing on this build, -1 cannot tell (yet)
  std::function<int(const std::string& name)> surface;
  // offset >= 0 found, -1 missing, -2 cannot tell (schema system not ready)
  std::function<int(const std::string& cls, const std::string& field)> schema;
  // 1 known, 0 unknown to this build, -1 cannot tell (event manager not ready)
  std::function<int(const std::string& name)> event;
  std::string cs2Build;  // for the reason text; "?" if unknown
};

struct NeedCheck {
  std::string kind;   // "surface" | "schema" | "schema_optional" | "event"
  std::string entry;  // as written in needs.json
  int state = -1;     // 1 ok, 0 missing, -1 pending
  std::string detail; // "0x3cb" / "resolved" / "not found" / "unknown to this CS2 build" / ...
};

struct NeedsVerdict {
  bool load = true;
  std::string reason;                 // "missing X, Y after CS2 build N" when !load
  std::vector<NeedCheck> checks;      // one per entry, manifest order
  std::vector<std::string> warnings;  // optional schema / events missing
};

NeedsVerdict EvaluateNeeds(const PluginNeeds& needs, const NeedsProbe& probe);

// The one-line log / chat text: "plugin[<name>] disabled: <reason>".
std::string NeedsDisabledLine(const std::string& plugin, const std::string& reason);

// Real engine probes (plugin_needs_engine.cpp, server only). The plugin host enforces needs only
// once a provider is set, so offline hosts (tests) behave as before.
using NeedsProbeProvider = NeedsProbe (*)();
void SetNeedsProbeProvider(NeedsProbeProvider provider);
// Tells in-game admins (chat) about plugins the core refused to load; set by the server build.
// Called from the game thread every few seconds once a map started; mapGen changes on every map
// start, so the notifier tells each admin once per map.
using NeedsAdminNotifier = void (*)(const std::vector<std::string>& lines, uint64_t mapGen);
void SetNeedsAdminNotifier(NeedsAdminNotifier notifier);

struct DisabledPlugin {
  std::string name;
  std::string reason;
};
// Plugins the core refused to load for unmet needs (since the last directory load).
std::vector<DisabledPlugin> NeedsDisabledPlugins();
// ru selftest: every plugin manifest in the plugins dir, evaluated now.
struct PluginNeedsReport {
  std::string plugin;
  NeedsVerdict verdict;
};
std::vector<PluginNeedsReport> EvaluateAllNeedsNow();

}  // namespace readyup::plugins
