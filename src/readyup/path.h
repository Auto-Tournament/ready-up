#pragma once

#include <string>

namespace readyup {

std::string DladdrDescribe(void* p);
std::string GetThisModuleDir();
std::string GetThisModulePath();

// Best-effort helper to derive CS2 `csgo/` dir from the shim location.
// module dir: <csgo>/readyup/bin/linuxsteamrt64  -> <csgo>
std::string GetCsgoDirFromModuleDir();

}  // namespace readyup

