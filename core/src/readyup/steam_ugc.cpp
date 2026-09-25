#include "readyup/steam_ugc.h"

#include <dlfcn.h>

#include <mutex>

namespace readyup::steam_ugc {
namespace {

using AccessorFn = void* (*)();
using DownloadInfoFn = bool (*)(void* ugc, uint64_t id, uint64_t* downloaded, uint64_t* total);

std::once_flag g_once;
AccessorFn g_accessor = nullptr;
DownloadInfoFn g_downloadInfo = nullptr;
std::string g_status = "not looked up";

void Resolve() {
  // Already loaded by the engine; RTLD_NOLOAD never loads a second copy.
  void* lib = dlopen("libsteam_api.so", RTLD_NOW | RTLD_NOLOAD);
  if (!lib) {
    g_status = "libsteam_api.so not loaded";
    return;
  }
  g_downloadInfo = reinterpret_cast<DownloadInfoFn>(dlsym(lib, "SteamAPI_ISteamUGC_GetItemDownloadInfo"));
  // The accessor carries the interface version (SteamGameServerUGC() in steam_api.h).
  for (int v = 22; v >= 14; --v) {
    const std::string name = "SteamAPI_SteamGameServerUGC_v" + std::string(v < 10 ? "00" : "0") + std::to_string(v);
    if (auto* fn = reinterpret_cast<AccessorFn>(dlsym(lib, name.c_str()))) {
      g_accessor = fn;
      g_status = name;
      break;
    }
  }
  if (!g_accessor) g_status = "no SteamAPI_SteamGameServerUGC_v0xx export";
  else if (!g_downloadInfo) g_status = "no SteamAPI_ISteamUGC_GetItemDownloadInfo export";
  else g_status = "ok (" + g_status + ")";
}

}  // namespace

bool DownloadProgress(uint64_t workshopId, uint64_t* downloaded, uint64_t* total) {
  std::call_once(g_once, Resolve);
  if (!g_accessor || !g_downloadInfo || workshopId == 0) return false;
  void* ugc = g_accessor();
  if (!ugc) return false;
  uint64_t d = 0, t = 0;
  if (!g_downloadInfo(ugc, workshopId, &d, &t)) return false;
  if (downloaded) *downloaded = d;
  if (total) *total = t;
  return true;
}

std::string Status() {
  std::call_once(g_once, Resolve);
  return g_status;
}

}  // namespace readyup::steam_ugc
