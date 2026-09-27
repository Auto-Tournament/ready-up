#pragma once
// Engine hooks for plugins (ru_api 1.8: surface_function / hook_function / hook_vtable).
//
// Only entries listed in the engine surface can be touched, which in practice means a plugin's own
// gamedata fragment (engine-surface.<plugin>.json, merged at load, shipped only with that plugin).
// Functions must be marked "hook": "funchook" (readyup_hookcheck checks their prologue on every
// CS2 build) and must not be ones the core hooks itself; vtable slots must verify (RTTI + target).
// Everything a plugin installed is removed by DropPluginHooks when it unloads. A server that does
// not load such a plugin resolves and hooks nothing extra.
#include <string>
#include <vector>

namespace readyup::plugins::hooks {

void* SurfaceFunction(const char* key);
bool HookFunction(int owner, const std::string& plugin, const char* key, void* detour, void** trampoline,
                  std::string* why);
bool HookVtable(int owner, const std::string& plugin, const char* key, const void* obj, void* fn, void** original,
                std::string* why);
// Removes every hook `owner` installed (vtable slots restored, detours uninstalled).
void DropPluginHooks(int owner, const std::string& plugin);
// "fn <key> (plugin <id>)" / "vtable <key> (plugin <id>)" for ru selftest.
std::vector<std::string> Report();

}  // namespace readyup::plugins::hooks
