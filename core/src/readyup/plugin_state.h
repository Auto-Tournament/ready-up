#pragma once

// Which plugins stay off: csgo/readyup/plugins/plugins.json (next to the .so files),
// `{"version": 1, "disabled": ["skins"]}`.
// `ru plugin disable <name>` adds a name (and unloads it), `ru plugin enable <name>` removes it
// (and loads it); the boot scan of csgo/readyup/plugins skips disabled plugins, so the choice
// survives a restart. Pure parsing / formatting here (ctest `plugin_state`); the loader reads
// and writes the file.

#include <set>
#include <string>

namespace readyup::plugins {

// The disabled names in plugins.json text. Unknown keys are ignored; a missing file (""), bad
// JSON or a non-string entry yields what could be read (bad JSON: nothing) and sets *ok false
// for bad JSON.
std::set<std::string> ParseDisabled(const std::string& json, bool* ok);
// plugins.json text for `disabled` (sorted, one line per name, trailing newline).
std::string DisabledJson(const std::set<std::string>& disabled);

// The file in the plugins dir (<dir>/plugins.json); "" without a dir.
std::string PluginStatePath(const std::string& pluginsDir);
// Read / write the file (write: temp file + rename). Read of a missing file: empty set.
std::set<std::string> LoadDisabled(const std::string& path);
bool SaveDisabled(const std::string& path, const std::set<std::string>& disabled);

}  // namespace readyup::plugins
