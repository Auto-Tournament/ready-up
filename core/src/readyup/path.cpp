#include "readyup/path.h"

#include <dlfcn.h>

namespace readyup {

std::string DladdrDescribe(void* p) {
  if (!p) return "(null)";
  Dl_info info{};
  if (dladdr(p, &info) == 0) return "(dladdr failed)";

  std::string out;
  if (info.dli_fname) out += info.dli_fname;
  else out += "(unknown object)";

  if (info.dli_sname) {
    out += " :: ";
    out += info.dli_sname;
  }
  return out;
}

std::string GetThisModuleDir() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(&GetThisModuleDir), &info) == 0 || info.dli_fname == nullptr) {
    return {};
  }

  std::string path(info.dli_fname);
  auto slash = path.find_last_of('/');
  if (slash == std::string::npos) return {};
  return path.substr(0, slash);
}

std::string GetThisModulePath() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(&GetThisModulePath), &info) == 0 || info.dli_fname == nullptr) {
    return {};
  }
  return std::string(info.dli_fname);
}

std::string GetCsgoDirFromModuleDir() {
  // module dir: <csgo>/readyup/bin/linuxsteamrt64
  // go up 3 levels to reach <csgo>
  std::string d = GetThisModuleDir();
  if (d.empty()) return {};
  for (int i = 0; i < 3; ++i) {
    auto slash = d.find_last_of('/');
    if (slash == std::string::npos) return {};
    d = d.substr(0, slash);
  }
  return d;
}

}  // namespace readyup

