#include "readyup/path.h"

#include <dlfcn.h>

#include <utility>
#include <vector>

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

std::string CanonicalizePath(std::string s) {
  const bool abs = !s.empty() && s[0] == '/';

  std::vector<std::string> parts;
  parts.reserve(32);

  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == '/') ++i;
    if (i >= s.size()) break;
    size_t j = i;
    while (j < s.size() && s[j] != '/') ++j;
    std::string seg = s.substr(i, j - i);
    i = j;

    if (seg.empty() || seg == ".") continue;
    if (seg == "..") {
      if (!parts.empty() && parts.back() != "..") {
        parts.pop_back();
      } else if (!abs) {
        parts.push_back("..");
      }
      continue;
    }
    parts.push_back(std::move(seg));
  }

  std::string out;
  if (abs) out.push_back('/');
  for (size_t k = 0; k < parts.size(); ++k) {
    if (k != 0) out.push_back('/');
    out.append(parts[k]);
  }
  if (out.empty()) out = abs ? "/" : ".";
  return out;
}

std::string CsgoDirFromModuleDir(const std::string& moduleDir) {
  if (moduleDir.empty()) return {};
  std::string d = CanonicalizePath(moduleDir);
  for (int i = 0; i < 3; ++i) {
    auto slash = d.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return {};
    d = d.substr(0, slash);
  }
  return d;
}

bool LooksLikeValveServerModule(const std::string& path) {
  if (path.empty()) return false;
  const std::string s = CanonicalizePath(path);
  static const std::string kTail = "/bin/linuxsteamrt64/libserver.so";
  if (s.size() < kTail.size() || s.compare(s.size() - kTail.size(), kTail.size(), kTail) != 0) return false;
  // Our own shim (csgo/readyup/bin/...) and anything installed as an addon that stands in for
  // the server library (Metamod: csgo/addons/metamod/bin/...) are not Valve's module.
  if (s.find("/readyup/bin/linuxsteamrt64/") != std::string::npos) return false;
  if (s.find("/addons/") != std::string::npos) return false;
  return true;
}

std::string GetThisModuleDir() {
  const std::string path = GetThisModulePath();
  auto slash = path.find_last_of('/');
  if (slash == std::string::npos) return {};
  return path.substr(0, slash);
}

std::string GetThisModulePath() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(&GetThisModulePath), &info) == 0 || info.dli_fname == nullptr) {
    return {};
  }
  return CanonicalizePath(std::string(info.dli_fname));
}

std::string GetCsgoDirFromModuleDir() {
  // module dir: <csgo>/readyup/bin/linuxsteamrt64
  return CsgoDirFromModuleDir(GetThisModuleDir());
}

}  // namespace readyup
