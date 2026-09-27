// readyup-addons (plugins/addons, addons.so): Steam Workshop addons for the server.
//
// The server downloads the addons listed in cfg/ReadyUp/addons.cfg (`workshop_addons=<id>,...`)
// and the engine mounts them on the next map change, like it mounts a workshop map. Making
// connecting players download them comes next (the connect / signon flow).
//
// Engine access, all verified before use:
//  - Steam Workshop: Steam's flat C API in libsteam_api.so (stable exported names, like the core's
//    steam_ugc.cpp). Nothing a CS2 update can move.
//  - Mounting: the engine only mounts addons while it loads a map, from the addon list of the
//    pending host-state request (calling its MountAddon from a tick crashed the server). This
//    plugin detours CHostStateMgr::SetPendingHostStateRequest (engine-surface entry
//    Engine2_SetPendingHostStateRequest: libengine2 signature + string anchor, prologue checked by
//    readyup_hookcheck on every CS2 build) and appends the installed addons to the request's
//    m_Addons (CUtlString at +0x58; the request is 0x68 bytes and the engine reads m_Desc +0x10,
//    m_ID +0x1c, m_iMode +0x20 there). The list keeps what the request already had first (a
//    workshop map's own addon). Strings use tier0's allocator (MemAlloc_StrDupFunc / FreeFunc), the
//    one CUtlString frees with.
#include "addons_rules.h"

#include "readyup/plugin_api.h"
#include "readyup/selftest_iface.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace addons {
namespace {

const ru_api* g_api = nullptr;

// ---- Steam Workshop (flat C API) -------------------------------------------------------------
using UgcAccessorFn = void* (*)();
using GetItemStateFn = uint32_t (*)(void*, uint64_t);
using DownloadItemFn = bool (*)(void*, uint64_t, bool);

struct Steam {
  UgcAccessorFn accessor = nullptr;
  GetItemStateFn itemState = nullptr;
  DownloadItemFn download = nullptr;
  std::string status = "not resolved";
} g_steam;

void ResolveSteam() {
  if (g_steam.accessor) return;
  void* lib = dlopen("libsteam_api.so", RTLD_NOW | RTLD_NOLOAD);
  if (!lib) {
    g_steam.status = "libsteam_api.so not loaded";
    return;
  }
  for (int v = 30; v >= 10 && !g_steam.accessor; --v) {
    const std::string name = "SteamAPI_SteamGameServerUGC_v0" + std::to_string(v);
    g_steam.accessor = reinterpret_cast<UgcAccessorFn>(dlsym(lib, name.c_str()));
    if (g_steam.accessor) g_steam.status = name;
  }
  g_steam.itemState = reinterpret_cast<GetItemStateFn>(dlsym(lib, "SteamAPI_ISteamUGC_GetItemState"));
  g_steam.download = reinterpret_cast<DownloadItemFn>(dlsym(lib, "SteamAPI_ISteamUGC_DownloadItem"));
  if (!g_steam.accessor) g_steam.status = "no SteamAPI_SteamGameServerUGC_v0xx export";
  else if (!g_steam.itemState || !g_steam.download) g_steam.status = "missing ISteamUGC exports";
}

void* Ugc() {
  ResolveSteam();
  if (!g_steam.accessor || !g_steam.itemState || !g_steam.download) return nullptr;
  return g_steam.accessor();  // NULL until the game server is logged on to Steam
}

// ---- tier0 allocator (CUtlString's) ------------------------------------------------------------
using StrDupFn = char* (*)(const char*);
using FreeFn = void (*)(void*);
StrDupFn g_strdup = nullptr;
FreeFn g_free = nullptr;

bool ResolveTier0() {
  if (g_strdup && g_free) return true;
  void* t0 = dlopen("libtier0.so", RTLD_NOW | RTLD_NOLOAD);
  if (!t0) return false;
  g_strdup = reinterpret_cast<StrDupFn>(dlsym(t0, "MemAlloc_StrDupFunc"));
  g_free = reinterpret_cast<FreeFn>(dlsym(t0, "MemAlloc_FreeFunc"));
  return g_strdup && g_free;
}

// ---- State -------------------------------------------------------------------------------------
struct Item {
  bool downloadAsked = false;
  bool installed = false;
  bool mounted = false;  // attached to a map change (the engine logs "Mounting addon '<id>'")
  bool refused = false;
  uint32_t lastState = 0xFFFFFFFFu;
};
std::vector<uint64_t> g_ids;  // from addons.cfg, in order
std::map<uint64_t, Item> g_items;
double g_lastConfig = -1e9, g_lastPoll = -1e9;
std::string g_hookStatus = "not installed";

std::string ConfigValue(const char* key) {
  char buf[1024] = {};
  return g_api->config_get(g_api->self, key, buf, sizeof(buf)) > 0 ? std::string(buf) : std::string();
}

// ---- The map-change hook -----------------------------------------------------------------------
constexpr int kRequestAddons = 0x58;  // CHostStateRequest::m_Addons (CUtlString)
using SetPendingFn = void (*)(void*, void*);
SetPendingFn g_origSetPending = nullptr;

// "a,b" + installed ids not in it yet -> "a,b,c". Empty when nothing to add.
std::string MergedAddons(const char* current) {
  std::string out = current ? current : "";
  int bad = 0;
  const auto have = ParseIds(out, &bad);
  bool added = false;
  for (uint64_t id : g_ids) {
    const auto it = g_items.find(id);
    if (it == g_items.end() || !it->second.installed) continue;
    if (std::find(have.begin(), have.end(), id) != have.end()) continue;
    out += (out.empty() ? "" : ",") + std::to_string(id);
    added = true;
  }
  return added ? out : std::string();
}

void DetourSetPending(void* mgr, void* request) {
  if (g_api && request && ResolveTier0()) {
    char** addons = reinterpret_cast<char**>(static_cast<unsigned char*>(request) + kRequestAddons);
    const std::string merged = MergedAddons(*addons);
    if (!merged.empty()) {
      char* dup = g_strdup(merged.c_str());
      if (dup) {
        if (*addons) g_free(*addons);
        *addons = dup;
        for (auto& kv : g_items) {
          if (kv.second.installed) kv.second.mounted = true;
        }
        ru_logf(g_api, RU_LOG_INFO, "map change: addons %s", merged.c_str());
      }
    }
  }
  g_origSetPending(mgr, request);
}

void InstallHook() {
  void* tramp = nullptr;
  if (g_api->hook_function(g_api->self, "Engine2_SetPendingHostStateRequest", reinterpret_cast<void*>(&DetourSetPending),
                           &tramp) == 1) {
    g_origSetPending = reinterpret_cast<SetPendingFn>(tramp);
    g_hookStatus = "ok";
  } else {
    g_hookStatus = "refused (see log): addons are downloaded but not mounted";
  }
}

// ---- Polling -----------------------------------------------------------------------------------
void Poll() {
  void* ugc = Ugc();
  if (!ugc) return;  // not logged on yet: next poll
  for (uint64_t id : g_ids) {
    Item& it = g_items[id];
    const uint32_t st = g_steam.itemState(ugc, id);
    if (st != it.lastState) {
      it.lastState = st;
      ru_logf(g_api, RU_LOG_INFO, "addon %llu: %s", static_cast<unsigned long long>(id), ItemStateText(st).c_str());
    }
    switch (NextAction(st, it.mounted, it.downloadAsked)) {
      case Action::kDownload:
        it.downloadAsked = g_steam.download(ugc, id, true);
        ru_logf(g_api, it.downloadAsked ? RU_LOG_INFO : RU_LOG_WARN, "addon %llu: download %s",
                static_cast<unsigned long long>(id), it.downloadAsked ? "requested" : "refused by Steam");
        break;
      case Action::kMount:
        if (!it.installed) {
          it.installed = true;
          ru_logf(g_api, RU_LOG_INFO, "addon %llu: installed; mounts with the next map change (.ru map)",
                  static_cast<unsigned long long>(id));
        }
        break;
      case Action::kRefuse:
        if (!it.refused) ru_logf(g_api, RU_LOG_WARN, "addon %llu: legacy item, not a CS2 addon", static_cast<unsigned long long>(id));
        it.refused = true;
        break;
      case Action::kNone:
        break;
    }
  }
}

void OnTick(void*, const ru_tick_info* t) {
  if (t->now - g_lastConfig >= 5.0) {
    g_lastConfig = t->now;
    int bad = 0;
    auto ids = ParseIds(ConfigValue("workshop_addons"), &bad);
    if (ids != g_ids) {
      g_ids = std::move(ids);
      ru_logf(g_api, RU_LOG_INFO, "workshop_addons: %zu addon(s)%s", g_ids.size(), bad ? " (some entries are not workshop ids)" : "");
    }
  }
  if (t->now - g_lastPoll >= 2.0 && !g_ids.empty()) {
    g_lastPoll = t->now;
    Poll();
  }
}

std::string StatusLine(uint64_t id) {
  const Item& it = g_items[id];
  std::string s = std::to_string(id) + ": " + (it.lastState == 0xFFFFFFFFu ? "unknown" : ItemStateText(it.lastState));
  if (it.mounted) s += ", mounted with this map";
  else if (it.installed) s += ", mounts with the next map change";
  return s;
}

void Reply(const ru_command_ctx* ctx, const std::string& msg) {
  const std::string line = "Ready Up addons: " + msg;
  if (ctx->slot >= 0) g_api->chat_to_slot(g_api->self, ctx->slot, line.c_str());
  else ru_logf(g_api, RU_LOG_INFO, "%s", msg.c_str());
}

// `ru addons`: status of every configured addon.
void OnRu(void*, const ru_command_ctx* ctx) {
  ResolveSteam();
  Reply(ctx, std::to_string(g_ids.size()) + " addon(s); steam " + g_steam.status + "; map-change hook " + g_hookStatus);
  for (uint64_t id : g_ids) Reply(ctx, StatusLine(id));
}

void RunSelftest(ru_selftest_add_fn add, void* ctx) {
  ResolveSteam();
  add(ctx, g_hookStatus == "ok" ? "INFO" : "WARN", "addons",
      (std::to_string(g_ids.size()) + " addon(s), steam " + g_steam.status + ", map-change hook " + g_hookStatus).c_str());
  for (uint64_t id : g_ids) add(ctx, "INFO", "addons", StatusLine(id).c_str());
}
const ru_selftest_iface_v1 g_selftestIface = {sizeof(ru_selftest_iface_v1), &RunSelftest};

}  // namespace
}  // namespace addons

using namespace addons;

extern "C" {

READYUP_PLUGIN_EXPORT const ru_plugin_info* readyup_plugin_info(void) {
  static const ru_plugin_info info = {
      sizeof(ru_plugin_info),
      (1u << 16) | 8u,  // needs API 1.8 (hook_function)
      "addons",
      ADDONS_VERSION,
      "Ready Up",
      "Steam Workshop addons for the server (cfg/ReadyUp/addons.cfg)",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, hook_function)) return 1;
  g_api = api;
  g_ids.clear();
  g_items.clear();
  g_lastConfig = g_lastPoll = -1e9;
  g_origSetPending = nullptr;
  InstallHook();
  api->on_tick(api->self, &OnTick, nullptr);
  api->register_ru_subcommand(api->self, "addons", &OnRu, nullptr);
  if (RU_API_HAS(api, provide_interface)) {
    api->provide_interface(api->self, RU_SELFTEST_IFACE_PREFIX "addons", RU_SELFTEST_IFACE_VERSION,
                           const_cast<ru_selftest_iface_v1*>(&g_selftestIface));
  }
  ru_logf(api, RU_LOG_INFO, "loaded " ADDONS_VERSION " (workshop_addons in cfg/ReadyUp/addons.cfg; map-change hook %s)",
          g_hookStatus.c_str());
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  // The core removes the detour after this returns. Mounted addons stay until the next map change.
  ru_logf(g_api, RU_LOG_INFO, "unloaded");
  g_api = nullptr;
}

}  // extern "C"
