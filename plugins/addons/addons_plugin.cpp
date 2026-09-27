// readyup-addons (plugins/addons, addons.so): Steam Workshop addons for the server.
//
// Step 1 (this file): the server downloads the addons listed in cfg/ReadyUp/addons.cfg
// (`workshop_addons=<id>,...`) and mounts them, so their models and materials exist server side.
// Making connecting players download them comes next (the connect / signon flow).
//
// Engine access, all verified before use:
//  - Steam Workshop: Steam's flat C API in libsteam_api.so (stable exported names, like the core's
//    steam_ugc.cpp). Nothing a CS2 update can move.
//  - Mounting: the engine's own `IApplication` (CreateInterface "VApplication001", libengine2.so)
//    slot 37, a thunk `mov rdi,[rdi+8]; mov edx,1; jmp MountAddon`. MountAddon is this plugin's
//    engine-surface entry Engine2_MountAddon (signature + string anchor, checked by CI on every CS2
//    build). The thunk's bytes and jump target are compared with it at runtime; anything else and
//    nothing is mounted.
#include "addons_rules.h"

#include "readyup/plugin_api.h"
#include "readyup/selftest_iface.h"

#include <dlfcn.h>

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
using GetItemInstallInfoFn = bool (*)(void*, uint64_t, uint64_t*, char*, uint32_t, uint32_t*);

struct Steam {
  UgcAccessorFn accessor = nullptr;
  GetItemStateFn itemState = nullptr;
  DownloadItemFn download = nullptr;
  GetItemInstallInfoFn installInfo = nullptr;
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
  g_steam.installInfo = reinterpret_cast<GetItemInstallInfoFn>(dlsym(lib, "SteamAPI_ISteamUGC_GetItemInstallInfo"));
  if (!g_steam.accessor) g_steam.status = "no SteamAPI_SteamGameServerUGC_v0xx export";
  else if (!g_steam.itemState || !g_steam.download || !g_steam.installInfo) g_steam.status = "missing ISteamUGC exports";
}

void* Ugc() {
  ResolveSteam();
  if (!g_steam.accessor || !g_steam.itemState || !g_steam.download || !g_steam.installInfo) return nullptr;
  return g_steam.accessor();  // NULL until the game server is logged on to Steam
}

// ---- Mounting ----------------------------------------------------------------------------------
constexpr int kMountSlot = 37;  // IApplication: void MountAddon(const char* name)
using CreateInterfaceFn = void* (*)(const char*, int*);
using MountFn = void (*)(void*, const char*);

struct Mounter {
  void* app = nullptr;
  MountFn mount = nullptr;
  std::string status = "not resolved";
} g_mounter;

// The slot must still be `48 8B 7F 08  BA 01 00 00 00  E9 <rel32>` jumping to Engine2_MountAddon.
void ResolveMounter() {
  if (g_mounter.mount) return;
  void* target = RU_API_HAS(g_api, surface_function) ? g_api->surface_function(g_api->self, "Engine2_MountAddon") : nullptr;
  if (!target) {
    g_mounter.status = "Engine2_MountAddon unresolved (engine-surface.addons.json)";
    return;
  }
  void* eng = dlopen("libengine2.so", RTLD_NOW | RTLD_NOLOAD);
  auto ci = eng ? reinterpret_cast<CreateInterfaceFn>(dlsym(eng, "CreateInterface")) : nullptr;
  void* app = ci ? ci("VApplication001", nullptr) : nullptr;
  if (!app) {
    g_mounter.status = "no VApplication001";
    return;
  }
  void** vt = *reinterpret_cast<void***>(app);
  const auto* p = static_cast<const uint8_t*>(vt[kMountSlot]);
  static const uint8_t kThunk[] = {0x48, 0x8B, 0x7F, 0x08, 0xBA, 0x01, 0x00, 0x00, 0x00, 0xE9};
  if (std::memcmp(p, kThunk, sizeof(kThunk)) != 0) {
    g_mounter.status = "IApplication slot 37 is not the MountAddon thunk on this build";
    return;
  }
  int32_t rel = 0;
  std::memcpy(&rel, p + sizeof(kThunk), sizeof(rel));
  if (p + sizeof(kThunk) + sizeof(rel) + rel != target) {
    g_mounter.status = "IApplication slot 37 does not jump to Engine2_MountAddon";
    return;
  }
  g_mounter.app = app;
  g_mounter.mount = reinterpret_cast<MountFn>(vt[kMountSlot]);
  g_mounter.status = "ok (VApplication001 slot 37 -> Engine2_MountAddon)";
}

// ---- State -------------------------------------------------------------------------------------
struct Item {
  bool downloadAsked = false;
  bool mounted = false;
  bool refused = false;
  uint32_t lastState = 0xFFFFFFFFu;
};
std::vector<uint64_t> g_ids;  // from addons.cfg, in order
std::map<uint64_t, Item> g_items;
double g_lastConfig = -1e9, g_lastPoll = -1e9;

std::string ConfigValue(const char* key) {
  char buf[1024] = {};
  return g_api->config_get(g_api->self, key, buf, sizeof(buf)) > 0 ? std::string(buf) : std::string();
}

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
      case Action::kMount: {
        ResolveMounter();
        if (!g_mounter.mount) break;  // status explains; `ru addons` shows it
        const std::string name = std::to_string(id);
        g_mounter.mount(g_mounter.app, name.c_str());
        it.mounted = true;
        ru_logf(g_api, RU_LOG_INFO, "addon %llu: mounted", static_cast<unsigned long long>(id));
        break;
      }
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
  if (it.mounted) s += ", mounted";
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
  Reply(ctx, std::to_string(g_ids.size()) + " addon(s); steam " + g_steam.status + "; mount " + g_mounter.status);
  for (uint64_t id : g_ids) Reply(ctx, StatusLine(id));
}

void RunSelftest(ru_selftest_add_fn add, void* ctx) {
  ResolveSteam();
  add(ctx, "INFO", "addons", (std::to_string(g_ids.size()) + " addon(s), steam " + g_steam.status + ", mount " + g_mounter.status).c_str());
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
      (1u << 16) | 8u,  // needs API 1.8 (surface_function)
      "addons",
      ADDONS_VERSION,
      "Ready Up",
      "Steam Workshop addons for the server (cfg/ReadyUp/addons.cfg)",
  };
  return &info;
}

READYUP_PLUGIN_EXPORT int readyup_plugin_load(const ru_api* api, uint32_t core_api_version) {
  if (RU_API_VERSION_MAJOR(core_api_version) != 1 || !RU_API_HAS(api, surface_function)) return 1;
  g_api = api;
  g_ids.clear();
  g_items.clear();
  g_lastConfig = g_lastPoll = -1e9;
  g_mounter = Mounter{};
  api->on_tick(api->self, &OnTick, nullptr);
  api->register_ru_subcommand(api->self, "addons", &OnRu, nullptr);
  if (RU_API_HAS(api, provide_interface)) {
    api->provide_interface(api->self, RU_SELFTEST_IFACE_PREFIX "addons", RU_SELFTEST_IFACE_VERSION,
                           const_cast<ru_selftest_iface_v1*>(&g_selftestIface));
  }
  ru_logf(api, RU_LOG_INFO, "loaded " ADDONS_VERSION " (workshop_addons in cfg/ReadyUp/addons.cfg)");
  return 0;
}

READYUP_PLUGIN_EXPORT void readyup_plugin_unload(void) {
  // Mounted addons stay mounted (the engine has no safe unmount for content in use).
  ru_logf(g_api, RU_LOG_INFO, "unloaded");
  g_api = nullptr;
}

}  // extern "C"
