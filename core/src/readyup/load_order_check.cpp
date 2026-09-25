// Runtime parts of load_order.h (called from readyup_ctor).
#include "readyup/load_order.h"

#include "readyup/logging.h"
#include "readyup/path.h"

#include <link.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace readyup {
namespace {

constexpr const char* kInstanceEnv = "READYUP_SHIM_INSTANCE";

}  // namespace

bool ClaimShimInstance(std::string* otherPath) {
  const std::string ours = GetThisModulePath();
  const char* cur = std::getenv(kInstanceEnv);
  const long pid = static_cast<long>(getpid());
  if (!ShimInstanceMayRun(cur ? cur : "", pid, ours, otherPath)) return false;
  const std::string marker = std::to_string(pid) + " " + ours;
  setenv(kInstanceEnv, marker.c_str(), 1);
  return true;
}

bool MetamodLoaded() {
  bool found = false;
  dl_iterate_phdr(
      [](struct dl_phdr_info* info, size_t, void* data) -> int {
        if (!info->dlpi_name || !*info->dlpi_name) return 0;
        const std::string p = CanonicalizePath(info->dlpi_name);
        if (p.find("/addons/metamod/bin/") != std::string::npos) {
          *static_cast<bool*>(data) = true;
          return 1;
        }
        return 0;
      },
      &found);
  return found;
}

void CheckLoadOrder() {
  if (MetamodLoaded()) {
    PrintLine("load-order: Metamod:Source is loaded ahead of Ready Up (supported order: Metamod -> Ready Up -> "
              "Valve). See docs/COMPATIBILITY.md for what is and is not supported alongside it.");
  }
  const std::string csgo = GetCsgoDirFromModuleDir();
  if (csgo.empty()) return;
  const std::string gi = csgo + "/gameinfo.gi";
  std::ifstream f(gi);
  if (!f) return;
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string problem = DescribeLoadOrderProblem(ParseGameSearchPaths(ss.str()));
  if (problem.empty()) return;
  Print("load-order: WARNING: %s. Fix: python3 %s/readyup/tools/patch_gameinfo.py %s\n", problem.c_str(),
        csgo.c_str(), gi.c_str());
}

}  // namespace readyup
