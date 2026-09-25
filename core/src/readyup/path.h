#pragma once

#include <string>

namespace readyup {

std::string DladdrDescribe(void* p);

// Path of this module (the shim) as the loader reported it, canonicalized lexically.
// Under Metamod the loader hands us paths like
//   <game>/bin/linuxsteamrt64/../../csgo/readyup/bin/linuxsteamrt64//libserver.so
// so everything that walks up from here must see the collapsed form.
std::string GetThisModuleDir();
std::string GetThisModulePath();

// Best-effort helper to derive CS2 `csgo/` dir from the shim location.
// module dir: <csgo>/readyup/bin/linuxsteamrt64  -> <csgo>
std::string GetCsgoDirFromModuleDir();

// ---- Pure helpers (no loader access; unit-tested in tests/load_order_test.cpp) ----

// Lexical canonicalization: collapses `//`, `/./` and `/../`, drops a trailing `/`.
// Does not touch the filesystem (symlinks are left as the loader named them).
std::string CanonicalizePath(std::string s);

// <csgo> from a module dir `<csgo>/readyup/bin/linuxsteamrt64` (any spelling). "" if too short.
std::string CsgoDirFromModuleDir(const std::string& moduleDir);

// Name-only fallback for "is this loaded object Valve's server library?". Excludes Ready Up's
// shim and other server-library proxies (Metamod's csgo/addons/metamod/.../libserver.so).
// The shim identifies the real module by its dlopen handle first; this is only used when the
// handle is unavailable.
bool LooksLikeValveServerModule(const std::string& path);

}  // namespace readyup
